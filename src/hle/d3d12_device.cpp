// Headless Direct3D 12 (D3D12.X) device.
//
// Without a GPU backend, titles still need a device to get through their
// graphics initialisation and frame loop. This device "executes" GPU work
// instantly and draws nothing:
//
//   * command queues complete everything immediately: Signal() sets the fence
//     value at once, so CPU waits on GPU fences return;
//   * resources get CPU memory for Map() and unique GPU virtual addresses;
//   * descriptor heaps hand out CPU/GPU descriptor handles;
//   * pipeline creation captures the title's shader binaries to disk
//     (--dump-shaders DIR) for the shader translator;
//   * everything else is a probe slot (see probe_com.h): logged and S_OK.
//
// Slot numbers follow the public d3d12.h interfaces. The console's D3D12.X
// adds methods, so typed slots log their calls; a mismatch shows up in the run
// log as calls with implausible arguments.

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <thread>
#include <chrono>

#include "gpu/root_signature.h"
#include "hle/d3d12_device.h"
#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/texture_layout.h"
#include "platform/memory.h"
#include "platform/window.h"

namespace xbem::hle {
namespace {

using namespace win;
namespace fs = std::filesystem;

// --------------------------------------------------------------- helpers

std::atomic<uint64_t> g_next_gpu_va{0x0000'1000'0000'0000ull};  // fake GPU virtual addresses

uint64_t allocate_gpu_va(uint64_t size) {
  const uint64_t aligned = (std::max<uint64_t>(size, 1) + 0xFFFF) & ~0xFFFFull;
  return g_next_gpu_va.fetch_add(aligned + 0x10000);
}

template <class F>
void* fn(F* f) {
  return reinterpret_cast<void*>(f);
}

// Methods returning a structure larger than 8 bytes. The console's D3D12.X
// passes the result buffer in RCX and `this` in RDX (a real title calls
// GetResourceAllocationInfo that way), the PC headers' MSVC form has them the
// other way round; whichever argument is the probe object is `this`.
bool is_probe_object(uint64_t p) {
  uint64_t magic = 0;
  return p >= 0x10000 && (p & 7) == 0 && platform::safe_read(p + 8, &magic, 8) && magic == kProbeMagic;
}
struct StructReturn {
  ProbeObject* self = nullptr;
  uint8_t* out = nullptr;
};
// The result buffer is scratch memory when the caller passed none, so a bad
// call is logged by the caller's crash instead of ours.
uint8_t g_struct_scratch[256];
StructReturn struct_return(uint64_t a0, uint64_t a1) {
  StructReturn r;
  if (is_probe_object(a0) && !is_probe_object(a1)) r = {reinterpret_cast<ProbeObject*>(a0), reinterpret_cast<uint8_t*>(a1)};
  else r = {reinterpret_cast<ProbeObject*>(a1), reinterpret_cast<uint8_t*>(a0)};
  if (reinterpret_cast<uint64_t>(r.out) < 0x10000) {
    XBEM_WARN_ONCE("d3d12", "structure-returning method called without a result buffer ({:#x}, {:#x})", a0, a1);
    r.out = g_struct_scratch;
  }
  return r;
}

ProbeVtable make_vtable(std::initializer_list<std::pair<int, void*>> overrides) { return make_probe_vtable(overrides); }

// SetEvent through the kernelx implementation (events are kernel objects there).
void set_guest_event(HANDLE event) {
  using SetEventFn = BOOL(XBEM_GUEST_ABI*)(HANDLE);
  static auto set_event = reinterpret_cast<SetEventFn>(Registry::instance().find("kernelx.dll", "SetEvent").exp->address);
  if (event) set_event(event);
}

// ------------------------------------------------------------ shaders

struct ShaderCapture {
  std::mutex mutex;
  std::unordered_map<uint64_t, bool> seen;
  size_t count = 0;
};
ShaderCapture g_shaders;

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// Saves a D3D12_SHADER_BYTECODE {pointer, size} if it looks valid.
void capture_shader(const char* stage, uint64_t pointer, uint64_t size) {
  if (!pointer || size < 16 || size > (64u << 20)) return;
  std::vector<uint8_t> bytes(size);
  if (!platform::safe_read(pointer, bytes.data(), size)) return;
  const uint64_t hash = fnv1a(bytes.data(), bytes.size());
  std::lock_guard lock(g_shaders.mutex);
  if (!g_shaders.seen.emplace(hash, true).second) return;
  ++g_shaders.count;
  const char magic[5] = {static_cast<char>(bytes[0]), static_cast<char>(bytes[1]), static_cast<char>(bytes[2]),
                         static_cast<char>(bytes[3]), 0};
  const fs::path& dir = proc().options().dump_shaders_dir;
  if (dir.empty()) {
    if (g_shaders.count <= 3)
      XBEM_INFO("d3d12", "shader {} ({} bytes, magic '{}') - use --dump-shaders DIR to save shaders", stage, size,
                std::isprint(static_cast<unsigned char>(magic[0])) ? magic : "?");
    return;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path file = dir / std::format("{}_{:016x}.bin", stage, hash);
  std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
  if (g_shaders.count <= 20 || g_shaders.count % 100 == 0)
    XBEM_INFO("d3d12", "captured shader #{}: {} ({} bytes)", g_shaders.count, file.string(), size);
}

// ------------------------------------------------------------- fence

struct FenceState {
  std::mutex mutex;
  uint64_t value = 0;
  std::vector<std::pair<uint64_t, HANDLE>> waiters;
};

void fence_signal(FenceState* f, uint64_t value) {
  std::vector<HANDLE> fire;
  {
    std::lock_guard lock(f->mutex);
    f->value = value;
    for (auto it = f->waiters.begin(); it != f->waiters.end();) {
      if (it->first <= value) {
        fire.push_back(it->second);
        it = f->waiters.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (HANDLE h : fire) set_guest_event(h);
}

FenceState* fence_of(void* object) { return static_cast<FenceState*>(static_cast<ProbeObject*>(object)->state); }

// ID3D12Fence: 8 GetCompletedValue, 9 SetEventOnCompletion, 10 Signal
uint64_t XBEM_GUEST_ABI fence_get_completed_value(ProbeObject* self) {
  auto* f = static_cast<FenceState*>(self->state);
  std::lock_guard lock(f->mutex);
  return f->value;
}
HRESULT XBEM_GUEST_ABI fence_set_event_on_completion(ProbeObject* self, uint64_t value, HANDLE event) {
  auto* f = static_cast<FenceState*>(self->state);
  {
    std::lock_guard lock(f->mutex);
    if (f->value < value) {
      if (event) f->waiters.emplace_back(value, event);
      return S_OK;  // NULL event + not reached would block: values are always reached instantly here
    }
  }
  set_guest_event(event);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI fence_signal_cpu(ProbeObject* self, uint64_t value) {
  fence_signal(static_cast<FenceState*>(self->state), value);
  return S_OK;
}
// 12 (console): the address of the fence's 64-bit value. Console fences are
// memory the GPU writes; a real title calls this and then reads the value
// itself, comparing it with UINT64_MAX (device removed) like the PC
// GetCompletedValue. The arguments (2, a structure on the caller's stack, a
// flag) are logged and left untouched.
const uint64_t* XBEM_GUEST_ABI fence_get_value_address(ProbeObject* self, uint64_t a0, uint64_t a1, uint64_t a2) {
  probe_log(self, 12, std::format("GetValueAddressX({:#x}, {:#x}, {:#x})", a0, a1, a2), XBEM_RETURN_ADDRESS());
  return &static_cast<FenceState*>(self->state)->value;
}
const ProbeVtable g_fence_vtable = make_vtable({{8, fn(&fence_get_completed_value)},
                                                {9, fn(&fence_set_event_on_completion)},
                                                {10, fn(&fence_signal_cpu)},
                                                {12, fn(&fence_get_value_address)}});

void* new_fence(const std::string& name, uint64_t initial) {
  auto* state = new FenceState;
  state->value = initial;
  return make_probe_object(name, &g_fence_vtable, state);
}

bool is_fence(void* object) {
  return object && static_cast<ProbeObject*>(object)->vtable == g_fence_vtable.data();
}

// ------------------------------------------------------- command queue

void capture_execute(ProbeObject* queue, uint64_t count, uint64_t lists);

// ID3D12CommandQueue. On the PC: 10 ExecuteCommandLists, 11 SetMarker,
// 12 BeginEvent, 13 EndEvent, 14 Signal, 15 Wait, 16 GetTimestampFrequency,
// 17 GetClockCalibration, 18 GetDesc. The console queue is shifted (a real
// title calls slot 16 with a fence - Signal - and writing a timestamp
// frequency there destroyed the fence), so slots 10-20 are dispatched by the
// shape of their arguments: a fence first means Signal/Wait, a count and an
// array of command lists means ExecuteCommandLists, a zeroed variable on the
// stack receives the timestamp frequency. Each slot's first calls are logged.
template <int Slot>
uint64_t XBEM_GUEST_ABI queue_method(ProbeObject* self, uint64_t a0, uint64_t a1, uint64_t a2) {
  const uint64_t ret = XBEM_RETURN_ADDRESS();
  void* const first = reinterpret_cast<void*>(a0);
  if (is_fence(first)) {
    const bool wait = Slot == 15 || Slot == 17;
    probe_log(self, Slot, std::format("{}(fence, {}) [by shape]", wait ? "Wait" : "Signal", a1), ret);
    if (!wait) fence_signal(fence_of(first), a1);
    return S_OK;
  }
  if (a0 >= 1 && a0 <= 256 && a1 >= 0x10000 && a1 < 0x800000000000ull && (Slot == 10 || Slot == 12)) {
    capture_execute(self, a0, a1);
    probe_log(self, Slot, std::format("ExecuteCommandLists({} lists) - completed instantly (headless) [by shape]", a0),
              ret);
    return 0;
  }
  if ((Slot == 16 || Slot == 18) && in_current_guest_stack(a0) && !is_probe_object(a0)) {
    probe_log(self, Slot, "GetTimestampFrequency [by shape] -> 100 MHz", ret);
    const uint64_t frequency = 100'000'000;
    platform::safe_write(a0, &frequency, 8);
    return S_OK;
  }
  probe_log(self, Slot, std::format("method{}({:#x}, {:#x}, {:#x}) - shape not recognised, S_OK", Slot, a0, a1, a2),
            ret);
  return S_OK;
}

// Console-only queue slots (measured): 35 ExecuteCommandListsX(count, lists,
// ...) - the title's command lists go through it; 32 is called once per frame
// with plane descriptions (PresentX). Both keep the probe's logging.
void capture_marker(ProbeObject* queue, const char* what);
template <int Slot>
uint64_t XBEM_GUEST_ABI queue_console_method(ProbeObject* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6) {
  const uint64_t args[6] = {a1, a2, a3, a4, a5, a6};
  if (Slot == 35 && a1 >= 1 && a1 <= 256) capture_execute(self, a1, a2);
  if (Slot == 32) capture_marker(self, "PresentX (queue slot 32)");
  return probe_dispatch(self, Slot, args, XBEM_RETURN_ADDRESS(), false);
}

template <int... Slots>
ProbeVtable make_queue_vtable(std::integer_sequence<int, Slots...>) {
  ProbeVtable v = make_vtable({{Slots + 10, fn(&queue_method<Slots + 10>)}...});
  v[32] = fn(&queue_console_method<32>);
  v[35] = fn(&queue_console_method<35>);
  return v;
}
const ProbeVtable g_queue_vtable = make_queue_vtable(std::make_integer_sequence<int, 11>{});

// ------------------------------------------------------------ resource

struct ResourceState {
  uint8_t desc[56] = {};  // D3D12_RESOURCE_DESC
  uint64_t gpu_va = 0;
  uint64_t size = 0;
  void* cpu = nullptr;
  std::mutex mutex;
};

// Bytes of a D3D12_RESOURCE_DESC in the emulator's linear texture layout
// (hle/texture_layout.h). Tiled console layouts pad more, which the title's
// own 64 KiB rounding absorbs for large surfaces.
uint64_t resource_size(const uint8_t* desc) { return linear_layout(desc).total; }

// ID3D12Resource: 8 Map, 9 Unmap, 10 GetDesc (hidden return), 11 GetGPUVirtualAddress
HRESULT XBEM_GUEST_ABI resource_map(ProbeObject* self, UINT, const void*, void** data) {
  auto* r = static_cast<ResourceState*>(self->state);
  std::lock_guard lock(r->mutex);
  if (!r->cpu) {
    r->cpu = virtual_alloc(nullptr, std::max<uint64_t>(r->size, 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0);
    if (!r->cpu) return E_OUTOFMEMORY;
  }
  if (data) *data = r->cpu;
  return S_OK;
}
void XBEM_GUEST_ABI resource_unmap(ProbeObject*, UINT, const void*) {}
void* XBEM_GUEST_ABI resource_get_desc(uint64_t a0, uint64_t a1) {
  const StructReturn r = struct_return(a0, a1);
  std::memcpy(r.out, static_cast<ResourceState*>(r.self->state)->desc, 56);
  return r.out;
}
uint64_t XBEM_GUEST_ABI resource_get_gpu_va(ProbeObject* self) { return static_cast<ResourceState*>(self->state)->gpu_va; }
const ProbeVtable g_resource_vtable = make_vtable({{8, fn(&resource_map)},
                                                   {9, fn(&resource_unmap)},
                                                   {10, fn(&resource_get_desc)},
                                                   {11, fn(&resource_get_gpu_va)}});

// The console has unified memory: a resource's GPU virtual address is the CPU
// address of its memory, and titles write through either. `memory` places the
// resource (heap + offset); otherwise it gets memory of its own.
void* new_resource(const std::string& name, const uint8_t* desc, uint64_t memory = 0) {
  auto* state = new ResourceState;
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), state->desc, 56);
  state->size = resource_size(state->desc);
  if (!memory)
    memory = reinterpret_cast<uint64_t>(
        virtual_alloc(nullptr, std::max<uint64_t>(state->size, 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0));
  state->cpu = reinterpret_cast<void*>(memory);
  state->gpu_va = memory ? memory : allocate_gpu_va(state->size);
  return make_probe_object(name, &g_resource_vtable, state);
}

// ---------------------------------------------------------------- heap

// ID3D12Heap: memory that placed resources live in.
struct HeapState {
  uint64_t base = 0;
  uint64_t size = 0;
  uint8_t desc[48] = {};  // D3D12_HEAP_DESC
};

std::mutex g_heaps_mutex;
std::unordered_map<const void*, HeapState*> g_heaps;

void* XBEM_GUEST_ABI heap_object_get_desc(uint64_t a0, uint64_t a1) {
  const StructReturn r = struct_return(a0, a1);
  std::memcpy(r.out, static_cast<HeapState*>(r.self->state)->desc, 40);
  return r.out;
}
const ProbeVtable g_heap_object_vtable = make_vtable({{8, fn(&heap_object_get_desc)}});

void* new_heap(const std::string& name, uint64_t base, uint64_t size, const uint8_t* desc) {
  auto* state = new HeapState;
  state->base = base;
  state->size = size;
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), state->desc, 40);
  else std::memcpy(state->desc, &size, 8);
  void* object = make_probe_object(name, &g_heap_object_vtable, state);
  std::lock_guard lock(g_heaps_mutex);
  g_heaps[object] = state;
  return object;
}

uint64_t heap_base(const void* heap) {
  std::lock_guard lock(g_heaps_mutex);
  auto it = g_heaps.find(heap);
  return it == g_heaps.end() ? 0 : it->second->base;
}

// ----------------------------------------------------- descriptor heap

struct DescriptorHeapState {
  uint8_t desc[16] = {};  // D3D12_DESCRIPTOR_HEAP_DESC
  uint64_t cpu = 0;
  uint64_t gpu = 0;
};
constexpr uint32_t kDescriptorSize = 32;

// ID3D12DescriptorHeap: 8 GetDesc (hidden return), 9 GetCPUDescriptorHandleForHeapStart,
// 10 GetGPUDescriptorHandleForHeapStart. The console's D3D12.X returns the
// 8-byte handles in RAX - unlike the PC headers' hidden-pointer form (a real
// title calls slot 9 with garbage in RDX).
void* XBEM_GUEST_ABI heap_get_desc(uint64_t a0, uint64_t a1) {
  const StructReturn r = struct_return(a0, a1);
  std::memcpy(r.out, static_cast<DescriptorHeapState*>(r.self->state)->desc, 16);
  return r.out;
}
uint64_t XBEM_GUEST_ABI heap_get_cpu_start(ProbeObject* self) {
  return static_cast<DescriptorHeapState*>(self->state)->cpu;
}
uint64_t XBEM_GUEST_ABI heap_get_gpu_start(ProbeObject* self) {
  return static_cast<DescriptorHeapState*>(self->state)->gpu;
}
const ProbeVtable g_heap_vtable = make_vtable({{8, fn(&heap_get_desc)},
                                               {9, fn(&heap_get_cpu_start)},
                                               {10, fn(&heap_get_gpu_start)}});

// ------------------------------------------- blobs and root signatures

struct BlobState {
  std::vector<uint8_t> bytes;
};
void* XBEM_GUEST_ABI blob_get_pointer(ProbeObject* self) {
  return static_cast<BlobState*>(self->state)->bytes.data();
}
SIZE_T XBEM_GUEST_ABI blob_get_size(ProbeObject* self) { return static_cast<BlobState*>(self->state)->bytes.size(); }
const ProbeVtable g_blob_vtable = make_vtable({{3, fn(&blob_get_pointer)}, {4, fn(&blob_get_size)}});

struct DeserializerState {
  std::vector<uint8_t> desc;  // D3D12_ROOT_SIGNATURE_DESC and its arrays
};
const void* XBEM_GUEST_ABI deserializer_get_desc(ProbeObject* self) {
  return static_cast<DeserializerState*>(self->state)->desc.data();
}
const ProbeVtable g_deserializer_vtable = make_vtable({{3, fn(&deserializer_get_desc)}});

// ID3D12RootSignature: the parsed signature, kept for pipeline creation.
struct RootSignatureState {
  gpu::RootSignature rs;
};

// -------------------------------------------------------------- device

std::atomic<int> g_object_counter{0};
std::string child_name(const char* kind) { return std::format("d3d12.{}#{}", kind, g_object_counter.fetch_add(1)); }

HRESULT create_into(void* object, void** out) {
  if (!out) return E_POINTER;
  *out = object;
  return S_OK;
}

// ------------------------------------------------------ command capture
//
// Every command-list call is decoded while a capture is armed
// (`--capture-commands FILE@SECONDS[:MS]`), and the lists a queue executes in
// the window are written to FILE: what the title draws, with the resources
// behind its descriptors, before anything is rendered. Slots follow the PC
// ID3D12GraphicsCommandList order up to 26 and are shifted by 4 from 32
// (SetDescriptorHeaps, PC 28) - measured from the title's calls up to 52
// (ClearRenderTargetView); later and console-only slots are shown by number.

struct RecordedCommand {
  int slot;
  uint64_t args[6];
  std::string text;
};
struct ListRecord {
  std::mutex mutex;
  std::vector<RecordedCommand> commands;
  bool closed = false;  // the next command starts a new recording (the console Reset is not slot 10)
};

struct CaptureWindow {
  std::mutex mutex;
  std::filesystem::path path;
  std::ofstream out;
  std::chrono::steady_clock::time_point origin, start, end;
  bool configured = false, finished = false;
  uint64_t executes = 0, commands = 0;
};
CaptureWindow g_capture;
std::atomic<bool> g_capture_recording{false};

// Parses the option once (at device creation); the time counts from then.
void capture_configure() {
  std::lock_guard lock(g_capture.mutex);
  if (g_capture.configured) return;
  g_capture.configured = true;
  g_capture.origin = std::chrono::steady_clock::now();
  const std::string& spec = proc().options().capture_commands;
  const size_t at = spec.rfind('@');
  if (spec.empty() || at == std::string::npos) return;
  try {
    const std::string when = spec.substr(at + 1);
    const size_t colon = when.find(':');
    const double seconds = std::stod(when.substr(0, colon));
    const double ms = colon == std::string::npos ? 100.0 : std::stod(when.substr(colon + 1));
    g_capture.path = spec.substr(0, at);
    g_capture.start = g_capture.origin + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
    g_capture.end = g_capture.start + std::chrono::milliseconds(static_cast<int64_t>(ms));
    XBEM_INFO("d3d12", "command capture: lists executed {}..{} ms after device creation -> {}",
              static_cast<int64_t>(seconds * 1000), static_cast<int64_t>(seconds * 1000 + ms),
              g_capture.path.string());
    // Recording starts a second early: lists are recorded before they run.
    if (g_capture.start - std::chrono::seconds(1) <= g_capture.origin) g_capture_recording.store(true);
    std::thread([] {
      std::this_thread::sleep_until(g_capture.start - std::chrono::seconds(1));
      g_capture_recording.store(true);
      std::this_thread::sleep_until(g_capture.end + std::chrono::milliseconds(50));
      g_capture_recording.store(false);
      std::lock_guard lock(g_capture.mutex);
      if (g_capture.out.is_open()) {
        g_capture.out << std::format("# end: {} ExecuteCommandLists, {} commands\n", g_capture.executes,
                                     g_capture.commands);
        g_capture.out.close();
      }
      g_capture.finished = true;
      XBEM_INFO("d3d12", "command capture written: {} ({} ExecuteCommandLists, {} commands)", g_capture.path.string(),
                g_capture.executes, g_capture.commands);
    }).detach();
  } catch (const std::exception&) {
    XBEM_WARN("d3d12", "--capture-commands {}: expected FILE@SECONDS[:MS]", spec);
  }
}

// Descriptors. On the console a descriptor is 32 bytes of memory the device
// writes and titles copy themselves (heaps live in title memory and GPU
// handles are the same addresses). The emulator writes its own encoding into
// those bytes, so copies made by the title or by CopyDescriptors carry the
// view along and a descriptor table can be decoded wherever it ends up.
struct XbemDescriptor {
  uint32_t magic;  // "XBDS"
  uint8_t kind;    // DescriptorKind
  uint8_t pad[3];
  uint64_t object;  // the resource (a probe object), or 0
  uint64_t a, b;    // CBV: address and size; views: the view description's first bytes
};
static_assert(sizeof(XbemDescriptor) == 32);
constexpr uint32_t kDescriptorMagic = 0x53444258;
enum DescriptorKind : uint8_t { kCbv = 1, kSrv, kUav, kRtv, kDsv, kSampler };
const char* const kDescriptorKindNames[] = {"?", "CBV", "SRV", "UAV", "RTV", "DSV", "sampler"};

std::string describe_resource(uint64_t p);

void write_descriptor(uint64_t handle, DescriptorKind kind, uint64_t object, uint64_t a = 0, uint64_t b = 0) {
  const XbemDescriptor d{kDescriptorMagic, kind, {}, object, a, b};
  platform::safe_write(handle, &d, sizeof d);
}
template <class T>
T read_guest(uint64_t address) {
  T value{};
  platform::safe_read(address, &value, sizeof value);
  return value;
}

std::string describe_object(uint64_t p) {
  if (!p) return "null";
  if (!is_probe_object(p)) return std::format("{:#x}", p);
  return describe_resource(p);
}

std::string describe_descriptor(uint64_t handle) {
  XbemDescriptor d{};
  if (!platform::safe_read(handle, &d, sizeof d) || d.magic != kDescriptorMagic || d.kind > kSampler)
    return std::format("{:#x}", handle);
  if (d.kind == kCbv) return std::format("{:#x} CBV va {:#x} {} bytes", handle, d.a, d.b);
  return std::format("{:#x} {} {}", handle, kDescriptorKindNames[d.kind], d.object ? describe_object(d.object) : "");
}

// A descriptor table: the first descriptors from `handle` (up to 4).
std::string describe_table(uint64_t handle) {
  std::string s = describe_descriptor(handle);
  for (int i = 1; i < 4; ++i) {
    XbemDescriptor d{};
    if (!platform::safe_read(handle + i * kDescriptorSize, &d, sizeof d) || d.magic != kDescriptorMagic) break;
    s += " | " + describe_descriptor(handle + i * kDescriptorSize).substr(std::format("{:#x} ", handle + i * kDescriptorSize).size());
  }
  return s;
}

std::string decode_command(int slot, const uint64_t* a) {
  const auto u32 = [&](int i) { return static_cast<uint32_t>(a[i]); };
  switch (slot) {
    case 9: return "Close";
    case 10: return std::format("Reset(allocator, pso {})", describe_object(a[1]));
    case 12: return std::format("DrawInstanced({} vertices, {} instances, start {} / {})", u32(0), u32(1), u32(2), u32(3));
    case 13:
      return std::format("DrawIndexedInstanced({} indices, {} instances, start {}, base vertex {}, start instance {})",
                         u32(0), u32(1), u32(2), static_cast<int32_t>(a[3]), u32(4));
    case 14: return std::format("Dispatch({}, {}, {})", u32(0), u32(1), u32(2));
    case 15:
      return std::format("CopyBufferRegion({} +{:#x} <- {} +{:#x}, {:#x} bytes)", describe_object(a[0]), a[1],
                         describe_object(a[2]), a[3], a[4]);
    case 16:
      return std::format("CopyTextureRegion(dst {} at {},{},{} <- src {})", describe_object(read_guest<uint64_t>(a[0])),
                         u32(1), u32(2), u32(3), describe_object(read_guest<uint64_t>(a[4])));
    case 17: return std::format("CopyResource({} <- {})", describe_object(a[0]), describe_object(a[1]));
    case 20: return std::format("IASetPrimitiveTopology({:#x})", u32(0));
    case 21: {
      std::string s = std::format("RSSetViewports({}", u32(0));
      for (uint32_t i = 0; i < std::min<uint32_t>(u32(0), 4); ++i) {
        float v[6];
        platform::safe_read(a[1] + i * 24, v, sizeof v);
        s += std::format(", [{} {} {}x{} z {}..{}]", v[0], v[1], v[2], v[3], v[4], v[5]);
      }
      return s + ")";
    }
    case 22: {
      std::string s = std::format("RSSetScissorRects({}", u32(0));
      for (uint32_t i = 0; i < std::min<uint32_t>(u32(0), 4); ++i) {
        int32_t r[4];
        platform::safe_read(a[1] + i * 16, r, sizeof r);
        s += std::format(", [{},{} - {},{}]", r[0], r[1], r[2], r[3]);
      }
      return s + ")";
    }
    case 23: {
      float f[4] = {};
      platform::safe_read(a[0], f, sizeof f);
      return std::format("OMSetBlendFactor({} {} {} {})", f[0], f[1], f[2], f[3]);
    }
    case 24: return std::format("OMSetStencilRef({})", u32(0));
    case 25: return std::format("SetPipelineState({})", describe_object(a[0]));
    case 26: return std::format("ResourceBarrier({} barriers)", u32(0));
    case 32: {
      std::string s = std::format("SetDescriptorHeaps({}", u32(0));
      for (uint32_t i = 0; i < std::min<uint32_t>(u32(0), 4); ++i)
        s += ", " + describe_object(read_guest<uint64_t>(a[1] + i * 8));
      return s + ")";
    }
    case 33: return std::format("SetComputeRootSignature({})", describe_object(a[0]));
    case 34: return std::format("SetGraphicsRootSignature({})", describe_object(a[0]));
    case 35: return std::format("SetComputeRootDescriptorTable({}, {})", u32(0), describe_table(a[1]));
    case 36: return std::format("SetGraphicsRootDescriptorTable({}, {})", u32(0), describe_table(a[1]));
    case 37: return std::format("SetComputeRoot32BitConstant({}, {:#x}, {})", u32(0), u32(1), u32(2));
    case 38: return std::format("SetGraphicsRoot32BitConstant({}, {:#x}, {})", u32(0), u32(1), u32(2));
    case 39:
    case 40: {
      std::string s = std::format("Set{}Root32BitConstants({}, {} values at {}:", slot == 39 ? "Compute" : "Graphics",
                                  u32(0), u32(1), u32(3));
      for (uint32_t i = 0; i < std::min<uint32_t>(u32(1), 16); ++i)
        s += std::format(" {:#x}", read_guest<uint32_t>(a[2] + i * 4));
      return s + ")";
    }
    case 41: return std::format("SetComputeRootConstantBufferView({}, va {:#x})", u32(0), a[1]);
    case 42: return std::format("SetGraphicsRootConstantBufferView({}, va {:#x})", u32(0), a[1]);
    case 43: return std::format("SetComputeRootShaderResourceView({}, va {:#x})", u32(0), a[1]);
    case 44: return std::format("SetGraphicsRootShaderResourceView({}, va {:#x})", u32(0), a[1]);
    case 45: return std::format("SetComputeRootUnorderedAccessView({}, va {:#x})", u32(0), a[1]);
    case 46: return std::format("SetGraphicsRootUnorderedAccessView({}, va {:#x})", u32(0), a[1]);
    case 47: {
      if (!a[0]) return "IASetIndexBuffer(null)";
      const uint64_t va = read_guest<uint64_t>(a[0]);
      return std::format("IASetIndexBuffer(va {:#x}, {} bytes, format {})", va, read_guest<uint32_t>(a[0] + 8),
                         read_guest<uint32_t>(a[0] + 12));
    }
    case 48: {
      std::string s = std::format("IASetVertexBuffers(slot {}, {}", u32(0), u32(1));
      for (uint32_t i = 0; i < std::min<uint32_t>(u32(1), 4) && a[2]; ++i)
        s += std::format(", [va {:#x} {} bytes stride {}]", read_guest<uint64_t>(a[2] + i * 16),
                         read_guest<uint32_t>(a[2] + i * 16 + 8), read_guest<uint32_t>(a[2] + i * 16 + 12));
      return s + ")";
    }
    case 50: {
      std::string s = std::format("OMSetRenderTargets({}", u32(0));
      const bool contiguous = static_cast<uint8_t>(a[2]) != 0;
      for (uint32_t i = 0; i < std::min<uint32_t>(u32(0), 8) && a[1]; ++i) {
        const uint64_t h = contiguous ? read_guest<uint64_t>(a[1]) + i * kDescriptorSize : read_guest<uint64_t>(a[1] + i * 8);
        s += ", rtv " + describe_descriptor(h);
      }
      if (a[3]) s += ", dsv " + describe_descriptor(read_guest<uint64_t>(a[3]));
      return s + ")";
    }
    case 51: return std::format("ClearDepthStencilView({}, flags {:#x})", describe_descriptor(a[0]), u32(1));
    case 52: {
      float c[4] = {};
      platform::safe_read(a[1], c, sizeof c);
      return std::format("ClearRenderTargetView({}, [{} {} {} {}])", describe_descriptor(a[0]), c[0], c[1], c[2], c[3]);
    }
    default:
      return std::format("method{}({:#x}, {:#x}, {:#x}, {:#x})", slot, a[0], a[1], a[2], a[3]);
  }
}

ListRecord* list_record_of(ProbeObject* self) { return static_cast<ListRecord*>(self->state); }

void record_command(ProbeObject* self, int slot, const uint64_t* args) {
  ListRecord* r = list_record_of(self);
  if (!r) return;
  std::lock_guard lock(r->mutex);
  if (r->closed || slot == 10) {
    r->commands.clear();
    r->closed = false;
  }
  if (slot == 9) r->closed = true;
  if (!g_capture_recording.load(std::memory_order_relaxed)) return;
  RecordedCommand c{slot, {}, decode_command(slot, args)};
  std::memcpy(c.args, args, sizeof c.args);
  r->commands.push_back(std::move(c));
}

// Writes the lists of one ExecuteCommandLists call when it falls in the window.
void capture_execute(ProbeObject* queue, uint64_t count, uint64_t lists);

template <int Slot>
uint64_t XBEM_GUEST_ABI list_method(ProbeObject* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6) {
  const uint64_t args[6] = {a1, a2, a3, a4, a5, a6};
  if (self && self->magic == kProbeMagic) record_command(self, Slot, args);
  return probe_dispatch(self, Slot, args, XBEM_RETURN_ADDRESS(), false);
}

std::string describe_resource(uint64_t p) {
  auto* o = reinterpret_cast<ProbeObject*>(p);
  if (o->vtable != g_resource_vtable.data()) return o->name;
  const auto* r = static_cast<ResourceState*>(o->state);
  uint32_t dimension, height, format;
  uint64_t width;
  uint16_t depth, mips;
  std::memcpy(&dimension, r->desc, 4);
  std::memcpy(&width, r->desc + 16, 8);
  std::memcpy(&height, r->desc + 24, 4);
  std::memcpy(&depth, r->desc + 28, 2);
  std::memcpy(&mips, r->desc + 30, 2);
  std::memcpy(&format, r->desc + 32, 4);
  if (dimension == 1) return std::format("{} (buffer {:#x} bytes at {:#x})", o->name, width, r->gpu_va);
  return std::format("{} ({}D {}x{}x{}, {} mips, format {}, at {:#x})", o->name, dimension - 1, width, height, depth,
                     mips, format, r->gpu_va);
}

void capture_execute(ProbeObject* queue, uint64_t count, uint64_t lists) {
  if (!g_capture_recording.load(std::memory_order_relaxed)) return;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_capture.mutex);
  if (g_capture.finished || now < g_capture.start || now > g_capture.end) return;
  if (!g_capture.out.is_open()) {
    g_capture.out.open(g_capture.path);
    g_capture.out << "# xbem command capture: lists as the title recorded them, in execution order\n";
  }
  ++g_capture.executes;
  g_capture.out << std::format(
      "== {} ExecuteCommandLists({}) at +{} ms\n", queue->name, count,
      std::chrono::duration_cast<std::chrono::milliseconds>(now - g_capture.origin).count());
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t p = read_guest<uint64_t>(lists + i * 8);
    if (!is_probe_object(p)) continue;
    auto* list = reinterpret_cast<ProbeObject*>(p);
    ListRecord* r = list_record_of(list);
    if (!r) continue;
    std::lock_guard list_lock(r->mutex);
    g_capture.out << std::format("-- {}: {} commands\n", list->name, r->commands.size());
    for (const RecordedCommand& c : r->commands) g_capture.out << "   " << c.text << "\n";
    g_capture.commands += r->commands.size();
    if (g_capture.executes == 1 && !r->commands.empty())
      XBEM_INFO("d3d12", "command capture: {} ExecuteCommandLists - {} first: {}", queue->name, list->name,
                r->commands.front().text);
  }
  g_capture.out.flush();
}

void capture_marker(ProbeObject* queue, const char* what) {
  if (!g_capture_recording.load(std::memory_order_relaxed)) return;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_capture.mutex);
  if (g_capture.finished || now < g_capture.start || now > g_capture.end || !g_capture.out.is_open()) return;
  g_capture.out << std::format(
      "== {} {} at +{} ms\n", queue->name, what,
      std::chrono::duration_cast<std::chrono::milliseconds>(now - g_capture.origin).count());
}

// Device descriptor creation (PC order, measured shapes): 17 CBV(desc, handle),
// 18 SRV(resource, desc, handle), 19 UAV(resource, counter, desc, handle),
// 20 RTV(resource, desc, handle), 21 DSV(resource, desc, handle), 22
// Sampler(desc, handle), 23 CopyDescriptors, 24 CopyDescriptorsSimple(count,
// dst, src, type).
template <int Slot>
uint64_t XBEM_GUEST_ABI device_descriptor_method(ProbeObject* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6) {
  const uint64_t args[6] = {a1, a2, a3, a4, a5, a6};
  const auto view = [](uint64_t desc) {  // a view description's first 16 bytes (format, dimension, ...)
    uint64_t v[2] = {};
    if (desc) platform::safe_read(desc, v, sizeof v);
    return std::pair{v[0], v[1]};
  };
  switch (Slot) {
    case 17: {
      uint64_t cbv[2] = {};
      if (a1) platform::safe_read(a1, cbv, sizeof cbv);
      write_descriptor(a2, kCbv, 0, cbv[0], static_cast<uint32_t>(cbv[1]));
      break;
    }
    case 18: { auto [x, y] = view(a2); write_descriptor(a3, kSrv, a1, x, y); break; }
    case 19: { auto [x, y] = view(a3); write_descriptor(a4, kUav, a1, x, y); break; }
    case 20: { auto [x, y] = view(a2); write_descriptor(a3, kRtv, a1, x, y); break; }
    case 21: { auto [x, y] = view(a2); write_descriptor(a3, kDsv, a1, x, y); break; }
    case 22: write_descriptor(a2, kSampler, 0); break;
    case 24:  // CopyDescriptorsSimple(count, dst, src, type): the bytes move, as on the console
      if (a1 && a1 <= (1u << 20)) {
        std::vector<uint8_t> bytes(a1 * kDescriptorSize);
        if (platform::safe_read(a3, bytes.data(), bytes.size())) platform::safe_write(a2, bytes.data(), bytes.size());
      }
      break;
    case 23: {  // CopyDescriptors(dst ranges, dst starts, dst sizes, src ranges, src starts, src sizes, type)
      std::vector<uint8_t> all;
      for (uint64_t r = 0; r < a4 && r < 4096; ++r) {
        const uint64_t start = read_guest<uint64_t>(a5 + r * 8);
        const uint32_t n = a6 ? read_guest<uint32_t>(a6 + r * 4) : 1;
        std::vector<uint8_t> bytes(uint64_t{n} * kDescriptorSize);
        if (n <= (1u << 20) && platform::safe_read(start, bytes.data(), bytes.size()))
          all.insert(all.end(), bytes.begin(), bytes.end());
      }
      size_t at = 0;
      for (uint64_t r = 0; r < a1 && r < 4096 && at < all.size(); ++r) {
        const uint64_t start = read_guest<uint64_t>(a2 + r * 8);
        const uint64_t n = std::min<uint64_t>(a3 ? read_guest<uint32_t>(a3 + r * 4) : 1, (all.size() - at) / kDescriptorSize);
        platform::safe_write(start, all.data() + at, n * kDescriptorSize);
        at += n * kDescriptorSize;
      }
      break;
    }
    default: break;
  }
  return probe_dispatch(self, Slot, args, XBEM_RETURN_ADDRESS(), false);
}


// Command lists. The title records commands partly itself: while a list's
// write cursor (+0x18) is below its limits (+0x28 for dispatches, +0x30 for
// other commands) it writes PM4 packets through the cursor instead of calling
// the list (seen in the executable at +0x3258a, and by Leonida in the shipped
// DLLs). With the cursor at the limit every command goes through the vtable,
// where it can be recorded. Cursor and limits point at a real buffer, so an
// unchecked inline write lands there instead of faulting.
alignas(4096) uint8_t g_inline_command_sink[0x10000];

constexpr size_t kListCursor = 0x18, kListDispatchLimit = 0x28, kListLimit = 0x30;

// Counted for the run window and logs: no renderer executes them yet.
std::atomic<uint64_t> g_command_lists{0}, g_draws{0}, g_dispatches{0};

// 12 DrawInstanced(vertices, instances, start vertex, start instance),
// 13 DrawIndexedInstanced(indices, instances, start index, base vertex, start
// instance), 14 Dispatch(x, y, z): the PC order, confirmed by the title's
// calls (a fullscreen pass draws 3 vertices, 1 instance).
void XBEM_GUEST_ABI list_draw_instanced(ProbeObject* self, uint32_t vertices, uint32_t instances, uint32_t sv,
                                        uint32_t si) {
  g_draws.fetch_add(1);
  const uint64_t args[6] = {vertices, instances, sv, si};
  record_command(self, 12, args);
  probe_log(self, 12, std::format("DrawInstanced({} vertices, {} instances)", vertices, instances),
            XBEM_RETURN_ADDRESS());
}
void XBEM_GUEST_ABI list_draw_indexed_instanced(ProbeObject* self, uint32_t indices, uint32_t instances, uint32_t si,
                                                int32_t bv, uint32_t sinst) {
  g_draws.fetch_add(1);
  const uint64_t args[6] = {indices, instances, si, static_cast<uint64_t>(static_cast<uint32_t>(bv)), sinst};
  record_command(self, 13, args);
  probe_log(self, 13, std::format("DrawIndexedInstanced({} indices, {} instances)", indices, instances),
            XBEM_RETURN_ADDRESS());
}
void XBEM_GUEST_ABI list_dispatch(ProbeObject* self, uint32_t x, uint32_t y, uint32_t z) {
  g_dispatches.fetch_add(1);
  const uint64_t args[6] = {x, y, z};
  record_command(self, 14, args);
  probe_log(self, 14, std::format("Dispatch({}, {}, {})", x, y, z), XBEM_RETURN_ADDRESS());
}
template <int... Slots>
ProbeVtable make_command_list_vtable(std::integer_sequence<int, Slots...>) {
  ProbeVtable v = make_vtable({{Slots + 9, fn(&list_method<Slots + 9>)}...});
  v[12] = fn(&list_draw_instanced);
  v[13] = fn(&list_draw_indexed_instanced);
  v[14] = fn(&list_dispatch);
  return v;
}
// Slots 9-159 are recorded (0-8: IUnknown, private data, name, device).
const ProbeVtable g_command_list_vtable = make_command_list_vtable(std::make_integer_sequence<int, 151>{});

void* new_command_list() {
  g_command_lists.fetch_add(1);
  auto* list =
      static_cast<ProbeObject*>(make_probe_object(child_name("command_list"), &g_command_list_vtable, new ListRecord));
  const uint64_t sink = reinterpret_cast<uint64_t>(g_inline_command_sink);
  for (const size_t off : {kListCursor, kListDispatchLimit, kListLimit})
    std::memcpy(reinterpret_cast<uint8_t*>(list) + off, &sink, 8);
  return list;
}

UINT XBEM_GUEST_ABI device_get_node_count(ProbeObject*) { return 1; }

// Interface IDs that mean "the D3D12 device itself": the IID the title created
// the device with, plus the public ID3D12Device..ID3D12Device9 IIDs.
std::mutex g_device_iid_mutex;
std::vector<std::array<uint8_t, 16>> g_device_iids;

bool is_device_iid(const uint8_t* iid) {
  static const uint32_t kPublic[] = {0x189819f1, 0x77acce80, 0x30baa41e, 0x81dadc15, 0x8754318e, 0x9727a022,
                                     0xc70b221b, 0x5c014b53, 0x9218e6bb, 0x4c80e962};
  uint32_t first;
  std::memcpy(&first, iid, 4);
  for (uint32_t p : kPublic)
    if (first == p) return true;
  std::lock_guard lock(g_device_iid_mutex);
  for (const auto& known : g_device_iids)
    if (std::memcmp(known.data(), iid, 16) == 0) return true;
  return false;
}

// ------------------------------------------------------------- DXGI
//
// Titles reach the display through the device: QueryInterface(IDXGIDevice)
// -> GetAdapter -> EnumOutputs(0) -> GetDisplayModeList. One adapter, one
// output, with 4K and 1080p modes at 60 and 120 Hz (what a TV offers a
// Series X). Slot numbers follow dxgi.h; the console's DXGI_MODE_DESC is
// 32 bytes (the title steps through the list in 32-byte strides).

constexpr HRESULT DXGI_ERROR_NOT_FOUND = static_cast<HRESULT>(0x887A0002);
constexpr HRESULT DXGI_ERROR_MORE_DATA = static_cast<HRESULT>(0x887A0003);

// IDXGIOutput: 21 GetDisplayModeList(format, flags, UINT* count, DXGI_MODE_DESC* modes)
HRESULT XBEM_GUEST_ABI output_get_display_mode_list(ProbeObject* self, uint32_t format, uint32_t flags, uint32_t* count,
                                                    uint8_t* modes) {
  struct Mode {
    uint32_t width, height, refresh;
  };
  static const Mode kModes[] = {{3840, 2160, 60}, {3840, 2160, 120}, {1920, 1080, 60}, {1920, 1080, 120}};
  constexpr uint32_t kStride = 32;
  probe_log(self, 21, std::format("GetDisplayModeList(format {}, flags {:#x}, {})", format, flags,
                                  modes ? "fill" : "count"),
            XBEM_RETURN_ADDRESS());
  if (!count) return E_INVALIDARG;
  const uint32_t n = sizeof kModes / sizeof kModes[0];
  if (!modes) {
    *count = n;
    return S_OK;
  }
  const uint32_t written = std::min(*count, n);
  for (uint32_t i = 0; i < written; ++i) {
    uint8_t* m = modes + i * kStride;
    std::memset(m, 0, kStride);
    const uint32_t fields[] = {kModes[i].width, kModes[i].height, kModes[i].refresh, 1,
                               format ? format : 87u /*B8G8R8A8_UNORM*/, 1 /*progressive*/, 0};
    std::memcpy(m, fields, sizeof fields);
  }
  *count = written;
  return written < n ? DXGI_ERROR_MORE_DATA : S_OK;
}
const ProbeVtable g_output_vtable = make_vtable({{21, fn(&output_get_display_mode_list)}});

// IDXGIAdapter: 7 EnumOutputs(index, IDXGIOutput**)
HRESULT XBEM_GUEST_ABI adapter_enum_outputs(ProbeObject* self, uint32_t index, void** out) {
  probe_log(self, 7, std::format("EnumOutputs({})", index), XBEM_RETURN_ADDRESS());
  if (!out) return E_POINTER;
  if (index != 0) {
    *out = nullptr;
    return DXGI_ERROR_NOT_FOUND;
  }
  static void* output = make_probe_object("dxgi.output", &g_output_vtable);
  *out = output;
  return S_OK;
}
const ProbeVtable g_adapter_vtable = make_vtable({{7, fn(&adapter_enum_outputs)}});

// IDXGIDevice: 7 GetAdapter(IDXGIAdapter**)
HRESULT XBEM_GUEST_ABI dxgi_device_get_adapter(ProbeObject* self, void** out) {
  probe_log(self, 7, "GetAdapter", XBEM_RETURN_ADDRESS());
  if (!out) return E_POINTER;
  static void* adapter = make_probe_object("dxgi.adapter", &g_adapter_vtable);
  *out = adapter;
  return S_OK;
}
const ProbeVtable g_dxgi_device_vtable = make_vtable({{7, fn(&dxgi_device_get_adapter)}});

bool is_dxgi_device_iid(const uint8_t* iid) {
  // IDXGIDevice {54ec77fa-1377-44e6-8c32-88fd5f44c84c} and IDXGIDevice1..4 share the family.
  static const uint32_t kDxgiDevice[] = {0x54ec77fa, 0x77db970f, 0x05008617, 0x6007896c, 0x95b4f95f};
  uint32_t first;
  std::memcpy(&first, iid, 4);
  for (uint32_t p : kDxgiDevice)
    if (first == p) return true;
  return false;
}

// QueryInterface: the device for D3D12 device IIDs; any other interface (for
// example IDXGIDevice, from which titles get the adapter and its outputs) is a
// separate object, so its methods do not collide with the device's slots.
HRESULT XBEM_GUEST_ABI device_query_interface(ProbeObject* self, const uint8_t* iid, void** out) {
  if (!out) return E_POINTER;
  uint8_t id[16] = {};
  if (iid) platform::safe_read(reinterpret_cast<uint64_t>(iid), id, 16);
  if (is_device_iid(id)) {
    self->refs.fetch_add(1);
    *out = self;
    return S_OK;
  }
  static std::mutex mutex;
  static std::vector<std::pair<std::array<uint8_t, 16>, void*>> interfaces;
  std::lock_guard lock(mutex);
  std::array<uint8_t, 16> key;
  std::memcpy(key.data(), id, 16);
  for (auto& [k, object] : interfaces)
    if (k == key) {
      *out = object;
      return S_OK;
    }
  void* object = is_dxgi_device_iid(id)
                     ? make_probe_object("dxgi.device", &g_dxgi_device_vtable)
                     : make_probe_object(std::format("d3d12.device.as{{{}}}", guid_to_string(id)));
  interfaces.emplace_back(key, object);
  probe_log(self, 0, std::format("QueryInterface({{{}}}) -> separate interface object", guid_to_string(id)),
            XBEM_RETURN_ADDRESS());
  *out = object;
  return S_OK;
}

// 8 CreateCommandQueue(desc, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_command_queue(ProbeObject* self, const uint32_t* desc, const void*, void** out) {
  probe_log(self, 8, std::format("CreateCommandQueue(type {})", desc ? desc[0] : 0), XBEM_RETURN_ADDRESS());
  return create_into(make_probe_object(child_name("queue"), &g_queue_vtable), out);
}

// 10 CreateGraphicsPipelineState(desc, riid, ppv): VS, PS, DS, HS, GS bytecode at +8
HRESULT XBEM_GUEST_ABI device_create_graphics_pso(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  probe_log(self, 10, "CreateGraphicsPipelineState", XBEM_RETURN_ADDRESS());
  static const char* kStages[] = {"vs", "ps", "ds", "hs", "gs"};
  if (desc)
    for (int i = 0; i < 5; ++i) {
      uint64_t bc[2] = {};
      if (platform::safe_read(reinterpret_cast<uint64_t>(desc) + 8 + i * 16, bc, 16)) capture_shader(kStages[i], bc[0], bc[1]);
    }
  return create_into(make_probe_object(child_name("pso")), out);
}

// 11 CreateComputePipelineState(desc, riid, ppv): CS bytecode at +8
HRESULT XBEM_GUEST_ABI device_create_compute_pso(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  probe_log(self, 11, "CreateComputePipelineState", XBEM_RETURN_ADDRESS());
  uint64_t bc[2] = {};
  if (desc && platform::safe_read(reinterpret_cast<uint64_t>(desc) + 8, bc, 16)) capture_shader("cs", bc[0], bc[1]);
  return create_into(make_probe_object(child_name("pso")), out);
}

// 13 CheckFeatureSupport(feature, data, size): report "not supported" values (zeros).
HRESULT XBEM_GUEST_ABI device_check_feature_support(ProbeObject* self, uint32_t feature, void* data, UINT size) {
  probe_log(self, 13, std::format("CheckFeatureSupport(feature {}, {} bytes) -> zeros", feature, size),
            XBEM_RETURN_ADDRESS());
  if (data && size <= 4096) platform::safe_write(reinterpret_cast<uint64_t>(data), std::vector<uint8_t>(size).data(), size);
  return S_OK;
}

// Is `address` inside memory the title allocated (VirtualAlloc / XMemVirtualAlloc)?
bool is_allocated_memory(uint64_t address) {
  if (address < 0x10000 || address >= 0x800000000000ull) return false;
  uint8_t mbi[48] = {};
  if (virtual_query(reinterpret_cast<void*>(address), mbi, sizeof mbi) != 48) return false;
  uint32_t state, kind;
  std::memcpy(&state, mbi + 32, 4);
  std::memcpy(&kind, mbi + 40, 4);
  return state != MEM_FREE && kind == MEM_PRIVATE;
}

// 14 CreateDescriptorHeap(desc, riid, ppv). The console's descriptor heap
// desc is the PC {Type, NumDescriptors, Flags, NodeMask} followed by the memory
// the title set aside for the descriptors and its size (+16, +24; the title
// checks that the heap starts there). Some call sites fill only the 16 PC
// bytes and leave stack contents behind them, so the memory is taken only when
// the size covers the heap.
HRESULT XBEM_GUEST_ABI device_create_descriptor_heap(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  auto* state = new DescriptorHeapState;
  uint8_t raw[32] = {};
  if (desc) {
    if (!platform::safe_read(reinterpret_cast<uint64_t>(desc), raw, sizeof raw))
      platform::safe_read(reinterpret_cast<uint64_t>(desc), raw, 16);
    std::memcpy(state->desc, raw, 16);
  }
  uint32_t count = 0;
  std::memcpy(&count, state->desc + 4, 4);
  const uint64_t bytes = std::max<uint64_t>(count, 1) * kDescriptorSize;
  uint64_t memory = 0, memory_size = 0;
  std::memcpy(&memory, raw + 16, 8);
  std::memcpy(&memory_size, raw + 24, 8);
  const bool title_memory = memory && memory_size >= bytes && memory_size < (1ull << 40) && is_allocated_memory(memory);
  if (!title_memory) memory = 0;
  if (!memory)
    memory = reinterpret_cast<uint64_t>(virtual_alloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0));
  state->cpu = memory;
  state->gpu = memory;  // unified memory: shader-visible descriptors are read from the same bytes
  std::string dump;
  for (int i = 0; i < 32; i += 8) {
    uint64_t v;
    std::memcpy(&v, raw + i, 8);
    dump += std::format(" {:016x}", v);
  }
  probe_log(self, 14,
            std::format("CreateDescriptorHeap({} descriptors, type {}, flags {:#x}) at {:#x} ({}) desc:{}", count,
                        raw[0], raw[8], memory,
                        title_memory ? "title memory from desc+16" : "own memory",
                        dump),
            XBEM_RETURN_ADDRESS());
  return create_into(make_probe_object(child_name("descriptor_heap"), &g_heap_vtable, state), out);
}

UINT XBEM_GUEST_ABI device_get_descriptor_increment(ProbeObject*, uint32_t) { return kDescriptorSize; }

// 27 CreateCommittedResource(heapProps, flags, desc, state, clear, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_committed_resource(ProbeObject* self, const void*, uint32_t, const uint8_t* desc,
                                                        uint32_t, const void*, const void*, void** out) {
  probe_log(self, 27, "CreateCommittedResource", XBEM_RETURN_ADDRESS());
  return create_into(new_resource(child_name("resource"), desc), out);
}
// 29 CreatePlacedResource(heap, offset, desc, state, clear, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_placed_resource(ProbeObject* self, void* heap, uint64_t offset,
                                                     const uint8_t* desc, uint32_t, const void*, const void*,
                                                     void** out) {
  const uint64_t base = heap_base(heap);
  probe_log(self, 29, std::format("CreatePlacedResource(heap base {:#x} + {:#x})", base, offset),
            XBEM_RETURN_ADDRESS());
  return create_into(new_resource(child_name("resource"), desc, base ? base + offset : 0), out);
}
// 28 CreateHeap(desc, riid, ppv): D3D12_HEAP_DESC starts with SizeInBytes.
HRESULT XBEM_GUEST_ABI device_create_heap(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  uint64_t size = 0;
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), &size, 8);
  void* memory = virtual_alloc(nullptr, std::max<uint64_t>(size, 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0);
  probe_log(self, 28, std::format("CreateHeap({:#x} bytes) -> {}", size, memory), XBEM_RETURN_ADDRESS());
  if (!memory) return E_OUTOFMEMORY;
  return create_into(new_heap(child_name("heap"), reinterpret_cast<uint64_t>(memory), size, desc), out);
}

