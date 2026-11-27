// Console-only hardware APIs that have no public documentation:
//
//   acphal.dll  - audio co-processor HAL (APU memory heaps, shape contexts)
//   chad.dll    - audio DSP command interface (XDsp*)
//   xapu.dll    - audio processing unit command queues (XApu*)
//
// acphal and xapu are emulated far enough for a title's audio start-up to
// succeed (nothing is played):
//
//   * ApuCreateHeap(cachedBytes, nonCachedBytes) reserves the APU heap once;
//     ApuAlloc(&va, &apuAddress, size, alignment, flags) carves blocks out of
//     it (flags 2 prefers the non-cached part; either part is used when the
//     other is full), ApuFree returns them. APU addresses are 32-bit offsets
//     into the heap plus kApuAddressBase; ApuMapVirtualAddress,
//     ApuMapApuAddress and ApuIsVirtualAddressValid(va, alignment) translate
//     and check them.
//   * AcpHalAllocateShapeContexts(SHAPE_CONTEXTS*): six u32 counts (SRC,
//     EQ/compressor, filter/volume, DMA, XMA, PCM) followed by six array
//     pointers at +0x18, which are filled with zeroed context arrays.
//   * AcpHalCreate(&hal) hands out a probe object: the title calls slot 0
//     (connect with two queue sizes), 1 (disconnect), 2 (submit a command)
//     and 5 (release); all succeed.
//   * XApuConnect(params, &shared, &handle): params+0xc is the size of the
//     command/result memory shared with the APU, written to shared+0 (and the
//     size to shared+8); the queue never produces results.
//
// chad's entry points log their first calls with all argument values and the
// call site and fail the way the caller checks for (E_NOTIMPL for HRESULT
// callers, otherwise 0/NULL).

#include <array>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>

#include "common/log.h"
#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

// ---- APU heap ------------------------------------------------------------

constexpr uint32_t kApuAddressBase = 0x10000000;  // APU address of the heap's first byte
constexpr uint32_t kApuInvalidAddress = 0xffffffffu;
constexpr uint32_t kApuAllocNonCached = 0x2;
constexpr HRESULT kAlreadyInitialized = static_cast<HRESULT>(0x800704DF);  // ERROR_ALREADY_INITIALIZED

struct ApuArena {
  uint64_t begin = 0, end = 0;     // offsets into the heap
  std::map<uint64_t, uint64_t> free;  // offset -> size
};

struct ApuHeap {
  std::mutex lock;
  uint8_t* base = nullptr;
  uint64_t size = 0;
  ApuArena arenas[2];                                       // 0 cached, 1 non-cached
  std::map<uint64_t, std::pair<uint64_t, int>> allocated;  // offset -> (size, arena)
};

ApuHeap& apu_heap() {
  static ApuHeap heap;
  return heap;
}

// First fit with alignment; the remainder before and after stays free.
bool arena_alloc(ApuArena& arena, uint64_t size, uint64_t alignment, uint64_t& out) {
  for (auto it = arena.free.begin(); it != arena.free.end(); ++it) {
    const uint64_t start = (it->first + alignment - 1) & ~(alignment - 1);
    const uint64_t block_end = it->first + it->second;
    if (start + size > block_end) continue;
    const uint64_t head = start - it->first;
    const uint64_t tail = block_end - (start + size);
    const uint64_t block = it->first;
    arena.free.erase(it);
    if (head) arena.free[block] = head;
    if (tail) arena.free[start + size] = tail;
    out = start;
    return true;
  }
  return false;
}

void arena_release(ApuArena& arena, uint64_t offset, uint64_t size) {
  auto next = arena.free.lower_bound(offset);
  if (next != arena.free.end() && offset + size == next->first) {
    size += next->second;
    next = arena.free.erase(next);
  }
  if (next != arena.free.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      prev->second += size;
      return;
    }
  }
  arena.free[offset] = size;
}

