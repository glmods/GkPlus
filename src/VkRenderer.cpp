#include "VkRenderer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include "Core.h"
#include "D3D8Capture.h"
#include "GUI.h"
#include "VkCapture.h"
#include "VkContext.h"
#include "VkDraw.h"
#include "VkInternal.h"
#include "VkResources.h"

namespace gk {
namespace vulkan {
namespace {

// Two frames in flight. Not a tuning choice so much as an address-space one: every frame in
// flight eventually owns its own staging ring, and on a 32-bit host (section 3 of the notes)
// three of those buys latency hiding the game does not need at 300 fps.
constexpr uint32_t kFramesInFlight = 2;

struct Frame {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkSemaphore acquired = VK_NULL_HANDLE;
  VkFence in_flight = VK_NULL_HANDLE;
};

bool Requested = false;
bool RequestedRead = false;
bool Ready = false;
bool NeedsRebuild = false;
std::string Error;
RendererStats TheStats;

HWND Window = nullptr;
VkSurfaceKHR Surface = VK_NULL_HANDLE;
VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
VkSurfaceFormatKHR Format = {};
VkPresentModeKHR PresentMode = VK_PRESENT_MODE_FIFO_KHR;
VkExtent2D Extent = {};

std::vector<VkImage> Images;
std::vector<VkImageView> Views;
// One per swapchain image, not per frame in flight: the semaphore a present waits on must
// not be reused until that image is done being presented, and only the image index tracks
// that. Using a per-frame semaphore here is the classic way to get a validation error that
// only appears when the swapchain has more images than frames in flight.
std::vector<VkSemaphore> RenderFinished;

Frame Frames[kFramesInFlight];
uint32_t FrameIndex = 0;

bool ImGuiReady = false;

// --- the offscreen colour target (§4.37) ------------------------------------------------------
//
// The world is rasterised at the size the GAME rasterises at - its D3D backbuffer, 640x480 - and
// scaled onto the swapchain afterwards. Not a quality choice: a pre-transformed draw's
// pixels-to-clip matrix is built from the D3D viewport, so a Vulkan viewport covering a 628x468
// swapchain scales every 2D draw by 628/640 *during rasterisation*, which resamples the texture
// and puts no sample on a texel. The original does exactly this - it rasterises 1:1 and lets its
// windowed Present stretch the finished frame - and reproducing the order is the whole fix.
//
// Sizing the Vulkan viewport from the D3D viewport instead makes the sampling exact and costs the
// framing (2.59 -> 13.07 whole-frame), because a larger viewport on a smaller swapchain clips
// where D3D stretches. That is the experiment §4.37 reverted; this is what it concluded.
VkImage OffscreenImage = VK_NULL_HANDLE;
VkDeviceMemory OffscreenMemory = VK_NULL_HANDLE;
VkImageView OffscreenView = VK_NULL_HANDLE;
// What the world pass rasterises into: the backbuffer size when the offscreen target is up, the
// swapchain extent when it is not. The depth buffer and every viewport follow it.
VkExtent2D RenderExtent = {};
// GKPLUS_VK_OFFSCREEN=0 / `render.offscreen`. On by default - drawing at the wrong size is not a
// defensible default - and off is the pre-§4.37 behaviour exactly, which is what makes it
// A/B-able on one paused frame the way `render.half_pixel` is.
bool OffscreenWanted = true;
bool OffscreenWantedRead = false;
// Whether the target actually exists this frame. False when the swapchain will not take
// TRANSFER_DST, or before the game has told us a backbuffer size.
bool OffscreenActive = false;
bool SwapchainTransferDst = false;
// The filter for the final scale, and NEAREST is not the lazy default: the original's 640->628
// windowed stretch preserves the probe quad's **16 distinct values, 100% multiples of 17**
// (§4.37), which a filtered downscale could not - a blend of two 4-bit levels is not on that
// ladder. So D3D's stretch drops columns rather than mixing them, and matching it means NEAREST.
// `render.present_linear` is the A/B, because that deduction is about one measurement.
bool PresentLinearFilter = false;

bool Fail(const std::string &message) {
  Error = message;
  DebugWrite("gkplus: vulkan renderer: " + message + "\n");
  return false;
}

void DestroySwapchainObjects() {
  VkDevice device = GetDevice();
  for (VkImageView view : Views) {
    vkDestroyImageView(device, view, nullptr);
  }
  Views.clear();
  for (VkSemaphore semaphore : RenderFinished) {
    vkDestroySemaphore(device, semaphore, nullptr);
  }
  RenderFinished.clear();
  Images.clear();
}

// Prefers a plain 8-bit BGRA UNORM surface. Deliberately NOT an _SRGB one: the engine's
// colours are already in whatever space D3D8 fixed function produced, and picking an sRGB
// swapchain silently applies a gamma curve to everything, which reads as "the port looks
// washed out" much later and is hard to trace back here.
VkSurfaceFormatKHR ChooseFormat(const std::vector<VkSurfaceFormatKHR> &formats) {
  for (const VkSurfaceFormatKHR &f : formats) {
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return f;
    }
  }
  return formats.front();
}

// FIFO is the only mode guaranteed to exist and the only one that cannot tear. MAILBOX is
// preferred where present because the game runs far above refresh (measured ~300 fps in
// level) and FIFO would otherwise throttle the whole engine loop to the monitor - which
// would change game timing, not just presentation.
VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR> &modes) {
  for (VkPresentModeKHR mode : modes) {
    if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return mode;
    }
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

bool CreateSwapchain() {
  VkDevice device = GetDevice();
  VkPhysicalDevice physical = GetPhysicalDevice();

  VkSurfaceCapabilitiesKHR caps = {};
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, Surface, &caps) != VK_SUCCESS) {
    return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
  }