bool read_guid(uint64_t address, uint8_t out[16]) {
  return address >= 0x10000 && address < 0x800000000000ull && platform::safe_read(address, out, 16);
}

// 133 (D3D12.X) CreatePlacedResourceX(va, desc, layout, ..., riid, ppv): a
// buffer on pages the title mapped itself (XMemMapPhysicalPages) - the
// resource's memory and GPU address are `va`. `layout` is a
// D3D12_BARRIER_LAYOUT (0xFFFFFFFF = undefined). The interface ID and output
// are found by shape (an ID3D12Resource IID followed by the output pointer).
HRESULT XBEM_GUEST_ABI device_create_placed_resource_x(ProbeObject* self, uint64_t va, const uint8_t* desc,
                                                       uint64_t layout, uint64_t a3, uint64_t a4, uint64_t a5,
                                                       uint64_t a6) {
  static const uint8_t kResourceIid[16] = {0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
                                           0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad};
  const uint64_t args[4] = {a3, a4, a5, a6};
  void** out = nullptr;
  for (int i = 0; i + 1 < 4 && !out; ++i) {
    uint8_t guid[16];
    if (read_guid(args[i], guid) && std::memcmp(guid, kResourceIid, 16) == 0 && args[i + 1] >= 0x10000)
      out = reinterpret_cast<void**>(args[i + 1]);
  }
  uint8_t d[56] = {};
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), d, sizeof d);
  uint64_t width = 0;
  std::memcpy(&width, d + 16, 8);
  probe_log(self, 133,
            std::format("CreatePlacedResourceX(va {:#x}, dimension {}, width {:#x}, layout {:#x}){}", va, d[0], width,
                        layout, out ? "" : " - no ID3D12Resource output found"),
            XBEM_RETURN_ADDRESS());
  if (!out) return E_INVALIDARG;
  return create_into(new_resource(child_name("resource"), desc, va), out);
}

