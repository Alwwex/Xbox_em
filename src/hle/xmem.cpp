// xmem.dll: the console title memory manager (XMem*).
//
// The exact prototypes are only documented under NDA, so the implementation is
// driven by the public description of the API and by evidence from titles:
//
//  * XMemAlloc/XMemFree(+Default): general allocator with XALLOC_ATTRIBUTES.
//    The attribute encoding is not public; blocks get a generous alignment
//    (at least 64 bytes, up to 64 KiB for large blocks) so any requirement a
//    title can express is met.
//  * XMemVirtualAlloc: VirtualAlloc plus console flags (page size, CPU/GPU
//    visibility, "mappable" reservations). The argument layout is decoded
//    from the values (allocation type and protection are recognisable), and
//    the decoding is logged for the first calls.
//  * Physical pages (XMemAllocatePhysicalPages, XMemMapPhysicalPages[Scatter],
//    XMemFreePhysicalPages): emulated with a host shared-memory object whose
//    64 KiB pages can be mapped (and aliased) anywhere in mappable ranges.
//
// Every call whose decoding is a guess is logged with all arguments and the
// call site, so a run of a real title shows whether the guess was right.

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

// ------------------------------------------------------------- XMemAlloc

std::mutex g_alloc_mutex;
std::unordered_map<uint64_t, uint64_t> g_alloc_sizes;  // pointer -> size (for diagnostics/validation)

size_t alloc_alignment(SIZE_T size) {
  const uint64_t pow2 = std::bit_ceil(std::max<uint64_t>(size, 1));
  return static_cast<size_t>(std::clamp<uint64_t>(pow2, 64, 0x10000));
}

void* aligned_host_alloc(size_t size, size_t alignment) {
#if defined(_WIN32)
  return _aligned_malloc(size, alignment);
#else
  void* p = nullptr;
  return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
}