// The allocated block holding `va`, or allocated.end().
std::map<uint64_t, std::pair<uint64_t, int>>::iterator find_block(ApuHeap& heap, uint64_t va) {
  const uint64_t base = reinterpret_cast<uint64_t>(heap.base);
  if (!heap.base || va < base || va >= base + heap.size) return heap.allocated.end();
  const uint64_t offset = va - base;
  auto it = heap.allocated.upper_bound(offset);
  if (it == heap.allocated.begin()) return heap.allocated.end();
  --it;
  return offset < it->first + it->second.first ? it : heap.allocated.end();
}

HRESULT XBEM_GUEST_ABI hle_ApuCreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes) {
  XBEM_LOG_CALL("acphal", "emulated", cached_bytes, non_cached_bytes);
  ApuHeap& heap = apu_heap();
  std::lock_guard guard(heap.lock);
  if (heap.base) return kAlreadyInitialized;
  const uint64_t cached = (uint64_t{cached_bytes} + 0xffff) & ~uint64_t{0xffff};
  const uint64_t non_cached = (uint64_t{non_cached_bytes} + 0xffff) & ~uint64_t{0xffff};
  const uint64_t total = cached + non_cached;
  if (total == 0 || total > kApuInvalidAddress - kApuAddressBase) return E_INVALIDARG;
  void* base = virtual_alloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0, XBEM_RETURN_ADDRESS());
  if (!base) return E_OUTOFMEMORY;
  heap.base = static_cast<uint8_t*>(base);
  heap.size = total;
  heap.arenas[0] = {0, cached, {}};
  heap.arenas[1] = {cached, total, {}};
  for (ApuArena& arena : heap.arenas)
    if (arena.end > arena.begin) arena.free[arena.begin] = arena.end - arena.begin;
  XBEM_INFO("acphal", "ApuCreateHeap: {:#x} cached + {:#x} non-cached bytes at {} (APU address {:#x})", cached,
            non_cached, base, kApuAddressBase);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_ApuAlloc(void** va, uint32_t* apu_address, uint32_t size, uint32_t alignment,
                                    uint32_t flags) {
  XBEM_LOG_CALL("acphal", "emulated", reinterpret_cast<uint64_t>(va), reinterpret_cast<uint64_t>(apu_address), size,
                alignment, flags);
  if (!va || size == 0) return E_INVALIDARG;
  *va = nullptr;
  if (apu_address) *apu_address = kApuInvalidAddress;
  uint64_t align = alignment ? alignment : 16;
  if (align & (align - 1)) return E_INVALIDARG;
  if (align < 16) align = 16;
  ApuHeap& heap = apu_heap();
  std::lock_guard guard(heap.lock);
  if (!heap.base) return E_FAIL;
  const uint64_t rounded = (uint64_t{size} + 15) & ~uint64_t{15};
  const int preferred = (flags & kApuAllocNonCached) ? 1 : 0;
  for (int arena : {preferred, 1 - preferred}) {
    uint64_t offset = 0;
    if (!arena_alloc(heap.arenas[arena], rounded, align, offset)) continue;
    heap.allocated[offset] = {rounded, arena};
    std::memset(heap.base + offset, 0, rounded);
    *va = heap.base + offset;
    if (apu_address) *apu_address = kApuAddressBase + static_cast<uint32_t>(offset);
    return S_OK;
  }
  XBEM_WARN("acphal", "ApuAlloc({:#x} bytes, alignment {:#x}, flags {:#x}): APU heap full", size, alignment, flags);
  return E_OUTOFMEMORY;
}

HRESULT XBEM_GUEST_ABI hle_ApuFree(void* va) {
  XBEM_LOG_CALL("acphal", "emulated", reinterpret_cast<uint64_t>(va));
  ApuHeap& heap = apu_heap();
  std::lock_guard guard(heap.lock);
  auto it = find_block(heap, reinterpret_cast<uint64_t>(va));
  if (it == heap.allocated.end() || heap.base + it->first != va) return E_INVALIDARG;
  arena_release(heap.arenas[it->second.second], it->first, it->second.first);
  heap.allocated.erase(it);
  return S_OK;
}