  // A zero-area surface is a minimized window, not an error. Leave the old swapchain alone
  // and report not-ready; DrawFrame retries next frame.
  if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
    return false;
  }

  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, Surface, &format_count, nullptr);
  if (format_count == 0) {
    return Fail("surface reports no formats");
  }
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, Surface, &format_count, formats.data());

  uint32_t mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, Surface, &mode_count, nullptr);
  std::vector<VkPresentModeKHR> modes(mode_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, Surface, &mode_count, modes.data());

  Format = ChooseFormat(formats);
  PresentMode = ChoosePresentMode(modes);
  Extent = caps.currentExtent;

  uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0 && image_count > caps.maxImageCount) {
    image_count = caps.maxImageCount;
  }

  // TRANSFER_DST is what the offscreen target is blitted through, and it is asked for rather
  // than assumed: every surface in practice supports it, but a swapchain created without a usage
  // bit it does not support fails outright, and this renderer has a working path without it.
  // `SwapchainTransferDst` is what the render-target reconcile reads.
  SwapchainTransferDst =
      (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
  if (!SwapchainTransferDst) {
    DebugWrite("gkplus: vulkan renderer: the surface will not take TRANSFER_DST; drawing "
               "straight into the swapchain, so 2D draws are rasterised at the window's size "
               "rather than the game's\n");
  }

  VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = Surface;
  info.minImageCount = image_count;
  info.imageFormat = Format.format;
  info.imageColorSpace = Format.colorSpace;
  info.imageExtent = Extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (SwapchainTransferDst ? VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                          : VkImageUsageFlags(0));
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = PresentMode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = Swapchain;

  VkSwapchainKHR created = VK_NULL_HANDLE;
  const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &created);

  // The old swapchain is retired by the create call whether it succeeded or not, so it is
  // destroyed either way - and its views and semaphores with it.
  DestroySwapchainObjects();
  if (Swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device, Swapchain, nullptr);
    Swapchain = VK_NULL_HANDLE;
  }
  if (result != VK_SUCCESS) {
    return Fail("vkCreateSwapchainKHR returned " + std::to_string(result));
  }
  Swapchain = created;

  uint32_t actual = 0;
  vkGetSwapchainImagesKHR(device, Swapchain, &actual, nullptr);
  Images.resize(actual);
  vkGetSwapchainImagesKHR(device, Swapchain, &actual, Images.data());

  Views.resize(actual);
  RenderFinished.resize(actual);
  for (uint32_t i = 0; i < actual; ++i) {
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = Images[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = Format.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view, nullptr, &Views[i]) != VK_SUCCESS) {
      return Fail("vkCreateImageView failed");
    }
    VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device, &semaphore, nullptr, &RenderFinished[i]) != VK_SUCCESS) {
      return Fail("vkCreateSemaphore failed");
    }
  }

  TheStats.width = Extent.width;
  TheStats.height = Extent.height;
  TheStats.image_count = actual;
  TheStats.format = static_cast<uint32_t>(Format.format);
  TheStats.present_mode = static_cast<uint32_t>(PresentMode);
  ++TheStats.swapchain_rebuilds;
  NeedsRebuild = false;
  if (ImGuiReady) {
    ImGui_ImplVulkan_SetMinImageCount(2);
  }
  return true;
}

