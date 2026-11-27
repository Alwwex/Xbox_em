// The Vulkan renderer: executes the command lists a title submits to its
// D3D12.X queue on the host GPU.
//
// Commands are recorded by the command-list objects (d3d12_device.cpp) with
// snapshots of their pointer arguments; ExecuteCommandListsX hands them here,
// where they run synchronously on the submitting thread: state tracking, then
// per draw the pipeline's shaders are translated (gpu/translate: RDNA2 ->
// GLSL -> SPIR-V through glslangValidator, cached), their scalar part runs on
// the CPU against guest memory, and the draw is recorded into a Vulkan
// command buffer. Guest buffers a draw reads are copied into an upload ring
// and reached through buffer device addresses; textures are uploaded once
// into bindless arrays; render targets live on the GPU. At PresentX the
// display texture is read back and handed to the run window.
//
// Scope (first screens of GTA VI): graphics pipelines from device slots
// 57/55, triangle lists and RECTLISTs (as a geometry shader), colour targets
// without depth, alpha blending. Compute work and pipelines that do not
// translate are skipped (counted in the log).

#include "hle/d3d12_internal.h"
#include "hle/discovery.h"
#include "hle/heap.h"

#if defined(XBEM_HAVE_VULKAN)

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "common/log.h"
#include "gpu/guest_memory_layout.h"
#include "gpu/swizzle.h"
#include "gpu/translate.h"
#include "gpu/vk/guest_memory.h"
#include "gpu/vk/vk_core.h"
#include "hle/hle_util.h"
#include "hle/texture_layout.h"
#include "platform/memory.h"
#include "platform/window.h"
#include "runtime/process.h"

