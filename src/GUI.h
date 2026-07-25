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

// Installs the in-game ImGui/D3D overlay: the 8 D3D/window detours plus the ImGui
// context. RAII: attaches in the ctor, detaches in the dtor - construct/destroy
// inside a Detours transaction.
class GUISystem {
public:
  GUISystem();
  ~GUISystem();
};
} // namespace gk