uint32_t XBEM_GUEST_ABI hle_ApuMapVirtualAddress(void* va) {
  ApuHeap& heap = apu_heap();
  std::lock_guard guard(heap.lock);
  if (find_block(heap, reinterpret_cast<uint64_t>(va)) == heap.allocated.end()) return kApuInvalidAddress;
  return kApuAddressBase + static_cast<uint32_t>(static_cast<uint8_t*>(va) - heap.base);
}

void* XBEM_GUEST_ABI hle_ApuMapApuAddress(uint32_t apu_address) {
  ApuHeap& heap = apu_heap();
  std::lock_guard guard(heap.lock);
  if (apu_address < kApuAddressBase) return nullptr;
  void* va = heap.base + (apu_address - kApuAddressBase);
  return find_block(heap, reinterpret_cast<uint64_t>(va)) == heap.allocated.end() ? nullptr : va;
}

BOOL XBEM_GUEST_ABI hle_ApuIsVirtualAddressValid(void* va, uint32_t alignment) {
  ApuHeap& heap = apu_heap();
  std::lock_guard guard(heap.lock);
  if (find_block(heap, reinterpret_cast<uint64_t>(va)) == heap.allocated.end()) return FALSE;
  const uint32_t apu = kApuAddressBase + static_cast<uint32_t>(static_cast<uint8_t*>(va) - heap.base);
  return alignment == 0 || apu % alignment == 0;
}

// ---- shape contexts and the HAL object -------------------------------------

constexpr int kShapeKinds = 6;           // SRC, EQ/compressor, filter/volume, DMA, XMA, PCM
constexpr uint64_t kShapeContextBytes = 0x100;  // generous: the title's XMA contexts are 64 bytes

struct ShapeContexts {
  uint32_t count[kShapeKinds];
  void* array[kShapeKinds];
};
static_assert(offsetof(ShapeContexts, array) == 0x18);

std::mutex g_shape_lock;
void* g_shape_arrays[kShapeKinds] = {};

