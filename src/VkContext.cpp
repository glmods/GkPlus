#include "VkContext.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <volk.h>

#include <cstdio>
#include <vector>

#include "Core.h"
#include "VkInternal.h"

namespace gk {
namespace vulkan {
namespace {

bool Initialized = false;
InitResult Result = InitResult::NoLoader;
std::string Error;
DeviceCaps TheCaps;

VkInstance Instance = VK_NULL_HANDLE;
VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
VkDevice Device = VK_NULL_HANDLE;
VkQueue GraphicsQueue = VK_NULL_HANDLE;
VkDebugUtilsMessengerEXT Messenger = VK_NULL_HANDLE;
uint64_t ValidationErrors = 0;
uint64_t ValidationWarnings = 0;
std::vector<std::string> ValidationLog;

// Validation output has nowhere to go without this: the layer reports through
// VK_EXT_debug_utils, and with no messenger installed its findings are simply discarded. That
// is worth stating because "no validation errors" and "no messenger" look identical.
VKAPI_ATTR VkBool32 VKAPI_CALL
DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data,
              void *) {
  const char *level = "info";
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
    level = "ERROR";
    ++ValidationErrors;
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
    level = "warning";
    ++ValidationWarnings;
  } else {
    return VK_FALSE; // info and verbose are noise at this scale
  }
  const std::string text =
      std::string(level) + ": " +
      (data->pMessage != nullptr ? data->pMessage : "(no message)");
  DebugWrite("gkplus: vulkan " + text + "\n");

  // Kept in memory as well, because OutputDebugString needs a debugger and attaching one to
  // Gunlok makes it crawl. Oldest dropped once full; the first few messages are the useful
  // ones anyway, since later frames tend to repeat the same complaint.
  if (ValidationLog.size() >= kValidationLog) {
    ValidationLog.erase(ValidationLog.begin());
  }
  ValidationLog.push_back(text);
  return VK_FALSE; // never abort the offending call - reporting is the whole point
}

InitResult Fail(InitResult result, const std::string &message) {
  Result = result;
  Error = message;
  DebugWrite("gkplus: vulkan init failed (" + std::string(InitResultName(result)) + "): " +
             message + "\n");
  return result;
}

// Everything the bindless design needs, in one place so the "can this machine run it"
// question has exactly one answer. Vulkan 1.2 core for the bindless half (section 2 of
// vulkan_renderer_notes.md), 1.3 core for dynamic rendering and synchronization2.
bool HasRequiredFeatures(const DeviceCaps &caps) {
  return caps.descriptor_indexing && caps.runtime_descriptor_array &&
         caps.partially_bound && caps.variable_descriptor_count &&
         caps.update_after_bind && caps.non_uniform_indexing &&
         caps.buffer_device_address && caps.dynamic_rendering &&
         caps.synchronization2;
}

// Reads a candidate device into a DeviceCaps. Does not decide anything - ScoreDevice and
// HasRequiredFeatures do that, so "what the device can do" and "do we want it" stay apart.
void QueryDevice(VkPhysicalDevice device, DeviceCaps &caps) {
  caps = DeviceCaps();

  VkPhysicalDeviceDescriptorIndexingProperties indexing_props = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
  VkPhysicalDeviceProperties2 props = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexing_props};
  vkGetPhysicalDeviceProperties2(device, &props);

  VkPhysicalDeviceVulkan13Features features13 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features features12 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &features13};
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                        &features12};
  vkGetPhysicalDeviceFeatures2(device, &features);

  caps.device_name = props.properties.deviceName;
  caps.api_version = props.properties.apiVersion;
  caps.driver_version = props.properties.driverVersion;
  caps.discrete =
      props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  caps.max_push_constants = props.properties.limits.maxPushConstantsSize;
  caps.max_bindless_textures =
      indexing_props.maxDescriptorSetUpdateAfterBindSampledImages;

  caps.descriptor_indexing = features12.descriptorIndexing != 0;
  caps.runtime_descriptor_array = features12.runtimeDescriptorArray != 0;
  caps.partially_bound = features12.descriptorBindingPartiallyBound != 0;
  caps.variable_descriptor_count =
      features12.descriptorBindingVariableDescriptorCount != 0;
  caps.update_after_bind =
      features12.descriptorBindingSampledImageUpdateAfterBind != 0;
  caps.non_uniform_indexing =
      features12.shaderSampledImageArrayNonUniformIndexing != 0;
  caps.buffer_device_address = features12.bufferDeviceAddress != 0;
  caps.dynamic_rendering = features13.dynamicRendering != 0;
  caps.synchronization2 = features13.synchronization2 != 0;

  VkPhysicalDeviceMemoryProperties memory = {};
  vkGetPhysicalDeviceMemoryProperties(device, &memory);
  for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
    if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
      caps.device_local_bytes += memory.memoryHeaps[i].size;
    }
  }
  // The ReBAR heap: device-local AND host-visible. Called out separately because on a
  // 32-bit host it is the one pool where mapping a large range costs scarce address space
  // (vulkan_renderer_notes.md section 3).
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if ((memory.memoryTypes[i].propertyFlags & want) == want) {
      const uint64_t size = memory.memoryHeaps[memory.memoryTypes[i].heapIndex].size;
      if (size > caps.host_visible_device_local_bytes) {
        caps.host_visible_device_local_bytes = size;
      }
    }
  }
}