// 16 CreateRootSignature(nodeMask, blob, size, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_root_signature(ProbeObject* self, uint32_t, const uint8_t* blob, SIZE_T size,
                                                    const void*, void** out) {
  auto* state = new RootSignatureState;
  std::vector<uint8_t> bytes(std::min<SIZE_T>(size, 1 << 20));
  std::string error;
  const bool ok = blob && platform::safe_read(reinterpret_cast<uint64_t>(blob), bytes.data(), bytes.size()) &&
                  gpu::deserialize_root_signature(bytes.data(), bytes.size(), state->rs, error);
  probe_log(self, 16,
            ok ? std::format("CreateRootSignature({})", state->rs.summary())
               : std::format("CreateRootSignature({} bytes): not parsed ({}) - kept as an opaque signature", size,
                             error),
            XBEM_RETURN_ADDRESS());
  return create_into(make_probe_object(child_name("root_signature"), nullptr, state), out);
}

// 25 GetResourceAllocationInfo(visibleMask, numDescs, descs) -> {SizeInBytes, Alignment},
// returned through the result buffer (see struct_return).
// Slot 108 is a console variant called the same way (result buffer, device,
// visible mask 1, ...): GetResourceAllocationInfo1/2/3 share the first
// arguments, the extra ones (per-resource info, castable formats) are not used.
template <int Slot>
void* XBEM_GUEST_ABI device_get_resource_allocation_info(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                         uint64_t a4) {
  const StructReturn r = struct_return(a0, a1);
  // (mask, count, descs) or, without a mask, (count, descs).
  uint64_t count = a3, descs = a4;
  if (a3 >= 0x10000) {
    count = a2;
    descs = a3;
  }
  uint64_t size = 0, alignment = 0x10000;
  std::string what;
  bool ok = count > 0 && count < 4096 && descs >= 0x10000;
  for (uint64_t i = 0; ok && i < count; ++i) {
    uint8_t d[56];
    if (!platform::safe_read(descs + i * 56, d, sizeof d)) {
      ok = false;
      break;
    }
    uint64_t align;
    std::memcpy(&align, d + 8, 8);
    uint32_t samples;
    std::memcpy(&samples, d + 36, 4);
    if (samples > 1) align = std::max<uint64_t>(align, 4ull << 20);  // MSAA: 4 MiB placement alignment
    alignment = std::max<uint64_t>(alignment, align);
    size = ((size + alignment - 1) & ~(alignment - 1)) + ((resource_size(d) + 0xFFFF) & ~0xFFFFull);
    if (i < 2) {
      uint64_t w;
      uint32_t h, f;
      uint16_t a, mip;
      std::memcpy(&w, d + 16, 8);
      std::memcpy(&h, d + 24, 4);
      std::memcpy(&a, d + 28, 2);
      std::memcpy(&mip, d + 30, 2);
      std::memcpy(&f, d + 32, 4);
      what += std::format(" [dim {} {}x{}x{} mips {} format {} samples {} flags {:#x}]", d[0], w, h, a, mip, f,
                          samples, d[48]);
    }
  }
  const uint64_t result[2] = {ok ? size : ~0ull, ok ? alignment : 0};
  std::memcpy(r.out, result, 16);
  probe_log(r.self, Slot,
            std::format("GetResourceAllocationInfo{}({} descs{}) -> size {:#x}, alignment {:#x}",
                        Slot == 25 ? "" : "X", count, what, result[0], result[1]),
            XBEM_RETURN_ADDRESS());
  return r.out;
}