HRESULT XBEM_GUEST_ABI hle_AcpHalAllocateShapeContexts(ShapeContexts* contexts) {
  XBEM_LOG_CALL("acphal", "emulated", reinterpret_cast<uint64_t>(contexts));
  if (!contexts) return E_POINTER;
  std::lock_guard guard(g_shape_lock);
  for (int i = 0; i < kShapeKinds; ++i)
    if (g_shape_arrays[i]) return kAlreadyInitialized;
  for (int i = 0; i < kShapeKinds; ++i) {
    contexts->array[i] = nullptr;
    if (!contexts->count[i]) continue;
    void* p = virtual_alloc(nullptr, contexts->count[i] * kShapeContextBytes, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE, 0, XBEM_RETURN_ADDRESS());
    if (!p) return E_OUTOFMEMORY;
    g_shape_arrays[i] = contexts->array[i] = p;
  }
  XBEM_INFO("acphal", "AcpHalAllocateShapeContexts: SRC {} EQ/comp {} filter/volume {} DMA {} XMA {} PCM {}",
            contexts->count[0], contexts->count[1], contexts->count[2], contexts->count[3], contexts->count[4],
            contexts->count[5]);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_AcpHalReleaseShapeContexts() {
  XBEM_LOG_CALL("acphal", "emulated");
  std::lock_guard guard(g_shape_lock);
  for (void*& p : g_shape_arrays) {
    if (p) virtual_free(p, 0, MEM_RELEASE);
    p = nullptr;
  }
  return S_OK;
}

// The HAL object is not COM: slot 0 connects with two queue sizes, slot 1
// disconnects, slot 2 submits a command (id, mask, timeout, &argument, 0).
HRESULT XBEM_GUEST_ABI hal_connect(ProbeObject* self, uint64_t command_bytes, uint64_t message_bytes) {
  probe_log(self, 0, std::format("Connect({:#x}, {:#x})", command_bytes, message_bytes), XBEM_RETURN_ADDRESS());
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hal_disconnect(ProbeObject* self) {
  probe_log(self, 1, "Disconnect()", XBEM_RETURN_ADDRESS());
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hal_submit(ProbeObject* self, uint64_t command, uint64_t mask, uint64_t timeout,
                                  const uint32_t* argument) {
  probe_log(self, 2,
            std::format("SubmitCommand({:#x}, {:#x}, {:#x}, {}) - ignored", command, mask, timeout,
                        argument ? std::format("[{:#x}]", *argument) : std::string("null")),
            XBEM_RETURN_ADDRESS());
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_AcpHalCreate(void** hal) {
  XBEM_LOG_CALL("acphal", "emulated", reinterpret_cast<uint64_t>(hal));
  if (!hal) return E_POINTER;
  static const ProbeVtable vtable = make_probe_vtable({{0, reinterpret_cast<void*>(&hal_connect)},
                                                       {1, reinterpret_cast<void*>(&hal_disconnect)},
                                                       {2, reinterpret_cast<void*>(&hal_submit)}});
  *hal = make_probe_object("acphal.hal", &vtable);
  return S_OK;
}

// ---- XApu queues --------------------------------------------------------------

struct XApuQueue {
  void* shared = nullptr;
  uint32_t shared_bytes = 0;
  // Results waiting to be dequeued (0x40 bytes each: +0 type, +4 id).
  std::mutex mutex;
  std::deque<std::array<uint8_t, 0x40>> results;
};

HRESULT XBEM_GUEST_ABI hle_XApuConnect(const uint8_t* params, uint64_t* shared, void** handle) {
  XBEM_LOG_CALL("xapu", "emulated", reinterpret_cast<uint64_t>(params), reinterpret_cast<uint64_t>(shared),
                reinterpret_cast<uint64_t>(handle));
  if (!params || !shared || !handle) return E_POINTER;
  uint32_t words[4];
  std::memcpy(words, params, sizeof words);
  const uint32_t bytes = words[3];
  auto* queue = new XApuQueue;
  queue->shared_bytes = bytes;
  if (bytes) {
    queue->shared = virtual_alloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0, XBEM_RETURN_ADDRESS());
    if (!queue->shared) {
      delete queue;
      return E_OUTOFMEMORY;
    }
  }
  shared[0] = reinterpret_cast<uint64_t>(queue->shared);
  shared[1] = bytes;
  *handle = queue;
  XBEM_INFO("xapu", "XApuConnect(params {:#x} {:#x} {:#x} {:#x}): {:#x} shared bytes at {}", words[0], words[1],
            words[2], words[3], bytes, queue->shared);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XApuDisconnect(void* handle) {
  XBEM_LOG_CALL("xapu", "emulated", reinterpret_cast<uint64_t>(handle));
  return S_OK;
}

// Commands are (type, id, ...) records. GTA VI (fn 0x144eaa840) moves its
// per-id state on results of the same shape: type 1 (start) acknowledged
// takes an entry from state 1 to 4, type 2 (stop) frees it. Nothing is
// played, so every command of type 1 or 2 is acknowledged at once with a
// result echoing its type and id; without them the title's entries stayed
// waiting forever.
HRESULT XBEM_GUEST_ABI hle_XApuEnqueueCommand(void* handle, const void* command, uint64_t flags) {
  XBEM_LOG_CALL("xapu", "S_OK (acknowledged)", reinterpret_cast<uint64_t>(handle),
                reinterpret_cast<uint64_t>(command), flags);
  if (!handle || !command) return E_INVALIDARG;
  auto* queue = static_cast<XApuQueue*>(handle);
  uint32_t head[2] = {};
  if (!platform::safe_read(reinterpret_cast<uint64_t>(command), head, sizeof head)) return E_INVALIDARG;
  if (head[0] == 1 || head[0] == 2) {
    std::array<uint8_t, 0x40> r{};
    std::memcpy(r.data(), head, sizeof head);
    std::lock_guard lock(queue->mutex);
    queue->results.push_back(r);
  }
  return S_OK;
}

uint32_t XBEM_GUEST_ABI hle_XApuGetResultCount(void* handle, uint64_t kind) {
  if (!handle) return 0;
  auto* queue = static_cast<XApuQueue*>(handle);
  std::lock_guard lock(queue->mutex);
  return static_cast<uint32_t>(queue->results.size());
}

HRESULT XBEM_GUEST_ABI hle_XApuDequeueResult(void* handle, void* result) {
  if (!handle || !result) return E_INVALIDARG;
  auto* queue = static_cast<XApuQueue*>(handle);
  std::array<uint8_t, 0x40> r;
  {
    std::lock_guard lock(queue->mutex);
    if (queue->results.empty()) return E_FAIL;
    r = queue->results.front();
    queue->results.pop_front();
  }
  if (!platform::safe_write(reinterpret_cast<uint64_t>(result), r.data(), r.size())) return E_POINTER;
  return S_OK;
}

// The audio DSP is not emulated. XDspActivate fails explicitly: GTA VI's
// wrapper (+0x11d4750) turns a non-zero result into -1 and drops the stream;
// "success" left its two outputs (stream state pointers) zero and the title
// dereferenced them in the story scene (+0x4e5ad4d).
HRESULT XBEM_GUEST_ABI hle_XDspActivate(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  XBEM_LOG_CALL("chad", "E_NOTIMPL (no audio DSP)", a, b, c, d);
  return E_NOTIMPL;
}
XBEM_DISCOVERY_FAIL("chad", XDspConnectWithMaximumStreamLimit)
XBEM_DISCOVERY_FAIL("chad", XDspDeactivate)
XBEM_DISCOVERY_FAIL("chad", XDspDisconnect)
XBEM_DISCOVERY_FAIL("chad", XDspSubmitCommand)

}  // namespace

std::vector<ModuleDef> make_console_modules() {
  std::vector<ModuleDef> out;
  auto add = [&](const char* name, const char* description, std::vector<ExportDef> exports) {
    ModuleDef m;
    m.name = name;
    m.description = description;
    m.exports = std::move(exports);
    out.push_back(std::move(m));
  };
  add("acphal.dll", "Audio co-processor HAL (APU heap and shape contexts; nothing is played)",
      {XBEM_HLE(AcpHalCreate), XBEM_HLE(AcpHalAllocateShapeContexts), XBEM_HLE(AcpHalReleaseShapeContexts),
       XBEM_HLE(ApuAlloc), XBEM_HLE(ApuCreateHeap), XBEM_HLE(ApuFree), XBEM_HLE(ApuIsVirtualAddressValid),
       XBEM_HLE(ApuMapApuAddress), XBEM_HLE(ApuMapVirtualAddress)});
  add("chad.dll", "Audio DSP interface (not emulated: calls are logged and fail)",
      {XBEM_HLE(XDspActivate), XBEM_HLE_STUB(XDspConnectWithMaximumStreamLimit), XBEM_HLE_STUB(XDspDeactivate),
       XBEM_HLE_STUB(XDspDisconnect), XBEM_HLE_STUB(XDspSubmitCommand)});
  add("xapu.dll", "Audio processing unit queues (commands acknowledged with results)",
      {XBEM_HLE(XApuConnect), XBEM_HLE(XApuDequeueResult), XBEM_HLE(XApuDisconnect), XBEM_HLE(XApuEnqueueCommand),
       XBEM_HLE(XApuGetResultCount)});
  return out;
}

}  // namespace xbem::hle
