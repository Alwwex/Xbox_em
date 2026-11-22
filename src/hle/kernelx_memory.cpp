// kernelx: virtual memory and heaps.
//
// Address space is emulated with host mappings. Windows' reserve/commit model
// is mapped onto PROT_NONE reservations whose pages are made accessible on
// commit. Console-specific allocation flags (graphics/ESRAM memory types on
// Xbox) are accepted and reported, but all memory is ordinary host memory.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>

#include "hle/hle_util.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;
using platform::Prot;

constexpr size_t kGranularity = platform::kAllocationGranularity;

struct Reservation {
  uint64_t size;
  DWORD protect;
};

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

void* XBEM_GUEST_ABI hle_VirtualAlloc(void* address, SIZE_T size, DWORD type, DWORD protect) {
  if (size == 0) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  constexpr DWORD kKnown = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_LARGE_PAGES |
                           0x200000 /*MEM_WRITE_WATCH*/ | 0x400000 /*MEM_PHYSICAL*/;
  if (type & ~kKnown)
    XBEM_WARN_ONCE("kernelx", "VirtualAlloc: console-specific allocation flags {:#x} ignored (plain host memory)",
                   type & ~kKnown);

  const size_t ps = platform::page_size();
  const auto addr = reinterpret_cast<uint64_t>(address);
  if (type & MEM_RESET) return address;  // contents may be discarded - keeping them is valid
  const bool commit = type & MEM_COMMIT;
  const Prot prot = commit ? to_prot(protect) : Prot::None;

  std::lock_guard lock(g_vm_mutex);
  if ((type & MEM_RESERVE) || !address) {
    void* p;
    uint64_t base, length;
    if (address) {
      base = platform::align_down(addr, kGranularity);
      length = platform::align_up(addr + size, ps) - base;
      p = platform::map_fixed(base, length, prot);
    } else {
      length = platform::align_up(size, ps);
      p = platform::map_anywhere(length, kGranularity, prot);
      base = reinterpret_cast<uint64_t>(p);
    }
    if (!p) {
      set_last_error(address ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
      return nullptr;
    }
    g_reservations[base] = Reservation{length, protect};
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
  return reinterpret_cast<void*>(start);
}

BOOL XBEM_GUEST_ABI hle_VirtualFree(void* address, SIZE_T size, DWORD type) {
  const auto addr = reinterpret_cast<uint64_t>(address);
  std::lock_guard lock(g_vm_mutex);
  if (type & MEM_RELEASE) {
    auto it = g_reservations.find(addr);
    if (size != 0 || it == g_reservations.end()) {
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
      state = MEM_COMMIT;
      prot = alloc_prot = it->second.protect ? it->second.protect : PAGE_READWRITE;
      kind = MEM_PRIVATE;
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

constexpr uint64_t kHeapMagic = 0x48454D4258ull;  // "XBMEH"
constexpr DWORD HEAP_GENERATE_EXCEPTIONS = 0x4;
constexpr DWORD HEAP_ZERO_MEMORY = 0x8;
constexpr DWORD HEAP_REALLOC_IN_PLACE_ONLY = 0x10;

struct alignas(16) BlockHeader {
  uint64_t size;
  uint64_t magic;
};

uint64_t g_default_heap_storage[2] = {kHeapMagic, 0};

BlockHeader* header_of(void* p) { return reinterpret_cast<BlockHeader*>(p) - 1; }

void* heap_alloc(SIZE_T size, bool zero) {
  auto* h = static_cast<BlockHeader*>(zero ? std::calloc(1, sizeof(BlockHeader) + size)
                                           : std::malloc(sizeof(BlockHeader) + size));
  if (!h) return nullptr;
  h->size = size;
  h->magic = kHeapMagic;
  return h + 1;
}

bool heap_free(void* p) {
  if (!p) return true;
  BlockHeader* h = header_of(p);
  if (h->magic != kHeapMagic) {
    XBEM_WARN("kernelx", "HeapFree of a pointer not allocated by HeapAlloc ({})", p);
    return false;
  }
  h->magic = 0;
  std::free(h);
  return true;
}

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
  BlockHeader* h = header_of(p);
  if (h->magic != kHeapMagic) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  const uint64_t old = h->size;
  if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
    if (size > old) {
      set_last_error(ERROR_NOT_ENOUGH_MEMORY);
      return nullptr;
    }
    h->size = size;
    return p;
  }
  auto* nh = static_cast<BlockHeader*>(std::realloc(h, sizeof(BlockHeader) + size));
  if (!nh) {
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  nh->size = size;
  if ((flags & HEAP_ZERO_MEMORY) && size > old) std::memset(reinterpret_cast<uint8_t*>(nh + 1) + old, 0, size - old);
  return nh + 1;
}

SIZE_T XBEM_GUEST_ABI hle_HeapSize(HANDLE, DWORD, const void* p) {
  if (!p) return ~0ull;
  const BlockHeader* h = reinterpret_cast<const BlockHeader*>(p) - 1;
  return h->magic == kHeapMagic ? h->size : ~0ull;
}

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

uint64_t default_process_heap() { return reinterpret_cast<uint64_t>(&g_default_heap_storage); }

void add_kernelx_memory_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(VirtualAlloc), XBEM_HLE(VirtualFree), XBEM_HLE(VirtualProtect), XBEM_HLE(VirtualQuery),
      XBEM_HLE(GetProcessHeap), XBEM_HLE(HeapCreate), XBEM_HLE(HeapDestroy), XBEM_HLE(HeapAlloc),
      XBEM_HLE(HeapFree), XBEM_HLE(HeapReAlloc), XBEM_HLE(HeapSize), XBEM_HLE(HeapValidate),
      XBEM_HLE(HeapSetInformation), XBEM_HLE(HeapLock), XBEM_HLE(HeapUnlock), XBEM_HLE(HeapCompact),
      XBEM_HLE(LocalAlloc), XBEM_HLE(LocalFree), XBEM_HLE(GlobalAlloc), XBEM_HLE(GlobalFree),
  });
}

}  // namespace xbem::hle