void aligned_host_free(void* p) {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

void* xmem_alloc(SIZE_T size, uint64_t attributes) {
  void* p = aligned_host_alloc(std::max<SIZE_T>(size, 1), alloc_alignment(size));
  if (!p) {
    XBEM_WARN("xmem", "XMemAlloc({:#x}, attributes {:#x}) failed: out of host memory", size, attributes);
    return nullptr;
  }
  std::lock_guard lock(g_alloc_mutex);
  g_alloc_sizes[reinterpret_cast<uint64_t>(p)] = size;
  return p;
}

void xmem_free(void* p, uint64_t attributes) {
  if (!p) return;
  {
    std::lock_guard lock(g_alloc_mutex);
    if (g_alloc_sizes.erase(reinterpret_cast<uint64_t>(p)) == 0) {
      XBEM_WARN("xmem", "XMemFree({}, attributes {:#x}): not allocated by XMemAlloc - ignored", p, attributes);
      return;
    }
  }
  aligned_host_free(p);
}

void* XBEM_GUEST_ABI hle_XMemAlloc(SIZE_T size, uint64_t attributes) {
  XBEM_LOG_CALL("xmem", "allocated", size, attributes);
  return xmem_alloc(size, attributes);
}
void XBEM_GUEST_ABI hle_XMemFree(void* p, uint64_t attributes) { xmem_free(p, attributes); }
void* XBEM_GUEST_ABI hle_XMemAllocDefault(SIZE_T size, uint64_t attributes) { return xmem_alloc(size, attributes); }
void XBEM_GUEST_ABI hle_XMemFreeDefault(void* p, uint64_t attributes) { xmem_free(p, attributes); }

BOOL XBEM_GUEST_ABI hle_XMemIsToolsMemAvailable() { return FALSE; }  // retail-like: no extra devkit memory

HRESULT XBEM_GUEST_ABI hle_XMemGetWorkingSetStatistics(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  XBEM_LOG_CALL("xmem", "E_NOTIMPL", a, b, c, d);
  return E_NOTIMPL;
}

// ------------------------------------------------------ XMemVirtualAlloc

bool looks_like_alloc_type(uint64_t v) {
  return v <= 0xFFFFFFFFull && (v & (MEM_COMMIT | MEM_RESERVE)) && (v & 0xFFF) == 0;
}

bool looks_like_protect(uint64_t v) {
  const uint32_t low = static_cast<uint32_t>(v & 0xFF);
  return v <= 0xFFFFFFFFull && low && (low & (low - 1)) == 0;
}

struct VirtualAllocArgs {
  bool ok = false;
  uint64_t address = 0, size = 0;
  uint32_t type = 0, protect = 0;
  int type_index = -1, protect_index = -1;
};

VirtualAllocArgs decode_virtual_alloc(const uint64_t* a, int n) {
  VirtualAllocArgs r;
  // (process, address, size, type, protect, ...) when the first argument is the
  // current-process pseudo handle, else (address, size, type, [flags,] protect).
  const bool process_first = a[0] == kCurrentProcessPseudo;
  for (int ti : {process_first ? 3 : 2, 1}) {
    if (ti >= n || !looks_like_alloc_type(a[ti])) continue;
    if (a[ti - 1] == 0 || a[ti - 1] > (1ull << 44)) continue;  // size
    // Protection: the next recognisable value, preferring one that grants access.
    int pi = -1;
    for (int k = ti + 1; k < n && k <= ti + 2; ++k)
      if (looks_like_protect(a[k]) && (pi < 0 || (a[pi] & 0xFF) == PAGE_NOACCESS)) pi = k;
    r.ok = true;
    r.type_index = ti;
    r.size = a[ti - 1];
    r.address = ti >= 2 ? a[ti - 2] : 0;
    r.type = static_cast<uint32_t>(a[ti]);
    r.protect_index = pi;
    r.protect = pi >= 0 ? static_cast<uint32_t>(a[pi]) : PAGE_READWRITE;
    return r;
  }
  return r;
}

// Host protection for console memory: accessible ranges become read/write
// (or RWX when executable), since GPU-only and read-only views of the console
// are all ordinary memory here.
uint32_t host_protect(uint32_t protect) {
  const uint32_t low = protect & 0xFF;
  if (low == PAGE_NOACCESS) return PAGE_NOACCESS;
  if (low & 0xF0) return PAGE_EXECUTE_READWRITE;
  return PAGE_READWRITE;
}

void* XBEM_GUEST_ABI hle_XMemVirtualAlloc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                         uint64_t a5) {
  const uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  const VirtualAllocArgs d = decode_virtual_alloc(args, 6);
  static std::atomic<int> logged{0};
  const bool log = logged.fetch_add(1) < 8;
  if (!d.ok) {
    XBEM_WARN("xmem", "XMemVirtualAlloc({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {}: unrecognised arguments -> NULL",
              a0, a1, a2, a3, a4, a5, describe_code_address(caller));
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  void* p = virtual_alloc(reinterpret_cast<void*>(d.address), d.size, d.type, host_protect(d.protect), 0);
  if (log || !p) {
    XBEM_WARN("xmem",
              "XMemVirtualAlloc({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {}: decoded address={:#x} size={:#x} "
              "type={:#x} (arg {}) protect={:#x} (arg {}) -> {}",
              a0, a1, a2, a3, a4, a5, describe_code_address(caller), d.address, d.size, d.type, d.type_index,
              d.protect, d.protect_index, p);
  }
  return p;
}

SIZE_T XBEM_GUEST_ABI hle_XMemVirtualQuery(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  XBEM_LOG_CALL("xmem", "decoded as VirtualQuery", a0, a1, a2, a3);
  // (address, MEMORY_BASIC_INFORMATION*, length) or (process, address, mbi, length).
  if (a2 >= 48 && a2 < 0x10000) return virtual_query(reinterpret_cast<void*>(a0), reinterpret_cast<uint8_t*>(a1), a2);
  if (a3 >= 48 && a3 < 0x10000) return virtual_query(reinterpret_cast<void*>(a1), reinterpret_cast<uint8_t*>(a2), a3);
  set_last_error(ERROR_INVALID_PARAMETER);
  return 0;
}

// --------------------------------------------------------- physical pages

constexpr uint64_t kPhysPageSize = 0x10000;       // 64 KiB physical pages
constexpr uint64_t kPhysMemorySize = 64ull << 30;  // sparse backing object
constexpr uint64_t kPfnBase = 0x100;              // page numbers handed out start here (never 0)

struct PhysicalMemory {
  std::mutex mutex;
  intptr_t handle = -1;
  bool init_done = false;
  uint64_t next_page = 0;
  std::vector<uint64_t> free_pages;
  uint64_t in_use = 0;

  bool init() {
    if (!init_done) {
      init_done = true;
      handle = platform::create_shared_memory(kPhysMemorySize);
      if (handle < 0) XBEM_ERROR("xmem", "cannot create the physical memory object - physical page APIs will fail");
    }
    return handle >= 0;
  }
};

PhysicalMemory g_phys;

bool readable_u64(uint64_t address, uint64_t& value) {
  return address >= 0x10000 && address < 0x800000000000ull && platform::safe_read(address, &value, 8);
}

bool plausible_count(uint64_t v) { return v >= 1 && v <= (1ull << 24); }

// Finds (pointer to page count, page array) among the arguments.
bool find_count_ptr_and_array(const uint64_t* a, int n, int& index, uint64_t& count) {
  for (int i = 0; i + 1 < n; ++i) {
    uint64_t v;
    if (!readable_u64(a[i], v)) continue;
    if (!plausible_count(v)) v &= 0xFFFFFFFF;  // ULONG count followed by unrelated data
    if (!plausible_count(v) || a[i + 1] < 0x10000) continue;
    index = i;
    count = v;
    return true;
  }
  return false;
}

// Finds (page count value, page array) among the arguments.
bool find_count_and_array(const uint64_t* a, int n, int& index) {
  for (int i = 0; i + 1 < n; ++i)
    if (plausible_count(a[i]) && a[i + 1] >= 0x10000 && a[i + 1] < 0x800000000000ull) {
      index = i;
      return true;
    }
  return false;
}

int32_t XBEM_GUEST_ABI hle_XMemAllocatePhysicalPages(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                     uint64_t a4) {
  const uint64_t args[5] = {a0, a1, a2, a3, a4};
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  int i = -1;
  uint64_t count = 0;
  if (!find_count_ptr_and_array(args, 5, i, count)) {
    XBEM_WARN("xmem", "XMemAllocatePhysicalPages({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {}: unrecognised arguments",
              a0, a1, a2, a3, a4, describe_code_address(caller));
    set_last_error(ERROR_INVALID_PARAMETER);
    return success_value_for_caller(caller) == 0 ? E_INVALIDARG : FALSE;
  }
  auto* pages = reinterpret_cast<uint64_t*>(args[i + 1]);
  {
    std::lock_guard lock(g_phys.mutex);
    if (!g_phys.init()) return success_value_for_caller(caller) == 0 ? E_OUTOFMEMORY : FALSE;
    for (uint64_t k = 0; k < count; ++k) {
      uint64_t page;
      if (!g_phys.free_pages.empty()) {
        page = g_phys.free_pages.back();
        g_phys.free_pages.pop_back();
      } else {
        page = g_phys.next_page++;
      }
      pages[k] = kPfnBase + page;
      ++g_phys.in_use;
    }
  }
  const int32_t ok = success_value_for_caller(caller);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8)
    XBEM_WARN("xmem",
              "XMemAllocatePhysicalPages({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {}: {} x 64K pages (count arg {}) -> {}",
              a0, a1, a2, a3, a4, describe_code_address(caller), count, i, ok == 0 ? "S_OK" : "TRUE");
  return ok;
}