// A graphics-capable queue family, or -1. Presentation support is deliberately not tested:
// there is no surface yet, and every graphics family on a desktop ICD presents.
int FindGraphicsQueue(VkPhysicalDevice device) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  for (uint32_t i = 0; i < count; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool HasLayer(const char *name) {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const VkLayerProperties &layer : layers) {
    if (std::string(layer.layerName) == name) {
      return true;
    }
  }
  return false;
}

InitResult DoInitialize() {
  if (volkInitialize() != VK_SUCCESS) {
    return Fail(InitResult::NoLoader,
                "no vulkan-1.dll, or it exposes no ICD for this process. gl.exe is 32-bit, "
                "so this needs the SysWOW64 loader and the driver's VulkanDriverNameWow ICD");
  }

  // 1.3 is a hard floor, not a preference: dynamic rendering and synchronization2 are 1.3
  // core, and the renderer is built on both. Checked against the loader first so an old
  // loader gets a legible message instead of a vkCreateInstance failure.
  uint32_t instance_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion != nullptr) {
    vkEnumerateInstanceVersion(&instance_version);
  }
  if (instance_version < VK_API_VERSION_1_3) {
    return Fail(InitResult::NoInstance, "loader reports Vulkan " +
                                            std::to_string(VK_VERSION_MAJOR(instance_version)) +
                                            "." +
                                            std::to_string(VK_VERSION_MINOR(instance_version)) +
                                            ", need 1.3 for dynamic rendering");
  }

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Gunlok";
  app.pEngineName = "GkPlus";
  app.apiVersion = VK_API_VERSION_1_3;

  // Surface extensions are requested now although the swapchain comes later: asking for
  // them here means a machine that cannot present fails at init, where the reason is
  // reportable, rather than halfway through bringing up a renderer.
  const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                              VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                              VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

  char validation[8] = {};
  const bool want_validation =
      ::GetEnvironmentVariableA("GKPLUS_VK_VALIDATION", validation, sizeof(validation)) ==
          1 &&
      validation[0] == '1';
  const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
  const bool use_validation = want_validation && HasLayer(layers[0]);

  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = use_validation ? 3 : 2;
  info.ppEnabledExtensionNames = extensions;
  info.enabledLayerCount = use_validation ? 1 : 0;
  info.ppEnabledLayerNames = use_validation ? layers : nullptr;

  const VkResult created = vkCreateInstance(&info, nullptr, &Instance);
  if (created != VK_SUCCESS) {
    return Fail(InitResult::NoInstance,
                "vkCreateInstance returned " + std::to_string(created));
  }
  volkLoadInstance(Instance);

  if (use_validation) {
    VkDebugUtilsMessengerCreateInfoEXT messenger = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = DebugCallback;
    vkCreateDebugUtilsMessengerEXT(Instance, &messenger, nullptr, &Messenger);
    DebugWrite("gkplus: vulkan validation enabled\n");
  }

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(Instance, &count, nullptr);
  if (count == 0) {
    return Fail(InitResult::NoPhysicalDevice, "no Vulkan physical devices");
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(Instance, &count, devices.data());

  // Prefer a discrete GPU that meets the requirements; fall back to any device that does.
  // A device that does not is never chosen, so Caps() always describes something usable.
  DeviceCaps best;
  VkPhysicalDevice chosen = VK_NULL_HANDLE;
  std::string rejected;
  for (VkPhysicalDevice device : devices) {
    DeviceCaps caps;
    QueryDevice(device, caps);
    if (!HasRequiredFeatures(caps) || FindGraphicsQueue(device) < 0) {
      rejected += (rejected.empty() ? "" : ", ") + caps.device_name;
      continue;
    }
    if (chosen == VK_NULL_HANDLE || (caps.discrete && !best.discrete)) {
      best = caps;
      chosen = device;
    }
  }
  if (chosen == VK_NULL_HANDLE) {
    return Fail(InitResult::MissingFeatures,
                "no device supports descriptor indexing + buffer device address + "
                "dynamic rendering; rejected: " +
                    rejected);
  }

  PhysicalDevice = chosen;
  TheCaps = best;
  TheCaps.graphics_queue_family = static_cast<uint32_t>(FindGraphicsQueue(chosen));

  // Only the features the design uses are enabled. Enabling a feature costs nothing at
  // runtime but does constrain driver fast paths, and an unused one here would be a claim
  // about the renderer that is not true.
  VkPhysicalDeviceVulkan13Features features13 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;

  VkPhysicalDeviceVulkan12Features features12 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &features13};
  features12.descriptorIndexing = VK_TRUE;
  features12.runtimeDescriptorArray = VK_TRUE;
  features12.descriptorBindingPartiallyBound = VK_TRUE;
  features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  features12.bufferDeviceAddress = VK_TRUE;

  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                        &features12};

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = TheCaps.graphics_queue_family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;

  // VK_KHR_dynamic_rendering is listed although dynamic rendering is Vulkan 1.3 CORE and
  // the feature bit is enabled above. ImGui's Vulkan backend requires the extension to be
  // explicitly enabled before it will use dynamic rendering, and says so in
  // imgui_impl_vulkan.h - promoted-to-core is not enough for it.
  const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                     VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};

  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue;
  device_info.enabledExtensionCount = 2;
  device_info.ppEnabledExtensionNames = device_extensions;

  const VkResult device_created =
      vkCreateDevice(PhysicalDevice, &device_info, nullptr, &Device);
  if (device_created != VK_SUCCESS) {
    return Fail(InitResult::NoDevice,
                "vkCreateDevice returned " + std::to_string(device_created));
  }
  // Device-level entry points bypass the loader's dispatch after this, which is the reason
  // volk is here at all beyond dynamic loading.
  volkLoadDevice(Device);
  vkGetDeviceQueue(Device, TheCaps.graphics_queue_family, 0, &GraphicsQueue);

  Result = InitResult::Ok;
  Error.clear();
  DebugWrite("gkplus: vulkan device ready: " + TheCaps.device_name + "\n");
  return Result;
}

} // namespace