void DestroyOffscreen() {
  VkDevice device = GetDevice();
  if (OffscreenView != VK_NULL_HANDLE) {
    vkDestroyImageView(device, OffscreenView, nullptr);
    OffscreenView = VK_NULL_HANDLE;
  }
  if (OffscreenImage != VK_NULL_HANDLE) {
    vkDestroyImage(device, OffscreenImage, nullptr);
    OffscreenImage = VK_NULL_HANDLE;
  }
  if (OffscreenMemory != VK_NULL_HANDLE) {
    vkFreeMemory(device, OffscreenMemory, nullptr);
    OffscreenMemory = VK_NULL_HANDLE;
  }
  OffscreenActive = false;
}

// Allocated directly rather than through VMA, for the reason CreateDepth in VkDraw.cpp gives for
// the depth image: one image that lives as long as the swapchain, and pulling the allocator in
// for it would export a handle nothing else needs.
bool CreateOffscreen(VkExtent2D extent) {
  DestroyOffscreen();
  VkDevice device = GetDevice();

  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = Format.format;
  info.extent = {extent.width, extent.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &info, nullptr, &OffscreenImage) != VK_SUCCESS) {
    return Fail("could not create the offscreen colour target");
  }

  VkMemoryRequirements requirements = {};
  vkGetImageMemoryRequirements(device, OffscreenImage, &requirements);
  VkPhysicalDeviceMemoryProperties memory = {};
  vkGetPhysicalDeviceMemoryProperties(GetPhysicalDevice(), &memory);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
        (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) {
    return Fail("no device-local memory type for the offscreen colour target");
  }

  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (vkAllocateMemory(device, &allocate, nullptr, &OffscreenMemory) != VK_SUCCESS ||
      vkBindImageMemory(device, OffscreenImage, OffscreenMemory, 0) != VK_SUCCESS) {
    return Fail("could not back the offscreen colour target");
  }

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = OffscreenImage;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = Format.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &view, nullptr, &OffscreenView) != VK_SUCCESS) {
    return Fail("could not create the offscreen colour target's view");
  }
  OffscreenActive = true;
  return true;
}

bool OffscreenEnvEnabled() {
  if (!OffscreenWantedRead) {
    OffscreenWantedRead = true;
    char value[16] = {};
    const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_OFFSCREEN", value, sizeof(value));
    if (len > 0) {
      const std::string text(value, len);
      OffscreenWanted = !(text == "0" || text == "off" || text == "no");
    }
  }
  return OffscreenWanted;
}

// The size the world pass should rasterise at, and whether that needs a target of its own.
// The game's backbuffer when it has told us one, the swapchain otherwise - which is also what
// windowed D3D8 does with a zero BackBufferWidth, so the two agree in that case rather than the
// fallback being a guess.
VkExtent2D DesiredRenderExtent(bool &offscreen) {
  uint32_t width = 0, height = 0;
  if (OffscreenEnvEnabled() && SwapchainTransferDst &&
      d3d8::BackBufferExtent(width, height)) {
    offscreen = true;
    return {width, height};
  }
  offscreen = false;
  return Extent;
}

// Brings the render target and the depth buffer in line with what the game is drawing at.
// Called once a frame and a no-op unless something moved - a resize, a Reset, or the toggle -
// because it waits for the device to go idle before it destroys anything the last frame read.
bool ReconcileRenderTarget() {
  bool want_offscreen = false;
  const VkExtent2D want = DesiredRenderExtent(want_offscreen);
  if (want_offscreen == OffscreenActive && want.width == RenderExtent.width &&
      want.height == RenderExtent.height) {
    return true;
  }
  {
    QueueGuard queue_guard(QueueMutex());
    vkDeviceWaitIdle(GetDevice());
  }
  RenderExtent = want;
  if (want_offscreen) {
    if (!CreateOffscreen(want)) {
      // Fall back rather than stop: rasterising at the window's size is what every build before
      // §4.37 did, and a renderer that draws slightly wrong beats one that draws nothing.
      DestroyOffscreen();
      RenderExtent = Extent;
    }
  } else {
    DestroyOffscreen();
  }
  // DrawReady() is false during bring-up, where StartDraw creates the depth buffer at this same
  // extent immediately afterwards.
  if (DrawReady() && !ResizeDraw(RenderExtent.width, RenderExtent.height)) {
    return Fail("could not resize the depth buffer to the render extent");
  }
  return true;
}

