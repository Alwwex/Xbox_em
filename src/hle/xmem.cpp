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
#include <format>
#include <map>
#include <mutex>
#include <vector>

#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "platform/crash.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

// ------------------------------------------------------------- XMemAlloc

struct XMemBlock {
  uint64_t size = 0;
  uint64_t attributes = 0;
  uint64_t caller = 0;
};
std::mutex g_alloc_mutex;
std::map<uint64_t, XMemBlock> g_alloc_blocks;  // pointer -> block (validation, crash reports)

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

void* xmem_alloc(SIZE_T size, uint64_t attributes, uint64_t caller) {
  void* p = aligned_host_alloc(std::max<SIZE_T>(size, 1), alloc_alignment(size));
  if (!p) {
    XBEM_WARN("xmem", "XMemAlloc({:#x}, attributes {:#x}) failed: out of host memory", size, attributes);
    return nullptr;
  }
  {
    std::lock_guard lock(g_alloc_mutex);
    g_alloc_blocks[reinterpret_cast<uint64_t>(p)] = XMemBlock{size, attributes, caller};
  }
  // Large blocks are what titles carve their own heaps and pools from.
  static std::atomic<int> logged{0};
  if (size >= 0x100000 && logged.fetch_add(1) < 32)
    XBEM_INFO("xmem", "XMemAlloc({:#x}, attributes {:#x}) from {} -> {}", size, attributes,
              describe_code_address(caller), p);
  return p;
}

void xmem_free(void* p, uint64_t attributes) {
  if (!p) return;
  {
    std::lock_guard lock(g_alloc_mutex);
    if (g_alloc_blocks.erase(reinterpret_cast<uint64_t>(p)) == 0) {
      XBEM_WARN("xmem", "XMemFree({}, attributes {:#x}): not allocated by XMemAlloc - ignored", p, attributes);
      return;
    }
  }
  aligned_host_free(p);
}

void* XBEM_GUEST_ABI hle_XMemAlloc(SIZE_T size, uint64_t attributes) {
  XBEM_LOG_CALL("xmem", "allocated", size, attributes);
  return xmem_alloc(size, attributes, XBEM_RETURN_ADDRESS());
}
void XBEM_GUEST_ABI hle_XMemFree(void* p, uint64_t attributes) { xmem_free(p, attributes); }
void* XBEM_GUEST_ABI hle_XMemAllocDefault(SIZE_T size, uint64_t attributes) {
  return xmem_alloc(size, attributes, XBEM_RETURN_ADDRESS());
}
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
    // The protection is a DWORD: the upper half of its register can be garbage.
    for (int k = ti + 1; k < n && k <= ti + 2; ++k)
      if (looks_like_protect(a[k] & 0xFFFFFFFF) && (pi < 0 || (a[pi] & 0xFF) == PAGE_NOACCESS)) pi = k;
    r.ok = true;
    r.type_index = ti;
    r.size = a[ti - 1];
    r.address = ti >= 2 ? a[ti - 2] : 0;
    r.type = static_cast<uint32_t>(a[ti]);
    r.protect_index = pi;
    r.protect = pi >= 0 ? static_cast<uint32_t>(a[pi] & 0xFFFFFFFF) : PAGE_READWRITE;
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
  void* p = virtual_alloc(reinterpret_cast<void*>(d.address), d.size, d.type, host_protect(d.protect), 0, caller);
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

struct PageState {
  bool allocated = false;
  std::vector<uint64_t> vas;  // where the page is mapped now (64 KiB aligned)
};

// What the title does with its physical pages that may lose data: a page
// mapped where another one was (without an unmap), mapped at a second address,
// freed while still mapped, mapped while not allocated, free calls with pages
// that are not allocated (nothing is freed then).
struct PhysEvents {
  uint64_t remaps = 0, aliases = 0, freed_mapped = 0, unallocated_maps = 0, bad_frees = 0;
};

struct PhysicalMemory {
  std::mutex mutex;
  intptr_t handle = -1;
  bool init_done = false;
  uint64_t next_page = 0;
  std::vector<uint64_t> free_pages;
  uint64_t in_use = 0;
  std::vector<PageState> pages;                  // by page index (pfn - kPfnBase)
  std::unordered_map<uint64_t, uint64_t> va_pfn;  // mapped 64 KiB page -> pfn
  PhysEvents events;

  bool init() {
    if (!init_done) {
      init_done = true;
      handle = platform::create_shared_memory(kPhysMemorySize);
      if (handle < 0) XBEM_ERROR("xmem", "cannot create the physical memory object - physical page APIs will fail");
    }
    return handle >= 0;
  }
  bool valid(uint64_t pfn) const { return pfn >= kPfnBase && pfn - kPfnBase < next_page; }
  PageState& state(uint64_t pfn) { return pages[pfn - kPfnBase]; }
};

