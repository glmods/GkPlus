#pragma once

// RenderDoc, driven from inside the process.
//
// The frame-debugging loop without it is: change one thing, rebuild, relaunch, load a level,
// screenshot, guess again. That is several minutes per hypothesis, and it answers "does this
// look right" rather than "what are the actual numbers". RenderDoc's mesh viewer shows the
// vertex shader's input and output side by side, which is the difference between guessing at a
// matrix convention and reading it.
//
// **The in-app API rather than the hotkey**, for two reasons that are specific to this process
// and not preference:
//
//   * the game has a LIVE D3D9 device from d3d8to9 at the same time as our Vulkan one, and
//     RenderDoc captures one API per frame - the hotkey may well grab the wrong one;
//   * GkPlus is a DLL proxy, so there is no "launch this exe" step for RenderDoc's UI to hook.
//     Loading the DLL ourselves and calling StartFrameCapture is unambiguous.
//
// Off unless `GKPLUS_RENDERDOC` is set, and it must be loaded before the Vulkan instance
// exists - RenderDoc works as a layer, and a layer cannot be inserted into an instance that has
// already been created. `vulkan::Initialize()` calls Load() as its first act for that reason.

#include <cstdint>
#include <string>

namespace gk {
namespace vulkan {

// Loads renderdoc.dll and fetches its API. Safe to call more than once; a no-op unless
// GKPLUS_RENDERDOC is set. Must run before vkCreateInstance.
void LoadRenderDoc();

bool RenderDocLoaded();

// Arms a capture of the next frame. Returns false if RenderDoc is not loaded. The capture is
// written next to the game as `gkplus_frame_NNNN.rdc`.
bool TriggerCapture();

// Called by the renderer around the frame it is recording. Both are no-ops unless a capture is
// armed, so the frame path can call them unconditionally.
void BeginFrameCaptureIfArmed();
void EndFrameCaptureIfArmed();

// A capture window that is NOT a frame, for the staging path.
//
// The upload corruption happens during a level load, and a load presents nothing at all - the
// copies go through FlushPendingNow on its own command buffer, so there is no frame to hang a
// capture on. Capturing the whole load is not an option either: it stages 360 MB through
// persistently mapped memory, all of which RenderDoc must serialise, inside a 32-bit process
// that is already short of address space.
//
// So the unit is one staging BATCH - everything between two records, at most a ring's worth.
// `CaptureStagingBatch(n)` captures the nth, which is reproducible because a load stages the
// same bytes in the same order every time: one run says which batch carries the copy of
// interest, the next captures it.
//
// The device pointer has to be given explicitly here, unlike the frame path: outside a frame
// "whatever is active" can resolve to the game's D3D9 device.
void CaptureStagingBatch(uint32_t batch);
void BeginBatchCaptureIfArmed(uint32_t batch, void *vk_instance);
void EndBatchCaptureIfArmed(uint32_t batch);

std::string FormatCaptureStatus();

} // namespace vulkan
} // namespace gk