// The ImGui *context*, the Win32 backend and the F11 toggle all belong to GUISystem
// (src/GUI.h) whichever renderer is running - only the rendering backend differs. This
// brings up that half and nothing else.
bool StartImGui() {
  if (ImGuiReady) {
    return true;
  }
  // The backend is compiled with IMGUI_IMPL_VULKAN_NO_PROTOTYPES (see
  // third_party/imgui_backends/README.md), so it has no entry points until this fills them.
  // Routed through volk's vkGetInstanceProcAddr rather than the loader's exported symbol,
  // which is the whole point: nothing here needs an import library.
  if (!ImGui_ImplVulkan_LoadFunctions(
          VK_API_VERSION_1_3,
          [](const char *name, void *user) {
            return vkGetInstanceProcAddr(static_cast<VkInstance>(user), name);
          },
          GetInstance())) {
    return Fail("ImGui_ImplVulkan_LoadFunctions failed");
  }

  // The overlay has its own pass on the swapchain image, with no depth attachment, so its
  // pipeline declares colour only. It used to share the world's pass and therefore had to name
  // the world's depth and stencil formats; moving it out is what keeps it 1:1 with the window
  // now that the world is rasterised at the game's size and scaled (§4.37). The overlay is drawn
  // for a human, not to match d3d9, so it is the one thing that should NOT go through the scale.
  VkPipelineRenderingCreateInfo rendering = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &Format.format;
  rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
  rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

  ImGui_ImplVulkan_InitInfo info = {};
  info.ApiVersion = VK_API_VERSION_1_3;
  info.Instance = GetInstance();
  info.PhysicalDevice = GetPhysicalDevice();
  info.Device = GetDevice();
  info.QueueFamily = Caps().graphics_queue_family;
  info.Queue = GetGraphicsQueue();
  // Non-zero DescriptorPoolSize asks the backend to own its pool. One less object here to
  // create, size and destroy, and it is the documented convenience path.
  info.DescriptorPoolSize = 16;
  info.MinImageCount = 2;
  info.ImageCount = static_cast<uint32_t>(Images.size());
  info.UseDynamicRendering = true;
  info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering;

  if (!ImGui_ImplVulkan_Init(&info)) {
    return Fail("ImGui_ImplVulkan_Init failed");
  }
  ImGuiReady = true;
  return true;
}

bool CreateFrames() {
  VkDevice device = GetDevice();
  for (Frame &frame : Frames) {
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = Caps().graphics_queue_family;
    if (vkCreateCommandPool(device, &pool, nullptr, &frame.pool) != VK_SUCCESS) {
      return Fail("vkCreateCommandPool failed");
    }

    VkCommandBufferAllocateInfo alloc = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = frame.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &alloc, &frame.cmd) != VK_SUCCESS) {
      return Fail("vkAllocateCommandBuffers failed");
    }

    VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device, &semaphore, nullptr, &frame.acquired) != VK_SUCCESS) {
      return Fail("vkCreateSemaphore failed");
    }

    // Created signalled so the first frame's wait returns immediately instead of hanging on
    // a fence nothing has submitted to.
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fence, nullptr, &frame.in_flight) != VK_SUCCESS) {
      return Fail("vkCreateFence failed");
    }
  }
  return true;
}

void DestroyFrames() {
  VkDevice device = GetDevice();
  for (Frame &frame : Frames) {
    if (frame.in_flight != VK_NULL_HANDLE) {
      vkDestroyFence(device, frame.in_flight, nullptr);
    }
    if (frame.acquired != VK_NULL_HANDLE) {
      vkDestroySemaphore(device, frame.acquired, nullptr);
    }
    if (frame.pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, frame.pool, nullptr); // frees its command buffers
    }
    frame = Frame();
  }
}

// Records one image barrier with synchronization2, which takes both stage masks explicitly
// rather than inferring them from the layouts.
void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
  VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

bool RendererRequested() {
  if (!RequestedRead) {
    RequestedRead = true;
    char value[16] = {};
    const DWORD len = ::GetEnvironmentVariableA("GKPLUS_RENDERER", value, sizeof(value));
    Requested = len > 0 && std::string(value, len) == "vulkan";
  }
  return Requested;
}

