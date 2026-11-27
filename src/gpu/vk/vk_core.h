#pragma once

// Vulkan, loaded at run time (libvulkan.so.1): the instance, one device and
// queue, and helpers for buffers, images and one-off submissions. Requires
// Vulkan 1.2 with buffer device addresses and descriptor indexing.
//
// On WSL2 the NVIDIA GPU has no Linux Vulkan driver; Mesa's Dozen driver
// (Vulkan on D3D12) provides one. It is not packaged by Ubuntu, so docs/GPU.md
// explains building it into ~/mesa-dzn; xbem points the loader at that build
// when VK_DRIVER_FILES / VK_ICD_FILENAMES are not set.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace xbem::gpu::vk {

#define XBEM_VK_INSTANCE_FUNCTIONS(X)            \
  X(vkDestroyInstance)                           \
  X(vkEnumeratePhysicalDevices)                  \
  X(vkGetPhysicalDeviceProperties)               \
  X(vkGetPhysicalDeviceFeatures2)                \
  X(vkGetPhysicalDeviceQueueFamilyProperties)    \
  X(vkGetPhysicalDeviceMemoryProperties)         \
  X(vkGetPhysicalDeviceFormatProperties)         \
  X(vkCreateDevice)                              \
  X(vkGetDeviceProcAddr)

#define XBEM_VK_DEVICE_FUNCTIONS(X)              \
  X(vkGetDeviceQueue)                            \
  X(vkDeviceWaitIdle)                            \
  X(vkCreateCommandPool)                         \
  X(vkAllocateCommandBuffers)                    \
  X(vkFreeCommandBuffers)                        \
  X(vkResetCommandPool)                          \
  X(vkBeginCommandBuffer)                        \
  X(vkEndCommandBuffer)                          \
  X(vkQueueSubmit)                               \
  X(vkQueueWaitIdle)                             \
  X(vkCreateFence)                               \
  X(vkWaitForFences)                             \
  X(vkResetFences)                               \
  X(vkCreateBuffer)                              \
  X(vkDestroyBuffer)                             \
  X(vkGetBufferMemoryRequirements)               \
  X(vkBindBufferMemory)                          \
  X(vkGetBufferDeviceAddress)                    \
  X(vkCreateImage)                               \
  X(vkDestroyImage)                              \
  X(vkGetImageMemoryRequirements)                \
  X(vkBindImageMemory)                           \
  X(vkCreateImageView)                           \
  X(vkDestroyImageView)                          \
  X(vkAllocateMemory)                            \
  X(vkFreeMemory)                                \
  X(vkMapMemory)                                 \
  X(vkCreateSampler)                             \
  X(vkCreateShaderModule)                        \
  X(vkDestroyShaderModule)                       \
  X(vkCreateDescriptorSetLayout)                 \
  X(vkCreatePipelineLayout)                      \
  X(vkCreateGraphicsPipelines)                   \
  X(vkCreateComputePipelines)                    \
  X(vkCreatePipelineCache)                       \
  X(vkGetPipelineCacheData)                      \
  X(vkCreateDescriptorPool)                      \
  X(vkAllocateDescriptorSets)                    \
  X(vkUpdateDescriptorSets)                      \
  X(vkCmdPipelineBarrier)                        \
  X(vkCmdCopyBufferToImage)                      \
  X(vkCmdCopyImageToBuffer)                      \
  X(vkCmdBlitImage)                              \
  X(vkCmdCopyBuffer)                             \
  X(vkCmdClearColorImage)                        \
  X(vkCmdClearDepthStencilImage)                 \
  X(vkCmdBeginRendering)                         \
  X(vkCmdEndRendering)                           \
  X(vkCmdBindPipeline)                           \
  X(vkCmdBindDescriptorSets)                     \
  X(vkCmdPushConstants)                          \
  X(vkCmdBindIndexBuffer)                        \
  X(vkCmdSetViewport)                            \
  X(vkCmdSetScissor)                             \
  X(vkCmdDraw)                                   \
  X(vkCmdDispatch)                               \
  X(vkCmdClearAttachments)                       \
  X(vkCmdDrawIndexed)

struct Functions {
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define XBEM_VK_DECLARE(name) PFN_##name name = nullptr;
  XBEM_VK_INSTANCE_FUNCTIONS(XBEM_VK_DECLARE)
  XBEM_VK_DEVICE_FUNCTIONS(XBEM_VK_DECLARE)
#undef XBEM_VK_DECLARE
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;  // host-visible buffers
  VkDeviceAddress address = 0;
  VkDeviceSize size = 0;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  uint32_t depth = 1, layers = 1;  // a 3D image has depth > 1; an array has layers > 1
  bool volume = false;             // VK_IMAGE_TYPE_3D
  VkImageView array_view = VK_NULL_HANDLE;  // 2D_ARRAY view of all layers (2D images)
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

class Core {
 public:
  // Creates the instance and device; false with `reason` when Vulkan cannot
  // be used.
  bool init(std::string& reason);

  const Functions& fn() const { return fn_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return queue_family_; }
  const std::string& device_name() const { return device_name_; }

  Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
  Image create_image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
  // A 2D array (`layers`) or a 3D image (`volume`, `depth`). `view` is a 2D
  // view of layer 0 (2D) or the 3D view; `array_view` a 2D_ARRAY view of all
  // layers (2D only).
  Image create_image(uint32_t width, uint32_t height, uint32_t depth, uint32_t layers, bool volume, VkFormat format,
                     VkImageUsageFlags usage);
  bool format_supported(VkFormat format, VkFormatFeatureFlags features) const;
  uint32_t max_allocations() const { return max_allocations_; }

 private:
  uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const;

  Functions fn_{};
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  uint32_t max_allocations_ = 0;
  VkPhysicalDeviceMemoryProperties memory_{};
  std::string device_name_;
};

}  // namespace xbem::gpu::vk
