#pragma once

// D3D12 HLE internals shared between the device (d3d12_device.cpp) and the
// Vulkan renderer (d3d12_render.cpp).

#include <cstdint>
#include <string>
#include <mutex>
#include <vector>

#include "gpu/root_signature.h"
#include "hle/d3d12_device.h"
#include "hle/probe_com.h"

namespace xbem::hle::d3d12 {

struct ResourceState {
  uint8_t desc[56] = {};  // D3D12_RESOURCE_DESC
  uint64_t gpu_va = 0;
  uint64_t size = 0;
  void* cpu = nullptr;
  // RDNA2 swizzle mode of the memory (ADDR_SW_*; 0 = linear): textures the
  // title streams in are stored swizzled - see gpu/swizzle.h.
  uint32_t swizzle = 0;
  std::mutex mutex;
};

struct RootSignatureState {
  gpu::RootSignature rs;
};

constexpr uint32_t kDescriptorSize = 32;        // CBV/SRV/UAV, RTV, DSV
constexpr uint32_t kSamplerDescriptorSize = 16;  // an S#: the title's shaders index sampler heaps by 16

// The emulator's own descriptor encoding for views of resources (see
// d3d12_device.cpp): written into the 32 bytes of descriptor memory.
struct XbemDescriptor {
  uint32_t magic;  // "XBDS"
  uint8_t kind;    // DescriptorKind
  uint8_t pad[3];
  uint64_t object;  // the resource (a probe object), or 0
  uint64_t a, b;    // views: the view description's first bytes
};
static_assert(sizeof(XbemDescriptor) == 32);
constexpr uint32_t kDescriptorMagic = 0x53444258;
enum DescriptorKind : uint8_t { kCbv = 1, kSrv, kUav, kRtv, kDsv, kSampler };

// A command-list call as recorded (d3d12_device.cpp): the slot, its
// arguments, a decoded text for captures and, while the renderer runs, a
// snapshot of the structures its pointer arguments point to.
struct RecordedCommand {
  int slot;
  uint64_t args[6];
  std::string text;
  std::vector<uint8_t> blob;
};

// Descriptor heaps: start address (CPU = GPU) and D3D12_DESCRIPTOR_HEAP_TYPE.
bool descriptor_heap_of(uint64_t p, uint64_t& start, uint32_t& type);

// True for textures created as display surfaces (heap flag ALLOW_DISPLAY).
bool is_display_resource(const ResourceState* r);

// The Vulkan renderer (d3d12_render.cpp).
bool render_start();
bool render_active();
void render_execute(const std::vector<RecordedCommand>& commands);
void render_present();

// Typed objects (null when `p` is not one).
ResourceState* resource_of(uint64_t p);
GraphicsPipeline* graphics_pipeline_of(uint64_t p);
const gpu::RootSignature* root_signature_of(uint64_t p);
bool is_probe_object(uint64_t p);

}  // namespace xbem::hle::d3d12