bool StartRenderer(HWND window) {
  if (Ready) {
    return true;
  }
  if (window == nullptr) {
    return Fail("no window");
  }
  if (Initialize() != InitResult::Ok) {
    return Fail("no vulkan device: " + LastError());
  }

  Window = window;
  VkWin32SurfaceCreateInfoKHR surface = {
      VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  surface.hinstance = reinterpret_cast<HINSTANCE>(
      ::GetWindowLongPtrW(window, GWLP_HINSTANCE));
  surface.hwnd = window;
  if (vkCreateWin32SurfaceKHR(GetInstance(), &surface, nullptr, &Surface) != VK_SUCCESS) {
    return Fail("vkCreateWin32SurfaceKHR failed");
  }

  // The graphics family was chosen before any surface existed (VkContext has no window at
  // init time), so this is where "can it actually present" gets checked rather than assumed.
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(GetPhysicalDevice(), Caps().graphics_queue_family,
                                       Surface, &supported);
  if (!supported) {
    return Fail("graphics queue family cannot present to the game window");
  }

  // StartDraw needs the bindless set layout StartResources creates, and the swapchain's format,
  // so it cannot come before either. **ReconcileRenderTarget comes before it**, because the size
  // StartDraw builds the depth buffer at is the render extent rather than the swapchain's - the
  // game is already up by the time Present first reaches us, so its backbuffer size is known
  // here (§4.37).
  if (!CreateFrames() || !CreateSwapchain() || !StartResources() ||
      !ReconcileRenderTarget() ||
      !StartDraw(RenderExtent.width, RenderExtent.height, Format.format) || !StartImGui()) {
    return false;
  }

  Ready = true;
  Error.clear();
  TheStats.ready = true;
  DebugWrite("gkplus: vulkan renderer up: swapchain " + std::to_string(Extent.width) + "x" +
             std::to_string(Extent.height) + ", " + std::to_string(Images.size()) +
             " images, rendering at " + std::to_string(RenderExtent.width) + "x" +
             std::to_string(RenderExtent.height) +
             (OffscreenActive ? " offscreen\n" : " direct\n"));
  return true;
}

bool RendererReady() { return Ready; }

void NotifyResize() { NeedsRebuild = true; }

void DrawFrame() {
  if (!Ready) {
    return;
  }
  VkDevice device = GetDevice();

  if (NeedsRebuild) {
    if (!CreateSwapchain()) {
      // Minimized, or a genuine failure CreateSwapchain has already recorded. The frame's
      // draws are dropped rather than carried over: they were built against the old extent's
      // projection and would be a frame stale by the time anything rendered.
      ClearDraws();
      // Their scratch goes with them, and it is emptied rather than rotated: nothing was
      // submitted from this slice, so it stays the one the next scene writes into.
      ResetFrameScratch();
      return;
    }
  }
  // Every frame, not only after a rebuild: the game can change its backbuffer size through Reset
  // without the swapchain going out of date, and the toggle can move under a paused frame. Cheap
  // and does nothing unless one of the three actually moved.
  if (!ReconcileRenderTarget()) {
    ClearDraws();
    ResetFrameScratch();
    return;
  }

  Frame &frame = Frames[FrameIndex];
  vkWaitForFences(device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
  // The fence is the proof that everything this slot staged last time round has been read, so
  // the staging ring may hand those bytes back. It must be here rather than after Present:
  // without it the ring reuses memory a frame in flight is still copying from, which is what
  // silently corrupted one texture per session during the startup burst.
  ReleaseFrameStaging(FrameIndex);
  // The scratch is NOT reset here, and that is the point of the fix in §4.22: the user-pointer
  // vertices this frame is about to draw were written by the game before Present was called, so
  // the slice they went into is the one that must still be current while RecordDraws reads it.
  // Resetting here read the previous scene's slice and let the next scene overwrite a live one.
  // It rotates at the bottom of this function instead - where this same fence wait, one frame
  // on, is what proves the incoming slice is free.

  uint32_t image_index = 0;
  VkResult acquired = vkAcquireNextImageKHR(device, Swapchain, UINT64_MAX, frame.acquired,
                                            VK_NULL_HANDLE, &image_index);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    // Do NOT reset the fence or advance the frame: nothing was submitted, so the fence is
    // still signalled from the last time round and the semaphore was never waited on.
    CreateSwapchain();
    return;
  }
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
    ++TheStats.acquire_failures;
    return;
  }

  vkResetFences(device, 1, &frame.in_flight);
  vkResetCommandBuffer(frame.cmd, 0);

  // Built before recording, because ImGui::Render() produces the draw data the command
  // buffer then consumes. Skipped entirely when the overlay is hidden - NewFrame and Render
  // must pair, so a half-built frame is worse than none.
  ImDrawData *draw_data = nullptr;
  if (ImGuiReady && IsOverlayVisible()) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RunOverlayDrawCallback();
    ImGui::Render();
    draw_data = ImGui::GetDrawData();
  }

  // Around the whole frame including the uploads, so a capture shows the copies that produced
  // the geometry as well as the draws that read it.
  BeginFrameCaptureIfArmed();

  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.cmd, &begin);

  RecordUploads(frame.cmd, FrameIndex);

  // Outside any render pass, which is the whole reason it is here and not in RecordDraws: a
  // compute dispatch inside vkCmdBeginRendering is invalid. Usually a no-op - the grid is rebuilt
  // once per level, not once per frame.
  BuildLightGrid(frame.cmd);
  // Also outside any render pass, and after the scene has been recorded: it walks the same draw
  // list the world pass is about to.
  RecordShadowPass(frame.cmd);
  // And so does the map lights' static atlas, which is why it is here rather than at level load -
  // the map's geometry is only reachable as a draw list, and a draw list only exists inside a
  // frame. Usually a no-op: it bakes a few lights a frame until the level's set is done and then
  // stops until the next level (§4.61).
  BakeMapShadows(frame.cmd);
  // ... and so does the per-frame atlas, for a stronger version of the same reason: its casters
  // are the frame's units and props as well as its map, and those exist only as a draw list.
  BakeDynamicShadows(frame.cmd);

  // Where the world pass draws: the offscreen target when it is up, the swapchain image when it
  // is not. UNDEFINED as the source layout in either case, on purpose - neither image's previous
  // contents are ours to preserve, and saying so lets the driver skip a decompress.
  const VkImage world_image = OffscreenActive ? OffscreenImage : Images[image_index];
  const VkImageView world_view = OffscreenActive ? OffscreenView : Views[image_index];
  Barrier(frame.cmd, world_image, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

  // The clear is the attachment's load op rather than a separate vkCmdClearColorImage: one
  // less barrier and one less layout.
  //
  // **The colour is the game's own**, taken from its last whole-surface `Clear`. It used to be a
  // hardcoded blue-grey, which is visible wherever the world does not cover the frame and - the
  // part that actually matters - is what every alpha-blended and additive draw over an uncovered
  // background blends *against*. A translucent beam against a black sky comes out lighter and
  // hazier over a blue-grey one, which reads as "this renderer gets translucency wrong".
  const d3d8::ClearValues &clears = d3d8::Clears();
  const auto channel = [](uint32_t argb, int shift) {
    return static_cast<float>((argb >> shift) & 0xff) / 255.0f;
  };
  VkRenderingAttachmentInfo colour = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  colour.imageView = world_view;
  colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colour.clearValue.color = {{channel(clears.colour, 16), channel(clears.colour, 8),
                              channel(clears.colour, 0), channel(clears.colour, 24)}};

  auto depth_view = reinterpret_cast<VkImageView>(DepthImageView());
  // The stencil aspect rides on the same image and the same view (§4.21): the game's shadow
  // volumes count into it, so it is cleared with the depth and discarded with it. Its layout,
  // aspect mask and attachment all have to acknowledge both aspects, which is why every one of
  // them asks DepthHasStencil() rather than assuming.
  const bool has_stencil = DepthHasStencil();
  const VkImageLayout depth_layout = has_stencil
                                         ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                         : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_view;
  depth.imageLayout = depth_layout;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  // DONT_CARE: nothing reads the depth buffer after the pass, so storing it is pure bandwidth.
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // The game's own values, for the same reason as the colour above - and the stencil one is what
  // the shadow-volume algorithm counts up from, so it is not a free choice either.
  depth.clearValue.depthStencil = {clears.z, clears.stencil};
  VkRenderingAttachmentInfo stencil = depth;

  // The depth image is fresh every frame from the renderer's point of view - UNDEFINED as the
  // old layout, because the clear load op is about to overwrite it anyway.
  if (depth_view != VK_NULL_HANDLE) {
    Barrier(frame.cmd, reinterpret_cast<VkImage>(DepthImage()),
            VK_IMAGE_LAYOUT_UNDEFINED, depth_layout,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT |
                (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VkImageAspectFlags(0)));
  }

  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = RenderExtent;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &colour;
  rendering.pDepthAttachment = depth_view != VK_NULL_HANDLE ? &depth : nullptr;
  rendering.pStencilAttachment =
      (depth_view != VK_NULL_HANDLE && has_stencil) ? &stencil : nullptr;

  vkCmdBeginRendering(frame.cmd, &rendering);

  // Viewport and scissor are dynamic so the pipeline survives a resize without being rebuilt.
  // Both cover the RENDER extent rather than the swapchain's: that is the size the game's own
  // pixels-to-clip matrix was built from, so a vertex at x=640 lands on pixel 640 exactly and
  // every 2D draw rasterises 1:1 (§4.37). Scaling to the window happens once, afterwards, on
  // finished pixels - which is the order the original does it in.
  //
  // The origin is half a pixel, not zero: D3D8/9 sample a pixel at its integer coordinate where
  // Vulkan samples at the centre, so without it every interpolated value - and therefore every
  // texture fetch - is half a pixel out. See SetHalfPixel in VkDraw.h.
  const float origin = ViewportOrigin();
  VkViewport viewport = {origin, origin, static_cast<float>(RenderExtent.width),
                         static_cast<float>(RenderExtent.height), 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, RenderExtent};
  vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
  vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
  RecordDraws(frame.cmd);
  vkCmdEndRendering(frame.cmd);

  // The scale, which is what makes the two sizes agree. NEAREST rather than LINEAR by default:
  // the original's own stretch preserves a 4-bit texture's sixteen distinct values (§4.37), which
  // only a point sample can, so D3D drops columns rather than mixing them.
  if (OffscreenActive) {
    Barrier(frame.cmd, OffscreenImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    Barrier(frame.cmd, Images[image_index], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(RenderExtent.width),
                          static_cast<int32_t>(RenderExtent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(Extent.width),
                          static_cast<int32_t>(Extent.height), 1};
    vkCmdBlitImage(frame.cmd, OffscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   Images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   PresentLinearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  }

  // The overlay, in its own pass on the swapchain image, so it is 1:1 with the window instead of
  // going through the scale above. It loads rather than clears - the world is already there,
  // whether by blit or because it was drawn straight into this image.
  if (draw_data != nullptr) {
    Barrier(frame.cmd, Images[image_index],
            OffscreenActive ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            OffscreenActive ? VK_PIPELINE_STAGE_2_BLIT_BIT
                            : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            OffscreenActive ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                            : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

    VkRenderingAttachmentInfo overlay = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    overlay.imageView = Views[image_index];
    overlay.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlay.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    overlay.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo overlay_pass = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    overlay_pass.renderArea.extent = Extent;
    overlay_pass.layerCount = 1;
    overlay_pass.colorAttachmentCount = 1;
    overlay_pass.pColorAttachments = &overlay;
    vkCmdBeginRendering(frame.cmd, &overlay_pass);
    // ImGui's backend sets its own viewport and scissor from the draw data's display size, which
    // is the window's - so nothing here has to.
    ImGui_ImplVulkan_RenderDrawData(draw_data, frame.cmd);
    vkCmdEndRendering(frame.cmd);
  }

  // Whichever of the three wrote the swapchain image last is what this barrier comes from.
  const bool ended_as_attachment = draw_data != nullptr || !OffscreenActive;
  Barrier(frame.cmd, Images[image_index],
          ended_as_attachment ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          ended_as_attachment ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                              : VK_PIPELINE_STAGE_2_BLIT_BIT,
          ended_as_attachment ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                              : VK_ACCESS_2_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

  vkEndCommandBuffer(frame.cmd);

  VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = frame.acquired;
  // Both stages the swapchain image is first touched at: the attachment write when the world
  // draws straight into it, and the blit when it does not. Naming only one of the two would let
  // the other run before the image had been acquired.
  wait.stageMask =
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;

  VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = RenderFinished[image_index];
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

  VkCommandBufferSubmitInfo cmd = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cmd.commandBuffer = frame.cmd;

  VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  {
    QueueGuard queue_guard(QueueMutex());
    vkQueueSubmit2(GetGraphicsQueue(), 1, &submit, frame.in_flight);
  }

  VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &RenderFinished[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &Swapchain;
  present.pImageIndices = &image_index;

  // Separate scope from the submit above rather than one spanning both: the present already
  // waits on RenderFinished, so an upload submitted between the two is harmless, and a shorter
  // hold keeps the executor's staging path from queueing behind a whole present.
  VkResult presented;
  {
    QueueGuard queue_guard(QueueMutex());
    presented = vkQueuePresentKHR(GetGraphicsQueue(), &present);
  }
  // After present, so the capture contains a complete frame from acquire to present rather
  // than one that stops short of the thing RenderDoc uses to delimit them.
  EndFrameCaptureIfArmed();
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
    NeedsRebuild = true;
  }

  ++TheStats.frames_presented;
  FrameIndex = (FrameIndex + 1) % kFramesInFlight;
  // After the submit that reads the outgoing slice, so the scene the game is about to draw
  // writes somewhere else. What makes the incoming slice safe is the fence waited at the top of
  // this function: with one more slice than frames in flight, the slice coming round now was
  // last read by the frame that fence retired.
  RotateFrameScratch();
}

void ShutdownRenderer() {
  if (GetDevice() == VK_NULL_HANDLE) {
    return;
  }
  {
    QueueGuard queue_guard(QueueMutex());
    vkDeviceWaitIdle(GetDevice());
  }
  // Before ShutdownResources: the pipeline layout references the bindless set layout that
  // owns, and destroying a layout still referenced by a live pipeline is undefined.
  ShutdownDraw();
  ShutdownResources();
  if (ImGuiReady) {
    ImGui_ImplVulkan_Shutdown();
    ImGuiReady = false;
  }
  DestroyOffscreen();
  RenderExtent = {};
  DestroySwapchainObjects();
  if (Swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(GetDevice(), Swapchain, nullptr);
    Swapchain = VK_NULL_HANDLE;
  }
  DestroyFrames();
  if (Surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(GetInstance(), Surface, nullptr);
    Surface = VK_NULL_HANDLE;
  }
  Ready = false;
  TheStats.ready = false;
}

const RendererStats &Stats() { return TheStats; }

bool FrameStagingRetired(uint32_t frame_index) {
  if (frame_index >= kFramesInFlight) {
    return true;
  }
  const VkFence fence = Frames[frame_index].in_flight;
  if (fence == VK_NULL_HANDLE) {
    return true; // no fence, so nothing of ours is reading that staging
  }
  return vkGetFenceStatus(GetDevice(), fence) == VK_SUCCESS;
}

void SetOffscreen(bool enabled) {
  // The env read is marked done as well, so a run-time write is not undone the first time
  // something else asks what GKPLUS_VK_OFFSCREEN said.
  OffscreenWantedRead = true;
  OffscreenWanted = enabled;
}

bool Offscreen() { return OffscreenEnvEnabled(); }

bool OffscreenRunning() { return OffscreenActive; }

void SetPresentLinear(bool enabled) { PresentLinearFilter = enabled; }

bool PresentLinear() { return PresentLinearFilter; }

void RenderSize(uint32_t &width, uint32_t &height) {
  width = RenderExtent.width;
  height = RenderExtent.height;
}

const std::string &RendererError() { return Error; }

std::string FormatStats() {
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  add("requested: %s   ready: %s\n", RendererRequested() ? "vulkan" : "d3d9",
      Ready ? "yes" : "no");
  if (!Error.empty()) {
    out += "error: " + Error + "\n";
  }
  if (Ready) {
    add("swapchain: %ux%u, %u images, format %u, present mode %u\n", TheStats.width,
        TheStats.height, TheStats.image_count, TheStats.format, TheStats.present_mode);
    // The two sizes and what closes the gap. They differ by design - the game rasterises into a
    // 640x480 backbuffer and the window's client area is 628x468 - and the whole of §4.37 is
    // that rasterising at the second resamples every 2D draw.
    add("rendering at: %ux%u %s%s\n", RenderExtent.width, RenderExtent.height,
        OffscreenActive ? "offscreen, scaled to the swapchain at present" : "direct",
        OffscreenActive ? (PresentLinearFilter ? " (linear)" : " (nearest)") : "");
    add("presented: %llu   rebuilds: %llu   acquire failures: %llu\n",
        (unsigned long long)TheStats.frames_presented,
        (unsigned long long)TheStats.swapchain_rebuilds,
        (unsigned long long)TheStats.acquire_failures);
  }
  return out;
}

} // namespace vulkan
} // namespace gk