// 26 GetCustomHeapProperties(nodeMask, heapType) -> D3D12_HEAP_PROPERTIES (20 bytes).
void* XBEM_GUEST_ABI device_get_custom_heap_properties(uint64_t a0, uint64_t a1, uint64_t, uint64_t heap_type) {
  const StructReturn r = struct_return(a0, a1);
  // Unified memory: everything is CPU-visible write-back memory in the one pool.
  const uint32_t props[5] = {4 /*CUSTOM*/, heap_type == 2 /*UPLOAD*/ ? 2u /*WRITE_COMBINE*/ : 3u /*WRITE_BACK*/,
                             1 /*L0*/, 1, 1};
  std::memcpy(r.out, props, sizeof props);
  return r.out;
}

// 48 CreateCommandQueueX(desc, riid, ppv): the desc starts like D3D12_COMMAND_QUEUE_DESC.
HRESULT XBEM_GUEST_ABI device_create_command_queue_x(ProbeObject* self, const uint32_t* desc, const void* riid,
                                                     void** out) {
  return device_create_command_queue(self, desc, riid, out);
}

// 62 CreateComputePipelineState(desc, w2, w3, riid, ppv) with a PC compute desc.
HRESULT XBEM_GUEST_ABI device_create_compute_pso_x(ProbeObject* self, const uint8_t* desc, uint64_t, uint64_t,
                                                   const void* riid, void** out) {
  return device_create_compute_pso(self, desc, riid, out);
}

