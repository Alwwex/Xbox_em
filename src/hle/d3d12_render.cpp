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

#if defined(XBEM_HAVE_VULKAN)

#include <algorithm>
#include <atomic>
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
};

struct PipelineKey {
  uint64_t vs, ps, blend;
  uint32_t format, rect;
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
      b[i].stageFlags = i >= 4 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
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
    next_texture_ = next_array_ = next_volume_ = 1;
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
    next_storage_ = next_storage_array_ = next_storage_volume_ = 1;

    const char* home = std::getenv("HOME");
    cache_dir_ = fs::path(home ? home : "/tmp") / ".cache" / "xbem" / "spirv";
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
    info = core_.device_name();
    return true;
  }

  void execute(const std::vector<RecordedCommand>& commands) {
    std::lock_guard lock(mutex_);
    begin();
    for (const RecordedCommand& c : commands) command(c);
    end_rendering();
    submit();
  }

  void present() {
    std::lock_guard lock(mutex_);
    if (const uint64_t n = presents_ + 1; n >= 64 && (n & (n - 1)) == 0 && memory_.ready()) {
      const auto& m = memory_.stats();
      XBEM_INFO("gpu", "guest memory copy at present {}: {} pages resident ({} MiB), {} uploaded, {} refreshed, "
                "{} written back, {} evicted, {} unmapped", n, m.resident, m.resident >> 4, m.uploaded, m.refreshed,
                m.written_back, m.evicted, m.unmapped);
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
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    core_.fn().vkCmdCopyImageToBuffer(cmd_, g.image.image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &region);
    submit();
    platform::window_set_surface(rb.mapped, w, h, uint64_t{w} * 4, display_format_);
    ++presents_;
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
    f.vkQueueSubmit(core_.queue(), 1, &si, fence_);
    f.vkWaitForFences(core_.device(), 1, &fence_, VK_TRUE, UINT64_MAX);
    f.vkResetFences(core_.device(), 1, &fence_);
    recording_ = false;
    memory_.end_submission();
    ring_offset_ = 0;
    uploads_.clear();
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
    if (!volume && next_texture_ < kMaxTextures) {
      g->texture_index = next_texture_++;
      bind_texture(g->texture_index, g->image.view);
    }
    if (!volume && g->image.array_view && next_array_ < kMaxArrayTextures) {
      g->array_index = next_array_++;
      bind_texture(g->array_index, g->image.array_view, 2);
    }
    if (volume && next_volume_ < kMaxVolumeTextures) {
      g->volume_index = next_volume_++;
      bind_texture(g->volume_index, g->image.view, 3);
    }
    if (storage) {
      if (!volume && next_storage_ < kMaxStorage) bind_storage(g->storage_index = next_storage_++, g->image.view, 4);
      if (!volume && g->image.array_view && next_storage_array_ < kMaxStorageArrays)
        bind_storage(g->storage_array_index = next_storage_array_++, g->image.array_view, 5);
      if (volume && next_storage_volume_ < kMaxStorageVolumes)
        bind_storage(g->storage_volume_index = next_storage_volume_++, g->image.view, 6);
    }
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
    e->shader = gpu::translate_shader(code.data(), code.size(), stage);
    if (!e->shader.ok) e->error = e->shader.error;
    else e->module = compile(e->shader.glsl, stage == gpu::ShaderStage::Vertex ? "vert" : "frag", e->error);
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
      if (std::system(cmd.c_str()) != 0) {
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
  VkPipeline pipeline(const GraphicsPipeline& p, ShaderEntry& vs, ShaderEntry& ps, VkFormat format, uint32_t mode) {
    const bool rect = mode == 1;
    // Blend: the variant's D3D12_BLEND_DESC (render target 0 at dword 3), else alpha blending.
    uint32_t blend[10] = {1, 0, 5, 6, 1, 2, 6, 1, 4, 0xf};
    if (p.blend_override) std::memcpy(blend, p.blend + 12, sizeof blend);
    const PipelineKey key{p.vs_hash, p.ps_hash, fnv1a(blend, sizeof blend), static_cast<uint32_t>(format), mode};
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t count = 0;
    stages[count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                       vs.module, "main", nullptr};
    if (rect) {
      VkShaderModule gs = rect_geometry(vs.shader.outputs);
      if (!gs) return pipelines_[key] = VK_NULL_HANDLE;
      stages[count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT,
                         gs, "main", nullptr};
    }
    stages[count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                       ps.module, "main", nullptr};
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
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = blend[0] ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = factor(blend[2]);
    att.dstColorBlendFactor = factor(blend[3]);
    att.colorBlendOp = op(blend[4]);
    att.srcAlphaBlendFactor = factor(blend[5]);
    att.dstAlphaBlendFactor = factor(blend[6]);
    att.alphaBlendOp = op(blend[7]);
    att.colorWriteMask = blend[9] & 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfoKHR ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.pNext = &ri;
    gci.stageCount = count;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy;
    gci.layout = layout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (core_.fn().vkCreateGraphicsPipelines(core_.device(), VK_NULL_HANDLE, 1, &gci, nullptr, &pipe) != VK_SUCCESS)
      XBEM_WARN("gpu", "pipeline vs {:016x} ps {:016x}: vkCreateGraphicsPipelines failed", p.vs_hash, p.ps_hash);
    pipelines_[key] = pipe;
    return pipe;
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
    GpuImage* target = nullptr;
    std::vector<uint8_t> vertex_buffers;  // D3D12_VERTEX_BUFFER_VIEW x n
    uint8_t index_buffer[16] = {};
    uint64_t heaps[2] = {};  // resource heap, sampler heap starts
  };

  void note(const std::string& what) {
    if (notes_.insert(what).second && notes_.size() < 64) XBEM_WARN("gpu", "{}", what);
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
        if (!p) note(std::format("pipeline {:#x} is not a known graphics pipeline - its draws are skipped", a[0]));
        break;
      }
      case 32:
        for (size_t i = 0; i + 8 <= c.blob.size(); i += 8) {
          uint64_t heap;
          std::memcpy(&heap, c.blob.data() + i, 8);
          uint64_t start;
          uint32_t type;
          if (descriptor_heap_of(heap, start, type) && type <= 1) state_.heaps[type] = start;
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
        state_.target = c.blob.size() >= kDescriptorSize && u32(0) ? image_from_descriptor(c.blob.data(), true) : nullptr;
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
  struct Tokens {
    std::vector<std::pair<uint64_t, uint64_t>> regions;  // host address, size
    uint32_t add(uint64_t host, uint64_t size) {
      regions.emplace_back(host, size);
      return 0x10000000u + static_cast<uint32_t>(regions.size() - 1) * 0x01000000u;
    }
    bool read(uint64_t address, void* out, size_t size) const {
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
        const uint64_t base = w[0] | (uint64_t{w[1] & 0xffff} << 32);
        const uint32_t stride = (w[1] >> 16) & 0x3fff;
        const uint64_t size = stride ? uint64_t{w[2]} * stride : w[2];
        uint64_t host = base;
        if (base < 0x100000000ull && base >= 0x10000000u) {
          const uint64_t i = (base - 0x10000000u) >> 24;
          if (i < tokens.regions.size()) host = tokens.regions[i].first + (base & 0xffffff);
        }
        // Guest memory is read through the GPU's copy (the slot holds the
        // guest address); anything else is copied into the ring.
        const uint64_t limited = std::min<uint64_t>(size, 64ull << 20);
        const uint64_t a = size && host && memory_.require(host, limited) ? host
                           : (size && host ? upload(host, size) : ring_.address) | gpu::mem::kDirect;
        out[0] = static_cast<uint32_t>(a);
        out[1] = static_cast<uint32_t>(a >> 32);
        out[2] = stride;
        out[3] = w[2];
        out[4] = w[3];
        break;
      }
      case gpu::SlotKind::Texture: {
        uint8_t bytes[32];
        std::memcpy(bytes, w, 32);
        GpuImage* g = image_from_descriptor(bytes, false);
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
    if (rendering_ && current_target_ == state_.target) return;
    end_rendering();
    VkRenderingAttachmentInfoKHR color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    color.imageView = state_.target->image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, {state_.target->image.width, state_.target->image.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    core_.fn().vkCmdBeginRendering(cmd_, &ri);
    rendering_ = true;
    current_target_ = state_.target;
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
    if (!state_.target) return skip("no render target");
    if (p->stream_clear) return stream_clear();
    if (p->vs_code.empty() || p->ps_code.empty()) return skip("pipeline without shader code");
    ShaderEntry* vs = shader(p->vs_code, p->vs_hash, gpu::ShaderStage::Vertex);
    ShaderEntry* ps = shader(p->ps_code, p->ps_hash, gpu::ShaderStage::Pixel);
    if (!vs->module) return skip(std::format("vs {:016x} not translated", p->vs_hash));
    if (!ps->module) return skip(std::format("ps {:016x} not translated", p->ps_hash));
    const bool rect = state_.topology == 0x11;
    const bool strip = state_.topology == 5;
    if (state_.topology != 4 && !rect && !strip) {
      note(std::format("topology {:#x} not supported - draws skipped", state_.topology));
      return skip(std::format("topology {:#x}", state_.topology));
    }
    VkPipeline pipe = pipeline(*p, *vs, *ps, state_.target->image.format, rect ? 1 : strip ? 2 : 0);
    if (!pipe) return skip("pipeline creation failed");
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
    std::vector<uint32_t> words[2];
    gpu::TranslatedShader* stages[2] = {&vs->shader, &ps->shader};
    for (int k = 0; k < 2; ++k) {
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
      if (!gpu::run_scalar(*stages[k], run, words[k])) {
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
                state_.topology, state_.target->image.width, state_.target->image.height, state_.viewport.x,
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
    begin_rendering();
    f.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    f.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    f.vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 8, &data[0]);
    f.vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 8, 8, &data[1]);
    const VkDeviceAddress table = memory_.table_address();
    f.vkCmdPushConstants(cmd_, layout_, kGraphicsStages, 16, 8, &table);
    VkViewport vp = state_.viewport;
    if (vp.width <= 0 || vp.height <= 0)
      vp = {0, 0, static_cast<float>(state_.target->image.width), static_cast<float>(state_.target->image.height), 0, 1};
    f.vkCmdSetViewport(cmd_, 0, 1, &vp);
    VkRect2D sc = state_.has_scissor ? state_.scissor
                                     : VkRect2D{{0, 0}, {state_.target->image.width, state_.target->image.height}};
    f.vkCmdSetScissor(cmd_, 0, 1, &sc);
    if (indexed) {
      f.vkCmdBindIndexBuffer(cmd_, ring_.buffer, index_at, index_type);
      f.vkCmdDrawIndexed(cmd_, count, std::max(instances, 1u), 0, 0, start_instance);
    } else {
      f.vkCmdDraw(cmd_, count, std::max(instances, 1u), 0, start_instance);
    }
    state_.target->rendered = true;
    state_.target->last_render = ++render_serial_;
    ++draws_;
    if (is_display_target()) {
      display_ = state_.target;
      display_format_ = 28;
    }
  }

  // ---------------------------------------------------------- compute

  ShaderEntry* compute_shader(const ComputePipeline& p) {
    auto it = shaders_.find(p.cs_hash);
    if (it != shaders_.end()) return it->second.get();
    auto e = std::make_unique<ShaderEntry>();
    const gpu::ComputeInfo info = gpu::ComputeInfo::from_state(p.state_a.data(), p.state_a.size());
    e->shader = gpu::translate_shader(p.code.data(), p.code.size(), gpu::ShaderStage::Compute, &info);
    if (!e->shader.ok) e->error = e->shader.error;
    else e->module = compile(e->shader.glsl, "comp", e->error);
    if (!e->error.empty()) XBEM_WARN("gpu", "compute shader {:016x}: {}", p.cs_hash, e->error);
    ShaderEntry* raw = e.get();
    shaders_[p.cs_hash] = std::move(e);
    return raw;
  }

  VkPipeline compute_pipeline(uint64_t hash, VkShaderModule module) {
    if (auto it = compute_pipelines_.find(hash); it != compute_pipelines_.end()) return it->second;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                "main", nullptr};
    ci.layout = compute_layout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (core_.fn().vkCreateComputePipelines(core_.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) != VK_SUCCESS) {
      note(std::format("compute pipeline {:016x} creation failed", hash));
      pipe = VK_NULL_HANDLE;
    }
    return compute_pipelines_[hash] = pipe;
  }

  void dispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (std::getenv("XBEM_NO_COMPUTE")) return;
    ComputePipeline* p = state_.compute;
    if (!p) return skip_dispatch("no compute pipeline");
    if (!x || !y || !z) return;
    ShaderEntry* cs = compute_shader(*p);
    if (!cs->module) return skip_dispatch(std::format("cs {:016x} not translated", p->cs_hash));
    VkPipeline pipe = compute_pipeline(p->cs_hash, cs->module);
    if (!pipe) return skip_dispatch("pipeline creation failed");
    end_rendering();
    Tokens tokens;
    gpu::ScalarRun run;
    static const std::vector<uint8_t> no_vb;
    user_sgprs(gpu::ShaderStage::Compute, tokens, run.sgpr, 0, no_vb);
    run.read = [&](uint64_t address, void* out, size_t size) { return tokens.read(address, out, size); };
    run.resolve = [&](gpu::SlotKind kind, const uint32_t* w, uint32_t* out) { resolve(kind, w, out, tokens); };
    std::vector<uint32_t> words;
    written_.clear();
    if (!gpu::run_scalar(cs->shader, run, words)) return skip_dispatch("scalar run");
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
  gpu::vk::GuestMemory memory_;
  VkDeviceSize ring_offset_ = 0;
  std::map<std::pair<uint64_t, uint64_t>, VkDeviceAddress> uploads_;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool recording_ = false, rendering_ = false;
  GpuImage* current_target_ = nullptr;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE, compute_layout_ = VK_NULL_HANDLE;
  Image scratch_storage_, scratch_storage_volume_;
  uint32_t next_storage_ = 0, next_storage_array_ = 0, next_storage_volume_ = 0;
  std::map<uint64_t, VkPipeline> compute_pipelines_;
  std::vector<GpuImage*> written_;
  uint64_t dispatches_ = 0, dispatches_skipped_ = 0;
  int dispatches_traced_ = 0;
  VkDescriptorPool dpool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkSampler samplers_[kSamplers] = {};
  Image white_, white_volume_;
  uint32_t next_texture_ = 0, next_array_ = 0, next_volume_ = 0;
  std::unordered_map<const ResourceState*, std::unique_ptr<GpuImage>> images_;
  std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> shaders_;
  std::map<uint32_t, VkShaderModule> rect_gs_;
  std::map<PipelineKey, VkPipeline> pipelines_;
  fs::path cache_dir_;
  State state_;
  GpuImage* display_ = nullptr;
  uint32_t display_format_ = 28;
  Buffer readback_[2];
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