const char *InitResultName(InitResult result) {
  switch (result) {
  case InitResult::Ok:
    return "ok";
  case InitResult::NoLoader:
    return "no-loader";
  case InitResult::NoInstance:
    return "no-instance";
  case InitResult::NoPhysicalDevice:
    return "no-physical-device";
  case InitResult::MissingFeatures:
    return "missing-features";
  case InitResult::NoDevice:
    return "no-device";
  }
  return "unknown";
}

InitResult Initialize() {
  if (Initialized) {
    return Result;
  }
  Initialized = true;
  return DoInitialize();
}

bool Available() { return Initialized && Result == InitResult::Ok; }

const DeviceCaps &Caps() { return TheCaps; }

InitResult Status() { return Result; }

const std::string &LastError() { return Error; }

std::string FormatCaps() {
  Initialize();

  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };

  add("status: %s\n", InitResultName(Result));
  if (!Available()) {
    return out + (Error.empty() ? "" : "reason: " + Error + "\n");
  }

  const DeviceCaps &c = TheCaps;
  add("device: %s%s\n", c.device_name.c_str(), c.discrete ? " (discrete)" : "");
  add("api: %u.%u.%u   driver: 0x%08x\n", VK_VERSION_MAJOR(c.api_version),
      VK_VERSION_MINOR(c.api_version), VK_VERSION_PATCH(c.api_version),
      c.driver_version);
  add("device-local: %llu MB   host-visible device-local: %llu MB\n",
      (unsigned long long)(c.device_local_bytes >> 20),
      (unsigned long long)(c.host_visible_device_local_bytes >> 20));
  add("bindless sampled images: %u   push constants: %u bytes   graphics family: %u\n",
      c.max_bindless_textures, c.max_push_constants, c.graphics_queue_family);
  add("descriptor_indexing=%d runtime_array=%d partially_bound=%d variable_count=%d\n",
      c.descriptor_indexing, c.runtime_descriptor_array, c.partially_bound,
      c.variable_descriptor_count);
  add("update_after_bind=%d non_uniform_indexing=%d buffer_device_address=%d\n",
      c.update_after_bind, c.non_uniform_indexing, c.buffer_device_address);
  add("dynamic_rendering=%d synchronization2=%d\n", c.dynamic_rendering,
      c.synchronization2);
  add("validation: %s   errors: %llu   warnings: %llu\n",
      ValidationEnabled() ? "on" : "off",
      (unsigned long long)ValidationErrorCount(),
      (unsigned long long)ValidationWarningCount());
  return out;
}

uint64_t ValidationErrorCount() { return ValidationErrors; }
uint64_t ValidationWarningCount() { return ValidationWarnings; }
bool ValidationEnabled() { return Messenger != VK_NULL_HANDLE; }

const std::vector<std::string> &ValidationMessages() { return ValidationLog; }

void ClearValidationMessages() {
  ValidationLog.clear();
  ValidationErrors = 0;
  ValidationWarnings = 0;
}

VkInstance GetInstance() { return Instance; }
VkPhysicalDevice GetPhysicalDevice() { return PhysicalDevice; }
VkDevice GetDevice() { return Device; }
VkQueue GetGraphicsQueue() { return GraphicsQueue; }

void Shutdown() {
  if (Device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(Device);
    vkDestroyDevice(Device, nullptr);
    Device = VK_NULL_HANDLE;
  }
  if (Messenger != VK_NULL_HANDLE) {
    vkDestroyDebugUtilsMessengerEXT(Instance, Messenger, nullptr);
    Messenger = VK_NULL_HANDLE;
  }
  if (Instance != VK_NULL_HANDLE) {
    vkDestroyInstance(Instance, nullptr);
    Instance = VK_NULL_HANDLE;
  }
  PhysicalDevice = VK_NULL_HANDLE;
  GraphicsQueue = VK_NULL_HANDLE;
  Initialized = false;
  Result = InitResult::NoLoader;
}

} // namespace vulkan
} // namespace gk