// 67 DeserializeComputePipelineStateX(desc, riid, ppv): native RDNA2 code at
// desc+0x28 (pointer) / +0x30 (size), root signature at +0x00.
HRESULT XBEM_GUEST_ABI device_deserialize_compute_pso_x(ProbeObject* self, const uint8_t* desc, const void*,
                                                        void** out) {
  uint64_t code[2] = {};
  if (desc && platform::safe_read(reinterpret_cast<uint64_t>(desc) + 0x28, code, 16)) capture_shader("cs-native", code[0], code[1]);
  probe_log(self, 67, std::format("DeserializeComputePipelineStateX(code {:#x}, {} bytes)", code[0], code[1]),
            XBEM_RETURN_ADDRESS());
  return create_into(make_probe_object(child_name("pso")), out);
}

// 76 texture creation: CreateCommittedResource's arguments; the desc's layout
// word is the console's (0x101 = tiled).
// The console variants of resource creation carry extra arguments before the
// (riid, ppv) pair, so the pair is found by shape: a pointer to an interface ID
// followed by the output pointer, searched from the end.
void** find_output_after_iid(const uint64_t* args, int n, int first) {
  for (int i = n - 2; i >= first; --i)
    if (is_interface_id_pointer(args[i]) && args[i + 1] >= 0x10000 && args[i + 1] < 0x800000000000ull)
      return reinterpret_cast<void**>(args[i + 1]);
  return nullptr;
}

