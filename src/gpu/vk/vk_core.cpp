#include "gpu/vk/vk_core.h"

#include <algorithm>

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>

#include "common/log.h"

namespace xbem::gpu::vk {
namespace {

bool file_exists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

// WSL2: point the loader at a Dozen build in ~/mesa-dzn (docs/GPU.md) unless
// the user chose drivers.
void select_wsl_driver() {
  if (std::getenv("VK_DRIVER_FILES") || std::getenv("VK_ICD_FILENAMES") || !file_exists("/dev/dxg")) return;
  const char* home = std::getenv("HOME");
  if (!home) return;
  const std::string icd = std::string(home) + "/mesa-dzn/share/vulkan/icd.d/dzn_icd.x86_64.json";
  if (file_exists(icd)) setenv("VK_DRIVER_FILES", icd.c_str(), 0);
}

}  // namespace

bool Core::init(std::string& reason) {
  select_wsl_driver();
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    reason = "libvulkan.so.1 is not installed";
    return false;
  }
  fn_.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
  auto create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(fn_.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!create_instance) {
    reason = "no vkCreateInstance";
    return false;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "xbem";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (VkResult r = create_instance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
    reason = std::format("vkCreateInstance failed ({})", static_cast<int>(r));
    return false;
  }
#define XBEM_VK_LOAD_INSTANCE(name) \
  fn_.name = reinterpret_cast<PFN_##name>(fn_.vkGetInstanceProcAddr(instance_, #name));
  XBEM_VK_INSTANCE_FUNCTIONS(XBEM_VK_LOAD_INSTANCE)
#undef XBEM_VK_LOAD_INSTANCE

  uint32_t count = 0;
  fn_.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  fn_.vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  const char* wanted = std::getenv("XBEM_VK_DEVICE");  // substring of the device name
  int best = -1;
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceProperties props{};
    fn_.vkGetPhysicalDeviceProperties(d, &props);
    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                             : 1;
    if (wanted) score = std::strstr(props.deviceName, wanted) ? 10 : 0;
    if (score > best && score > 0) {
      best = score;
      physical_ = d;
      device_name_ = props.deviceName;
    }
  }
  if (!physical_) {
    reason = "no Vulkan device";
    return false;
  }

  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  fn_.vkGetPhysicalDeviceFeatures2(physical_, &f2);
  if (!f12.bufferDeviceAddress || !f12.descriptorIndexing || !f12.runtimeDescriptorArray) {
    reason = std::format("{} lacks buffer device addresses or descriptor indexing", device_name_);
    return false;
  }

  fn_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  fn_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
  queue_family_ = UINT32_MAX;
  for (uint32_t i = 0; i < count; ++i)
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_ = i;
      break;
    }
  if (queue_family_ == UINT32_MAX) {
    reason = "no graphics queue";
    return false;
  }
  fn_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  enable12.bufferDeviceAddress = VK_TRUE;
  enable12.descriptorIndexing = VK_TRUE;
  enable12.runtimeDescriptorArray = VK_TRUE;
  enable12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
  enable12.descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound;
  enable12.descriptorBindingSampledImageUpdateAfterBind = f12.descriptorBindingSampledImageUpdateAfterBind;
  enable12.scalarBlockLayout = f12.scalarBlockLayout;
  enable12.timelineSemaphore = f12.timelineSemaphore;
  VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
  dynamic.dynamicRendering = VK_TRUE;
  enable12.pNext = &dynamic;
  VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  enable.pNext = &enable12;
  enable.features.geometryShader = f2.features.geometryShader;
  enable.features.independentBlend = f2.features.independentBlend;
  enable.features.shaderInt64 = f2.features.shaderInt64;
  enable.features.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics;
  enable.features.vertexPipelineStoresAndAtomics = f2.features.vertexPipelineStoresAndAtomics;
  const char* extensions[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &enable;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = extensions;
  if (VkResult r = fn_.vkCreateDevice(physical_, &dci, nullptr, &device_); r != VK_SUCCESS) {
    reason = std::format("vkCreateDevice on {} failed ({})", device_name_, static_cast<int>(r));
    return false;
  }
#define XBEM_VK_LOAD_DEVICE(name) fn_.name = reinterpret_cast<PFN_##name>(fn_.vkGetDeviceProcAddr(device_, #name));
  XBEM_VK_DEVICE_FUNCTIONS(XBEM_VK_LOAD_DEVICE)
#undef XBEM_VK_LOAD_DEVICE
  if (!fn_.vkCmdBeginRendering)
    fn_.vkCmdBeginRendering =
        reinterpret_cast<PFN_vkCmdBeginRendering>(fn_.vkGetDeviceProcAddr(device_, "vkCmdBeginRenderingKHR"));
  if (!fn_.vkCmdEndRendering)
    fn_.vkCmdEndRendering =
        reinterpret_cast<PFN_vkCmdEndRendering>(fn_.vkGetDeviceProcAddr(device_, "vkCmdEndRenderingKHR"));
  if (!fn_.vkGetBufferDeviceAddress)
    fn_.vkGetBufferDeviceAddress =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(fn_.vkGetDeviceProcAddr(device_, "vkGetBufferDeviceAddressKHR"));
  if (!fn_.vkCmdBeginRendering || !fn_.vkGetBufferDeviceAddress) {
    reason = "dynamic rendering or buffer device addresses unavailable";
    return false;
  }
  fn_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  return true;
}

