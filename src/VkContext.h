#pragma once

// The Vulkan device GkPlus renders with. Phase 1 of vulkan_renderer_notes.md: bring up an
// instance and a logical device, and report whether this machine actually supports the
// features the bindless design in section 2 of that file is built on. No surface, no
// swapchain and no drawing yet - those come next, and there is no point building them on a
// device that cannot do descriptor indexing.
//
// Three things about the shape of this, in order of how much trouble they save:
//
// - **Initialization is lazy, and must NOT be moved into DllMain.** Vulkan is reached through
//   volk, whose volkInitialize does LoadLibrary("vulkan-1.dll"); calling LoadLibrary under
//   the loader lock is a deadlock. Everything here initializes on first use instead, from the
//   main thread, long after DLL_PROCESS_ATTACH.
//
// - **A machine with no Vulkan is not an error.** GkPlus ships as `d3d8.dll`, so a hard
//   dependency on the loader would stop the game launching at all. volk is used precisely
//   because it resolves the loader at run time: Initialize() returns a reason and the game
//   keeps running on the d3d8to9 path.
//
// - **32-bit is the constraint that matters, not the GPU.** gl.exe is x86, so this needs the
//   SysWOW64 loader and the ICD's 32-bit half (`VulkanDriverNameWow` in the display adapter
//   key, not the legacy Khronos\Vulkan\Drivers key, which modern drivers no longer write).
//   Device memory is reported here mainly so the address-space budget in section 3 of the
//   notes can be checked against a real adapter.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gk {
namespace vulkan {

// Why initialization failed, if it did. Ordered by how early the failure happens.
enum class InitResult {
  Ok,
  NoLoader,        // no vulkan-1.dll, or volkInitialize failed - not an error, just absent
  NoInstance,      // vkCreateInstance failed
  NoPhysicalDevice, // zero devices enumerated
  MissingFeatures, // a device exists but cannot do bindless
  NoDevice,        // vkCreateDevice failed
};

const char *InitResultName(InitResult result);

// What the chosen device can do. Only the fields the renderer design actually depends on.
struct DeviceCaps {
  std::string device_name;
  uint32_t api_version = 0;
  uint32_t driver_version = 0;
  bool discrete = false;

  // The features section 2 of the notes requires. All of these are Vulkan 1.2 core.
  bool descriptor_indexing = false;
  bool runtime_descriptor_array = false;
  bool partially_bound = false;
  bool variable_descriptor_count = false;
  bool update_after_bind = false;
  bool non_uniform_indexing = false;
  bool buffer_device_address = false;

  // Vulkan 1.3 core. Required, not optional: with dynamic rendering there are no render
  // pass or framebuffer objects to create, invalidate on resize, or keep in step with the
  // swapchain - which removes an entire category of bookkeeping from the renderer. Anything
  // that can do descriptor indexing and buffer device address can do these.
  bool dynamic_rendering = false;
  bool synchronization2 = false;

  // Vulkan 1.0 core and optional. Used only by the pre-transformed pipelines, which need D3D's
  // clamp of an out-of-slice z rather than Vulkan's clip (notes §4.45). Not required: without
  // it those draws clip where D3D clamps, which is one draw's worth of difference rather than
  // a renderer that will not start.
  bool depth_clamp = false;

  // Vulkan 1.0 core and optional. Without it `vkCmdDrawIndexedIndirect` is limited to a
  // `drawCount` of 1, which is the same thing as not having it - the whole point is one command
  // for a batch. Used only by the map lights' shadow bake (§4.62), which falls back to a draw
  // call per caster per face without it: slower to bake, identical atlas.
  bool multi_draw_indirect = false;

  // Vulkan 1.0 core and optional. The two extra stages the PN-triangle amplification pass needs
  // (§4.71). Not required, and deliberately not part of HasRequiredFeatures: without it the
  // tessellated pipelines are simply never built and every draw takes the ordinary two-stage
  // one, which is the same frame the renderer drew before the feature existed.
  bool tessellation_shader = false;

  // The sample counts the world pass can actually use, as a `VkSampleCountFlags` bitmask - the
  // INTERSECTION of `framebufferColorSampleCounts`, `framebufferDepthSampleCounts` and
  // `framebufferStencilSampleCounts`, because the pass has one colour and one depth/stencil
  // attachment and dynamic rendering requires every attachment and the pipeline to agree on the
  // count. Intersecting rather than reading the colour limit alone is not caution: a device may
  // advertise 8x colour and 4x depth, and asking for the colour figure would build an attachment
  // set no pipeline could be created against. Always contains VK_SAMPLE_COUNT_1_BIT.
  uint32_t sample_counts = 0;

  // Limits that size the design rather than merely describing the device.
  uint32_t max_bindless_textures = 0; // maxDescriptorSetUpdateAfterBindSampledImages
  // maxTessellationGenerationLevel - the ceiling on a tess factor. 64 is the guaranteed
  // minimum and every desktop device reports exactly that, so `render.tess_max` clamps to it
  // rather than to a constant.
  uint32_t max_tessellation_level = 0;
  uint32_t max_push_constants = 0;    // must fit FrameAddrs; 128 is the guaranteed minimum
  uint32_t graphics_queue_family = 0;
  uint64_t device_local_bytes = 0;
  uint64_t host_visible_device_local_bytes = 0; // the ReBAR heap, if there is one
};

// Brings up the instance and device if they are not up already. Idempotent, and cheap after
// the first call. Main thread only.
InitResult Initialize();

// True once Initialize() has succeeded.
bool Available();

// Valid only when Available(); all-zero otherwise.
const DeviceCaps &Caps();

// The result of the first Initialize() call, without re-running it.
InitResult Status();

// Any diagnostic text the failing step produced; empty on success.
const std::string &LastError();

// Validation-layer accounting. Enabled by GKPLUS_VK_VALIDATION=1 *and* the layer being
// installed; without a debug messenger the layer's findings go nowhere, so "no errors" and
// "no messenger" are told apart by ValidationEnabled() rather than by a zero count.
bool ValidationEnabled();
uint64_t ValidationErrorCount();
uint64_t ValidationWarningCount();

// The most recent messages, oldest first, capped at kValidationLog entries.
//
// This exists because DebugWrite is OutputDebugString and nothing else, so without a
// debugger attached the layer's findings are invisible - and attaching one to Gunlok makes
// it crawl (game_defects_notes.md). The REPL is how this project observes a running game, so
// the messages have to be reachable from it. A count alone says something is wrong without
// saying what.
constexpr size_t kValidationLog = 64;
const std::vector<std::string> &ValidationMessages();
void ClearValidationMessages();

// Human-readable capability report, for the REPL and the overlay.
std::string FormatCaps();

// Tears the device down. Safe to call when nothing was ever brought up.
//
// Deliberately NOT wired to DLL_PROCESS_DETACH: the game faults on exit
// (game_defects_notes.md section 4), so process-exit cleanup is not something this codebase
// relies on - src/Vfs.cpp takes the same view of its temp directories.
void Shutdown();

} // namespace vulkan
} // namespace gk
