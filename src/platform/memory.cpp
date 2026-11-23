#include "platform/memory.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
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

bool safe_write(uint64_t address, const void* data, size_t size) {
  SIZE_T done = 0;
  return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, size, &done) && done == size;
}

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

void* map_fixed(uint64_t address, size_t size, Prot prot) {
  void* want = reinterpret_cast<void*>(address);
  void* p = mmap(want, size, to_native(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (p != want) {  // very old kernels treat MAP_FIXED_NOREPLACE as a hint only
    munmap(p, size);
    return nullptr;
  }
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

bool unmap(void* address, size_t size) { return munmap(address, size) == 0; }

bool protect(void* address, size_t size, Prot prot) { return mprotect(address, size, to_native(prot)) == 0; }

bool decommit(void* address, size_t size) {
  madvise(address, size, MADV_DONTNEED);
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
  return p != MAP_FAILED;
}

bool remap_anonymous(uint64_t address, size_t size, Prot prot) {
  void* p = mmap(reinterpret_cast<void*>(address), size, to_native(prot),
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  return p != MAP_FAILED;
}

bool discard_shared(intptr_t memory, uint64_t offset, size_t size) {
  return fallocate(static_cast<int>(memory), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                   static_cast<off_t>(size)) == 0;
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
