#include "platform/memory.h"

#include "platform/page_tracker.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#endif

namespace xbem::platform {

#if defined(_WIN32)

namespace {
DWORD to_native(Prot p) {
  switch (p) {
    case Prot::None: return PAGE_NOACCESS;
    case Prot::R: return PAGE_READONLY;
    case Prot::RW: return PAGE_READWRITE;
    case Prot::RX: return PAGE_EXECUTE_READ;
    case Prot::RWX: return PAGE_EXECUTE_READWRITE;
  }
  return PAGE_NOACCESS;
}
}  // namespace

size_t page_size() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
}

void* map_fixed(uint64_t address, size_t size, Prot prot) {
  return VirtualAlloc(reinterpret_cast<void*>(address), size, MEM_RESERVE | MEM_COMMIT, to_native(prot));
}

void* map_anywhere(size_t size, size_t alignment, Prot prot) {
  // VirtualAlloc already aligns to the 64 KiB allocation granularity.
  (void)alignment;
  return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, to_native(prot));
}

bool unmap(void* address, size_t) { return VirtualFree(address, 0, MEM_RELEASE) != 0; }

bool protect(void* address, size_t size, Prot prot) {
  DWORD old = 0;
  return VirtualProtect(address, size, to_native(prot), &old) != 0;
}

bool decommit(void* address, size_t size) { return VirtualFree(address, size, MEM_DECOMMIT) != 0; }

intptr_t create_shared_memory(uint64_t) { return -1; }
bool map_shared(uint64_t, size_t, Prot, intptr_t, uint64_t) { return false; }
bool remap_anonymous(uint64_t, size_t, Prot) { return false; }
bool discard_shared(intptr_t, uint64_t, size_t) { return false; }
bool write_shared(intptr_t, uint64_t, const void*, size_t, bool) { return false; }

bool safe_write(uint64_t address, const void* data, size_t size) {
  SIZE_T done = 0;
  return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, size, &done) && done == size;
}

void* map_guest(size_t size, size_t alignment, Prot prot) { return map_anywhere(size, alignment, prot); }
bool in_guest_arena(uint64_t) { return false; }

bool safe_read(uint64_t address, void* out, size_t size) {
  SIZE_T done = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &done) &&
         done == size;
}

#else

namespace {
int to_native(Prot p) {
  switch (p) {
    case Prot::None: return PROT_NONE;
    case Prot::R: return PROT_READ;
    case Prot::RW: return PROT_READ | PROT_WRITE;
    case Prot::RX: return PROT_READ | PROT_EXEC;
    case Prot::RWX: return PROT_READ | PROT_WRITE | PROT_EXEC;
  }
  return PROT_NONE;
}

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
}  // namespace

size_t page_size() {
  static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  return ps;
}

// ------------------------------------------------------------ guest arena
//
// [kArenaStart, kArenaEnd) is reserved once (PROT_NONE, no backing) and
// handed out first-fit; the kernel's own mappings (host heap, stacks,
// libraries) live near the top of the 47-bit address space and never get
// there. Granule 64 KiB, as Windows.
namespace {
constexpr uint64_t kArenaStart = 0x10'0000'0000ull;  // 64 GiB: clear of images at 0x1'4000'0000 & co.
constexpr uint64_t kArenaEnd = 0xff'0000'0000ull;    // below 2^40
constexpr uint64_t kArenaGranule = 0x10000;

struct Arena {
  std::mutex mutex;
  bool tried = false, ok = false;
  std::map<uint64_t, uint64_t> free;  // start -> size
};
Arena& arena() {
  static Arena a;
  return a;
}
bool arena_ready(Arena& a) {
  if (!a.tried) {
    a.tried = true;
    if (const char* v = std::getenv("XBEM_HIGH_GUEST_MEMORY"); v && *v && *v != '0') return false;
    void* want = reinterpret_cast<void*>(kArenaStart);
    void* p = mmap(want, kArenaEnd - kArenaStart, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == want) {
      a.ok = true;
      a.free[kArenaStart] = kArenaEnd - kArenaStart;
    } else if (p != MAP_FAILED) {
      munmap(p, kArenaEnd - kArenaStart);
    }
  }
  return a.ok;
}
// Makes [start, start+size) of the arena accessible memory (fresh zero pages).
bool arena_commit(uint64_t start, uint64_t size, Prot prot) {
  void* p = mmap(reinterpret_cast<void*>(start), size, to_native(prot),
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  if (p == MAP_FAILED) return false;
  pages::mapped_anonymous(start, size);
  return true;
}
// Removes [start, start+size) from the free list if all of it is free.
bool arena_take(Arena& a, uint64_t start, uint64_t size) {
  auto it = a.free.upper_bound(start);
  if (it == a.free.begin()) return false;
  --it;
  const uint64_t fs = it->first, fe = it->first + it->second;
  if (start < fs || start + size > fe) return false;
  a.free.erase(it);
  if (start > fs) a.free[fs] = start - fs;
  if (start + size < fe) a.free[start + size] = fe - (start + size);
  return true;
}
void arena_give_back(Arena& a, uint64_t start, uint64_t size) {
  auto next = a.free.lower_bound(start);
  if (next != a.free.end() && start + size == next->first) {
    size += next->second;
    next = a.free.erase(next);
  }
  if (next != a.free.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == start) {
      prev->second += size;
      return;
    }
  }
  a.free[start] = size;
}
}  // namespace

bool in_guest_arena(uint64_t address) {
  Arena& a = arena();
  std::lock_guard lock(a.mutex);
  return a.ok && address >= kArenaStart && address < kArenaEnd;
}

