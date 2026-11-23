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
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpu/root_signature.h"
#include "hle/d3d12_device.h"
#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "platform/memory.h"

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

ProbeVtable make_vtable(std::initializer_list<std::pair<int, void*>> overrides) {
  ProbeVtable v = default_probe_vtable();
  for (const auto& [slot, f] : overrides) v[static_cast<size_t>(slot)] = f;
  return v;
}

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
const ProbeVtable g_fence_vtable = make_vtable({{8, fn(&fence_get_completed_value)},
                                                {9, fn(&fence_set_event_on_completion)},
                                                {10, fn(&fence_signal_cpu)}});

void* new_fence(const std::string& name, uint64_t initial) {
  auto* state = new FenceState;
  state->value = initial;
  return make_probe_object(name, &g_fence_vtable, state);
}

bool is_fence(void* object) {
  return object && static_cast<ProbeObject*>(object)->vtable == g_fence_vtable.data();
}

// ------------------------------------------------------- command queue

// ID3D12CommandQueue: 10 ExecuteCommandLists, 14 Signal, 15 Wait, 16 GetTimestampFrequency
void XBEM_GUEST_ABI queue_execute(ProbeObject* self, UINT count, void* const* lists) {
  probe_log(self, 10, std::format("ExecuteCommandLists({} lists) - completed instantly (headless)", count),
            XBEM_RETURN_ADDRESS());
  (void)lists;
}
HRESULT XBEM_GUEST_ABI queue_signal(ProbeObject* self, void* fence, uint64_t value) {
  probe_log(self, 14, std::format("Signal(fence, {})", value), XBEM_RETURN_ADDRESS());
  if (is_fence(fence)) fence_signal(fence_of(fence), value);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI queue_wait(ProbeObject*, void*, uint64_t) { return S_OK; }
HRESULT XBEM_GUEST_ABI queue_get_timestamp_frequency(ProbeObject*, uint64_t* frequency) {
  if (frequency) *frequency = 100'000'000;  // 100 MHz GPU timestamps
  return S_OK;
}
const ProbeVtable g_queue_vtable = make_vtable({{10, fn(&queue_execute)},
                                                {14, fn(&queue_signal)},
                                                {15, fn(&queue_wait)},
                                                {16, fn(&queue_get_timestamp_frequency)}});

// ------------------------------------------------------------ resource

struct ResourceState {
  uint8_t desc[56] = {};  // D3D12_RESOURCE_DESC
  uint64_t gpu_va = 0;
  uint64_t size = 0;
  void* cpu = nullptr;
  std::mutex mutex;
};

// DXGI format -> element size. Block-compressed formats are 4x4 blocks of
// `bytes`; others are one pixel of `bytes` (`bits` for sub-byte formats).
struct FormatInfo {
  uint32_t bytes;  // per pixel, or per 4x4 block when block is true
  bool block;
};
FormatInfo format_info(uint32_t f) {
  if (f >= 1 && f <= 4) return {16, false};    // R32G32B32A32
  if (f >= 5 && f <= 8) return {12, false};    // R32G32B32
  if (f >= 9 && f <= 22) return {8, false};    // R16G16B16A16, R32G32, R32G8X24
  if (f >= 23 && f <= 47) return {4, false};   // R10G10B10A2 .. R24G8
  if (f >= 48 && f <= 59) return {2, false};   // R8G8, R16
  if (f >= 60 && f <= 66) return {1, false};   // R8, A8, R1 (rounded up)
  if (f >= 67 && f <= 69) return {4, false};   // R9G9B9E5, R8G8_B8G8, G8R8_G8B8
  if (f >= 70 && f <= 72) return {8, true};    // BC1
  if (f >= 73 && f <= 78) return {16, true};   // BC2, BC3
  if (f >= 79 && f <= 81) return {8, true};    // BC4
  if (f >= 82 && f <= 84) return {16, true};   // BC5
  if (f == 85 || f == 86 || f == 115) return {2, false};  // B5G6R5, B5G5R5A1, B4G4R4A4
  if (f >= 87 && f <= 93) return {4, false};   // B8G8R8A8 / X8
  if (f >= 94 && f <= 99) return {16, true};   // BC6H, BC7
  if (f == 100 || f == 101 || f == 107) return {4, false};  // AYUV, Y410, YUY2
  if (f == 102 || f == 108 || f == 109) return {8, false};  // Y416, Y210, Y216
  if (f >= 103 && f <= 106) return {2, false};  // NV12/P010/P016/420_OPAQUE (upper bound)
  if (f >= 110 && f <= 114) return {2, false};
  return {4, false};  // unknown / console-specific: assume 32 bits per pixel
}

// Bytes of a D3D12_RESOURCE_DESC (Dimension, Alignment, Width, Height,
// DepthOrArraySize, MipLevels, Format, SampleDesc, Layout, Flags): every mip of
// every array slice, rows aligned to 256 bytes and subresources to 512, as
// D3D12 lays out linear textures. Tiled console layouts pad more, which the
// title's own 64 KiB rounding absorbs for large surfaces.
uint64_t resource_size(const uint8_t* desc) {
  uint32_t dimension, height, format, samples;
  uint64_t width;
  uint16_t depth_or_array, mips;
  std::memcpy(&dimension, desc, 4);
  std::memcpy(&width, desc + 16, 8);
  std::memcpy(&height, desc + 24, 4);
  std::memcpy(&depth_or_array, desc + 28, 2);
  std::memcpy(&mips, desc + 30, 2);
  std::memcpy(&format, desc + 32, 4);
  std::memcpy(&samples, desc + 36, 4);
  if (dimension == 1) return width;  // BUFFER
  width = std::max<uint64_t>(width, 1);
  height = std::max<uint32_t>(height, 1);
  const uint32_t depth = dimension == 4 ? std::max<uint16_t>(depth_or_array, 1) : 1;
  const uint32_t slices = dimension == 4 ? 1 : std::max<uint16_t>(depth_or_array, 1);
  if (mips == 0) {  // full chain
    uint64_t largest = std::max<uint64_t>({width, height, depth});
    while (largest) {
      ++mips;
      largest >>= 1;
    }
  }
  const FormatInfo fi = format_info(format);
  uint64_t per_slice = 0;
  for (uint32_t m = 0; m < mips; ++m) {
    const uint64_t w = std::max<uint64_t>(width >> m, 1), h = std::max<uint64_t>(height >> m, 1),
                   d = std::max<uint64_t>(depth >> m, 1);
    const uint64_t row = fi.block ? (w + 3) / 4 * fi.bytes : w * fi.bytes;
    const uint64_t rows = fi.block ? (h + 3) / 4 : h;
    per_slice += (((row + 255) & ~255ull) * rows * d + 511) & ~511ull;
  }
  const uint64_t total = per_slice * slices * std::clamp<uint32_t>(samples, 1, 16);
  return std::min<uint64_t>(total, 16ull << 30);
}

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

// 14 CreateDescriptorHeap(desc, riid, ppv). D3D12_DESCRIPTOR_HEAP_DESC is
// {Type, NumDescriptors, Flags, NodeMask}; the console version also carries the
// memory the title set aside for the descriptors (a real title checks that the
// heap starts there), found as a pointer to allocated memory after the PC fields.
HRESULT XBEM_GUEST_ABI device_create_descriptor_heap(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  auto* state = new DescriptorHeapState;
  uint8_t raw[64] = {};
  if (desc) {
    if (!platform::safe_read(reinterpret_cast<uint64_t>(desc), raw, sizeof raw))
      platform::safe_read(reinterpret_cast<uint64_t>(desc), raw, 16);
    std::memcpy(state->desc, raw, 16);
  }
  uint32_t count = 0;
  std::memcpy(&count, state->desc + 4, 4);
  const uint64_t bytes = std::max<uint64_t>(count, 1) * kDescriptorSize;
  uint64_t memory = 0;
  int memory_offset = -1;
  for (int off = 16; off + 8 <= 64 && !memory; off += 8) {
    uint64_t v;
    std::memcpy(&v, raw + off, 8);
    if (is_allocated_memory(v)) {
      memory = v;
      memory_offset = off;
    }
  }
  if (!memory)
    memory = reinterpret_cast<uint64_t>(virtual_alloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0));
  state->cpu = memory;
  state->gpu = memory;  // unified memory: shader-visible descriptors are read from the same bytes
  std::string dump;
  for (int i = 0; i < 64; i += 8) {
    uint64_t v;
    std::memcpy(&v, raw + i, 8);
    dump += std::format(" {:016x}", v);
  }
  probe_log(self, 14,
            std::format("CreateDescriptorHeap({} descriptors, type {}, flags {:#x}) at {:#x} ({}) desc:{}", count,
                        raw[0], raw[8], memory,
                        memory_offset >= 0 ? std::format("title memory from desc+{}", memory_offset) : "own memory",
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
  probe_log(r.self, 25,
            std::format("GetResourceAllocationInfo({} descs{}) -> size {:#x}, alignment {:#x}", count, what,
                        result[0], result[1]),
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
    {13, fn(&device_check_feature_support)},
    {14, fn(&device_create_descriptor_heap)},
    {15, fn(&device_get_descriptor_increment)},
    {27, fn(&device_create_committed_resource)},
    {28, fn(&device_create_heap)},
    {29, fn(&device_create_placed_resource)},
    {30, fn(&device_create_reserved_resource)},
    {36, fn(&device_create_fence)},
    {37, fn(&device_get_removed_reason)},
    {16, fn(&device_create_root_signature)},
    {25, fn(&device_get_resource_allocation_info)},
    {26, fn(&device_get_custom_heap_properties)},
    {48, fn(&device_create_command_queue_x)},
    {62, fn(&device_create_compute_pso_x)},
    {67, fn(&device_deserialize_compute_pso_x)},
    {76, fn(&device_create_texture_x<76>)},
    // 127: CreateCommittedResource3 (the title's back buffers, heap flag ALLOW_DISPLAY).
    {127, fn(&device_create_texture_x<127>)},
    {78, fn(&device_set_frame_interval_x)},
    {79, fn(&device_schedule_frame_event_x)},
    {80, fn(&device_wait_frame_event_x)},
    {133, fn(&device_create_placed_resource_x)},
});

}  // namespace

void* create_headless_d3d12_device(const void* iid) {
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