// Resource creation with CreateCommittedResource's arguments (slot 76: heap
// properties, heap flags, description, state, clear value, riid, ppv) or
// CreateCommittedResource3's (slot 127, measured at the title's call site:
// heap properties, heap flags, description, initial layout, clear value,
// protected session, castable format count, castable formats, riid, ppv).
// The (riid, ppv) pair is located by shape among the trailing arguments.
template <int Slot>
HRESULT XBEM_GUEST_ABI device_create_texture_x(ProbeObject* self, const void* heap_props, uint64_t heap_flags,
                                               const uint8_t* desc, uint64_t state, uint64_t a4, uint64_t a5,
                                               uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9) {
  (void)heap_props;
  uint8_t d[56] = {};
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), d, sizeof d);
  uint64_t w;
  uint32_t h, f;
  std::memcpy(&w, d + 16, 8);
  std::memcpy(&h, d + 24, 4);
  std::memcpy(&f, d + 32, 4);
  // Slot 127 has exactly 10 arguments; slot 76 has 7 (later words are the caller's stack).
  constexpr int kTrailing = Slot == 127 ? 6 : 3;
  const uint64_t args[6] = {a4, a5, a6, a7, a8, a9};
  void** out = find_output_after_iid(args, kTrailing, 0);
  void* resource = out ? new_resource(child_name("texture"), desc) : nullptr;
  // The first 2D texture with D3D12_HEAP_FLAG_ALLOW_DISPLAY (0x8) is a back
  // buffer: the run window shows it.
  static std::atomic<bool> have_display_surface{false};
  if (resource && (heap_flags & 0x8) && d[0] == 3 && !have_display_surface.exchange(true)) {
    const auto* r = static_cast<ResourceState*>(static_cast<ProbeObject*>(resource)->state);
    const FormatInfo fi = format_info(f);
    const uint64_t pitch = (w * fi.bytes + 255) & ~255ull;
    if (r->cpu && !fi.block) platform::window_set_surface(r->cpu, static_cast<uint32_t>(w), h, pitch, f);
  }
  probe_log(self, Slot,
            std::format("CreateCommittedResource{}(dim {} {}x{} format {}, heap flags {:#x}, state/layout {:#x}, "
                        "clear value {:#x}) -> {}{}",
                        Slot == 127 ? "3" : "X", d[0], w, h, f, static_cast<uint32_t>(heap_flags),
                        static_cast<uint32_t>(state), a4, resource, out ? "" : " - no (riid, ppv) pair found"),
            XBEM_RETURN_ADDRESS());
  if (!out) return E_INVALIDARG;
  return create_into(resource, out);
}