PhysicalMemory g_phys;

// The first 16 events of a kind are logged, then every power of two.
bool log_event(uint64_t n) { return n <= 16 || (n & (n - 1)) == 0; }

bool readable_u64(uint64_t address, uint64_t& value) {
  return address >= 0x10000 && address < 0x800000000000ull && platform::safe_read(address, &value, 8);
}

bool plausible_count(uint64_t v) { return v >= 1 && v <= (1ull << 24); }

bool plausible_pointer(uint64_t v) { return v >= 0x10000 && v < 0x800000000000ull; }

// Finds (pointer to page count, page array) among the arguments.
bool find_count_ptr_and_array(const uint64_t* a, int n, int& index, uint64_t& count, int from = 0) {
  for (int i = from; i + 1 < n; ++i) {
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

// For the log: how a page array (or address array) is laid out - its first
// entries, how many distinct values, the longest run of one value, and the
// step between neighbours when it is constant.
std::string summarize_array(const uint64_t* a, uint64_t count) {
  if (!a || !count) return "-";
  std::string out = "[";
  for (uint64_t k = 0; k < count && k < 6; ++k) out += std::format("{}{:#x}", k ? " " : "", a[k]);
  if (count > 6) out += " ...";
  out += "]";
  std::unordered_map<uint64_t, uint32_t> seen;
  uint64_t longest = 1, run = 1, min = a[0], max = a[0];
  bool constant_step = count > 1;
  const uint64_t step = count > 1 ? a[1] - a[0] : 0;
  for (uint64_t k = 0; k < count; ++k) {
    ++seen[a[k]];
    min = std::min(min, a[k]);
    max = std::max(max, a[k]);
    if (k) {
      run = a[k] == a[k - 1] ? run + 1 : 1;
      longest = std::max(longest, run);
      constant_step &= a[k] - a[k - 1] == step;
    }
  }
  out += std::format(" {} distinct, longest run {}, {:#x}..{:#x}", seen.size(), longest, min, max);
  if (constant_step) out += std::format(", step {:#x}", step);
  return out;
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
  uint64_t reused = 0, total = 0;
  {
    std::lock_guard lock(g_phys.mutex);
    if (!g_phys.init()) return success_value_for_caller(caller) == 0 ? E_OUTOFMEMORY : FALSE;
    for (uint64_t k = 0; k < count; ++k) {
      uint64_t page;
      if (!g_phys.free_pages.empty()) {
        page = g_phys.free_pages.back();
        g_phys.free_pages.pop_back();
        ++reused;
      } else {
        page = g_phys.next_page++;
        g_phys.pages.emplace_back();
      }
      g_phys.pages[page].allocated = true;
      pages[k] = kPfnBase + page;
      ++g_phys.in_use;
    }
    total = g_phys.in_use;
  }
  const int32_t ok = success_value_for_caller(caller);
  static std::atomic<uint64_t> calls{0};
  if (log_event(calls.fetch_add(1) + 1))
    XBEM_WARN("xmem",
              "XMemAllocatePhysicalPages({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {}: {} x 64K pages (count arg {}), "
              "pages {}, {} reused; {} in use -> {}",
              a0, a1, a2, a3, a4, describe_code_address(caller), count, i, summarize_array(pages, count), reused,
              total, ok == 0 ? "S_OK" : "TRUE");
  return ok;
}

// True when the first `count` entries of `pages` are all allocated pages.
bool all_allocated(const uint64_t* pages, uint64_t count) {
  for (uint64_t k = 0; k < count; ++k) {
    uint64_t pfn;
    if (!platform::safe_read(reinterpret_cast<uint64_t>(pages + k), &pfn, 8) || !g_phys.valid(pfn) ||
        !g_phys.state(pfn).allocated)
      return false;
  }
  return true;
}

// XMemFreePhysicalPages: like the allocation, (flags, pointer to the count,
// page array), or (count, page array). A layout is only taken when all the
// pages it names are allocated: freeing pages the title still uses would give
// them to another allocation, which then overwrites the first one's data.
int32_t XBEM_GUEST_ABI hle_XMemFreePhysicalPages(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  const uint64_t args[4] = {a0, a1, a2, a3};
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  const uint64_t* pages = nullptr;
  uint64_t count = 0;
  const char* layout = "unrecognised arguments";
  uint64_t freed = 0, freed_mapped = 0;
  {
    std::lock_guard lock(g_phys.mutex);
    for (int i = 0; i + 1 < 4 && !pages; ++i) {  // pointer to the count
      int at = -1;
      uint64_t n = 0;
      if (!find_count_ptr_and_array(args, 4, at, n, i) || at != i) continue;
      if (all_allocated(reinterpret_cast<const uint64_t*>(args[i + 1]), n)) {
        pages = reinterpret_cast<const uint64_t*>(args[i + 1]);
        count = n;
        layout = i == 0 ? "(&count, pages)" : "(flags, &count, pages)";
      }
    }
    for (int i = 0; i + 1 < 4 && !pages; ++i) {  // the count itself
      if (!plausible_count(args[i]) || !plausible_pointer(args[i + 1])) continue;
      if (all_allocated(reinterpret_cast<const uint64_t*>(args[i + 1]), args[i])) {
        pages = reinterpret_cast<const uint64_t*>(args[i + 1]);
        count = args[i];
        layout = "(count, pages)";
      }
    }
    if (!pages) {
      ++g_phys.events.bad_frees;
    } else {
      for (uint64_t k = 0; k < count; ++k) {
        const uint64_t pfn = pages[k];
        PageState& st = g_phys.state(pfn);
        if (!st.allocated) continue;  // listed twice
        if (!st.vas.empty()) ++freed_mapped;
        st.allocated = false;
        // The contents go (a freed page reads as zero when allocated again);
        // where it is still mapped, the title sees that too.
        platform::discard_shared(g_phys.handle, (pfn - kPfnBase) * kPhysPageSize, kPhysPageSize);
        g_phys.free_pages.push_back(pfn - kPfnBase);
        --g_phys.in_use;
        ++freed;
      }
      g_phys.events.freed_mapped += freed_mapped;
    }
  }
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1) + 1;
  if (log_event(n) || !pages || freed_mapped)
    XBEM_WARN("xmem", "XMemFreePhysicalPages({:#x}, {:#x}, {:#x}, {:#x}) from {}: {}{}", a0, a1, a2, a3,
              describe_code_address(caller), pages ? std::format("{} {} pages freed", layout, freed)
                                                   : std::string("no layout names allocated pages - nothing freed"),
              freed_mapped ? std::format(", {} of them still mapped", freed_mapped) : std::string());
  return success_value_for_caller(caller);
}

void forget_mapping(uint64_t va, uint64_t pfn) {
  if (!g_phys.valid(pfn)) return;
  auto& vas = g_phys.state(pfn).vas;
  vas.erase(std::remove(vas.begin(), vas.end(), va), vas.end());
}

// Maps physical page `pfn` at `va` (64 KiB aligned) and records it. Called
// with the mutex held.
bool map_page(uint64_t va, uint64_t pfn, uint64_t caller) {
  if (!g_phys.valid(pfn)) return false;
  PageState& st = g_phys.state(pfn);
  if (!st.allocated && log_event(++g_phys.events.unallocated_maps))
    XBEM_WARN("xmem", "maps page {:#x} at {:#x}, but it is not allocated (freed?) - from {} (#{})", pfn, va,
              describe_code_address(caller), g_phys.events.unallocated_maps);
  auto it = g_phys.va_pfn.find(va);
  if (it != g_phys.va_pfn.end() && it->second != pfn) {
    if (log_event(++g_phys.events.remaps))
      XBEM_WARN("xmem", "maps page {:#x} at {:#x}, where page {:#x} was mapped (no unmap) - from {} (#{})", pfn, va,
                it->second, describe_code_address(caller), g_phys.events.remaps);
    forget_mapping(va, it->second);
  }
  const bool here = std::find(st.vas.begin(), st.vas.end(), va) != st.vas.end();
  if (!here && !st.vas.empty() && log_event(++g_phys.events.aliases))
    XBEM_WARN("xmem", "maps page {:#x} at {:#x} while it is also mapped at {:#x} ({} other views) - from {} (#{})", pfn,
              va, st.vas.front(), st.vas.size(), describe_code_address(caller), g_phys.events.aliases);
  if (!platform::map_shared(va, kPhysPageSize, platform::Prot::RW, g_phys.handle, (pfn - kPfnBase) * kPhysPageSize))
    return false;
  g_phys.va_pfn[va] = pfn;
  if (!here) st.vas.push_back(va);
  return true;
}

bool unmap_page(uint64_t va) {
  if (auto it = g_phys.va_pfn.find(va); it != g_phys.va_pfn.end()) {
    forget_mapping(va, it->second);
    g_phys.va_pfn.erase(it);
  }
  return platform::remap_anonymous(va, kPhysPageSize, platform::Prot::None);
}

// PVOID XMemMapPhysicalPages(PVOID VirtualAddress, SIZE_T NumberOfPages, <page array>, ...)
void* XBEM_GUEST_ABI hle_XMemMapPhysicalPages(uint64_t va, uint64_t count, uint64_t a2, uint64_t a3, uint64_t a4) {
  const uint64_t caller = XBEM_RETURN_ADDRESS();
  // The page array is the third argument (the title passes it there); NULL
  // unmaps. Later registers hold unrelated values.
  const uint64_t* pages = plausible_pointer(a2) ? reinterpret_cast<const uint64_t*>(a2) : nullptr;
  bool ok = va != 0 && plausible_count(count) && (va % kPhysPageSize) == 0;
  if (ok) {
    std::lock_guard lock(g_phys.mutex);
    for (uint64_t k = 0; k < count && ok; ++k) {
      const uint64_t page_va = va + k * kPhysPageSize;
      ok = pages ? map_page(page_va, pages[k], caller) : unmap_page(page_va);
    }
  }
  platform::refresh_watchpoint_views();  // a new mapping replaced the protection of watched pages
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1) + 1;
  if (log_event(n) || !ok)
    XBEM_WARN("xmem", "XMemMapPhysicalPages({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) from {} -> {} (call {}){}", va, count, a2,
              a3, a4, describe_code_address(caller), ok ? (pages ? "mapped" : "unmapped") : "FAILED", n,
              pages && plausible_count(count) ? "; pages " + summarize_array(pages, count) : std::string());
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
      ok = (vas[k] % kPhysPageSize) == 0 && (pages ? map_page(vas[k], pages[k], caller) : unmap_page(vas[k]));
  }
  platform::refresh_watchpoint_views();
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1) + 1;
  const bool arrays = a0 >= 0x10000 && plausible_count(a1);
  if (log_event(n) || !ok)
    XBEM_WARN("xmem", "XMemMapPhysicalPagesScatter({:#x}, {:#x}, {:#x}, {:#x}) from {} -> {} (call {}){}{}", a0, a1, a2,
              a3, describe_code_address(caller), ok ? "mapped" : "FAILED", n,
              arrays ? "; addresses " + summarize_array(reinterpret_cast<const uint64_t*>(a0), a1) : std::string(),
              arrays && a2 >= 0x10000 ? "; pages " + summarize_array(reinterpret_cast<const uint64_t*>(a2), a1)
                                      : std::string());
  return ok ? success_value_for_caller(caller) : (success_value_for_caller(caller) == 0 ? E_INVALIDARG : FALSE);
}