namespace xbem::hle::d3d12 {
namespace {

namespace fs = std::filesystem;
using gpu::vk::Buffer;
using gpu::vk::Core;
using gpu::vk::Image;

constexpr uint32_t kMaxTextures = 4096, kSamplers = 8, kMaxArrayTextures = 1024, kMaxVolumeTextures = 256;
// Storage images (UAVs that compute shaders write): 2D, 2D array, 3D.
constexpr uint32_t kMaxStorage = 2048, kMaxStorageArrays = 512, kMaxStorageVolumes = 256;
constexpr VkDeviceSize kRingSize = 256ull << 20;
constexpr VkShaderStageFlags kGraphicsStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

uint64_t fnv1a(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// DXGI -> Vulkan formats for render targets and textures.
VkFormat vk_format(uint32_t dxgi) {
  switch (dxgi) {
    case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 10: case 9: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 11: return VK_FORMAT_R16G16B16A16_UNORM;
    case 24: case 23: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 26: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 27: case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    case 29: return VK_FORMAT_R8G8B8A8_SRGB;
    case 34: return VK_FORMAT_R16G16_SFLOAT;
    case 41: return VK_FORMAT_R32_SFLOAT;
    case 49: return VK_FORMAT_R8G8_UNORM;
    case 54: return VK_FORMAT_R16_SFLOAT;
    case 56: return VK_FORMAT_R16_UNORM;
    case 61: case 60: case 65: return VK_FORMAT_R8_UNORM;
    case 71: case 70: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 74: case 73: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 77: case 76: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 80: case 79: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 83: case 82: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 87: case 88: case 90: case 91: return VK_FORMAT_B8G8R8A8_UNORM;
    case 93: return VK_FORMAT_B8G8R8A8_SRGB;
    case 95: case 94: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 98: case 97: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 99: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
  }
}

struct ShaderEntry {
  gpu::TranslatedShader shader;
  VkShaderModule module = VK_NULL_HANDLE;
  std::string error;
  // The program itself on the GPU (compute): s_getpc_b64 points into it, for
  // the constant tables that follow the code.
  Buffer code;
  std::vector<uint8_t> code_bytes;
  // Compute shaders are prepared on a worker: 0/1 being prepared, 2 ready
  // (pipeline set), 3 failed.
  std::atomic<int> state{0};
  VkPipeline pipeline = VK_NULL_HANDLE;
};

// Microseconds since `t0` added to a counter.
inline void add_us(std::atomic<uint64_t>& counter, std::chrono::steady_clock::time_point t0) {
  counter += static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

// Worker threads for shader preparation (detached: they live as long as the
// process).
class CompilePool {
 public:
  explicit CompilePool(unsigned workers) {
    for (unsigned i = 0; i < workers; ++i)
      std::thread([this] {
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
          }
          job();
        }
      }).detach();
  }
  void run(std::function<void()> job) {
    {
      std::lock_guard lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
};

CompilePool& compile_pool() {
  static CompilePool* pool = new CompilePool(3);
  return *pool;
}

// Graphics pipelines have their own workers: compute programs can take the
// shared ones for minutes each, and the frame's draws (the final composite
// among them) waited behind them.
CompilePool& graphics_compile_pool() {
  static CompilePool* pool = new CompilePool(2);
  return *pool;
}

struct PipelineKey {
  uint64_t vs, ps, blend;
  uint64_t format;  // the bound targets' formats (8 bits each), slot 0 lowest
  uint32_t rect;
  bool operator<(const PipelineKey& o) const {
    return std::tie(vs, ps, blend, format, rect) < std::tie(o.vs, o.ps, o.blend, o.format, o.rect);
  }
};

// One render target or texture on the GPU, for a guest resource.
struct GpuImage {
  Image image;
  uint32_t texture_index = 0;  // in the bindless 2D array, 0 = not yet (or a 3D image)
  uint32_t array_index = 0;    // in the bindless 2D-array array (2D images, all layers)
  uint32_t volume_index = 0;   // in the bindless 3D array (3D images)
  uint32_t storage_index = 0, storage_array_index = 0, storage_volume_index = 0;  // storage bindings (0 = none)
  bool rendered = false;       // the GPU holds its contents
  uint64_t last_render = 0;
  uint64_t uploaded_frame = 0;  // textures from guest memory: refreshed once per frame they are used
  uint64_t content_hash = 0;
  uint64_t last_used = 0;   // the present it was last looked up at (eviction)
  uint64_t bytes = 0;       // estimated memory
};

// Bindless array indices, recycled when images are evicted (index 0 is the
// placeholder).
struct IndexPool {
  uint32_t next = 1, max = 0;
  std::vector<uint32_t> free;
  uint32_t take() {
    if (!free.empty()) {
      const uint32_t i = free.back();
      free.pop_back();
      return i;
    }
    return next < max ? next++ : 0;
  }
  void give(uint32_t i) {
    if (i) free.push_back(i);
  }
};

class Renderer {
 public:
  bool start(std::string& info) {
    if (!core_.init(info)) return false;
    const auto& f = core_.fn();
    VkDevice dev = core_.device();
    ring_ = core_.create_buffer(kRingSize,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                true);
    if (!ring_.mapped) {
      info = "cannot allocate the upload ring";
      return false;
    }
    {
      // The GPU's copy of guest memory (XBEM_GPU_MEMORY_MB, default 2048).
      const char* mb = std::getenv("XBEM_GPU_MEMORY_MB");
      const uint64_t budget = (mb ? std::strtoull(mb, nullptr, 10) : 2048ull) << 20;
      std::string why;
      if (!memory_.init(core_, budget, why)) XBEM_WARN("gpu", "{} - guest buffers are copied per submission", why);
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = core_.queue_family();
    f.vkCreateCommandPool(dev, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    f.vkAllocateCommandBuffers(dev, &cai, &cmd_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    f.vkCreateFence(dev, &fci, nullptr, &fence_);

    // Bindless textures and samplers.
    // Bindings: 0 2D textures, 1 samplers, 2 2D-array textures, 3 3D textures,
    // 4-6 storage images (2D, 2D array, 3D) for compute shaders.
    VkDescriptorSetLayoutBinding b[7]{};
    const uint32_t counts[7] = {kMaxTextures, kSamplers, kMaxArrayTextures, kMaxVolumeTextures,
                                kMaxStorage, kMaxStorageArrays, kMaxStorageVolumes};
    for (uint32_t i = 0; i < 7; ++i) {
      b[i].binding = i;
      b[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_SAMPLER
                            : i >= 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b[i].descriptorCount = counts[i];
      b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;  // (pixel shaders write UAVs too)
    }
    const VkDescriptorBindingFlags bound = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorBindingFlags bflags[7] = {bound, 0, bound, bound, bound, bound, bound};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 7;
    bfi.pBindingFlags = bflags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &bfi;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 7;
    lci.pBindings = b;
    if (f.vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set_layout_) != VK_SUCCESS) {
      info = "descriptor set layout failed";
      return false;
    }
    // Per-draw words of the vertex and pixel stages, then the guest memory table.
    VkPushConstantRange push[3] = {{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 8},
                                   {VK_SHADER_STAGE_FRAGMENT_BIT, 8, 8},
                                   {kGraphicsStages, 16, 8}};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 3;
    plci.pPushConstantRanges = push;
    f.vkCreatePipelineLayout(dev, &plci, nullptr, &layout_);
    VkPushConstantRange cpush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &cpush;
    f.vkCreatePipelineLayout(dev, &plci, nullptr, &compute_layout_);
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures + kMaxArrayTextures + kMaxVolumeTextures},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplers},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxStorage + kMaxStorageArrays + kMaxStorageVolumes}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    f.vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool_);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout_;
    if (f.vkAllocateDescriptorSets(dev, &dsai, &set_) != VK_SUCCESS) {
      info = "descriptor set allocation failed";
      return false;
    }
    // Samplers: index = linear (1) | wrap (2) | mirror (4).
    for (uint32_t i = 0; i < kSamplers; ++i) {
      VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sci.magFilter = sci.minFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      const VkSamplerAddressMode mode = (i & 4)   ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                        : (i & 2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                  : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sci.addressModeU = sci.addressModeV = sci.addressModeW = mode;
      sci.maxLod = 16.0f;
      f.vkCreateSampler(dev, &sci, nullptr, &samplers_[i]);
      VkDescriptorImageInfo ii{samplers_[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set_;
      w.dstBinding = 1;
      w.dstArrayElement = i;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      w.pImageInfo = &ii;
      f.vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    // Texture 0: opaque white, for textures the renderer cannot resolve.
    white_ = core_.create_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    begin();
    transition(white_.image, VK_IMAGE_ASPECT_COLOR_BIT);
    const uint32_t px = 0xffffffffu;
    const VkDeviceSize at = ring_alloc(4, 4);
    std::memcpy(static_cast<uint8_t*>(ring_.mapped) + at, &px, 4);
    copy_to_image(white_, at, 1, 1);
    submit();
    bind_texture(0, white_.view);
    bind_texture(0, white_.array_view, 2);
    white_volume_ = core_.create_image(1, 1, 1, 1, true, VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    begin();
    transition(white_volume_.image, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkDeviceSize at3 = ring_alloc(4, 4);
    std::memcpy(static_cast<uint8_t*>(ring_.mapped) + at3, &px, 4);
    copy_to_image(white_volume_, at3, 1, 1);
    submit();
    bind_texture(0, white_volume_.view, 3);
    const uint32_t pool_max[6] = {kMaxTextures, kMaxArrayTextures, kMaxVolumeTextures,
                                  kMaxStorage, kMaxStorageArrays, kMaxStorageVolumes};
    for (int i = 0; i < 6; ++i) pools_[i].max = pool_max[i];
    // Storage index 0: scratch images that unresolved UAV writes land in.
    scratch_storage_ = core_.create_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
    scratch_storage_volume_ = core_.create_image(1, 1, 1, 1, true, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
    begin();
    transition(scratch_storage_.image, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(scratch_storage_volume_.image, VK_IMAGE_ASPECT_COLOR_BIT);
    submit();
    bind_storage(0, scratch_storage_.view, 4);
    bind_storage(0, scratch_storage_.array_view, 5);
    bind_storage(0, scratch_storage_volume_.view, 6);


    const char* home = std::getenv("HOME");
    cache_dir_ = fs::path(home ? home : "/tmp") / ".cache" / "xbem" / "spirv";
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
    // Pipelines (the driver's SPIR-V -> DXIL work) persist between runs.
    {
      std::ifstream in(cache_dir_ / "pipelines.bin", std::ios::binary);
      std::vector<char> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
      pci.initialDataSize = blob.size();
      pci.pInitialData = blob.empty() ? nullptr : blob.data();
      if (core_.fn().vkCreatePipelineCache(core_.device(), &pci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        pci.initialDataSize = 0, pci.pInitialData = nullptr;
        core_.fn().vkCreatePipelineCache(core_.device(), &pci, nullptr, &pipeline_cache_);
      }
      XBEM_INFO("gpu", "pipeline cache: {} KiB from {}", blob.size() >> 10, (cache_dir_ / "pipelines.bin").string());
    }
    info = core_.device_name();
    return true;
  }

  void execute(const std::vector<RecordedCommand>& commands) {
    std::lock_guard lock(mutex_);
    begin();
    resolve_descriptor_misses();
    for (const RecordedCommand& c : commands) command(c);
    end_rendering();
    submit();
  }

  void present() {
    std::lock_guard lock(mutex_);
    if (const uint64_t n = presents_ + 1; n >= 64 && (n & (n - 1)) == 0 && memory_.ready()) {
      const auto& m = memory_.stats();
      XBEM_INFO("gpu", "guest memory copy at present {}: {} pages resident ({} MiB), {} uploaded, {} refreshed, "
                "{} written back ({} merged word by word), {} evicted, {} unmapped, {} misses ({} dropped); {} image descriptors looked up on the "
                "GPU ({} misses, {} table resets)", n, m.resident, m.resident >> 4, m.uploaded, m.refreshed,
                m.written_back, m.merged, m.evicted, m.unmapped, m.misses, m.misses_dropped, descriptors_, descriptor_misses_,
                descriptor_table_resets_);
    }
    if (!display_) return;
    GpuImage& g = *display_;
    const uint32_t w = g.image.width, h = g.image.height;
    Buffer& rb = readback_[readback_index_ ^= 1];
    if (rb.size < VkDeviceSize{w} * h * 4) {
      rb = core_.create_buffer(VkDeviceSize{w} * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      if (!rb.mapped) return;
    }
    begin();
    // The readback is RGBA8: other display formats (10-bit, half float -
    // GTA VI's scene) are converted by a blit first; copying them as they
    // are wrote past the 4-byte-per-pixel buffer.
    VkImage source = g.image.image;
    if (g.image.format != VK_FORMAT_R8G8B8A8_UNORM && g.image.format != VK_FORMAT_R8G8B8A8_SRGB) {
      if (present_rgba_.width != w || present_rgba_.height != h) {
        const auto& f = core_.fn();
        if (present_rgba_.view) f.vkDestroyImageView(core_.device(), present_rgba_.view, nullptr);
        if (present_rgba_.image) f.vkDestroyImage(core_.device(), present_rgba_.image, nullptr);
        if (present_rgba_.memory) f.vkFreeMemory(core_.device(), present_rgba_.memory, nullptr);
        present_rgba_ = core_.create_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!present_rgba_.image) return;
        transition(present_rgba_.image, VK_IMAGE_ASPECT_COLOR_BIT);
      }
      VkImageBlit blit{};
      blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = blit.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
      core_.fn().vkCmdBlitImage(cmd_, g.image.image, VK_IMAGE_LAYOUT_GENERAL, present_rgba_.image,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
      full_barrier();
      source = present_rgba_.image;
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    core_.fn().vkCmdCopyImageToBuffer(cmd_, source, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &region);
    submit();
    platform::window_set_surface(rb.mapped, w, h, uint64_t{w} * 4, display_format_);
    ++presents_;
    evict_images();
    if (presents_ == 1 || (presents_ & (presents_ - 1)) == 0) {
      const auto* px = static_cast<const uint32_t*>(rb.mapped);
      uint64_t lit = 0;
      uint32_t sample = 0;
      for (uint64_t i = 0; i < uint64_t{w} * h; i += 97)
        if (px[i] & 0x00ffffff) ++lit, sample = px[i];
      XBEM_INFO("gpu", "present #{}: {}x{} display image ({} of {} sampled pixels lit, e.g. {:#010x}); {} draws "
                "rendered, {} skipped; {} dispatches run, {} skipped so far{}", presents_, w, h, lit,
                uint64_t{w} * h / 97, sample, draws_, skipped_, dispatches_, dispatches_skipped_, top_skip_reasons());
      if (std::getenv("XBEM_GPU_STATS")) image_stats();
    }
  }

 private:
  // ---------------------------------------------------------- submission

  void begin() {
    if (recording_) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    core_.fn().vkBeginCommandBuffer(cmd_, &bi);
    recording_ = true;
    memory_.begin_submission();
  }

  void submit() {
    if (!recording_) return;
    const auto& f = core_.fn();
    f.vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    for (const auto& [a, b] : gpu_output_) memory_.snapshot(a, b - a);
    const auto started = std::chrono::steady_clock::now();
    VkResult r = f.vkQueueSubmit(core_.queue(), 1, &si, fence_);
    if (r == VK_SUCCESS) r = f.vkWaitForFences(core_.device(), 1, &fence_, VK_TRUE, UINT64_MAX);
    f.vkResetFences(core_.device(), 1, &fence_);
    // A submission that takes long or loses the device (a shader that hangs
    // the GPU): name the compute shaders it ran.
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (r != VK_SUCCESS || ms > 250) {
      static int logged = 0;
      if (logged++ < 80) {
        std::string list;
        for (uint64_t h : submitted_cs_) list += std::format(" {:016x}", h);
        std::string ps;
        for (const auto& [h, wave] : submitted_ps_) ps += std::format(" {:016x}{}", h, wave ? "(wave)" : "");
        uint32_t st[4];
        memory_.take_block_stats(st);
        uint64_t st_hash = 0;
        if (st[3]) {
          std::lock_guard lock(tags_mutex_);
          if (auto it = shader_tags_.find(st[3]); it != shader_tags_.end()) st_hash = it->second;
        }
        XBEM_WARN("gpu", "submission {} after {} ms ({} draws, {} dispatches); compute shaders:{}; pixel shaders:{}; "
                  "last draw: {}{}",
                  r == VK_SUCCESS ? "done" : std::format("FAILED ({})", static_cast<int>(r)), ms, submitted_draws_,
                  submitted_dispatches_, list.empty() ? " none" : list, ps.empty() ? " none" : ps, last_draw_,
                  st[2] ? std::format("; blocks per invocation: max {} ({:016x}), mean {:.1f} over {}", st[0], st_hash,
                                      double(st[1]) / st[2], st[2])
                        : std::string());
      }
    }
    // A compute shader ended by its loop guard (a loop that would not end).
    uint32_t guard_block = 0;
    if (const uint32_t hit = memory_.take_guard_tag(&guard_block)) {
      {
        uint64_t hash = 0;
        {
          std::lock_guard lock(tags_mutex_);
          if (auto it = shader_tags_.find(hit); it != shader_tags_.end()) hash = it->second;
        }
        static int logged = 0;
        if (logged++ < 16)
          XBEM_WARN("gpu", "compute shader {:016x} (tag {:#x}) ran into its loop guard in block {} - a loop that does "
                    "not end", hash, hit, guard_block);
      }
    }
    submitted_cs_.clear();
    submitted_ps_.clear();
    submitted_draws_ = submitted_dispatches_ = 0;
    wave_draws_ = 0;
    gpu_output_.clear();
    // Every ~10 s: how much of the time went into waiting for the GPU.
    gpu_wait_us_ += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    ++gpu_submissions_;
    const auto now = std::chrono::steady_clock::now();
    if (stats_since_ == std::chrono::steady_clock::time_point{}) stats_since_ = now;
    if (now - stats_since_ >= std::chrono::seconds(10)) {
      const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats_since_).count();
      XBEM_INFO("gpu", "last {} ms: {} submissions ({} to read GPU results on the CPU), {} ms waiting for the GPU; "
                "shaders so far: {} ms translating, {} glslang runs {} ms, {} pipelines {} ms ({} graphics {} ms, on "
                "the render thread)",
                span, gpu_submissions_, flushes_for_gpu_output_, gpu_wait_us_ / 1000, translate_us_.load() / 1000,
                glslang_runs_.load(), glslang_us_.load() / 1000, pipelines_created_.load(), pipeline_us_.load() / 1000,
                graphics_pipelines_.load(), graphics_pipeline_us_.load() / 1000);
      save_pipeline_cache();
      flushes_for_gpu_output_ = 0;
      gpu_wait_us_ = gpu_submissions_ = 0;
      stats_since_ = now;
    }
    recording_ = false;
    memory_.end_submission();
    ring_offset_ = 0;
    uploads_.clear();
  }

  // The pipeline cache to disk when pipelines were made since the last save.
  void save_pipeline_cache() {
    const uint64_t made = pipelines_created_.load();
    if (!pipeline_cache_ || made == pipelines_saved_.load()) return;
    pipelines_saved_ = made;
    const auto& f = core_.fn();
    size_t size = 0;
    if (f.vkGetPipelineCacheData(core_.device(), pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<char> blob(size);
    if (f.vkGetPipelineCacheData(core_.device(), pipeline_cache_, &size, blob.data()) != VK_SUCCESS) return;
    const fs::path tmp = cache_dir_ / "pipelines.bin.tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out.write(blob.data(), static_cast<std::streamsize>(size));
      if (!out) return;
    }
    std::error_code ec;
    fs::rename(tmp, cache_dir_ / "pipelines.bin", ec);
  }

  VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align) {
    VkDeviceSize at = (ring_offset_ + align - 1) & ~(align - 1);
    if (at + size > kRingSize) {
      // Full: run what is recorded and start over.
      const bool rendering = rendering_;
      end_rendering();
      submit();
      begin();
      (void)rendering;
      at = 0;
    }
    ring_offset_ = at + size;
    return at;
  }

  // Image descriptors shaders looked up on the GPU and did not find (see
  // desc_lookup in gpu/guest_memory_layout.h): resolved like a slot and
  // added to the table, so later submissions find them.
  void resolve_descriptor_misses() {
    if (!memory_.ready()) return;
    using namespace gpu::mem;
    uint32_t* t = memory_.table_words();
    const uint32_t n = std::min(t[kDescMissWord], kMaxDescMisses);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t* m = t + kDescMissWord + 1 + i * kDescMissWords;
      const uint32_t kind = m[0];
      if (kind != kDescTexture && kind != kDescImage) continue;
      uint32_t out[4] = {0, 0x00010001, 0, 0};
      Tokens tokens;
      tokens.windows = &windows_;
      resolve(kind == kDescTexture ? gpu::SlotKind::Texture : gpu::SlotKind::Image, m + 1, out, tokens);
      insert_descriptor(kind, m + 1, out);
    }
    descriptor_misses_ += n;
    t[kDescMissWord] = 0;
    std::memset(t + kDescMissedWord, 0, kDescMissedBits / 8);
  }

  void insert_descriptor(uint32_t kind, const uint32_t* words, const uint32_t* value) {
    using namespace gpu::mem;
    uint32_t* t = memory_.table_words();
    const uint32_t h = desc_hash(kind, words);
    for (int attempt = 0; attempt < 2; ++attempt) {
      for (uint32_t i = 0; i < 8; ++i) {
        uint32_t* e = t + kDescWord + ((h + i) & (kDescEntries - 1)) * kDescEntryWords;
        const bool same = e[0] == kind && std::memcmp(e + 1, words, 32) == 0;
        if (e[0] && !same) continue;
        if (!e[0]) ++descriptors_;
        std::memcpy(e + 1, words, 32);
        std::memcpy(e + 9, value, 16);
        e[0] = kind;
        return;
      }
      // A full chain: start the table over (entries come back as misses).
      std::memset(t + kDescWord, 0, uint64_t{kDescEntries} * kDescEntryWords * 4);
      descriptors_ = 0;
      ++descriptor_table_resets_;
    }
  }

  void transition(VkImage image, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    core_.fn().vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                    nullptr, 0, nullptr, 1, &b);
  }

  // Everything before is visible to everything after (simple and safe).
  void full_barrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    core_.fn().vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                    &mb, 0, nullptr, 0, nullptr);
  }

  // One layer (or the whole volume of a 3D image) from the ring.
  void copy_to_image(const Image& img, VkDeviceSize at, uint32_t row_texels, uint32_t rows, uint32_t layer = 0) {
    VkBufferImageCopy region{};
    region.bufferOffset = at;
    region.bufferRowLength = row_texels;
    region.bufferImageHeight = rows;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    region.imageExtent = {img.width, img.height, img.depth};
    core_.fn().vkCmdCopyBufferToImage(cmd_, ring_.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  }

  void bind_texture(uint32_t index, VkImageView view, uint32_t binding = 0) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    core_.fn().vkUpdateDescriptorSets(core_.device(), 1, &w, 0, nullptr);
  }

  void bind_storage(uint32_t index, VkImageView view, uint32_t binding) {
    if (!view) return;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    core_.fn().vkUpdateDescriptorSets(core_.device(), 1, &w, 0, nullptr);
  }

  // ---------------------------------------------------------- resources

  // The GPU image of a guest resource (render target or texture).
  GpuImage* image_of(ResourceState* r, bool for_render) {
    if (!r) return nullptr;
    auto it = images_.find(r);
    if (it != images_.end()) {
      GpuImage* g = it->second.get();
      g->last_used = presents_;
      // Guest-written textures change (glyph caches, streamed data): refresh
      // once per frame when the memory changed.
      if (!for_render && !g->rendered && g->uploaded_frame != presents_ && r->gpu_va) {
        g->uploaded_frame = presents_;
        uint32_t format;
        std::memcpy(&format, r->desc + 32, 4);
        const VkFormat vf = vk_format(format);
        upload_texture(*g, r, format, vf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vf <= VK_FORMAT_BC7_SRGB_BLOCK);
      }
      return g;
    }
    uint32_t dimension, height, format;
    uint64_t width;
    uint16_t depth_or_array;
    std::memcpy(&dimension, r->desc, 4);
    std::memcpy(&width, r->desc + 16, 8);
    std::memcpy(&height, r->desc + 24, 4);
    std::memcpy(&depth_or_array, r->desc + 28, 2);
    std::memcpy(&format, r->desc + 32, 4);
    const VkFormat vf = vk_format(format);
    if (dimension == 2) height = 1;  // 1D: a 2D image of one row
    if ((dimension != 2 && dimension != 3 && dimension != 4) || vf == VK_FORMAT_UNDEFINED || !width || !height || width > 16384 ||
        height > 16384 || (for_render && dimension == 4)) {
      note(std::format("resource format {} dimension {} not supported", format, dimension));
      return nullptr;
    }
    const bool volume = dimension == 4;
    const uint32_t slices = std::clamp<uint32_t>(depth_or_array, 1, volume ? 2048 : 2048);
    const bool block = vf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vf <= VK_FORMAT_BC7_SRGB_BLOCK;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!block) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const bool storage = !block && format != 65 && core_.format_supported(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    if (storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    auto g = std::make_unique<GpuImage>();
    if (volume || slices > 1) usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    g->image = volume || slices > 1
                   ? core_.create_image(static_cast<uint32_t>(width), height, slices, slices, volume, vf, usage)
                   : core_.create_image(static_cast<uint32_t>(width), height, vf, usage);
    if (!g->image.image) {
      note(std::format("cannot create a {}x{} image of format {}", width, height, format));
      return nullptr;
    }
    // A8: sample alpha from the red channel.
    if (format == 65) {
      VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vci.image = g->image.image;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = vf;
      vci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                        VK_COMPONENT_SWIZZLE_R};
      vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      core_.fn().vkCreateImageView(core_.device(), &vci, nullptr, &g->image.view);
      if (!volume) {  // the array view too (shaders sample A8 glyphs as arrays)
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.subresourceRange.layerCount = g->image.layers;
        core_.fn().vkCreateImageView(core_.device(), &vci, nullptr, &g->image.array_view);
      }
    }
    transition(g->image.image, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!for_render && r->gpu_va) {
      upload_texture(*g, r, format, block);
      g->uploaded_frame = presents_;
    }
    if (!volume && (g->texture_index = pools_[0].take())) bind_texture(g->texture_index, g->image.view);
    if (!volume && g->image.array_view && (g->array_index = pools_[1].take()))
      bind_texture(g->array_index, g->image.array_view, 2);
    if (volume && (g->volume_index = pools_[2].take())) bind_texture(g->volume_index, g->image.view, 3);
    if (storage) {
      if (!volume && (g->storage_index = pools_[3].take())) bind_storage(g->storage_index, g->image.view, 4);
      if (!volume && g->image.array_view && (g->storage_array_index = pools_[4].take()))
        bind_storage(g->storage_array_index, g->image.array_view, 5);
      if (volume && (g->storage_volume_index = pools_[5].take())) bind_storage(g->storage_volume_index, g->image.view, 6);
    }
    g->last_used = presents_;
    g->bytes = uint64_t{g->image.width} * g->image.height * std::max<uint32_t>(g->image.layers, 1) *
               std::max<uint32_t>(g->image.depth, 1) * 8;  // (rough: up to 8 bytes per texel)
    image_bytes_ += g->bytes;
    GpuImage* raw = g.get();
    images_[r] = std::move(g);
    return raw;
  }

  // Mip 0 of a texture from guest memory (the emulator's linear layout).
  void upload_texture(GpuImage& g, ResourceState* r, uint32_t format, bool block) {
    const TextureLayout l = linear_layout(r->desc);
    if (l.mip.empty()) return;
    if (r->swizzle) {
      upload_swizzled(g, r, l, block);
      return;
    }
    const MipLayout& m = l.mip[0];
    // Mip 0 of every array slice (each slice_stride apart), or all depth
    // slices of a 3D image's mip 0 (contiguous, slice_2d apart).
    const uint32_t layers = g.image.volume ? 1 : g.image.layers;
    const VkDeviceSize one = g.image.volume ? m.slice_2d * g.image.depth : uint64_t{m.pitch_bytes} * m.height_elements;
    const VkDeviceSize size = one * layers;
    if (size > (64u << 20)) return;
    scratch_.resize(size);
    for (uint32_t k = 0; k < layers; ++k)
      if (!platform::safe_read(r->gpu_va + k * l.slice_stride, scratch_.data() + k * one, one)) return;
    const uint64_t hash = fnv1a(scratch_.data(), size);
    if (hash == g.content_hash) return;
    g.content_hash = hash;
    const VkDeviceSize at = ring_alloc(size, 16);
    std::memcpy(static_cast<uint8_t*>(ring_.mapped) + at, scratch_.data(), size);
    const uint32_t row_texels = block ? m.pitch_bytes / l.element.bytes * 4 : m.pitch_bytes / l.element.bytes;
    const uint32_t rows = block ? m.height_elements * 4 : m.height_elements;
    for (uint32_t k = 0; k < layers; ++k) copy_to_image(g.image, at + k * one, row_texels, rows, k);
    full_barrier();
    (void)format;
  }

  // A texture in an RDNA2 swizzled layout: mip 0 of every slice gathered
  // into rows through addrlib's element offsets, then uploaded.
  void upload_swizzled(GpuImage& g, ResourceState* r, const TextureLayout& l, bool block) {
    const MipLayout& m = l.mip[0];
    const uint32_t layers = g.image.volume ? 1 : g.image.layers;
    const auto table = gpu::swizzle_table(r->swizzle, l.element.bytes, m.width_elements, m.height_elements,
                                          g.image.volume ? g.image.depth : layers, g.image.volume, l.mips);
    if (!table || table->size > (256u << 20)) {
      note(std::format("swizzle mode {} of a {}x{} texture (format {}) not supported", r->swizzle, m.width, m.height,
                       l.format));
      return;
    }
    const uint64_t read = std::min<uint64_t>(table->size, table->slice_size * layers);
    scratch_.resize(read ? read : table->size);
    // Streamed textures map only their resident mips: unreadable pages read as zeros.
    if (!platform::safe_read(r->gpu_va, scratch_.data(), scratch_.size())) {
      for (uint64_t off = 0; off < scratch_.size();) {
        const uint64_t n = std::min<uint64_t>(4096 - ((r->gpu_va + off) & 4095), scratch_.size() - off);
        if (!platform::safe_read(r->gpu_va + off, scratch_.data() + off, n)) std::memset(scratch_.data() + off, 0, n);
        off += n;
      }
    }
    const uint64_t hash = fnv1a(scratch_.data(), scratch_.size());
    if (hash == g.content_hash) return;
    g.content_hash = hash;
    if (std::getenv("XBEM_DUMP_TEXTURES")) {
      uint64_t nonzero = 0;
      for (uint8_t b : scratch_) nonzero += b != 0;
      XBEM_INFO("gpu", "swizzled texture {}x{} format {} mode {} at {:#x}: {:#x} bytes, {:#x} non-zero", m.width,
                m.height, l.format, r->swizzle, r->gpu_va, scratch_.size(), nonzero);
    }
    const uint32_t e = l.element.bytes;
    const VkDeviceSize one = uint64_t{m.width_elements} * m.height_elements * e;
    if (one * layers > (64u << 20)) return;
    const VkDeviceSize at = ring_alloc(one * layers, 16);
    auto* dst = static_cast<uint8_t*>(ring_.mapped) + at;
    for (uint32_t k = 0; k < layers; ++k) {
      const uint64_t base = k * table->slice_size;
      for (size_t i = 0; i < table->offset.size(); ++i) {
        const uint64_t src = base + table->offset[i];
        if (src + e <= scratch_.size()) std::memcpy(dst + k * one + i * e, scratch_.data() + src, e);
      }
    }
    const uint32_t row_texels = block ? m.width_elements * 4 : m.width_elements;
    const uint32_t rows = block ? m.height_elements * 4 : m.height_elements;
    for (uint32_t k = 0; k < layers; ++k) copy_to_image(g.image, at + k * one, row_texels, rows, k);
    full_barrier();
    if (const char* dir = std::getenv("XBEM_DUMP_TEXTURES"))
      dump_texture(dir, r, l.format, m.width_elements, m.height_elements, dst);
  }

  // Development: a decoded copy of a texture as a PPM (BC1/BC3 colour, RGBA8).
  void dump_texture(const char* dir, const ResourceState* r, uint32_t format, uint32_t we, uint32_t he,
                    const uint8_t* data) {
    static int dumped = 0;
    if (dumped >= 200) return;
    const bool bc1 = format >= 70 && format <= 72, bc3 = format >= 76 && format <= 78;
    const bool rgba = format == 28 || format == 29 || format == 87;
    if (!bc1 && !bc3 && !rgba) return;
    const uint32_t w = rgba ? we : we * 4, h = rgba ? he : he * 4;
    if (w > 4096 || h > 4096) return;
    std::vector<uint8_t> px(uint64_t{w} * h * 3);
    if (rgba) {
      for (uint64_t i = 0; i < uint64_t{w} * h; ++i)
        for (int c = 0; c < 3; ++c) px[i * 3 + c] = data[i * 4 + (format == 87 ? 2 - c : c)];
    } else {
      const uint32_t bytes = bc1 ? 8 : 16;
      for (uint32_t by = 0; by < he; ++by)
        for (uint32_t bx = 0; bx < we; ++bx) {
          const uint8_t* b = data + (uint64_t{by} * we + bx) * bytes + (bc3 ? 8 : 0);
          const uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
          uint8_t pal[4][3];
          const auto rgb = [](uint16_t c, uint8_t* o) {
            o[0] = static_cast<uint8_t>((c >> 11) * 255 / 31);
            o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
            o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
          };
          rgb(c0, pal[0]);
          rgb(c1, pal[1]);
          for (int c = 0; c < 3; ++c) {
            pal[2][c] = static_cast<uint8_t>((2 * pal[0][c] + pal[1][c]) / 3);
            pal[3][c] = static_cast<uint8_t>((pal[0][c] + 2 * pal[1][c]) / 3);
          }
          const uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | static_cast<uint32_t>(b[7]) << 24;
          for (int i = 0; i < 16; ++i) {
            const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
            std::memcpy(&px[(uint64_t{y} * w + x) * 3], pal[(bits >> (2 * i)) & 3], 3);
          }
        }
    }
    const std::string path = std::format("{}/tex_{:03}_{}x{}_f{}_sw{}.ppm", dir, dumped++, w, h, format, r->swizzle);
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
  }

  // Copies guest memory a draw reads into the ring (once per submission).
  VkDeviceAddress upload(uint64_t address, uint64_t size) {
    size = std::min<uint64_t>(size, 64ull << 20);
    const auto key = std::make_pair(address, size);
    if (auto it = uploads_.find(key); it != uploads_.end()) return it->second;
    const VkDeviceSize at = ring_alloc(size + 16, 256);
    auto* dst = static_cast<uint8_t*>(ring_.mapped) + at;
    if (!platform::safe_read(address, dst, size)) {
      // Partly unreadable: page by page.
      for (uint64_t off = 0; off < size; off += 4096) {
        const uint64_t n = std::min<uint64_t>(4096 - ((address + off) & 4095), size - off);
        if (!platform::safe_read(address + off, dst + off, n)) std::memset(dst + off, 0, n);
        off += n - 4096;
      }
    }
    std::memset(dst + size, 0, 16);
    const VkDeviceAddress a = ring_.address + at;
    uploads_[key] = a;
    return a;
  }

  // ---------------------------------------------------------- shaders

  ShaderEntry* shader(const std::vector<uint8_t>& code, uint64_t hash, gpu::ShaderStage stage) {
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) return it->second.get();
    auto e = std::make_unique<ShaderEntry>();
    const auto t_translate = std::chrono::steady_clock::now();
    e->shader = gpu::translate_shader(code.data(), code.size(), stage);
    add_us(translate_us_, t_translate);
    {
      std::lock_guard lock(tags_mutex_);  // (the loop guard reports wave-mode vertex/pixel shaders too)
      shader_tags_[e->shader.tag] = hash;
    }
    if (!e->shader.ok) e->error = e->shader.error;
    else e->module = compile(e->shader.glsl, stage == gpu::ShaderStage::Vertex ? "vert" : "frag", e->error);
    // The GLSL (megabytes for large wave-mode programs) is not needed any more.
    std::string().swap(e->shader.glsl);
    if (!e->error.empty())
      XBEM_WARN("gpu", "{} shader {:016x}: {}", stage == gpu::ShaderStage::Vertex ? "vertex" : "pixel", hash,
                e->error);
    ShaderEntry* raw = e.get();
    shaders_[hash] = std::move(e);
    return raw;
  }

  VkShaderModule compile(const std::string& glsl, const char* ext, std::string& error) {
    const uint64_t h = fnv1a(glsl.data(), glsl.size());
    const fs::path spv = cache_dir_ / std::format("{:016x}.{}.spv", h, ext);
    std::error_code ec;
    if (!fs::exists(spv, ec)) {
      const fs::path src = cache_dir_ / std::format("{:016x}.{}", h, ext);
      std::ofstream(src) << glsl;
      const std::string cmd = std::format("glslangValidator -V --target-env vulkan1.2 -o '{}' '{}' > '{}.log' 2>&1",
                                          spv.string(), src.string(), src.string());
      const auto t_glslang = std::chrono::steady_clock::now();
      const int rc = std::system(cmd.c_str());
      add_us(glslang_us_, t_glslang);
      ++glslang_runs_;
      if (rc != 0) {
        std::ifstream log(src.string() + ".log");
        std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        error = std::format("GLSL compile failed ({}): {}", src.string(), text.substr(0, 400));
        return VK_NULL_HANDLE;
      }
    }
    std::ifstream in(spv, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (core_.fn().vkCreateShaderModule(core_.device(), &ci, nullptr, &m) != VK_SUCCESS)
      error = "vkCreateShaderModule failed";
    return m;
  }

  // RECTLIST: a geometry shader completes each rectangle. The corner the
  // three vertices share is the one off the longest edge; the fourth vertex
  // is the other two plus it mirrored (a + b - corner).
  VkShaderModule rect_geometry(uint32_t outputs) {
    if (auto it = rect_gs_.find(outputs); it != rect_gs_.end()) return it->second;
    std::string g = "#version 460\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
    for (uint32_t k = 0; k < outputs; ++k)
      g += std::format("layout(location = {0}) in vec4 i_{0}[];\nlayout(location = {0}) out vec4 o_{0};\n", k);
    g += "void main() {\n"
         "  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;\n"
         "  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;\n"
         "  vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;\n"
         "  float d0 = distance(p1, p2), d1 = distance(p0, p2), d2 = distance(p0, p1);\n"
         "  int c = d0 >= d1 && d0 >= d2 ? 0 : (d1 >= d2 ? 1 : 2);\n"
         "  int a = (c + 1) % 3, b = (c + 2) % 3;\n"
         "  int order[4] = int[4](c, a, b, 3);\n"
         "  for (int n = 0; n < 4; ++n) {\n"
         "    int v = order[n];\n"
         "    if (v < 3) {\n      gl_Position = gl_in[v].gl_Position;\n";
    for (uint32_t k = 0; k < outputs; ++k) g += std::format("      o_{0} = i_{0}[v];\n", k);
    g += "    } else {\n      gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;\n";
    for (uint32_t k = 0; k < outputs; ++k) g += std::format("      o_{0} = i_{0}[a] + i_{0}[b] - i_{0}[c];\n", k);
    g += "    }\n    EmitVertex();\n  }\n  EndPrimitive();\n}\n";
    std::string error;
    VkShaderModule m = compile(g, "geom", error);
    if (!error.empty()) XBEM_WARN("gpu", "RECTLIST geometry shader: {}", error);
    rect_gs_[outputs] = m;
    return m;
  }

  // mode: 0 triangle list, 1 RECTLIST (geometry shader), 2 triangle strip.
  VkPipeline pipeline(const GraphicsPipeline& p, ShaderEntry& vs, ShaderEntry* ps, uint32_t mode, bool& building) {
    const bool rect = mode == 1;
    // Attachment formats: the bound targets'.
    const uint32_t slots = bound_slots();
    VkFormat formats[8]{};
    uint64_t format_key = slots;
    for (uint32_t i = 0; i < slots; ++i) {
      GpuImage* g = slots == 1 ? state_.target : state_.targets[i];
      formats[i] = g ? g->image.format : VK_FORMAT_UNDEFINED;
      format_key = format_key * 1000003u + static_cast<uint64_t>(formats[i]);
    }
    // Blend: the variant's D3D12_BLEND_DESC (render target 0 at dword 3, its
    // IndependentBlendEnable at dword 2), else alpha blending.
    uint32_t blend[8][10];
    for (auto& b : blend) {
      const uint32_t dflt[10] = {1, 0, 5, 6, 1, 2, 6, 1, 4, 0xf};
      std::memcpy(b, dflt, sizeof dflt);
    }
    if (p.blend_override) {
      uint32_t independent = 0;
      std::memcpy(&independent, p.blend + 8, 4);
      for (uint32_t i = 0; i < 8; ++i) std::memcpy(blend[i], p.blend + 12 + 40 * (independent ? i : 0), 40);
    } else if (p.has_registers) {
      // CB_BLENDn_CONTROL (AMD factors and functions) in the D3D12 terms above.
      static const uint32_t kFactor[21] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 2, 2, 14, 15, 16, 17, 18, 19, 14, 15};
      static const uint32_t kComb[8] = {1, 2, 4, 5, 3, 1, 1, 1};
      const auto factor_of = [](uint32_t f) { return f < 21 ? kFactor[f] : 2u; };
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t c = p.cb_blend[i];
        const bool separate = (c >> 29) & 1;
        blend[i][0] = (c >> 30) & 1;
        blend[i][2] = factor_of(c & 31);
        blend[i][4] = kComb[(c >> 5) & 7];
        blend[i][3] = factor_of((c >> 8) & 31);
        blend[i][5] = separate ? factor_of((c >> 16) & 31) : blend[i][2];
        blend[i][7] = separate ? kComb[(c >> 21) & 7] : blend[i][4];
        blend[i][6] = separate ? factor_of((c >> 24) & 31) : blend[i][3];
        blend[i][9] = 0xf;
      }
    }
    // Targets the pixel shader does not export are left alone.
    for (uint32_t i = 0; i < slots; ++i)
      if (!ps || (!(ps->shader.color_targets & (1u << i)) && slots > 1)) blend[i][9] = 0;
    // Depth: DB_DEPTH_CONTROL from the console state when a depth target is bound.
    const VkFormat depth_format = state_.depth ? state_.depth->image.format : VK_FORMAT_UNDEFINED;
    const uint32_t depth_control = state_.depth && p.has_registers ? p.db_depth_control & 0x77u : 0;
    format_key = format_key * 1000003u + static_cast<uint64_t>(depth_format) * 131u + depth_control;
    const PipelineKey key{p.vs_hash, p.ps_hash, fnv1a(blend, sizeof(uint32_t) * 10 * slots), format_key, mode};
    if (auto it = pipelines_.find(key); it != pipelines_.end()) {
      const int st = it->second->state.load(std::memory_order_acquire);
      if (st == 0) building = true;
      return st == 1 ? it->second->pipeline : VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t count = 0;
    stages[count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                       vs.module, "main", nullptr};
    if (rect) {
      VkShaderModule gs = rect_geometry(vs.shader.outputs);
      if (!gs) {
        auto failed = std::make_shared<PendingPipeline>();
        failed->state = 2;
        pipelines_[key] = failed;
        return VK_NULL_HANDLE;
      }
      stages[count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT,
                         gs, "main", nullptr};
    }
    // (Depth-only pipelines have no pixel shader.)
    if (ps)
      stages[count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                         ps->module, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = mode == 2 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // DB_DEPTH_CONTROL: Z_ENABLE bit 1, Z_WRITE_ENABLE bit 2, ZFUNC bits 4-6
    // (NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS - VkCompareOp's order).
    ds.depthTestEnable = (depth_control >> 1) & 1;
    ds.depthWriteEnable = (depth_control >> 2) & 1;
    ds.depthCompareOp = static_cast<VkCompareOp>((depth_control >> 4) & 7);
    static const VkBlendFactor kFactors[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA};
    static const VkBlendOp kOps[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT,
                                     VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    const auto factor = [&](uint32_t v) { return v < 20 ? kFactors[v] : VK_BLEND_FACTOR_ONE; };
    const auto op = [&](uint32_t v) { return v < 6 ? kOps[v] : VK_BLEND_OP_ADD; };
    VkPipelineColorBlendAttachmentState att[8]{};
    for (uint32_t i = 0; i < slots; ++i) {
      att[i].blendEnable = blend[i][0] ? VK_TRUE : VK_FALSE;
      att[i].srcColorBlendFactor = factor(blend[i][2]);
      att[i].dstColorBlendFactor = factor(blend[i][3]);
      att[i].colorBlendOp = op(blend[i][4]);
      att[i].srcAlphaBlendFactor = factor(blend[i][5]);
      att[i].dstAlphaBlendFactor = factor(blend[i][6]);
      att[i].alphaBlendOp = op(blend[i][7]);
      att[i].colorWriteMask = formats[i] == VK_FORMAT_UNDEFINED ? 0 : blend[i][9] & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = slots;
    cb.pAttachments = att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfoKHR ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ri.colorAttachmentCount = slots;
    ri.pColorAttachmentFormats = formats;
    ri.depthAttachmentFormat = depth_format;
    auto entry = std::make_shared<PendingPipeline>();
    pipelines_[key] = entry;
    // Everything the create info points at, copied for the worker.
    struct Job {
      VkPipelineShaderStageCreateInfo stages[3];
      uint32_t count;
      VkPipelineVertexInputStateCreateInfo vi;
      VkPipelineInputAssemblyStateCreateInfo ia;
      VkPipelineViewportStateCreateInfo vp;
      VkPipelineRasterizationStateCreateInfo rs;
      VkPipelineMultisampleStateCreateInfo ms;
      VkPipelineDepthStencilStateCreateInfo ds;
      VkPipelineColorBlendAttachmentState att[8];
      VkPipelineColorBlendStateCreateInfo cb;
      VkDynamicState dyn[2];
      VkPipelineDynamicStateCreateInfo dy;
      VkFormat formats[8];
      VkPipelineRenderingCreateInfoKHR ri;
      uint64_t vs_hash, ps_hash;
    };
    auto job = std::make_shared<Job>();
    std::memcpy(job->stages, stages, sizeof stages);
    job->count = count;
    job->vi = vi, job->ia = ia, job->vp = vp, job->rs = rs, job->ms = ms, job->ds = ds;
    std::memcpy(job->att, att, sizeof att);
    job->cb = cb;
    job->cb.pAttachments = job->att;
    std::memcpy(job->dyn, dyn, sizeof dyn);
    job->dy = dy;
    job->dy.pDynamicStates = job->dyn;
    std::memcpy(job->formats, formats, sizeof formats);
    job->ri = ri;
    job->ri.pColorAttachmentFormats = job->formats;
    job->vs_hash = p.vs_hash, job->ps_hash = p.ps_hash;
    const auto build = [this, entry, job] {
      VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      gci.pNext = &job->ri;
      gci.stageCount = job->count;
      gci.pStages = job->stages;
      gci.pVertexInputState = &job->vi;
      gci.pInputAssemblyState = &job->ia;
      gci.pViewportState = &job->vp;
      gci.pRasterizationState = &job->rs;
      gci.pMultisampleState = &job->ms;
      gci.pDepthStencilState = &job->ds;
      gci.pColorBlendState = &job->cb;
      gci.pDynamicState = &job->dy;
      gci.layout = layout_;
      VkPipeline pipe = VK_NULL_HANDLE;
      const auto t_pipe = std::chrono::steady_clock::now();
      const VkResult made = core_.fn().vkCreateGraphicsPipelines(core_.device(), pipeline_cache_, 1, &gci, nullptr, &pipe);
      add_us(pipeline_us_, t_pipe);
      add_us(graphics_pipeline_us_, t_pipe);
      ++pipelines_created_;
      ++graphics_pipelines_;
      if (made != VK_SUCCESS) {
        XBEM_WARN("gpu", "pipeline vs {:016x} ps {:016x}: vkCreateGraphicsPipelines failed", job->vs_hash, job->ps_hash);
        pipe = VK_NULL_HANDLE;
      }
      entry->pipeline = pipe;
      entry->state.store(pipe ? 1 : 2, std::memory_order_release);
    };
    static const bool sync = [] {
      const char* e = std::getenv("XBEM_SYNC_PIPELINES");
      return e && *e == '1';
    }();
    if (sync) {
      build();
      return entry->pipeline;
    }
    building = true;
    graphics_compile_pool().run(build);
    return VK_NULL_HANDLE;
  }

  // ---------------------------------------------------------- state

  struct RootValue {
    uint64_t value = 0;            // table handle or root descriptor address
    std::vector<uint32_t> constants;
  };

  struct State {
    ComputePipeline* compute = nullptr;
    const gpu::RootSignature* compute_root = nullptr;
    std::map<uint32_t, RootValue> compute_params;
    GraphicsPipeline* pipeline = nullptr;
    std::string pipeline_name;  // for skip statistics
    const gpu::RootSignature* root = nullptr;
    std::map<uint32_t, RootValue> params;
    uint32_t topology = 4;
    VkViewport viewport{};
    VkRect2D scissor{};
    bool has_scissor = false;
    GpuImage* target = nullptr;      // the first bound render target
    GpuImage* targets[8] = {};       // OMSetRenderTargets' slots
    uint32_t target_count = 0;
    GpuImage* depth = nullptr;       // its depth-stencil view's image (same size as the targets)
    std::vector<uint8_t> vertex_buffers;  // D3D12_VERTEX_BUFFER_VIEW x n
    uint8_t index_buffer[16] = {};
    uint64_t heaps[2] = {};  // resource heap, sampler heap starts
  };

  void note(const std::string& what) {
    if (notes_.insert(what).second && notes_.size() < 256) XBEM_WARN("gpu", "{}", what);
  }

  // GPU images of resources unused for a while are dropped when their
  // memory passes the budget (XBEM_IMAGE_BUDGET_MB, default 1536): resources
  // are never released (probe objects live on), and titles place new ones at
  // reused addresses, so images only accumulated until the device ran out.
  void evict_images() {
    static const uint64_t budget = [] {
      const char* e = std::getenv("XBEM_IMAGE_BUDGET_MB");
      return (e ? std::strtoull(e, nullptr, 10) : 1536ull) << 20;
    }();
    // Each image is a memory allocation of its own, and the device's count is
    // limited (creation of 1x1 images failed near 4000): a count budget too.
    static const size_t max_count = [] {
      const char* e = std::getenv("XBEM_IMAGE_COUNT_MAX");
      return e ? std::strtoull(e, nullptr, 10) : 2500ull;
    }();
    static const bool logged = [&] {
      XBEM_INFO("gpu", "image budget: {} MiB, {} images (the device allows {} memory allocations)", budget >> 20,
                max_count, core_.max_allocations());
      return true;
    }();
    (void)logged;
    const size_t count_now = images_.size() + depth_images_.size();
    if ((image_bytes_ <= budget && count_now <= max_count) || (presents_ & 15) != 0) return;
    std::vector<std::pair<uint64_t, std::pair<bool, const ResourceState*>>> old;
    for (const auto& [r, g] : images_)
      if (g && g->last_used + 32 < presents_ && g.get() != display_) old.push_back({g->last_used, {false, r}});
    for (const auto& [r, g] : depth_images_)
      if (g && g->last_used + 32 < presents_) old.push_back({g->last_used, {true, r}});
    std::sort(old.begin(), old.end());
    const auto& f = core_.fn();
    uint64_t freed = 0, count = 0;
    for (const auto& [used, which] : old) {
      if (image_bytes_ <= budget * 3 / 4 && images_.size() + depth_images_.size() <= max_count * 3 / 4) break;
      auto& map = which.first ? depth_images_ : images_;
      auto it = map.find(which.second);
      if (it == map.end() || !it->second) continue;
      GpuImage& g = *it->second;
      if (g.texture_index) bind_texture(g.texture_index, white_.view), pools_[0].give(g.texture_index);
      if (g.array_index) bind_texture(g.array_index, white_.array_view, 2), pools_[1].give(g.array_index);
      if (g.volume_index) bind_texture(g.volume_index, white_volume_.view, 3), pools_[2].give(g.volume_index);
      if (g.storage_index) bind_storage(g.storage_index, scratch_storage_.view, 4), pools_[3].give(g.storage_index);
      if (g.storage_array_index)
        bind_storage(g.storage_array_index, scratch_storage_.array_view, 5), pools_[4].give(g.storage_array_index);
      if (g.storage_volume_index)
        bind_storage(g.storage_volume_index, scratch_storage_volume_.view, 6), pools_[5].give(g.storage_volume_index);
      if (g.image.array_view) f.vkDestroyImageView(core_.device(), g.image.array_view, nullptr);
      if (g.image.view) f.vkDestroyImageView(core_.device(), g.image.view, nullptr);
      if (g.image.image) f.vkDestroyImage(core_.device(), g.image.image, nullptr);
      if (g.image.memory) f.vkFreeMemory(core_.device(), g.image.memory, nullptr);
      image_bytes_ -= std::min(image_bytes_, g.bytes);
      freed += g.bytes;
      ++count;
      map.erase(it);
    }
    // Pointers into the maps the pass state may still hold.
    state_.target = state_.depth = nullptr;
    for (GpuImage*& t : state_.targets) t = nullptr;
    state_.target_count = 0;
    for (GpuImage*& t : current_targets_) t = nullptr;
    current_depth_ = nullptr;
    current_count_ = 0;
    if (count && memory_.ready()) {
      // The GPU-side descriptor table holds image indices: start it over.
      using namespace gpu::mem;
      std::memset(memory_.table_words() + kDescWord, 0, uint64_t{kDescEntries} * kDescEntryWords * 4);
      descriptors_ = 0;
      ++descriptor_table_resets_;
    }
    if (count)
      XBEM_INFO("gpu", "{} images not used for 32 presents dropped ({} MiB); {} MiB of images left", count, freed >> 20,
                image_bytes_ >> 20);
  }

  // Copies between resources happen in guest memory (unified memory, as on
  // the console): the renderer re-reads textures whose memory changed. The
  // CPU does them when the command list runs, in order with the rest.
  void copy_buffer_region(ResourceState* dst, uint64_t dst_offset, ResourceState* src, uint64_t src_offset,
                          uint64_t bytes) {
    if (!dst || !src || !dst->gpu_va || !src->gpu_va || !bytes || bytes > (256ull << 20)) return;
    if (dst_offset + bytes > std::max<uint64_t>(dst->size, 1) || src_offset + bytes > std::max<uint64_t>(src->size, 1))
      return;
    std::vector<uint8_t> tmp(bytes);
    if (platform::safe_read(src->gpu_va + src_offset, tmp.data(), bytes))
      platform::safe_write(dst->gpu_va + dst_offset, tmp.data(), bytes);
    ++copies_;
  }

  // CopyTextureRegion with the two D3D12_TEXTURE_COPY_LOCATIONs (pResource,
  // Type 0 subresource index / 1 placed footprint {Offset u64, Format, Width,
  // Height, Depth, RowPitch}) and the source box: buffer -> linear texture
  // (uploads, e.g. video frames) and linear texture -> buffer (readbacks).
  void copy_texture_region(const uint8_t* blob, uint32_t dst_x, uint32_t dst_y, uint32_t dst_z, bool has_box) {
    struct Location {
      uint64_t resource;
      uint32_t type, pad;
      uint64_t offset;  // footprint offset, or the subresource index (first 4 bytes)
      uint32_t format, width, height, depth, row_pitch, pad2;
    };
    static_assert(sizeof(Location) == 48);
    Location d{}, sl{};
    std::memcpy(&d, blob, 40);
    std::memcpy(&sl, blob + 40, 40);
    uint32_t box[6] = {};
    std::memcpy(box, blob + 80, 24);
    ResourceState* dr = resource_of(d.resource);
    ResourceState* sr = resource_of(sl.resource);
    if (!dr || !sr || !dr->gpu_va || !sr->gpu_va) return;
    // One side a texture subresource, the other a placed footprint.
    const bool upload = d.type == 0 && sl.type == 1, readback = d.type == 1 && sl.type == 0;
    if (!upload && !readback) return;
    ResourceState* tex = upload ? dr : sr;
    const Location& fp = upload ? sl : d;
    const uint32_t sub = static_cast<uint32_t>(upload ? d.offset : sl.offset);
    if (tex->swizzle != 0) {
      note("CopyTextureRegion into a swizzled texture - not done");
      return;
    }
    const TextureLayout l = linear_layout(tex->desc);
    if (l.mip.empty() || !l.mips) return;
    const uint32_t mip = sub % l.mips, slice = (sub / l.mips) % std::max<uint32_t>(l.slices, 1);
    const MipLayout& m = l.mip[mip];
    const uint32_t e = std::max<uint32_t>(l.element.bytes, 1), block = l.element.block ? 4 : 1;
    // The region in elements: the box (source) or the whole footprint.
    uint32_t x0 = 0, y0 = 0, w = fp.width, h = fp.height;
    if (upload && has_box) {
      x0 = box[0];
      y0 = box[1];
      w = box[3] - box[0];
      h = box[4] - box[1];
    }
    const uint32_t tx = upload ? dst_x : (has_box ? box[0] : 0), ty = upload ? dst_y : (has_box ? box[1] : 0);
    const uint32_t ew = (w + block - 1) / block, eh = (h + block - 1) / block;
    const uint64_t row_bytes = uint64_t{ew} * e;
    const uint64_t tex_base = tex->gpu_va + slice * l.slice_stride + m.offset + uint64_t{dst_z} * m.slice_2d;
    if (row_bytes == 0 || row_bytes > fp.row_pitch || row_bytes > m.pitch_bytes) return;
    std::vector<uint8_t> row(row_bytes);
    for (uint32_t r = 0; r < eh && ty / block + r < m.height_elements; ++r) {
      const uint64_t tex_at = tex_base + uint64_t{ty / block + r} * m.pitch_bytes + uint64_t{tx / block} * e;
      const uint64_t buf_at = (upload ? sr : dr)->gpu_va + fp.offset + uint64_t{y0 / block + r} * fp.row_pitch +
                              uint64_t{x0 / block} * e;
      if (upload) {
        if (platform::safe_read(buf_at, row.data(), row_bytes)) platform::safe_write(tex_at, row.data(), row_bytes);
      } else if (platform::safe_read(tex_at, row.data(), row_bytes)) {
        platform::safe_write(buf_at, row.data(), row_bytes);
      }
    }
    ++copies_;
  }

  static VkImageAspectFlags depth_aspects(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT
               ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
               : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT);
  }

  // The depth image of a depth-stencil resource (D32/D24S8/D32S8/D16 and
  // their typeless forms), created on first use. Depth only (D32_SFLOAT):
  // the stencil test is not emulated yet.
  GpuImage* depth_of(ResourceState* r) {
    if (!r) return nullptr;
    if (auto it = depth_images_.find(r); it != depth_images_.end()) {
      if (it->second) it->second->last_used = presents_;
      return it->second.get();
    }
    uint32_t dimension, height, format;
    uint64_t width;
    std::memcpy(&dimension, r->desc, 4);
    std::memcpy(&width, r->desc + 16, 8);
    std::memcpy(&height, r->desc + 24, 4);
    std::memcpy(&format, r->desc + 32, 4);
    const bool depth_format = format == 40 || format == 39 || format == 45 || format == 44 || format == 20 ||
                              format == 19 || format == 55 || format == 53;
    std::unique_ptr<GpuImage> g;
    if (dimension == 3 && depth_format && width && height && width <= 16384 && height <= 16384) {
      g = std::make_unique<GpuImage>();
      g->image = core_.create_image(static_cast<uint32_t>(width), height, VK_FORMAT_D32_SFLOAT,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT);
      if (!g->image.image) {
        g.reset();
      } else {
        transition(g->image.image, VK_IMAGE_ASPECT_DEPTH_BIT);
        g->last_used = presents_;
        g->bytes = static_cast<uint64_t>(width) * height * 4;
        image_bytes_ += g->bytes;
      }
    } else {
      note(std::format("depth-stencil resource format {} dimension {} not supported", format, dimension));
    }
    GpuImage* raw = g.get();
    depth_images_[r] = std::move(g);
    return raw;
  }

  GpuImage* depth_from_descriptor(const uint8_t* bytes) {
    XbemDescriptor d{};
    std::memcpy(&d, bytes, sizeof d);
    if (d.magic != kDescriptorMagic) return nullptr;
    return depth_of(resource_of(d.object));
  }

  GpuImage* image_from_descriptor(const uint8_t* bytes, bool for_render) {
    XbemDescriptor d{};
    std::memcpy(&d, bytes, sizeof d);
    if (d.magic != kDescriptorMagic) return nullptr;
    return image_of(resource_of(d.object), for_render);
  }

  void command(const RecordedCommand& c) {
    const uint64_t* a = c.args;
    const auto u32 = [&](int i) { return static_cast<uint32_t>(a[i]); };
    switch (c.slot) {
      case 20: state_.topology = u32(0); break;
      case 21:
        if (c.blob.size() >= 24) {
          float v[6];
          std::memcpy(v, c.blob.data(), sizeof v);
          state_.viewport = {v[0], v[1], v[2], v[3], v[4], v[5]};
        }
        break;
      case 22:
        if (c.blob.size() >= 16) {
          int32_t r[4];
          std::memcpy(r, c.blob.data(), sizeof r);
          state_.scissor = {{r[0], r[1]},
                            {static_cast<uint32_t>(std::max(0, r[2] - r[0])), static_cast<uint32_t>(std::max(0, r[3] - r[1]))}};
          state_.has_scissor = true;
        }
        break;
      case 25: {
        if (ComputePipeline* cp = compute_pipeline_of(a[0])) {
          state_.compute = cp;
          break;
        }
        GraphicsPipeline* p = graphics_pipeline_of(a[0]);
        state_.pipeline = p;
        state_.pipeline_name = !p && is_probe_object(a[0]) ? reinterpret_cast<ProbeObject*>(a[0])->name.substr(0, 40)
                                                            : std::format("{:#x}", a[0]);
        if (!p) {
          // What the title passed: its first words (a vtable / an object of
          // another kind) tell which creation path is missing.
          uint64_t w[6] = {};
          platform::safe_read(a[0], w, sizeof w);
          std::string words;
          for (uint64_t x : w) words += std::format(" {:016x}", x);
          const std::string vt = w[0] ? describe_code_address(w[0]) : std::string();
          note(std::format("pipeline {:#x} is not a known graphics pipeline - its draws are skipped; memory:{}{}", a[0],
                           words, vt.empty() ? "" : " (word 0: " + vt + ")"));
        }
        break;
      }
      case 32:
        for (size_t i = 0; i + 8 <= c.blob.size(); i += 8) {
          uint64_t heap;
          std::memcpy(&heap, c.blob.data() + i, 8);
          uint64_t start;
          uint32_t type;
          uint64_t size = 0;
          if (descriptor_heap_of(heap, start, type, &size) && type <= 1) {
            state_.heaps[type] = start;
            heap_window(start, size);
          }
        }
        break;
      case 34:
        state_.root = root_signature_of(a[0]);
        state_.params.clear();
        break;
      case 33:
        state_.compute_root = root_signature_of(a[0]);
        state_.compute_params.clear();
        break;
      case 35: state_.compute_params[u32(0)].value = a[1]; break;
      case 37: {
        auto& rv = state_.compute_params[u32(0)];
        if (rv.constants.size() <= u32(2)) rv.constants.resize(u32(2) + 1);
        rv.constants[u32(2)] = u32(1);
        break;
      }
      case 39: {
        auto& rv = state_.compute_params[u32(0)];
        const uint32_t n = static_cast<uint32_t>(c.blob.size() / 4);
        if (rv.constants.size() < u32(3) + n) rv.constants.resize(u32(3) + n);
        std::memcpy(rv.constants.data() + u32(3), c.blob.data(), n * 4);
        break;
      }
      case 41: case 43: case 45: state_.compute_params[u32(0)].value = a[1]; break;
      case 14: dispatch(u32(0), u32(1), u32(2)); break;
      case 36: state_.params[u32(0)].value = a[1]; break;
      case 38: {
        auto& rv = state_.params[u32(0)];
        if (rv.constants.size() <= u32(2)) rv.constants.resize(u32(2) + 1);
        rv.constants[u32(2)] = u32(1);
        break;
      }
      case 40: {
        auto& rv = state_.params[u32(0)];
        const uint32_t n = static_cast<uint32_t>(c.blob.size() / 4);
        if (rv.constants.size() < u32(3) + n) rv.constants.resize(u32(3) + n);
        std::memcpy(rv.constants.data() + u32(3), c.blob.data(), n * 4);
        break;
      }
      case 42: case 44: case 46: state_.params[u32(0)].value = a[1]; break;
      case 47: std::memcpy(state_.index_buffer, c.blob.data(), std::min<size_t>(16, c.blob.size())); break;
      case 48: {
        const uint32_t start = u32(0);
        const size_t need = (start + c.blob.size() / 16) * 16;
        if (state_.vertex_buffers.size() < need) state_.vertex_buffers.resize(need);
        std::memcpy(state_.vertex_buffers.data() + start * 16, c.blob.data(), c.blob.size());
        break;
      }
      case 50: {
        end_rendering();
        // Render target descriptors (one per slot), then the DSV's.
        state_.target = nullptr;
        state_.target_count = std::min<uint32_t>(u32(0), 8);
        for (uint32_t i = 0; i < 8; ++i) {
          GpuImage* g = i < state_.target_count && c.blob.size() >= (i + 1) * kDescriptorSize
                            ? image_from_descriptor(c.blob.data() + i * kDescriptorSize, true)
                            : nullptr;
          // (Every bound target has the first one's size, as dynamic rendering needs.)
          if (g && state_.target && (g->image.width != state_.target->image.width ||
                                     g->image.height != state_.target->image.height || !g->image.view))
            g = nullptr;
          state_.targets[i] = g;
          if (g && !state_.target) state_.target = g;
        }
        state_.depth = nullptr;
        if (c.blob.size() >= (state_.target_count + 1) * kDescriptorSize) {
          GpuImage* d = depth_from_descriptor(c.blob.data() + state_.target_count * kDescriptorSize);
          if (d && (!state_.target || (d->image.width == state_.target->image.width &&
                                       d->image.height == state_.target->image.height)))
            state_.depth = d;
        }
        break;
      }
      case 52: {
        if (c.blob.size() < kDescriptorSize + 16) break;
        GpuImage* g = image_from_descriptor(c.blob.data(), true);
        if (!g) break;
        end_rendering();
        float rgba[4];
        std::memcpy(rgba, c.blob.data() + kDescriptorSize, 16);
        VkClearColorValue color{};
        std::memcpy(color.float32, rgba, 16);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        core_.fn().vkCmdClearColorImage(cmd_, g->image.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        full_barrier();
        g->rendered = true;
        break;
      }
      case 51: {
        // ClearDepthStencilView(dsv, flags, depth bits, stencil): flag 1 depth.
        if (c.blob.size() < kDescriptorSize || !(u32(1) & 1)) break;
        GpuImage* d = depth_from_descriptor(c.blob.data());
        if (!d) break;
        end_rendering();
        float depth;
        const uint32_t bits = u32(2);
        std::memcpy(&depth, &bits, 4);
        VkClearDepthStencilValue v{std::isfinite(depth) ? std::clamp(depth, 0.0f, 1.0f) : 0.0f, u32(3) & 0xff};
        VkImageSubresourceRange range{depth_aspects(d->image.format), 0, 1, 0, 1};
        core_.fn().vkCmdClearDepthStencilImage(cmd_, d->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        full_barrier();
        break;
      }
      case 15:  // CopyBufferRegion(dst, dst offset, src, src offset, bytes)
        copy_buffer_region(resource_of(a[0]), a[1], resource_of(a[2]), a[3], a[4]);
        break;
      case 16:
        if (c.blob.size() >= 104) copy_texture_region(c.blob.data(), u32(1), u32(2), u32(3), a[5] != 0);
        break;
      case 17: {  // CopyResource(dst, src): buffers and linear textures are plain memory
        ResourceState* dst = resource_of(a[0]);
        ResourceState* src = resource_of(a[1]);
        if (dst && src && dst->size == src->size && dst->swizzle == src->swizzle)
          copy_buffer_region(dst, 0, src, 0, dst->size);
        break;
      }
      case 12: draw(u32(0), u32(1), u32(2), u32(3), false, 0); break;
      case 13: draw(u32(0), u32(1), u32(2), u32(4), true, static_cast<int32_t>(u32(3))); break;
      default: break;
    }
  }

  void end_rendering() {
    if (!rendering_) return;
    core_.fn().vkCmdEndRendering(cmd_);
    full_barrier();
    rendering_ = false;
  }

  // User SGPRs (measured on GTA VI, see GPU.md): vertex stage s[0:1] vertex
  // buffer table, s2 base vertex; from s8 the root parameters in order -
  // descriptor tables as 32-bit pointers, constants in place, root
  // descriptors as 64-bit addresses - then the sampler heap's 32-bit pointer
  // when the root signature indexes it directly.
  //
  // Shaders build descriptor table addresses from 32 bits, so tables are
  // given 32-bit addresses: inside a bound descriptor heap its window below
  // 4 GiB (heap_window: the same address on the CPU and the GPU for the
  // whole run), anything else a per-draw token from 0x10000000.
  struct HeapWindow {
    uint64_t real, size, window;
  };
  static constexpr uint64_t kWindowStart = 0x80000000ull, kWindowEnd = 0x100000000ull;

  uint64_t heap_window(uint64_t real, uint64_t size) {
    for (const HeapWindow& h : windows_)
      if (h.real == real && h.size >= size) return h.window;
    const uint64_t window = ((next_window_ + 0xffff) & ~0xffffull) + (real & 0xffff);
    if (window + size > kWindowEnd) {
      note("descriptor heap windows below 4 GiB used up - tables get per-draw addresses");
      return 0;
    }
    next_window_ = window + size;
    windows_.push_back({real, size, window});
    memory_.alias(window, real, size);
    XBEM_INFO("gpu", "descriptor heap {:#x} ({} KiB) at GPU address {:#x}", real, size >> 10, window);
    return window;
  }

  struct Tokens {
    const std::vector<HeapWindow>* windows = nullptr;
    std::vector<std::pair<uint64_t, uint64_t>> regions;  // host address, size
    uint32_t add(uint64_t host, uint64_t size) {
      if (windows)
        for (const HeapWindow& h : *windows)
          if (host >= h.real && host < h.real + h.size) return static_cast<uint32_t>(h.window + (host - h.real));
      regions.emplace_back(host, size);
      return 0x10000000u + static_cast<uint32_t>(regions.size() - 1) * 0x01000000u;
    }
    // The memory behind a 32-bit address (0 when it is not one).
    uint64_t host_of(uint64_t address) const {
      if (address >= kWindowStart && address < kWindowEnd && windows) {
        for (const HeapWindow& h : *windows)
          if (address >= h.window && address < h.window + h.size) return h.real + (address - h.window);
        return 0;
      }
      if (address >= 0x10000000u && address < kWindowStart) {
        const uint64_t i = (address - 0x10000000u) >> 24;
        return i < regions.size() ? regions[i].first + (address & 0xffffff) : 0;
      }
      return 0;
    }
    bool read(uint64_t address, void* out, size_t size) const {
      if (address >= kWindowStart && address < kWindowEnd) {
        const uint64_t host = host_of(address);
        return host && platform::safe_read(host, out, size);
      }
      if (address >= 0x10000000u && address < 0x100000000ull) {
        const uint64_t i = (address - 0x10000000u) >> 24;
        if (i >= regions.size()) return false;
        return platform::safe_read(regions[i].first + (address & 0xffffff), out, size);
      }
      return platform::safe_read(address, out, size);
    }
  };

  void user_sgprs(gpu::ShaderStage stage, Tokens& tokens, uint32_t* s, uint32_t base_vertex,
                  const std::vector<uint8_t>& vb_table) {
    if (stage == gpu::ShaderStage::Vertex) {
      const uint32_t t = tokens.add(reinterpret_cast<uint64_t>(vb_table.data()), vb_table.size());
      s[0] = t;
      s[1] = 0;
      s[2] = base_vertex;
    }
    const bool cs = stage == gpu::ShaderStage::Compute;
    // Compute: the root parameters from s4 (GTA VI's compute metadata maps
    // its descriptor tables to s4, s5, ...); graphics from s8.
    root_sgprs(cs ? state_.compute_root : state_.root, cs ? state_.compute_params : state_.params, stage, tokens, s,
               cs ? 4 : 8);
  }

  void root_sgprs(const gpu::RootSignature* root, const std::map<uint32_t, RootValue>& params, gpu::ShaderStage stage,
                  Tokens& tokens, uint32_t* s, uint32_t r) {
    if (!root) return;
    for (uint32_t i = 0; i < root->params.size() && r < 100; ++i) {
      const auto& p = root->params[i];
      const uint32_t vis = p.visibility;
      if (stage != gpu::ShaderStage::Compute && vis != 0 && vis != (stage == gpu::ShaderStage::Vertex ? 1u : 5u))
        continue;
      const auto it = params.find(i);
      switch (p.type) {
        case gpu::RootSignature::ParamType::DescriptorTable:
          s[r++] = it != params.end() ? tokens.add(it->second.value, 1 << 20) : 0;
          break;
        case gpu::RootSignature::ParamType::Constants32:
          for (uint32_t k = 0; k < p.num_constants && r < 100; ++k)
            s[r++] = it != params.end() && k < it->second.constants.size() ? it->second.constants[k] : 0;
          break;
        default: {
          const uint64_t va = it != params.end() ? it->second.value : 0;
          s[r++] = static_cast<uint32_t>(va);
          s[r++] = static_cast<uint32_t>(va >> 32);
          break;
        }
      }
    }
    if ((root->flags & 0x800) && state_.heaps[1]) s[r++] = tokens.add(state_.heaps[1], 1 << 20);
  }

  void resolve(gpu::SlotKind kind, const uint32_t* w, uint32_t* out, const Tokens& tokens) {
    switch (kind) {
      case gpu::SlotKind::Buffer: {
        uint64_t base = w[0] | (uint64_t{w[1] & 0xffff} << 32);
        uint32_t stride = (w[1] >> 16) & 0x3fff;
        uint64_t size = stride ? uint64_t{w[2]} * stride : w[2];
        uint32_t v2 = w[2], v3 = w[3];
        // A view the title made with the device's Create*View (the emulator's
        // own descriptor, "XBDS", not an AMD V#): the whole resource, raw.
        uint32_t ours[4];
        std::memcpy(ours, w, sizeof ours);
        if (ours[0] == kDescriptorMagic) {
          const uint64_t object = ours[2] | (uint64_t{ours[3]} << 32);
          const ResourceState* r = resource_of(object);
          base = r ? r->gpu_va : 0;
          size = r ? std::min<uint64_t>(r->size, 0xffffffffu) : 0;
          stride = 0;
          v2 = static_cast<uint32_t>(size);
          v3 = 0x0002403c;  // dst_sel xyzw, 32-bit format (as vertex buffer views)
        }
        uint64_t host = base;
        if (base < 0x100000000ull && base >= 0x10000000u)
          if (const uint64_t h = tokens.host_of(base)) host = h;
        // Guest memory is read through the GPU's copy (the slot holds the
        // guest address); anything else is copied into the ring.
        const uint64_t limited = std::min<uint64_t>(size, 64ull << 20);
        const bool mirrored = size && host && memory_.require(host, limited);
        const uint64_t a = mirrored ? host : (size && host ? upload(host, size) : ring_.address) | gpu::mem::kDirect;
        if (!mirrored && size && host) {
          char where[256] = {};
          if (!describe_virtual_memory(host, where, sizeof where)) describe_xmem_allocation(host, where, sizeof where);
          note(std::format("buffer at {:#x} ({:#x} bytes) is not in the GPU's copy of guest memory - a copy is used, "
                           "shader writes to it are lost ({})", host & ~0xffffffull, size,
                           where[0] ? where : "memory of no known allocation"));
        }
        out[0] = static_cast<uint32_t>(a);
        out[1] = static_cast<uint32_t>(a >> 32);
        out[2] = stride;
        out[3] = v2;
        out[4] = v3;
        break;
      }
      case gpu::SlotKind::Texture: {
        uint8_t bytes[32];
        std::memcpy(bytes, w, 32);
        GpuImage* g = image_from_descriptor(bytes, false);
        if (trace_textures_) {
          // XBEM_TRACE_PS: what the draw samples, and whether guest memory
          // under the texture holds anything (8 probes of 256 bytes).
          const uint64_t base = (uint64_t{w[0]} | (uint64_t{w[1] & 0xff} << 32)) << 8;
          uint64_t nonzero = 0, probed = 0;
          const uint64_t span = g ? uint64_t{g->image.width} * g->image.height : 0;
          uint8_t buf[256];
          for (int k = 0; k < 8 && base; ++k)
            if (platform::safe_read(base + span / 8 * k, buf, sizeof buf)) {
              probed += sizeof buf;
              for (uint8_t b : buf) nonzero += b != 0;
            }
          XBEM_INFO("gpu", "  texture T# {:08x} {:08x} {:08x} {:08x}: base {:#x} -> {}; guest memory {} of {} probed "
                    "bytes non-zero", w[0], w[1], w[2], w[3], base,
                    g ? std::format("{}x{} vk format {}, {}, index {}", g->image.width, g->image.height,
                                    static_cast<int>(g->image.format), g->rendered ? "GPU-written" : "from memory",
                                    g->texture_index)
                      : std::string("unresolved"),
                    nonzero, probed);
        }
        out[0] = g ? g->texture_index : 0;
        out[1] = g ? (g->image.width | (g->image.height << 16)) : 0x00010001;
        out[2] = g ? g->array_index : 0;
        out[3] = g ? g->volume_index : 0;
        break;
      }
      case gpu::SlotKind::Image: {
        uint8_t bytes[32];
        std::memcpy(bytes, w, 32);
        GpuImage* g = image_from_descriptor(bytes, false);
        if (g && !g->storage_index && !g->storage_array_index && !g->storage_volume_index)
          note(std::format("UAV image {}x{} (format {}) cannot be a storage image - writes are dropped", g->image.width,
                           g->image.height, static_cast<int>(g->image.format)));
        out[0] = out[1] = g ? g->storage_index : 0;
        out[2] = g ? g->storage_array_index : 0;
        out[3] = g ? g->storage_volume_index : 0;
        if (g) {
          g->rendered = true;  // the GPU holds its contents now
          g->last_render = ++render_serial_;
          written_.push_back(g);
        }
        break;
      }
      case gpu::SlotKind::Sampler: {
        // S#: clamp mode in dword0 bits 0-2, xy mag filter in dword2 bits 20-21.
        const uint32_t clamp = w[0] & 7, mag = (w[2] >> 20) & 3;
        out[0] = (mag ? 1u : 0u) | (clamp == 0 ? 2u : 0u) | (clamp == 1 ? 4u : 0u);
        break;
      }
      default: break;
    }
  }

  void begin_rendering() {
    const uint32_t n = bound_slots();
    if (rendering_ && current_count_ == n && current_depth_ == state_.depth &&
        std::equal(state_.targets, state_.targets + n, current_targets_))
      return;
    end_rendering();
    VkRenderingAttachmentInfoKHR color[8]{};
    for (uint32_t i = 0; i < n; ++i) {
      color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
      GpuImage* g = n == 1 ? state_.target : state_.targets[i];
      color[i].imageView = g ? g->image.view : VK_NULL_HANDLE;
      color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      current_targets_[i] = state_.targets[i];
    }
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    const GpuImage* frame = state_.target ? state_.target : state_.depth;
    ri.renderArea = {{0, 0}, {frame->image.width, frame->image.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = n;
    ri.pColorAttachments = color;
    VkRenderingAttachmentInfoKHR depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    if (state_.depth) {
      depth.imageView = state_.depth->image.view;
      depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      ri.pDepthAttachment = &depth;
    }
    core_.fn().vkCmdBeginRendering(cmd_, &ri);
    rendering_ = true;
    current_count_ = n;
    current_depth_ = state_.depth;
  }

  // The DXIL clear pipeline (device slot 86): the colour is the float4 its
  // root parameter 0 (a root SRV) points at; the fill covers the scissor.
  void stream_clear() {
    float rgba[4] = {0, 0, 0, 1};
    const auto it = state_.params.find(0);
    const uint64_t va = it != state_.params.end() ? it->second.value : 0;
    if (!va || !platform::safe_read(va, rgba, sizeof rgba)) return skip("clear pipeline without a colour");
    static int logged = 0;
    if (logged++ < 4) {
      uint32_t raw[16] = {};
      platform::safe_read(va, raw, sizeof raw);
      std::string w;
      for (uint32_t x : raw) w += std::format(" {:08x}", x);
      XBEM_INFO("gpu", "clear pipeline: root parameter 0 at {:#x}:{}", va, w);
    }
    begin_rendering();
    VkClearAttachment ca{};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    std::memcpy(ca.clearValue.color.float32, rgba, sizeof rgba);
    VkClearRect rect{};
    rect.rect = state_.has_scissor ? state_.scissor
                                   : VkRect2D{{0, 0}, {state_.target->image.width, state_.target->image.height}};
    rect.rect.extent.width = std::min(rect.rect.extent.width, state_.target->image.width - std::min<uint32_t>(rect.rect.offset.x, state_.target->image.width));
    rect.rect.extent.height = std::min(rect.rect.extent.height, state_.target->image.height - std::min<uint32_t>(rect.rect.offset.y, state_.target->image.height));
    rect.layerCount = 1;
    if (rect.rect.extent.width && rect.rect.extent.height) core_.fn().vkCmdClearAttachments(cmd_, 1, &ca, 1, &rect);
    state_.target->rendered = true;
    state_.target->last_render = ++render_serial_;
    ++draws_;
  }

  void draw(uint32_t count, uint32_t instances, uint32_t start, uint32_t start_instance, bool indexed,
            int32_t base_vertex) {
    GraphicsPipeline* p = state_.pipeline;
    if (!p) return skip(std::format("pipeline {} not from slot 57/55", state_.pipeline_name));
    if (!state_.target && !state_.depth) return skip("no render target");
    if (p->stream_clear) {
      if (!state_.target) return skip("clear without a render target");
      return stream_clear();
    }
    if (p->vs_code.empty()) {
      note(std::format("pipeline {} (vs {:016x}, ps {:016x}, {} bytes of pixel code) has no vertex shader code - its "
                       "draws are skipped", state_.pipeline_name, p->vs_hash, p->ps_hash, p->ps_code.size()));
      return skip("pipeline without shader code");
    }
    // Without a pixel shader: a depth-only draw (needs a depth target).
    if (p->ps_code.empty() && !state_.depth) return skip("pipeline without a pixel shader, no depth target");
    ShaderEntry* vs = shader(p->vs_code, p->vs_hash, gpu::ShaderStage::Vertex);
    ShaderEntry* ps = p->ps_code.empty() ? nullptr : shader(p->ps_code, p->ps_hash, gpu::ShaderStage::Pixel);
    if (!vs->module) return skip(std::format("vs {:016x} not translated", p->vs_hash));
    if (ps && !ps->module) return skip(std::format("ps {:016x} not translated", p->ps_hash));
    GpuImage* frame = state_.target ? state_.target : state_.depth;
    const bool rect = state_.topology == 0x11;
    const bool strip = state_.topology == 5;
    if (state_.topology != 4 && !rect && !strip) {
      note(std::format("topology {:#x} not supported - draws skipped", state_.topology));
      return skip(std::format("topology {:#x}", state_.topology));
    }
    bool building = false;
    VkPipeline pipe = pipeline(*p, *vs, ps, rect ? 1 : strip ? 2 : 0, building);
    if (!pipe) return skip(building ? "pipeline being built" : "pipeline creation failed");
    // Vertex buffer table: one V# per view (base, stride, records).
    std::vector<uint8_t> vb_table(std::max<size_t>(state_.vertex_buffers.size(), 16));
    for (size_t i = 0; i + 16 <= state_.vertex_buffers.size(); i += 16) {
      uint64_t va;
      uint32_t size, stride;
      std::memcpy(&va, state_.vertex_buffers.data() + i, 8);
      std::memcpy(&size, state_.vertex_buffers.data() + i + 8, 4);
      std::memcpy(&stride, state_.vertex_buffers.data() + i + 12, 4);
      const uint32_t v[4] = {static_cast<uint32_t>(va), static_cast<uint32_t>(va >> 32) | ((stride & 0x3fff) << 16),
                             stride ? size / stride : size, 0x0002403c};
      std::memcpy(vb_table.data() + i, v, 16);
    }
    static const uint64_t trace_reads_vs = [] {
      const char* t = std::getenv("XBEM_GPU_TRACE_READS");
      return t ? std::strtoull(t, nullptr, 16) : 0ull;
    }();
    trace_reads_ = trace_reads_vs == p->vs_hash && reads_traced_++ < 2;
    Tokens tokens;
    tokens.windows = &windows_;
    std::vector<uint32_t> words[2];
    gpu::TranslatedShader* stages[2] = {&vs->shader, ps ? &ps->shader : nullptr};
    for (int k = 0; k < 2; ++k) {
      if (!stages[k]) continue;
      gpu::ScalarRun run;
      user_sgprs(k == 0 ? gpu::ShaderStage::Vertex : gpu::ShaderStage::Pixel, tokens, run.sgpr,
                 indexed ? static_cast<uint32_t>(base_vertex) : start, vb_table);
      run.read = [&](uint64_t address, void* out, size_t size) {
        const bool ok = tokens.read(address, out, size);
        if (trace_reads_) {
          std::string w;
          for (size_t i = 0; i < size / 4 && i < 8; ++i) w += std::format(" {:08x}", static_cast<const uint32_t*>(out)[i]);
          XBEM_INFO("gpu", "  {} read {:#x} ({} bytes){}:{}", k == 0 ? "vs" : "ps", address, size, ok ? "" : " FAILED", w);
        }
        return ok;
      };
      run.resolve = [&](gpu::SlotKind kind, const uint32_t* w, uint32_t* out) { resolve(kind, w, out, tokens); };
      static const uint64_t trace_ps = [] {
        const char* t = std::getenv("XBEM_TRACE_PS");
        return t ? std::strtoull(t, nullptr, 16) : 0ull;
      }();
      static uint64_t ps_draws = 0;  // traced at draws 1, 2, 4, 8, ... of that shader
      trace_textures_ = k == 1 && trace_ps && trace_ps == p->ps_hash && std::has_single_bit(++ps_draws);
      if (trace_textures_) XBEM_INFO("gpu", "draw #{} with ps {:016x}:", ps_draws, p->ps_hash);
      const bool ok_run = gpu::run_scalar(*stages[k], run, words[k]);
      trace_textures_ = false;
      if (!ok_run) {
        note(std::format("scalar run of {:016x}: {}", k == 0 ? p->vs_hash : p->ps_hash, run.error));
        return skip(std::format("scalar run of {:016x}", k == 0 ? p->vs_hash : p->ps_hash));
      }
    }
    static const int trace = [] {
      const char* t = std::getenv("XBEM_GPU_TRACE");
      return t ? std::atoi(t) : 0;
    }();
    static const uint64_t trace_from = [] {
      const char* t = std::getenv("XBEM_GPU_TRACE_FROM");
      return t ? std::strtoull(t, nullptr, 0) : 0ull;
    }();
    static const uint64_t trace_vs = [] {
      const char* t = std::getenv("XBEM_GPU_TRACE_VS");
      return t ? std::strtoull(t, nullptr, 16) : 0ull;
    }();
    if (traced_ < trace && draws_ >= trace_from && (!trace_vs || trace_vs == p->vs_hash)) {
      ++traced_;
      std::string w[2];
      for (int k = 0; k < 2; ++k)
        for (uint32_t x : words[k]) w[k] += std::format(" {:08x}", x);
      XBEM_INFO("gpu", "draw {}: vs {:016x} ps {:016x} {} {} (start {}, base {}), topology {:#x}, target {}x{}, "
                "viewport {} {} {}x{}\n  vs words:{}\n  ps words:{}",
                draws_, p->vs_hash, p->ps_hash, indexed ? "indexed" : "vertices", count, start, base_vertex,
                state_.topology, frame->image.width, frame->image.height, state_.viewport.x,
                state_.viewport.y, state_.viewport.width, state_.viewport.height, w[0], w[1]);
    }
    VkDeviceAddress data[2];
    for (int k = 0; k < 2; ++k) {
      const VkDeviceSize n = std::max<VkDeviceSize>(words[k].size() * 4, 16);
      const VkDeviceSize at = ring_alloc(n, 16);
      std::memset(static_cast<uint8_t*>(ring_.mapped) + at, 0, n);
      std::memcpy(static_cast<uint8_t*>(ring_.mapped) + at, words[k].data(), words[k].size() * 4);
      data[k] = ring_.address + at;
    }
    const auto& f = core_.fn();
    // Index data for this draw.
    VkDeviceSize index_at = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    if (indexed) {
      uint64_t va;
      uint32_t size, format;
      std::memcpy(&va, state_.index_buffer, 8);
      std::memcpy(&size, state_.index_buffer + 8, 4);
      std::memcpy(&format, state_.index_buffer + 12, 4);
      const uint32_t bytes = format == 42 ? 4 : 2;
      index_type = bytes == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      const uint64_t from = uint64_t{start} * bytes, n = uint64_t{count} * bytes;
      if (!va || from + n > size) {
        ++skipped_;
        return;
      }
      index_at = ring_alloc(n + 4, 4);
      if (!platform::safe_read(va + from, static_cast<uint8_t*>(ring_.mapped) + index_at, n)) {
        ++skipped_;
        return;
      }
    }
    VkViewport vp = state_.viewport;
    if (vp.width <= 0 || vp.height <= 0)
      vp = {0, 0, static_cast<float>(frame->image.width), static_cast<float>(frame->image.height), 0, 1};
    VkRect2D sc = state_.has_scissor ? state_.scissor
                                     : VkRect2D{{0, 0}, {frame->image.width, frame->image.height}};
    const VkDeviceAddress table = memory_.table_address();
    // Wave-mode shaders are slow: an instanced draw of them (GTA VI's light
    // volumes: 459 instances over 2560x1440, 34 million pixels) can take
    // longer than Windows' 2 s GPU timeout alone. Such draws run in parts
    // of a few instances, each part its own submission.
    const uint32_t total = std::max(instances, 1u);
    const bool slow = (ps && ps->shader.wave) || vs->shader.wave;
    const uint32_t part = slow ? wave_instances_per_submit() : total;
    for (uint32_t first = 0; first < total; first += part) {
      if (first) {
        // The draw's data (push constants, indices) stays in the ring.
        const VkDeviceSize keep = ring_offset_;
        end_rendering();
        submit();
        begin();
        ring_offset_ = keep;
      }
      const uint32_t n = std::min(part, total - first);
      begin_rendering();
      f.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      f.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
      f.vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 8, &data[0]);
      f.vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 8, 8, &data[1]);
      f.vkCmdPushConstants(cmd_, layout_, kGraphicsStages, 16, 8, &table);
      f.vkCmdSetViewport(cmd_, 0, 1, &vp);
      f.vkCmdSetScissor(cmd_, 0, 1, &sc);
      if (indexed) {
        f.vkCmdBindIndexBuffer(cmd_, ring_.buffer, index_at, index_type);
        f.vkCmdDrawIndexed(cmd_, count, n, 0, 0, start_instance + first);
      } else {
        f.vkCmdDraw(cmd_, count, n, 0, start_instance + first);
      }
    }
    frame->rendered = true;
    frame->last_render = ++render_serial_;
    for (uint32_t i = 0; i < 8 && ps; ++i)
      if (state_.targets[i] && (ps->shader.color_targets & (1u << i))) {
        state_.targets[i]->rendered = true;
        state_.targets[i]->last_render = render_serial_;
      }
    ++draws_;
    ++submitted_draws_;
    last_draw_ = std::format("vs {:016x}{} ps {:016x}, {} {} x {} instances, viewport {}x{}, target {}x{}", p->vs_hash,
                             vs->shader.wave ? "(wave)" : "", p->ps_hash, count, indexed ? "indices" : "vertices",
                             std::max(instances, 1u), vp.width, vp.height, frame->image.width, frame->image.height);
    if (ps && submitted_ps_.size() < 32) submitted_ps_[p->ps_hash] = ps->shader.wave;
    if (is_display_target()) {
      display_ = state_.target;
      display_format_ = 28;
    }
    // Wave-mode pixel shaders can run for tens of milliseconds per draw:
    // dozens of them in one submission pass Windows' 2 s GPU timeout and
    // the device is removed. Run what is recorded every few such draws.
    if (ps && ps->shader.wave && ++wave_draws_ >= wave_draws_per_submit()) {
      end_rendering();
      submit();
      begin();
    }
  }

  static uint32_t wave_instances_per_submit() {
    static const uint32_t n = [] {
      const char* v = std::getenv("XBEM_WAVE_INSTANCES_PER_SUBMIT");
      const uint32_t x = v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 32u;
      return x ? x : 1u;
    }();
    return n;
  }

  static uint32_t wave_draws_per_submit() {
    static const uint32_t n = [] {
      const char* v = std::getenv("XBEM_WAVE_DRAWS_PER_SUBMIT");
      const uint32_t x = v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 2u;
      return x ? x : 1u;
    }();
    return n;
  }

  // ---------------------------------------------------------- compute

  // Compute shaders are prepared on worker threads (translation, GLSL ->
  // SPIR-V, pipeline creation - seconds for the largest programs): the
  // title's render thread must not stall on them (its watchdog ends the run
  // when the main loop stops for 30 s). Dispatches of a shader still being
  // prepared are skipped.
  ShaderEntry* compute_shader(const ComputePipeline& p) {
    auto it = shaders_.find(p.cs_hash);
    if (it != shaders_.end()) return it->second.get();
    auto e = std::make_unique<ShaderEntry>();
    ShaderEntry* raw = e.get();
    shaders_[p.cs_hash] = std::move(e);
    struct Job {
      uint64_t hash;
      std::vector<uint8_t> code, state_a;
    };
    auto job = std::make_shared<Job>(Job{p.cs_hash, p.code, p.state_a});
    compile_pool().run([this, raw, job] {
      const gpu::ComputeInfo info = gpu::ComputeInfo::from_state(job->state_a.data(), job->state_a.size());
      const auto t_translate = std::chrono::steady_clock::now();
      raw->shader = gpu::translate_shader(job->code.data(), job->code.size(), gpu::ShaderStage::Compute, &info);
      add_us(translate_us_, t_translate);
      {
        std::lock_guard lock(tags_mutex_);
        shader_tags_[raw->shader.tag] = job->hash;
      }
      if (!raw->shader.ok) raw->error = raw->shader.error;
      else raw->module = compile(raw->shader.glsl, "comp", raw->error);
      std::string().swap(raw->shader.glsl);
      if (raw->module) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                    raw->module, "main", nullptr};
        ci.layout = compute_layout_;
        const auto t_pipe = std::chrono::steady_clock::now();
        const VkResult made = core_.fn().vkCreateComputePipelines(core_.device(), pipeline_cache_, 1, &ci, nullptr,
                                                                  &raw->pipeline);
        add_us(pipeline_us_, t_pipe);
        ++pipelines_created_;
        if (made != VK_SUCCESS) {
          raw->pipeline = VK_NULL_HANDLE;
          raw->error = "compute pipeline creation failed";
        }
      }
      if (raw->pipeline && !job->code.empty()) {
        raw->code = core_.create_buffer((job->code.size() + 15) & ~size_t{15}, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (raw->code.mapped) std::memcpy(raw->code.mapped, job->code.data(), job->code.size());
        raw->code_bytes = job->code;
      }
      if (!raw->error.empty()) XBEM_WARN("gpu", "compute shader {:016x}: {}", job->hash, raw->error);
      raw->state.store(raw->pipeline ? 2 : 3, std::memory_order_release);
    });
    return raw;
  }

  // Buffers the recorded (not yet run) dispatches write: [start, end) guest
  // addresses from their written Buffer slots.
  std::vector<std::pair<uint64_t, uint64_t>> gpu_output_;
  uint64_t flushes_for_gpu_output_ = 0;

  bool reads_gpu_output(uint64_t address, size_t size) const {
    for (const auto& [a, b] : gpu_output_)
      if (address < b && address + size > a) return true;
    return false;
  }

  void note_gpu_output(const gpu::TranslatedShader& shader, const std::vector<uint32_t>& words) {
    for (const gpu::Slot& slot : shader.slots) {
      if (slot.kind != gpu::SlotKind::Buffer || !slot.written || slot.offset + 4 > words.size()) continue;
      const uint64_t address = words[slot.offset] | (uint64_t{words[slot.offset + 1]} << 32);
      if (!address || (address >> 63)) continue;  // nothing, or data of the upload ring
      const uint64_t stride = words[slot.offset + 2], records = words[slot.offset + 3];
      uint64_t bytes = stride ? records * stride : records;
      if (!bytes || bytes > (uint64_t{1} << 32)) bytes = uint64_t{1} << 20;
      gpu_output_.emplace_back(address, address + bytes);
    }
  }

  void dispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (std::getenv("XBEM_NO_COMPUTE")) return;
    ComputePipeline* p = state_.compute;
    if (!p) return skip_dispatch("no compute pipeline");
    if (!x || !y || !z) return;
    ShaderEntry* cs = compute_shader(*p);
    const int state = cs->state.load(std::memory_order_acquire);
    if (state < 2) return skip_dispatch("compute shader being prepared");
    if (state != 2) return skip_dispatch(std::format("cs {:016x} not translated", p->cs_hash));
    VkPipeline pipe = cs->pipeline;
    end_rendering();
    Tokens tokens;
    tokens.windows = &windows_;
    gpu::ScalarRun run;
    static const std::vector<uint8_t> no_vb;
    user_sgprs(gpu::ShaderStage::Compute, tokens, run.sgpr, 0, no_vb);
    // s_getpc_b64: the program's copy on the GPU, as a direct device address
    // (bit 63); the CPU reads such addresses from the program bytes.
    if (cs->code.mapped) run.code_address = cs->code.address | (uint64_t{1} << 63);
    // A scalar load from a buffer an earlier dispatch of this command buffer
    // writes (GPU-driven counts and arguments) would read guest memory before
    // that dispatch ran: run what is recorded first (its results are written
    // back) and run the scalar part again.
    bool stale = false;
    run.read = [&](uint64_t address, void* out, size_t size) {
      if (run.code_address && address >= run.code_address && address - run.code_address + size <= cs->code_bytes.size()) {
        std::memcpy(out, cs->code_bytes.data() + (address - run.code_address), size);
        return true;
      }
      if (!stale && reads_gpu_output(address, size)) stale = true;
      return tokens.read(address, out, size);
    };
    run.resolve = [&](gpu::SlotKind kind, const uint32_t* w, uint32_t* out) { resolve(kind, w, out, tokens); };
    std::vector<uint32_t> words;
    written_.clear();
    if (!gpu::run_scalar(cs->shader, run, words)) return skip_dispatch("scalar run");
    if (stale) {
      ++flushes_for_gpu_output_;
      submit();
      begin();
      stale = false;
      written_.clear();
      if (!gpu::run_scalar(cs->shader, run, words)) return skip_dispatch("scalar run");
    }
    note_gpu_output(cs->shader, words);
    const VkDeviceSize n = std::max<VkDeviceSize>(words.size() * 4, 16);
    const VkDeviceSize at = ring_alloc(n, 16);
    std::memset(static_cast<uint8_t*>(ring_.mapped) + at, 0, n);
    std::memcpy(static_cast<uint8_t*>(ring_.mapped) + at, words.data(), words.size() * 4);
    const VkDeviceAddress data = ring_.address + at;
    const auto& f = core_.fn();
    f.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    f.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_, 0, 1, &set_, 0, nullptr);
    const VkDeviceAddress push[2] = {data, memory_.table_address()};
    f.vkCmdPushConstants(cmd_, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
    f.vkCmdDispatch(cmd_, x, y, z);
    submitted_cs_.insert(p->cs_hash);
    ++submitted_dispatches_;
    full_barrier();
    ++dispatches_;
    static const int trace = [] {
      const char* t = std::getenv("XBEM_GPU_TRACE_CS");
      return t ? std::atoi(t) : 0;
    }();
    if (dispatches_traced_ < trace) {
      ++dispatches_traced_;
      std::string w;
      for (uint32_t v : words) w += std::format(" {:08x}", v);
      XBEM_INFO("gpu", "dispatch {}: cs {:016x} ({}, {}, {}), {} images written\n  words:{}", dispatches_, p->cs_hash, x, y,
                z, written_.size(), w);
    }
  }

  void skip_dispatch(const std::string& reason) {
    ++dispatches_skipped_;
    ++skip_reasons_["dispatch: " + reason];
  }

  // Development: what every rendered image holds (lit sampled pixels).
  void image_stats() {
    for (auto& [r, g] : images_) {
      if (!g->rendered || g->image.format != VK_FORMAT_R8G8B8A8_UNORM) continue;
      const uint32_t w = g->image.width, h = g->image.height;
      Buffer rb = core_.create_buffer(VkDeviceSize{w} * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      if (!rb.mapped) continue;
      begin();
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {w, h, 1};
      core_.fn().vkCmdCopyImageToBuffer(cmd_, g->image.image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &region);
      submit();
      const auto* px = static_cast<const uint32_t*>(rb.mapped);
      uint64_t lit = 0, alpha = 0;
      uint32_t sample = 0;
      for (uint64_t i = 0; i < uint64_t{w} * h; i += 97) {
        if (px[i] & 0x00ffffff) ++lit, sample = px[i];
        if (px[i] >> 24) ++alpha;
      }
      XBEM_INFO("gpu", "  image (texture {}) {}x{}: {} lit, {} with alpha, e.g. {:#010x}", g->texture_index, w, h,
                lit, alpha, sample);
      core_.fn().vkDestroyBuffer(core_.device(), rb.buffer, nullptr);
      core_.fn().vkFreeMemory(core_.device(), rb.memory, nullptr);
    }
  }

  bool is_display_target() {
    for (const auto& [r, g] : images_)
      if (g.get() == state_.target) return is_display_resource(r);
    return false;
  }

  std::mutex mutex_;
  Core core_;
  Buffer ring_;
  // What the command buffer being recorded runs (for slow / failed submissions).
  std::set<uint64_t> submitted_cs_;
  std::map<uint64_t, bool> submitted_ps_;  // pixel shaders of the submission's draws (wave mode or not)
  uint32_t submitted_draws_ = 0, submitted_dispatches_ = 0, wave_draws_ = 0;
  std::string last_draw_;
  bool trace_textures_ = false;
  uint64_t gpu_wait_us_ = 0, gpu_submissions_ = 0;
  std::mutex tags_mutex_;
  std::map<uint32_t, uint64_t> shader_tags_;  // translated code tag -> compute shader hash
  std::chrono::steady_clock::time_point stats_since_{};
  gpu::vk::GuestMemory memory_;
  uint64_t descriptors_ = 0, descriptor_misses_ = 0, descriptor_table_resets_ = 0;
  std::vector<HeapWindow> windows_;
  uint64_t next_window_ = kWindowStart;
  VkDeviceSize ring_offset_ = 0;
  std::map<std::pair<uint64_t, uint64_t>, VkDeviceAddress> uploads_;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool recording_ = false, rendering_ = false;
  GpuImage* current_targets_[8] = {};
  uint32_t current_count_ = 0;
  GpuImage* current_depth_ = nullptr;
  uint64_t copies_ = 0;

  // Attachment slots of the current pass: up to the last bound target (one
  // when only slot 0 - or a target set by a clear - is bound).
  uint32_t bound_slots() const {
    if (!state_.target) return 0;  // a depth-only pass
    uint32_t n = 0;
    for (uint32_t i = 0; i < state_.target_count && i < 8; ++i)
      if (state_.targets[i]) n = i + 1;
    return std::max<uint32_t>(n, 1);
  }
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE, compute_layout_ = VK_NULL_HANDLE;
  Image scratch_storage_, scratch_storage_volume_;
  IndexPool pools_[6];  // textures, 2D arrays, 3D; storage images, arrays, 3D
  uint64_t image_bytes_ = 0;
  std::map<uint64_t, VkPipeline> compute_pipelines_;
  std::vector<GpuImage*> written_;
  uint64_t dispatches_ = 0, dispatches_skipped_ = 0;
  int dispatches_traced_ = 0;
  VkDescriptorPool dpool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkSampler samplers_[kSamplers] = {};
  Image white_, white_volume_;

  std::unordered_map<const ResourceState*, std::unique_ptr<GpuImage>> images_;
  std::unordered_map<const ResourceState*, std::unique_ptr<GpuImage>> depth_images_;
  std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> shaders_;
  std::map<uint32_t, VkShaderModule> rect_gs_;
  // Graphics pipelines: built on the compile pool (seconds each on dzn for
  // large wave-mode shaders); draws are skipped until theirs is ready.
  struct PendingPipeline {
    std::atomic<int> state{0};  // 0 building, 1 ready, 2 failed
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  std::map<PipelineKey, std::shared_ptr<PendingPipeline>> pipelines_;
  fs::path cache_dir_;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  std::atomic<uint64_t> pipelines_created_{0}, pipelines_saved_{0};
  std::atomic<uint64_t> graphics_pipelines_{0}, graphics_pipeline_us_{0};
  std::atomic<uint64_t> translate_us_{0}, glslang_us_{0}, pipeline_us_{0}, glslang_runs_{0};
  State state_;
  GpuImage* display_ = nullptr;
  uint32_t display_format_ = 28;
  Buffer readback_[2];
  Image present_rgba_;  // the display image converted to RGBA8 (other display formats)
  int readback_index_ = 0;
  uint64_t render_serial_ = 0, draws_ = 0, skipped_ = 0, presents_ = 0;
  std::map<std::string, uint64_t> skip_reasons_;  // why draws were skipped, with counts

  void skip(const std::string& reason) {
    ++skipped_;
    ++skip_reasons_[reason];
  }

  std::string top_skip_reasons() const {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (const auto& [r, n] : skip_reasons_) v.emplace_back(n, r);
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 6; ++i) out += std::format("\n    {} x {}", v[i].first, v[i].second);
    return out;
  }
  int traced_ = 0;
  std::vector<uint8_t> scratch_;
  bool trace_reads_ = false;
  int reads_traced_ = 0;
  std::set<std::string> notes_;
};

Renderer* g_renderer = nullptr;
std::atomic<bool> g_active{false};

}  // namespace

bool render_start() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (const char* off = std::getenv("XBEM_NO_GPU"); off && *off) return;
    if (!platform::window_is_open()) return;  // headless runs (tests, --no-window) record only
    auto* r = new Renderer;
    std::string info;
    if (!r->start(info)) {
      XBEM_WARN("gpu", "no Vulkan renderer: {} - GPU work is recorded only", info);
      delete r;
      return;
    }
    g_renderer = r;
    g_active.store(true);
    XBEM_INFO("gpu", "Vulkan renderer on {}: command lists are executed on the GPU", info);
  });
  return g_active.load();
}

bool render_active() { return g_active.load(std::memory_order_relaxed); }

void render_execute(const std::vector<RecordedCommand>& commands) {
  if (g_renderer) g_renderer->execute(commands);
}

void render_present() {
  if (g_renderer) g_renderer->present();
}

}  // namespace xbem::hle::d3d12

#else  // no Vulkan headers at build time

namespace xbem::hle::d3d12 {
bool render_start() { return false; }
bool render_active() { return false; }
void render_execute(const std::vector<RecordedCommand>&) {}
void render_present() {}
}  // namespace xbem::hle::d3d12

#endif