// 78 SetFrameIntervalX(output, interval_us, latency, flags), 79
// ScheduleFrameEventX(event, offset_us, pEvent, flags), 80
// WaitFrameEventX(event, timeout_ms, pEvent, flags, &token). No display is
// paced yet: waits return at once with a fresh non-zero token.
std::atomic<uint64_t> g_frame_token{1};
HRESULT XBEM_GUEST_ABI device_set_frame_interval_x(ProbeObject* self, uint64_t output, uint64_t interval_us,
                                                   uint64_t latency, uint64_t flags) {
  probe_log(self, 78,
            std::format("SetFrameIntervalX(output {:#x}, {} us, latency {}, flags {:#x})", output,
                        static_cast<uint32_t>(interval_us), static_cast<uint32_t>(latency), flags),
            XBEM_RETURN_ADDRESS());
  return S_OK;
}
HRESULT XBEM_GUEST_ABI device_schedule_frame_event_x(ProbeObject* self, uint64_t event, uint64_t offset_us,
                                                     uint64_t, uint64_t flags) {
  probe_log(self, 79,
            std::format("ScheduleFrameEventX(event {}, offset {} us, flags {:#x})", static_cast<uint32_t>(event),
                        static_cast<uint32_t>(offset_us), flags),
            XBEM_RETURN_ADDRESS());
  return S_OK;
}
HRESULT XBEM_GUEST_ABI device_wait_frame_event_x(ProbeObject* self, uint64_t event, uint64_t timeout_ms, uint64_t,
                                                 uint64_t, uint64_t* token) {
  probe_log(self, 80,
            std::format("WaitFrameEventX(event {}, timeout {} ms)", static_cast<uint32_t>(event),
                        static_cast<uint32_t>(timeout_ms)),
            XBEM_RETURN_ADDRESS());
  if (!token || reinterpret_cast<uint64_t>(token) < 0x10000) return E_POINTER;
  const uint64_t value = g_frame_token.fetch_add(1);
  platform::safe_write(reinterpret_cast<uint64_t>(token), &value, 8);
  return S_OK;
}