uint32_t Core::memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const {
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
  return UINT32_MAX;
}

Buffer Core::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (fn_.vkCreateBuffer(device_, &bci, nullptr, &b.buffer) != VK_SUCCESS) return {};
  VkMemoryRequirements req{};
  fn_.vkGetBufferMemoryRequirements(device_, b.buffer, &req);
  const VkMemoryPropertyFlags flags = host_visible
                                          ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                          : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &flags_info;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memory_type(req.memoryTypeBits, flags);
  if (mai.memoryTypeIndex == UINT32_MAX || fn_.vkAllocateMemory(device_, &mai, nullptr, &b.memory) != VK_SUCCESS) {
    fn_.vkDestroyBuffer(device_, b.buffer, nullptr);
    return {};
  }
  fn_.vkBindBufferMemory(device_, b.buffer, b.memory, 0);
  if (host_visible) fn_.vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
  VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  ai.buffer = b.buffer;
  b.address = fn_.vkGetBufferDeviceAddress(device_, &ai);
  return b;
}

Image Core::create_image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage) {
  return create_image(width, height, 1, 1, false, format, usage);
}

Image Core::create_image(uint32_t width, uint32_t height, uint32_t depth, uint32_t layers, bool volume,
                         VkFormat format, VkImageUsageFlags usage) {
  Image img;
  img.format = format;
  img.width = width;
  img.height = height;
  img.depth = volume ? std::max(depth, 1u) : 1;
  img.layers = volume ? 1 : std::max(layers, 1u);
  img.volume = volume;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {width, height, img.depth};
  ici.mipLevels = 1;
  ici.arrayLayers = img.layers;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (fn_.vkCreateImage(device_, &ici, nullptr, &img.image) != VK_SUCCESS) return {};
  VkMemoryRequirements req{};
  fn_.vkGetImageMemoryRequirements(device_, img.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || fn_.vkAllocateMemory(device_, &mai, nullptr, &img.memory) != VK_SUCCESS) {
    fn_.vkDestroyImage(device_, img.image, nullptr);
    return {};
  }
  fn_.vkBindImageMemory(device_, img.image, img.memory, 0);
  const bool is_depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                        format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM;
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = img.image;
  vci.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  vci.subresourceRange = {static_cast<VkImageAspectFlags>(is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                          0, 1, 0, 1};
  fn_.vkCreateImageView(device_, &vci, nullptr, &img.view);
  if (!volume && !is_depth) {
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.subresourceRange.layerCount = img.layers;
    fn_.vkCreateImageView(device_, &vci, nullptr, &img.array_view);
  }
  return img;
}

bool Core::format_supported(VkFormat format, VkFormatFeatureFlags features) const {
  VkFormatProperties props{};
  fn_.vkGetPhysicalDeviceFormatProperties(physical_, format, &props);
  return (props.optimalTilingFeatures & features) == features;
}

}  // namespace xbem::gpu::vk
