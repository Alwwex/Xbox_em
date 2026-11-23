// kernelx: virtual memory and heaps.
//
// Address space is emulated with host mappings. Windows' reserve/commit model
// is mapped onto PROT_NONE reservations whose pages are made accessible on
// commit. Console-specific allocation flags (graphics/ESRAM memory types on
// Xbox) are accepted and reported, but all memory is ordinary host memory.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <cstdio>
#include <mutex>

#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "platform/memory.h"

namespace xbem::hle {

// ------------------------------------------------------- block allocator
// Shared by HeapAlloc/LocalAlloc and the C runtime (malloc), like on Windows
// where the CRT allocates from the process heap.

namespace {
constexpr uint64_t kHeapMagic = 0x48454D4258ull;  // "XBMEH"

struct alignas(16) BlockHeader {
  uint64_t size;
  uint64_t magic;
};

BlockHeader* header_of(const void* p) { return const_cast<BlockHeader*>(reinterpret_cast<const BlockHeader*>(p) - 1); }
}  // namespace

void* heap_alloc(size_t size, bool zero) {
  auto* h = static_cast<BlockHeader*>(zero ? std::calloc(1, sizeof(BlockHeader) + size)
                                           : std::malloc(sizeof(BlockHeader) + size));
  if (!h) return nullptr;
  h->size = size;
  h->magic = kHeapMagic;
  return h + 1;
}

bool heap_owns(const void* p) { return p && header_of(p)->magic == kHeapMagic; }

bool heap_free(void* p) {
  if (!p) return true;
  BlockHeader* h = header_of(p);
  if (h->magic != kHeapMagic) {
    XBEM_WARN("heap", "free of a pointer this heap did not allocate ({})", p);
    return false;
  }
  h->magic = 0;
  std::free(h);
  return true;
}

void* heap_realloc(void* p, size_t size, bool zero_new) {
  if (!p) return heap_alloc(size, zero_new);
  BlockHeader* h = header_of(p);
  if (h->magic != kHeapMagic) return nullptr;
  const uint64_t old = h->size;
  auto* nh = static_cast<BlockHeader*>(std::realloc(h, sizeof(BlockHeader) + size));
  if (!nh) return nullptr;
  nh->size = size;
  if (zero_new && size > old) std::memset(reinterpret_cast<uint8_t*>(nh + 1) + old, 0, size - old);
  return nh + 1;
}

size_t heap_block_size(const void* p) { return heap_owns(p) ? header_of(p)->size : ~size_t(0); }

namespace {

using namespace win;
using platform::Prot;

constexpr size_t kGranularity = platform::kAllocationGranularity;

struct Reservation {
  uint64_t size;
  DWORD protect;            // protection given at reservation
  DWORD type = 0;           // allocation type of the reservation
  uint64_t caller = 0;      // guest return address of the allocating call
  DWORD commit_protect = 0; // last commit / VirtualProtect
  uint64_t commit_caller = 0;
  // Placeholders (MEM_RESERVE_PLACEHOLDER): reserved address space that can be
  // split, coalesced and replaced by an allocation or a mapped view.
  bool placeholder = false;
  bool replaced_placeholder = false;  // an allocation made in a placeholder
};

constexpr DWORD MEM_COALESCE_PLACEHOLDERS = 0x1;
constexpr DWORD MEM_PRESERVE_PLACEHOLDER = 0x2;
constexpr DWORD MEM_REPLACE_PLACEHOLDER = 0x4000;  // VirtualAlloc2 / MapViewOfFile3
constexpr DWORD MEM_RESERVE_PLACEHOLDER = 0x40000;

std::mutex g_vm_mutex;
std::map<uint64_t, Reservation> g_reservations;  // keyed by base address

Prot to_prot(DWORD protect) {
  switch (protect & 0xFF) {  // strip PAGE_GUARD / NOCACHE / WRITECOMBINE modifiers
    case PAGE_NOACCESS: return Prot::None;
    case PAGE_READONLY: return Prot::R;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: return Prot::RW;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ: return Prot::RX;
    case PAGE_EXECUTE_READWRITE:
    case 0x80 /*PAGE_EXECUTE_WRITECOPY*/: return Prot::RWX;
    default:
      XBEM_WARN_ONCE("kernelx", "unknown page protection {:#x}, using read/write", protect);
      return Prot::RW;
  }
}

std::map<uint64_t, Reservation>::iterator find_reservation(uint64_t addr) {
  auto it = g_reservations.upper_bound(addr);
  if (it == g_reservations.begin()) return g_reservations.end();
  --it;
  return (addr - it->first < it->second.size) ? it : g_reservations.end();
}

}  // namespace

void* virtual_alloc(void* address, uint64_t size, uint32_t type, uint32_t protect, uint64_t alignment,
                    uint64_t caller) {
  if (size == 0) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  constexpr DWORD kKnown = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_LARGE_PAGES |
                           MEM_REPLACE_PLACEHOLDER | MEM_RESERVE_PLACEHOLDER |
                           0x200000 /*MEM_WRITE_WATCH*/ | 0x400000 /*MEM_PHYSICAL*/ | 0x80000000u /*MEM_4MB_PAGES*/;
  if (alignment == 0) {
    // Large-page allocations come back aligned to their page size on the console.
    alignment = (type & 0x80000000u) ? 4ull << 20 : (type & MEM_LARGE_PAGES) ? 2ull << 20 : kGranularity;
  }
  alignment = std::max<uint64_t>(alignment, kGranularity);
  if (type & ~kKnown)
    XBEM_WARN_ONCE("kernelx", "VirtualAlloc: console-specific allocation flags {:#x} ignored (plain host memory)",
                   type & ~kKnown);

  const size_t ps = platform::page_size();
  const auto addr = reinterpret_cast<uint64_t>(address);
  XBEM_DEBUG("kernelx", "VirtualAlloc({:#x}, {:#x}, type {:#x}, protect {:#x}) from {:#x}", addr, size, type, protect,
             caller);
  if (type & MEM_RESET) return address;  // contents may be discarded - keeping them is valid
  if (type & 0x1000000 /*MEM_RESET_UNDO*/) return address;
  if (!(type & (MEM_COMMIT | MEM_RESERVE))) {
    // Neither reserve nor commit: nothing to do (never change the protection).
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  const bool commit = type & MEM_COMMIT;
  const Prot prot = commit ? to_prot(protect) : Prot::None;

  std::lock_guard lock(g_vm_mutex);
  if (type & MEM_REPLACE_PLACEHOLDER) {
    // The allocation takes the place of exactly one placeholder.
    auto it = g_reservations.find(addr);
    if (!address || it == g_reservations.end() || !it->second.placeholder ||
        it->second.size != platform::align_up(size, ps)) {
      XBEM_WARN("kernelx", "VirtualAlloc({:#x}, {:#x}, MEM_REPLACE_PLACEHOLDER) does not match a placeholder", addr,
                size);
      set_last_error(ERROR_INVALID_ADDRESS);
      return nullptr;
    }
    if (commit && !platform::protect(address, it->second.size, prot)) {
      set_last_error(ERROR_INVALID_ADDRESS);
      return nullptr;
    }
    Reservation& r = it->second;
    r.placeholder = false;
    r.replaced_placeholder = true;
    r.protect = protect;
    r.type = type;
    r.caller = caller;
    r.commit_protect = commit ? protect : 0;
    r.commit_caller = commit ? caller : 0;
    return address;
  }
  if ((type & MEM_RESERVE) || !address) {
    void* p;
    uint64_t base, length;
    if (address) {
      base = platform::align_down(addr, kGranularity);
      length = platform::align_up(addr + size, ps) - base;
      p = platform::map_fixed(base, length, prot);
    } else {
      length = platform::align_up(size, ps);
      p = platform::map_anywhere(length, alignment, prot);
      base = reinterpret_cast<uint64_t>(p);
    }
    if (!p) {
      set_last_error(address ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
      return nullptr;
    }
    Reservation r{length, protect, type, caller, commit ? protect : 0, commit ? caller : 0};
    r.placeholder = (type & MEM_RESERVE_PLACEHOLDER) != 0;
    g_reservations[base] = r;
    XBEM_TRACE("kernelx", "VirtualAlloc reserve {:#x} +{:#x} commit={}", base, length, commit);
    return p;
  }

  // Commit inside an existing reservation.
  auto it = find_reservation(addr);
  if (it == g_reservations.end()) {
    set_last_error(ERROR_INVALID_ADDRESS);
    return nullptr;
  }
  const uint64_t start = platform::align_down(addr, ps);
  const uint64_t end = std::min(platform::align_up(addr + size, ps), it->first + it->second.size);
  if (!platform::protect(reinterpret_cast<void*>(start), end - start, prot)) {
    set_last_error(ERROR_INVALID_ADDRESS);
    return nullptr;
  }
  it->second.commit_protect = protect;
  it->second.commit_caller = caller;
  return reinterpret_cast<void*>(start);
}

namespace {

void* XBEM_GUEST_ABI hle_VirtualAlloc(void* address, SIZE_T size, DWORD type, DWORD protect) {
  return virtual_alloc(address, size, type, protect, 0, XBEM_RETURN_ADDRESS());
}

// Splits the placeholder at `it` so that [start, start + size) becomes a
// placeholder of its own.
bool split_placeholder(std::map<uint64_t, Reservation>::iterator it, uint64_t start, uint64_t size) {
  const uint64_t base = it->first, end = base + it->second.size;
  if (start < base || start + size > end || size == 0) return false;
  Reservation piece = it->second;
  if (start + size < end) {
    Reservation tail = piece;
    tail.size = end - (start + size);
    g_reservations[start + size] = tail;
  }
  if (start > base) {
    it->second.size = start - base;
    piece.size = size;
    g_reservations[start] = piece;
  } else {
    it->second.size = size;
  }
  return true;
}

// Merges the placeholders exactly covering [start, start + size) into one.
bool coalesce_placeholders(uint64_t start, uint64_t size) {
  auto first = g_reservations.find(start);
  if (first == g_reservations.end()) return false;
  uint64_t cursor = start;
  auto it = first;
  while (cursor < start + size) {
    if (it == g_reservations.end() || it->first != cursor || !it->second.placeholder) return false;
    cursor += it->second.size;
    ++it;
  }
  if (cursor != start + size) return false;
  first->second.size = size;
  g_reservations.erase(std::next(first), it);
  return true;
}

// PVOID VirtualAlloc2(HANDLE process, PVOID base, SIZE_T size, ULONG type, ULONG protect,
//                     MEM_EXTENDED_PARAMETER* params, ULONG count)
void* XBEM_GUEST_ABI hle_VirtualAlloc2(HANDLE process, void* address, SIZE_T size, DWORD type, DWORD protect,
                                      const uint64_t* params, DWORD count) {
  uint64_t alignment = 0;
  if (process && process != kCurrentProcessPseudo)
    XBEM_WARN_ONCE("kernelx", "VirtualAlloc2 for another process ({:#x}) - allocating in this one", process);
  for (DWORD i = 0; params && i < count; ++i) {
    const uint64_t kind = params[i * 2] & 0xFF;
    if (kind == 1 /*MemExtendedParameterAddressRequirements*/ && params[i * 2 + 1]) {
      // MEM_ADDRESS_REQUIREMENTS { LowestStartingAddress, HighestEndingAddress, Alignment }
      alignment = reinterpret_cast<const uint64_t*>(params[i * 2 + 1])[2];
    } else if (kind != 0) {
      XBEM_WARN_ONCE("kernelx", "VirtualAlloc2: extended parameter type {} ignored", kind);
    }
  }
  return virtual_alloc(address, size, type, protect, alignment, XBEM_RETURN_ADDRESS());
}

BOOL XBEM_GUEST_ABI hle_VirtualFree(void* address, SIZE_T size, DWORD type) {
  const auto addr = reinterpret_cast<uint64_t>(address);
  std::lock_guard lock(g_vm_mutex);
  if ((type & MEM_RELEASE) && (type & (MEM_PRESERVE_PLACEHOLDER | MEM_COALESCE_PLACEHOLDERS))) {
    auto it = find_reservation(addr);
    bool ok = false;
    if (it != g_reservations.end()) {
      Reservation& r = it->second;
      if (type & MEM_COALESCE_PLACEHOLDERS) {
        ok = coalesce_placeholders(addr, size);
      } else if (r.placeholder) {
        ok = split_placeholder(it, addr, size);
      } else if (r.replaced_placeholder && addr == it->first && (size == 0 || size == r.size)) {
        // Back to a placeholder: drop the contents (and any mapped view).
        ok = platform::remap_anonymous(addr, r.size, Prot::None);
        r.placeholder = true;
        r.replaced_placeholder = false;
        r.commit_protect = 0;
      }
    }
    if (!ok) {
      XBEM_WARN("kernelx", "VirtualFree({:#x}, {:#x}, {:#x}): no matching placeholder", addr, size, type);
      set_last_error(ERROR_INVALID_PARAMETER);
      return FALSE;
    }
    return TRUE;
  }
  if (type & MEM_RELEASE) {
    auto it = g_reservations.find(addr);
    if (size != 0 || it == g_reservations.end()) {
      XBEM_WARN("kernelx", "VirtualFree({:#x}, {:#x}, MEM_RELEASE): not the base of an allocation", addr, size);
      set_last_error(ERROR_INVALID_PARAMETER);
      return FALSE;
    }
    platform::unmap(address, it->second.size);
    g_reservations.erase(it);
    return TRUE;
  }
  if (type & MEM_DECOMMIT) {
    auto it = find_reservation(addr);
    if (it == g_reservations.end()) {
      set_last_error(ERROR_INVALID_ADDRESS);
      return FALSE;
    }
    const size_t ps = platform::page_size();
    const uint64_t start = platform::align_down(addr, ps);
    const uint64_t end = size ? platform::align_up(addr + size, ps) : it->first + it->second.size;
    platform::decommit(reinterpret_cast<void*>(start), end - start);
    return TRUE;
  }
  set_last_error(ERROR_INVALID_PARAMETER);
  return FALSE;
}

BOOL XBEM_GUEST_ABI hle_VirtualProtect(void* address, SIZE_T size, DWORD protect, DWORD* old_protect) {
  if (!old_protect) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const size_t ps = platform::page_size();
  const auto addr = reinterpret_cast<uint64_t>(address);
  const uint64_t start = platform::align_down(addr, ps);
  const uint64_t end = platform::align_up(addr + size, ps);
  *old_protect = PAGE_READWRITE;  // per-page protections are not tracked yet
  {
    std::lock_guard lock(g_vm_mutex);
    if (auto it = find_reservation(addr); it != g_reservations.end()) {
      it->second.commit_protect = protect;
      it->second.commit_caller = XBEM_RETURN_ADDRESS();
    }
  }
  if (!platform::protect(reinterpret_cast<void*>(start), end - start, to_prot(protect))) {
    set_last_error(ERROR_INVALID_ADDRESS);
    return FALSE;
  }
  return TRUE;
}

SIZE_T XBEM_GUEST_ABI hle_VirtualQuery(const void* address, uint8_t* mbi, SIZE_T length) {
  if (!mbi || length < 48) {
    set_last_error(24 /*ERROR_BAD_LENGTH*/);
    return 0;
  }
  const size_t ps = platform::page_size();
  const auto addr = reinterpret_cast<uint64_t>(address);
  uint64_t base = platform::align_down(addr, ps), alloc_base = base, region = ps;
  DWORD state = MEM_FREE, prot = PAGE_NOACCESS, alloc_prot = 0, kind = 0;

  {
    std::lock_guard lock(g_vm_mutex);
    if (auto it = find_reservation(addr); it != g_reservations.end()) {
      alloc_base = it->first;
      region = it->first + it->second.size - base;
      kind = MEM_PRIVATE;
      if (it->second.placeholder) {
        state = MEM_RESERVE;
        prot = 0;
        alloc_prot = PAGE_NOACCESS;
      } else {
        state = MEM_COMMIT;
        prot = alloc_prot = it->second.protect ? it->second.protect : PAGE_READWRITE;
      }
    }
  }
  if (state == MEM_FREE) {
    if (rt::LoadedModule* m = proc().loader().find_containing(addr)) {
      alloc_base = m->handle();
      region = m->handle() + m->size - base;
      state = MEM_COMMIT;
      prot = alloc_prot = PAGE_EXECUTE_READWRITE;
      kind = MEM_IMAGE;
    } else if (rt::GuestThread* t = rt::current_thread(); t && t->teb) {
      const uint64_t lo = *reinterpret_cast<uint64_t*>(t->teb + rt::teb::kStackLimit);
      const uint64_t hi = *reinterpret_cast<uint64_t*>(t->teb + rt::teb::kStackBase);
      if (addr >= lo && addr < hi) {
        alloc_base = lo;
        region = hi - base;
        state = MEM_COMMIT;
        prot = alloc_prot = PAGE_READWRITE;
        kind = MEM_PRIVATE;
      }
    }
  }
  std::memset(mbi, 0, 48);
  std::memcpy(mbi + 0, &base, 8);
  std::memcpy(mbi + 8, &alloc_base, 8);
  std::memcpy(mbi + 16, &alloc_prot, 4);
  std::memcpy(mbi + 24, &region, 8);
  std::memcpy(mbi + 32, &state, 4);
  std::memcpy(mbi + 36, &prot, 4);
  std::memcpy(mbi + 40, &kind, 4);
  return 48;
}

// ------------------------------------------------------------------- heaps

constexpr DWORD HEAP_GENERATE_EXCEPTIONS = 0x4;
constexpr DWORD HEAP_ZERO_MEMORY = 0x8;
constexpr DWORD HEAP_REALLOC_IN_PLACE_ONLY = 0x10;

uint64_t g_default_heap_storage[2] = {kHeapMagic, 0};

HANDLE XBEM_GUEST_ABI hle_GetProcessHeap() { return reinterpret_cast<HANDLE>(&g_default_heap_storage); }

HANDLE XBEM_GUEST_ABI hle_HeapCreate(DWORD, SIZE_T, SIZE_T) {
  // All heaps share the host allocator; a heap handle only needs to be unique.
  auto* token = new uint64_t[2]{kHeapMagic, 0};
  return reinterpret_cast<HANDLE>(token);
}

BOOL XBEM_GUEST_ABI hle_HeapDestroy(HANDLE) { return TRUE; }  // blocks are leaked, like a never-compacted heap

void* XBEM_GUEST_ABI hle_HeapAlloc(HANDLE, DWORD flags, SIZE_T size) {
  void* p = heap_alloc(size, flags & HEAP_ZERO_MEMORY);
  if (!p) {
    if (flags & HEAP_GENERATE_EXCEPTIONS) {
      XBEM_ERROR("kernelx", "HeapAlloc({}) failed with HEAP_GENERATE_EXCEPTIONS", size);
      proc().exit_process(0xC0000017);  // STATUS_NO_MEMORY
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
  }
  return p;
}

BOOL XBEM_GUEST_ABI hle_HeapFree(HANDLE, DWORD, void* p) { return heap_free(p) ? TRUE : FALSE; }

void* XBEM_GUEST_ABI hle_HeapReAlloc(HANDLE heap, DWORD flags, void* p, SIZE_T size) {
  if (!p) return hle_HeapAlloc(heap, flags, size);
  if (!heap_owns(p)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
    if (size > heap_block_size(p)) {
      set_last_error(ERROR_NOT_ENOUGH_MEMORY);
      return nullptr;
    }
    return p;
  }
  void* np = heap_realloc(p, size, flags & HEAP_ZERO_MEMORY);
  if (!np) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
  return np;
}

SIZE_T XBEM_GUEST_ABI hle_HeapSize(HANDLE, DWORD, const void* p) { return heap_block_size(p); }

BOOL XBEM_GUEST_ABI hle_HeapValidate(HANDLE, DWORD, const void*) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_HeapSetInformation(HANDLE, DWORD, void*, SIZE_T) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_HeapLock(HANDLE) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_HeapUnlock(HANDLE) { return TRUE; }
SIZE_T XBEM_GUEST_ABI hle_HeapCompact(HANDLE, DWORD) { return 0; }

constexpr UINT LMEM_ZEROINIT = 0x40;

void* XBEM_GUEST_ABI hle_LocalAlloc(UINT flags, SIZE_T size) { return heap_alloc(size, flags & LMEM_ZEROINIT); }
void* XBEM_GUEST_ABI hle_LocalFree(void* p) { return heap_free(p) ? nullptr : p; }
void* XBEM_GUEST_ABI hle_GlobalAlloc(UINT flags, SIZE_T size) { return heap_alloc(size, flags & LMEM_ZEROINIT); }
void* XBEM_GUEST_ABI hle_GlobalFree(void* p) { return heap_free(p) ? nullptr : p; }

}  // namespace

bool describe_virtual_memory(uint64_t address, char* out, size_t out_size) {
  std::unique_lock lock(g_vm_mutex, std::try_to_lock);  // may run in a signal handler: never block
  if (!lock.owns_lock()) return false;
  auto it = find_reservation(address);
  if (it == g_reservations.end()) return false;
  const Reservation& r = it->second;
  std::snprintf(out, out_size,
                "VirtualAlloc region %#llx+%#llx (type %#x, protect %#x, by %#llx; last commit/protect %#x by %#llx)",
                static_cast<unsigned long long>(it->first), static_cast<unsigned long long>(r.size), r.type, r.protect,
                static_cast<unsigned long long>(r.caller), r.commit_protect,
                static_cast<unsigned long long>(r.commit_caller));
  return true;
}

bool virtual_free(void* address, uint64_t size, uint32_t type) { return hle_VirtualFree(address, size, type) != 0; }

size_t virtual_query(const void* address, uint8_t* mbi, size_t length) { return hle_VirtualQuery(address, mbi, length); }

uint64_t default_process_heap() { return reinterpret_cast<uint64_t>(&g_default_heap_storage); }

void add_kernelx_memory_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(VirtualAlloc), XBEM_HLE(VirtualAlloc2), XBEM_HLE(VirtualFree), XBEM_HLE(VirtualProtect), XBEM_HLE(VirtualQuery),
      XBEM_HLE(GetProcessHeap), XBEM_HLE(HeapCreate), XBEM_HLE(HeapDestroy), XBEM_HLE(HeapAlloc),
      XBEM_HLE(HeapFree), XBEM_HLE(HeapReAlloc), XBEM_HLE(HeapSize), XBEM_HLE(HeapValidate),
      XBEM_HLE(HeapSetInformation), XBEM_HLE(HeapLock), XBEM_HLE(HeapUnlock), XBEM_HLE(HeapCompact),
      XBEM_HLE(LocalAlloc), XBEM_HLE(LocalFree), XBEM_HLE(GlobalAlloc), XBEM_HLE(GlobalFree),
  });
}

}  // namespace xbem::hle
