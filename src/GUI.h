#pragma once

namespace gk {
// Per-frame overlay draw callback. Called between ImGui::NewFrame and
// ImGui::Render while the overlay is visible (toggled with F11). Null by default,
// so the overlay renders empty. This is the seam the QuickJS imgui module
// (imgui-quickjs) plugs into - src/Script.cpp installs a callback that forwards
// to the script's `draw_gui` export. The callback runs on the render thread
// inside an active ImGui frame, so it may issue ImGui:: calls directly.
using OverlayDrawCallback = void (*)();
void SetOverlayDrawCallback(OverlayDrawCallback callback);

// Per-frame heartbeat, called from the PresentScene hook once per presented
// frame on the main thread. Unlike OverlayDrawCallback it runs whether or not
// the overlay is visible, and *outside* any ImGui frame - so it must not issue
// ImGui:: calls. The script host uses it to drain the QuickJS job queue.
using FrameCallback = void (*)();
void SetFrameCallback(FrameCallback callback);

// Invokes whatever SetFrameCallback installed, if anything. Idempotent and
// cheap, because it is called from two independent places (see below).
void RunFrameCallback();

// A callback run from HookedWndProc in response to a private WM_APP message,
// which PostMessageLoopWork() posts to the game window. One slot, like the two
// callbacks above; Session.cpp owns it.
//
// **The frame callback is not a safe place for anything that reloads the
// world.** In a level it is driven from inside HookedPresentScene - i.e. from
// within the game's own render/present path - so a LoadLevel from there would
// tear down and rebuild the D3D resources underneath the frame that is still
// being presented. draw_gui is worse again: it runs between NewFrame and
// Render, so anything that throws or reloads leaves ImGui's stack unbalanced.
//
// A posted message lands at exactly the point the game does this kind of work
// itself: WM_KEYDOWN -> MainWindowWndProc -> HandleKeyMessage -> ... ->
// MenuScreenInputHandler -> OnMenuItemClicked is how every menu transition and
// every level start reaches the engine, and that is the top of the message
// loop, outside the renderer. PostMessage rather than SendMessage because the
// poster is usually the main thread itself, already inside a frame.
//
// Returns false if the game window is not resolved yet, or if PostMessage
// failed - in which case the work was NOT scheduled.
using MessageLoopCallback = void (*)();
void SetMessageLoopCallback(MessageLoopCallback callback);
bool PostMessageLoopWork();

// Enables (or removes) a WM_TIMER on the game window that runs the frame
// callback from HookedWndProc, roughly every 20 ms, in addition to the
// PresentScene hook.
//
// It exists because **Gunlok stops running frames entirely while its window is
// inactive**, which is measured, not inferred. With the game unfocused:
//
//   * `HasWindowFocus` @ 0x006a3744 goes 0 and `OnActivateApp` @ 0x0046f400
//     calls FUN_00574960, which releases the D3D resources and clears
//     DAT_007c1230;
//   * `RenderSceneAndPresent` @ 0x00574c50 wraps its whole body - scene,
//     EndScene, PresentScene - in `if (DAT_007c1230 != 0)`, so PresentScene is
//     never reached and the frame hook stops;
//   * both per-thread clock accumulators freeze at +0 over seconds, so the
//     simulation is not merely unrendered, it is not running. Those two are
//     `MainClock+0x18` and `ExecutorClock+0x18` (DAT_007c07e8 and DAT_007c07b8),
//     where ExecutorClock is 0x007c07a0 and a `Clock` is **0x24 bytes** - a 0x30
//     figure recorded elsewhere was the instance *stride*, not the size;
//   * yet the process still spins a full core, and SendMessageTimeout(WM_NULL)
//     answers in 7 ms - the main thread is in a reactivation loop that pumps
//     messages.
//
// A message is therefore the only way into that state, which is the state a
// debug channel is used in: you are typing in a terminal, so the game is behind
// you and by definition not focused. WM_TIMER rather than a thread posting
// PostMessage, because StopRepl runs from DllMain(DLL_PROCESS_DETACH) and
// waiting on a worker thread under the loader lock deadlocks. Must be called
// from the main thread, which owns the window.
void SetFrameWakeupEnabled(bool enabled);

// GKPLUS_RENDER_UNFOCUSED=1 makes the game keep rendering while its window is
// inactive, by skipping OnActivateApp @ 0x0046f400 entirely. Measured in level
// with the app genuinely deactivated: 0 frames over 5 s without it, 7,153 with.
//
// It is off by default and only the exact string "1" enables it, because it is
// sound only in WINDOWED mode - in exclusive fullscreen the device really is lost
// on Alt-Tab and keeping the gate set just turns skipped Presents into failed
// ones. It also leaves HasWindowFocus set and the DirectInput devices acquired.
//
// Do NOT try to do this by suppressing only ReleaseD3DResources @ 0x00574960, and
// do not no-op that function globally: the first kills the game (OnActivateApp's
// gain branch runs restore work with no counterpart on the loss side) and the
// second breaks resolution changes and multiplayer session setup, which are four
// of its six callers. vulkan_renderer_notes.md section 4.2 has the full account.
//
// This does not replace SetFrameWakeupEnabled above: the WM_TIMER heartbeat is
// still what drives the REPL at the front-end menus, where the game stops running
// frames for reasons beyond the D3D gate.

// Is the overlay currently toggled on (F11)? The ImGui *context* and the Win32
// backend belong to GUISystem whichever renderer is in use; only the rendering
// backend differs, so src/VkRenderer.cpp asks these two rather than owning any of
// the overlay's state.
bool IsOverlayVisible();

// Runs whatever SetOverlayDrawCallback installed, inside the caller's ImGui frame.
// Valid only between NewFrame and Render.
void RunOverlayDrawCallback();

// Installs the in-game ImGui/D3D overlay: the 8 D3D/window detours plus the ImGui
// context. RAII: attaches in the ctor, detaches in the dtor - construct/destroy
// inside a Detours transaction.
class GUISystem {
public:
  GUISystem();
  ~GUISystem();
};
} // namespace gk