int32_t XBEM_GUEST_ABI hle_XMemTransferMemory(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  XBEM_LOG_CALL("xmem", "not implemented (0)", a0, a1, a2, a3);
  return 0;
}

}  // namespace

int physical_views(uint64_t address, uint64_t* out, int max, bool may_block) {
  std::unique_lock lock(g_phys.mutex, std::defer_lock);
  if (may_block) lock.lock();
  else if (!lock.try_lock()) return -1;
  const uint64_t va = address & ~(kPhysPageSize - 1);
  auto it = g_phys.va_pfn.find(va);
  if (it == g_phys.va_pfn.end() || !g_phys.valid(it->second)) return 0;
  int n = 0;
  for (uint64_t v : g_phys.state(it->second).vas)
    if (n < max) out[n++] = v + (address - va);
  return n;
}

bool describe_physical_page(uint64_t address, char* out, size_t out_size) {
  std::unique_lock lock(g_phys.mutex, std::try_to_lock);  // may run in a signal handler: never block
  if (!lock.owns_lock()) return false;
  const uint64_t va = address & ~(kPhysPageSize - 1);
  auto it = g_phys.va_pfn.find(va);
  if (it == g_phys.va_pfn.end() || !g_phys.valid(it->second)) return false;
  const PageState& st = g_phys.state(it->second);
  size_t used = static_cast<size_t>(std::snprintf(out, out_size, "physical page %#llx%s, mapped at %zu address(es)",
                                                  static_cast<unsigned long long>(it->second),
                                                  st.allocated ? "" : " (freed)", st.vas.size()));
  for (size_t k = 0; k < st.vas.size() && k < 4 && used + 24 < out_size; ++k)
    used += static_cast<size_t>(std::snprintf(out + used, out_size - used, "%s%#llx", k ? ", " : ": ",
                                              static_cast<unsigned long long>(st.vas[k] + (address - va))));
  return true;
}

bool describe_xmem_allocation(uint64_t address, char* out, size_t out_size) {
  std::unique_lock lock(g_alloc_mutex, std::try_to_lock);  // may run in a signal handler: never block
  if (!lock.owns_lock()) return false;
  auto it = g_alloc_blocks.upper_bound(address);
  if (it == g_alloc_blocks.begin()) return false;
  --it;
  if (address >= it->first + std::max<uint64_t>(it->second.size, 1)) return false;
  std::snprintf(out, out_size, "XMemAlloc block %#llx+%#llx (attributes %#llx, by %#llx)",
                static_cast<unsigned long long>(it->first), static_cast<unsigned long long>(it->second.size),
                static_cast<unsigned long long>(it->second.attributes),
                static_cast<unsigned long long>(it->second.caller));
  return true;
}

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
