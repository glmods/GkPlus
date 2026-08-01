#pragma once

// Raw Vulkan handles, for the renderer's own translation units.
//
// Separate from VkContext.h on purpose: that header is what the JS bindings and anything
// else outside the renderer include, and it deliberately mentions no Vulkan type, so
// including it costs nothing and pulls in no volk. This one is the inside view.
//
// Every handle is null until vulkan::Initialize() has returned InitResult::Ok.

#include <volk.h>

namespace gk {
namespace vulkan {

VkInstance GetInstance();
VkPhysicalDevice GetPhysicalDevice();
VkDevice GetDevice();
VkQueue GetGraphicsQueue();

} // namespace vulkan
} // namespace gk