void* map_guest(size_t size, size_t alignment, Prot prot) {
  Arena& a = arena();
  {
    std::lock_guard lock(a.mutex);
    if (arena_ready(a)) {
      const uint64_t length = align_up(size, page_size());
      const uint64_t align = std::max<uint64_t>(alignment, kArenaGranule);
      for (auto it = a.free.begin(); it != a.free.end(); ++it) {
        const uint64_t start = align_up(it->first, align);
        if (start + length > it->first + it->second) continue;
        if (!arena_take(a, start, length)) break;
        if (!arena_commit(start, length, prot)) {
          arena_give_back(a, start, length);
          break;
        }
        return reinterpret_cast<void*>(start);
      }
    }
  }
  return map_anywhere(size, alignment, prot);
}

void* map_fixed(uint64_t address, size_t size, Prot prot) {
  {
    Arena& a = arena();
    std::lock_guard lock(a.mutex);
    if (arena_ready(a) && address < kArenaEnd && address + size > kArenaStart) {
      // Inside the arena: only a range that is entirely free.
      const uint64_t start = align_down(address, page_size()), length = align_up(address + size, page_size()) - start;
      if (start < kArenaStart || start + length > kArenaEnd || !arena_take(a, start, length)) return nullptr;
      if (!arena_commit(start, length, prot)) {
        arena_give_back(a, start, length);
        return nullptr;
      }
      return reinterpret_cast<void*>(address);
    }
  }
  void* want = reinterpret_cast<void*>(address);
  void* p = mmap(want, size, to_native(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (p != want) {  // very old kernels treat MAP_FIXED_NOREPLACE as a hint only
    munmap(p, size);
    return nullptr;
  }
  pages::mapped_anonymous(address, align_up(size, page_size()));
  return p;
}

void* map_anywhere(size_t size, size_t alignment, Prot prot) {
  if (alignment <= page_size()) {
    void* p = mmap(nullptr, size, to_native(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
  }
  // Over-allocate, then trim the unaligned head and the tail.
  const size_t total = size + alignment;
  void* raw = mmap(nullptr, total, to_native(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (raw == MAP_FAILED) return nullptr;
  const uint64_t start = reinterpret_cast<uint64_t>(raw);
  const uint64_t aligned = align_up(start, alignment);
  if (aligned > start) munmap(raw, aligned - start);
  const uint64_t end = aligned + size;
  const uint64_t raw_end = start + total;
  if (raw_end > end) munmap(reinterpret_cast<void*>(end), raw_end - end);
  return reinterpret_cast<void*>(aligned);
}

bool unmap(void* address, size_t size) {
  const auto start = reinterpret_cast<uint64_t>(address);
  {
    Arena& a = arena();
    std::lock_guard lock(a.mutex);
    if (a.ok && start >= kArenaStart && start < kArenaEnd) {
      const uint64_t length = align_up(start + size, page_size()) - start;
      void* p = mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
      if (p == MAP_FAILED) return false;
      arena_give_back(a, start, length);
      pages::unmapped(start, length);
      return true;
    }
  }
  if (munmap(address, size) != 0) return false;
  pages::unmapped(start, align_up(size, page_size()));
  return true;
}

bool protect(void* address, size_t size, Prot prot) { return mprotect(address, size, to_native(prot)) == 0; }

bool decommit(void* address, size_t size) {
  madvise(address, size, MADV_DONTNEED);
  pages::mapped_anonymous(reinterpret_cast<uint64_t>(address), align_up(size, page_size()));  // contents reset
  return mprotect(address, size, PROT_NONE) == 0;
}

intptr_t create_shared_memory(uint64_t size) {
  const int fd = memfd_create("xbem-physical", MFD_CLOEXEC);
  if (fd < 0) return -1;
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {  // sparse: costs nothing until touched
    close(fd);
    return -1;
  }
  return fd;
}

bool map_shared(uint64_t address, size_t size, Prot prot, intptr_t memory, uint64_t offset) {
  void* p = mmap(reinterpret_cast<void*>(address), size, to_native(prot), MAP_SHARED | MAP_FIXED,
                 static_cast<int>(memory), static_cast<off_t>(offset));
  if (p == MAP_FAILED) return false;
  pages::mapped_shared(address, size, memory, offset);
  return true;
}

bool remap_anonymous(uint64_t address, size_t size, Prot prot) {
  void* p = mmap(reinterpret_cast<void*>(address), size, to_native(prot),
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  if (p == MAP_FAILED) return false;
  pages::mapped_anonymous(address, size);
  return true;
}

bool discard_shared(intptr_t memory, uint64_t offset, size_t size) {
  pages::shared_written(offset, size);
  return fallocate(static_cast<int>(memory), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                   static_cast<off_t>(size)) == 0;
}

bool write_shared(intptr_t memory, uint64_t offset, const void* data, size_t size, bool report) {
  if (report) pages::shared_written(offset, size);
  const auto* p = static_cast<const uint8_t*>(data);
  while (size) {
    const ssize_t n = pwrite(static_cast<int>(memory), p, size, static_cast<off_t>(offset));
    if (n <= 0) return false;
    p += n;
    offset += static_cast<uint64_t>(n);
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool safe_write(uint64_t address, const void* data, size_t size) {
  iovec local{const_cast<void*>(data), size};
  iovec remote{reinterpret_cast<void*>(address), size};
  return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

bool safe_read(uint64_t address, void* out, size_t size) {
  iovec local{out, size};
  iovec remote{reinterpret_cast<void*>(address), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

#endif

}  // namespace xbem::platform
