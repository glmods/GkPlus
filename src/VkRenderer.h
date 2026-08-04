#pragma once

// The presentation spine: a Vulkan surface on the game's own window, a swapchain, and a
// frame loop driven at exactly the rate the game presents. Phase 1b of
// vulkan_renderer_notes.md. It draws a clear colour and nothing else yet - the bindless
// drawing goes on top of this, and everything hard about presentation is here rather than
// there.
//
// **Who owns the window.** D3D9 (through d3d8to9) and this cannot both present to the game's
// HWND. They do not have to: `CaptureDevice::Present` in src/D3D8Capture.cpp is already
// intercepted, so under GKPLUS_RENDERER=vulkan it returns D3D_OK without forwarding and
// calls DrawFrame() instead. The game keeps its D3D9 device and every resource it created -
// there is nothing to invalidate and nothing to crash - it simply stops reaching the screen.
// That is what makes the switch a single branch rather than a prerequisite null device.
//
// **Frame pacing comes from the game, not from us.** DrawFrame runs inside the game's own
// present call, on the main thread, so it inherits the engine's frame rate and its
// focus behaviour. Note that the game does not present at all while its window is inactive
// unless GKPLUS_RENDER_UNFOCUSED=1 (see src/GUI.h).
//
// **No render passes or framebuffers.** Dynamic rendering (Vulkan 1.3 core, required by
// VkContext) means a resize recreates the swapchain, its images and its views, and nothing
// else - there is no framebuffer or render pass object to keep in step.

#include <cstdint>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gk {
namespace vulkan {

// True when GKPLUS_RENDERER names the vulkan backend. Read once, at first use.
bool RendererRequested();

// Brings up the surface, swapchain and per-frame objects on `window`. Idempotent. Returns
// false and records a reason if anything fails; the caller is expected to fall back to the
// D3D path rather than treat it as fatal.
bool StartRenderer(HWND window);

// True once StartRenderer has succeeded and the swapchain is usable.
bool RendererReady();

// Acquire, record, submit, present. Recreates the swapchain when it reports itself out of
// date, which covers both a resize and the game's own ResetD3D paths. Cheap no-op if the
// renderer is not up. Main thread only.
void DrawFrame();

// Forces a swapchain rebuild before the next frame. Not normally needed - an actual resize
// surfaces as VK_ERROR_OUT_OF_DATE_KHR and is handled - but the game's Reset paths change
// the backbuffer size behind our back, and this makes that explicit rather than relying on
// the driver to notice.
void NotifyResize();

void ShutdownRenderer();

// Diagnostics for `render.vulkan` / the REPL.
struct RendererStats {
  bool ready = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t image_count = 0;
  uint32_t format = 0;
  uint32_t present_mode = 0;
  uint64_t frames_presented = 0;
  uint64_t swapchain_rebuilds = 0;
  uint64_t acquire_failures = 0;
};

const RendererStats &Stats();

// Whether the world is rasterised into an offscreen target at the GAME's backbuffer size and
// scaled onto the swapchain afterwards, rather than straight into the swapchain (§4.37).
//
// On by default, and off is the pre-§4.37 behaviour exactly, which is what makes it A/B-able on
// one paused frame the way `render.half_pixel` is - and it is worth A/B-ing, because it moves
// every pre-transformed pixel in the frame. `GKPLUS_VK_OFFSCREEN=0` sets the same thing at
// launch. `OffscreenRunning` is the separate question of whether it is actually up: it needs a
// swapchain that takes TRANSFER_DST and a backbuffer size from the game.
void SetOffscreen(bool enabled);
bool Offscreen();
bool OffscreenRunning();

// The filter for the final scale. NEAREST by default, and that is a measurement rather than a
// default: the original's own windowed stretch preserves a 4-bit texture's sixteen distinct
// values (§4.37), which a filtered downscale could not - so D3D drops columns rather than mixing
// them. `render.present_linear` is the A/B for that deduction.
void SetPresentLinear(bool enabled);
bool PresentLinear();

// What the world pass rasterises at, which is the game's backbuffer size when the offscreen
// target is up and the swapchain extent when it is not.
void RenderSize(uint32_t &width, uint32_t &height);
// Named apart from vulkan::LastError() in VkContext.h on purpose: that one is why the
// DEVICE failed to come up, this one is why presentation did. Same namespace, and the
// two failures have different fixes.
const std::string &RendererError();
std::string FormatStats();

} // namespace vulkan
} // namespace gk