// The output of a creation call: the argument after the interface ID.
void** out_after_iid(std::initializer_list<uint64_t> args) {
  const uint64_t* a = args.begin();
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (is_interface_id_pointer(a[i]) && a[i + 1] >= 0x10000) return reinterpret_cast<void**>(a[i + 1]);
  return nullptr;
}

// 12 CreateCommandList(node mask, type, allocator, initial state, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_command_list(ProbeObject* self, uint64_t, uint64_t type, uint64_t allocator,
                                                  uint64_t state, uint64_t riid, uint64_t ppv) {
  probe_log(self, 12, std::format("CreateCommandList(type {})", static_cast<uint32_t>(type)), XBEM_RETURN_ADDRESS());
  return create_into(new_command_list(), out_after_iid({allocator, state, riid, ppv}));
}
// 73 CreateCommandListX(desc, allocator, initial state, riid, ppv): the console
// form; the output is found after the interface ID.
HRESULT XBEM_GUEST_ABI device_create_command_list_x(ProbeObject* self, uint64_t desc, uint64_t allocator,
                                                    uint64_t state, uint64_t riid, uint64_t ppv, uint64_t extra) {
  uint32_t head[4] = {};
  platform::safe_read(desc, head, sizeof head);
  void** out = out_after_iid({allocator, state, riid, ppv, extra});
  probe_log(self, 73,
            std::format("CreateCommandListX(desc {:#x} {:#x} {:#x} {:#x}) -> {}", head[0], head[1], head[2], head[3],
                        out ? "typed command list (cursor at its limit)" : "no output pointer"),
            XBEM_RETURN_ADDRESS());
  return create_into(new_command_list(), out);
}
// 30 CreateReservedResource(desc, state, clear, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_reserved_resource(ProbeObject* self, const uint8_t* desc, uint32_t, const void*,
                                                       const void*, void** out) {
  probe_log(self, 30, "CreateReservedResource", XBEM_RETURN_ADDRESS());
  return create_into(new_resource(child_name("resource"), desc), out);
}
// 36 CreateFence(initial, flags, riid, ppv)
HRESULT XBEM_GUEST_ABI device_create_fence(ProbeObject* self, uint64_t initial, uint32_t, const void*, void** out) {
  probe_log(self, 36, std::format("CreateFence(initial {})", initial), XBEM_RETURN_ADDRESS());
  return create_into(new_fence(child_name("fence"), initial), out);
}
HRESULT XBEM_GUEST_ABI device_get_removed_reason(ProbeObject*) { return S_OK; }

const ProbeVtable g_device_vtable = make_vtable({
    {0, fn(&device_query_interface)},
    {7, fn(&device_get_node_count)},
    {8, fn(&device_create_command_queue)},
    {10, fn(&device_create_graphics_pso)},
    {11, fn(&device_create_compute_pso)},
    {12, fn(&device_create_command_list)},
    {13, fn(&device_check_feature_support)},
    {14, fn(&device_create_descriptor_heap)},
    {15, fn(&device_get_descriptor_increment)},
    {17, fn(&device_descriptor_method<17>)},
    {18, fn(&device_descriptor_method<18>)},
    {19, fn(&device_descriptor_method<19>)},
    {20, fn(&device_descriptor_method<20>)},
    {21, fn(&device_descriptor_method<21>)},
    {22, fn(&device_descriptor_method<22>)},
    {23, fn(&device_descriptor_method<23>)},
    {24, fn(&device_descriptor_method<24>)},
    {27, fn(&device_create_committed_resource)},
    {28, fn(&device_create_heap)},
    {29, fn(&device_create_placed_resource)},
    {30, fn(&device_create_reserved_resource)},
    {36, fn(&device_create_fence)},
    {37, fn(&device_get_removed_reason)},
    {16, fn(&device_create_root_signature)},
    {25, fn(&device_get_resource_allocation_info<25>)},
    {108, fn(&device_get_resource_allocation_info<108>)},
    {26, fn(&device_get_custom_heap_properties)},
    {48, fn(&device_create_command_queue_x)},
    {62, fn(&device_create_compute_pso_x)},
    {67, fn(&device_deserialize_compute_pso_x)},
    {73, fn(&device_create_command_list_x)},
    {76, fn(&device_create_texture_x<76>)},
    // 127: CreateCommittedResource3 (the title's back buffers, heap flag ALLOW_DISPLAY).
    {127, fn(&device_create_texture_x<127>)},
    {78, fn(&device_set_frame_interval_x)},
    {79, fn(&device_schedule_frame_event_x)},
    {80, fn(&device_wait_frame_event_x)},
    {133, fn(&device_create_placed_resource_x)},
});

}  // namespace

D3d12Counters d3d12_counters() {
  return {g_frame_token.load() - 1, g_command_lists.load(), g_draws.load(), g_dispatches.load()};
}

void* create_headless_d3d12_device(const void* iid) {
  capture_configure();
  if (iid) {
    std::array<uint8_t, 16> id{};
    if (platform::safe_read(reinterpret_cast<uint64_t>(iid), id.data(), 16)) {
      std::lock_guard lock(g_device_iid_mutex);
      g_device_iids.push_back(id);
    }
  }
  static void* device = make_probe_object("d3d12.device", &g_device_vtable);
  return device;
}

bool signal_d3d12_fence(void* fence, uint64_t value) {
  if (!is_fence(fence)) return false;
  fence_signal(fence_of(fence), value);
  return true;
}

void* make_d3d_blob(std::vector<uint8_t> bytes) {
  auto* state = new BlobState{std::move(bytes)};
  return make_probe_object("d3d.blob", &g_blob_vtable, state);
}

void* make_root_signature_deserializer(const gpu::RootSignature& rs) {
  auto* state = new DeserializerState;
  gpu::build_root_signature_desc_1_0(rs, state->desc);
  return make_probe_object("d3d12.root_signature_deserializer", &g_deserializer_vtable, state);
}

}  // namespace xbem::hle