int32_t XBEM_GUEST_ABI hle_XMemFreePhysicalPages(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  const uint64_t args[4] = {a0, a1, a2, a3};
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  int i = -1;
  uint64_t count = 0;
  bool found = find_count_and_array(args, 4, i);
  if (found) count = args[i];
  else found = find_count_ptr_and_array(args, 4, i, count);
  XBEM_LOG_CALL("xmem", found ? "freed" : "unrecognised arguments", a0, a1, a2, a3);
  if (found) {
    const auto* pages = reinterpret_cast<const uint64_t*>(args[i + 1]);
    std::lock_guard lock(g_phys.mutex);
    for (uint64_t k = 0; k < count; ++k) {
      if (pages[k] < kPfnBase || pages[k] - kPfnBase >= g_phys.next_page) continue;
      const uint64_t page = pages[k] - kPfnBase;
      platform::discard_shared(g_phys.handle, page * kPhysPageSize, kPhysPageSize);
      g_phys.free_pages.push_back(page);
      --g_phys.in_use;
    }
  }
  return success_value_for_caller(caller);
}

bool map_page(uint64_t va, uint64_t pfn) {
  if (pfn < kPfnBase || pfn - kPfnBase >= g_phys.next_page) return false;
  return platform::map_shared(va, kPhysPageSize, platform::Prot::RW, g_phys.handle, (pfn - kPfnBase) * kPhysPageSize);
}

