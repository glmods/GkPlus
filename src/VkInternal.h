#pragma once

// Raw Vulkan handles, for the renderer's own translation units.
//
// Separate from VkContext.h on purpose: that header is what the JS bindings and anything
// else outside the renderer include, and it deliberately mentions no Vulkan type, so
// including it costs nothing and pulls in no Vulkan header. This one is the inside view.
//
// Every handle is null until vulkan::Initialize() has returned InitResult::Ok.

#include <vulkan/vulkan.h>

#include <mutex>

namespace gk {
namespace vulkan {

VkInstance GetInstance();
VkPhysicalDevice GetPhysicalDevice();
VkDevice GetDevice();
VkQueue GetGraphicsQueue();

// **Vulkan requires host access to a VkQueue to be externally synchronized**, and to ALL of a
// device's queues for vkDeviceWaitIdle. There is one graphics queue here and two threads reach
// it: the main thread submits and presents the frame (VkRenderer.cpp), and the executor thread
// arrives through CaptureVertexBuffer::Unlock -> UploadIntoSlot -> AllocateStaging, which under
// ring pressure calls WaitForLiveFrames (vkDeviceWaitIdle) or FlushPendingNow (vkQueueSubmit).
// That is undefined behaviour, not merely a lost update - the driver's own queue state is what
// gets corrupted, so it presents as a device loss or a hang inside the driver rather than as
// anything in this codebase.
//
// `ResourceLock` cannot serve: it is file-local to VkResources.cpp, and VkRenderer.cpp - the
// other submitter - cannot see it.
//
// **Lock order is ResourceLock -> QueueMutex, never the reverse.** Every scope that takes this
// is deliberately tight, around the queue call itself and nothing else, which is what keeps
// that true: hold it across a call into VkResources' public API and the inversion appears.
std::recursive_mutex &QueueMutex();
using QueueGuard = std::lock_guard<std::recursive_mutex>;

// `vkQueueSubmit` under that mutex. A helper rather than a guard at each site because every
// one of these calls is in expression position - inside an `if (... == VK_SUCCESS)` - where a
// scope cannot be introduced without restructuring the caller.
VkResult SubmitToQueue(const VkSubmitInfo &submit, VkFence fence);

} // namespace vulkan
} // namespace gk