// PVOID XMemMapPhysicalPages(PVOID VirtualAddress, SIZE_T NumberOfPages, <page array>, ...)
void* XBEM_GUEST_ABI hle_XMemMapPhysicalPages(uint64_t va, uint64_t count, uint64_t a2, uint64_t a3, uint64_t a4) {
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  // The page array is the first pointer-like argument after the count.
  const uint64_t* pages = nullptr;
  for (uint64_t cand : {a2, a3, a4})
    if (cand >= 0x10000 && cand < 0x800000000000ull) {
      pages = reinterpret_cast<const uint64_t*>(cand);
      break;
    }
  bool ok = va != 0 && plausible_count(count) && (va % kPhysPageSize) == 0;
  if (ok) {
    std::lock_guard lock(g_phys.mutex);
    for (uint64_t k = 0; k < count && ok; ++k) {
      const uint64_t page_va = va + k * kPhysPageSize;
      ok = pages ? map_page(page_va, pages[k]) : platform::remap_anonymous(page_va, kPhysPageSize, platform::Prot::None);
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8 || !ok)
    XBEM_WARN("xmem", "XMemMapPhysicalPages({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {} -> {}", va, count, a2, a3, a4,
              describe_code_address(caller), ok ? (pages ? "mapped" : "unmapped") : "FAILED");
  if (!ok) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  return reinterpret_cast<void*>(va);
}

// Maps one physical page at each of an array of virtual addresses.
int32_t XBEM_GUEST_ABI hle_XMemMapPhysicalPagesScatter(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  bool ok = false;
  // (PVOID* VirtualAddresses, count, page array) as in MapUserPhysicalPagesScatter.
  if (a0 >= 0x10000 && plausible_count(a1)) {
    const auto* vas = reinterpret_cast<const uint64_t*>(a0);
    const auto* pages = a2 >= 0x10000 ? reinterpret_cast<const uint64_t*>(a2) : nullptr;
    std::lock_guard lock(g_phys.mutex);
    ok = true;
    for (uint64_t k = 0; k < a1 && ok; ++k)
      ok = pages ? map_page(vas[k], pages[k]) : platform::remap_anonymous(vas[k], kPhysPageSize, platform::Prot::None);
  }
  XBEM_LOG_CALL("xmem", ok ? "mapped" : "FAILED", a0, a1, a2, a3);
  return ok ? success_value_for_caller(caller) : (success_value_for_caller(caller) == 0 ? E_INVALIDARG : FALSE);
}

int32_t XBEM_GUEST_ABI hle_XMemTransferMemory(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  XBEM_LOG_CALL("xmem", "not implemented (0)", a0, a1, a2, a3);
  return 0;
}

}  // namespace

ModuleDef make_xmem_module() {
  ModuleDef m;
  m.name = "xmem.dll";
  m.description = "Console memory manager: XMemAlloc, XMemVirtualAlloc, emulated physical pages";
  m.exports = {
      XBEM_HLE(XMemAlloc),
      XBEM_HLE(XMemFree),
      XBEM_HLE(XMemAllocDefault),
      XBEM_HLE(XMemFreeDefault),
      XBEM_HLE(XMemIsToolsMemAvailable),
      XBEM_HLE_STUB(XMemGetWorkingSetStatistics),
      XBEM_HLE(XMemVirtualAlloc),
      XBEM_HLE(XMemVirtualQuery),
      XBEM_HLE(XMemAllocatePhysicalPages),
      XBEM_HLE(XMemFreePhysicalPages),
      XBEM_HLE(XMemMapPhysicalPages),
      XBEM_HLE(XMemMapPhysicalPagesScatter),
      XBEM_HLE_STUB(XMemTransferMemory),
  };
  return m;
}

}  // namespace xbem::hle
