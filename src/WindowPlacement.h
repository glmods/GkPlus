#pragma once

// Puts Gunlok's windowed-mode window inside the monitor's *work area* instead of
// at the screen origin, so it does not sit underneath a taskbar docked to the
// left or top edge.
//
// --- What the game does ----------------------------------------------------------
//
// `WinMain` @ 0x0046aef0 has the only two CreateWindowExA call sites in the whole
// image, chosen by `ViewFlags & 1` (bit 0 set = windowed):
//
//   windowed   @ 0x0046b585  ex WS_EX_APPWINDOW, style 0x90CF0000
//                            (WS_POPUP|WS_VISIBLE|WS_OVERLAPPEDWINDOW),
//                            X 0, Y 0, size ResolutionWidth/Height plus a
//                            hand-rolled non-client margin
//   fullscreen @ 0x0046b5cf  ex WS_EX_TOPMOST, style 0x90080000, X 0, Y 0,
//                            SM_CXSCREEN x SM_CYSCREEN
//
// **X and Y are literal zeroes** - not CW_USEDEFAULT, not a centering
// computation. The game never asks Windows where it may put a window: user32's
// import set contains no SystemParametersInfo, GetMonitorInfo, MonitorFromWindow,
// AdjustWindowRect or SetWindowPlacement at all. A taskbar on the left or top
// edge is simply overlapped.
//
// --- Why the import table, and why at creation -----------------------------------
//
// Same argument as src/FileHooks.h: one pointer write to gl.exe's own IAT slot
// (0x0064d294) catches the call regardless of how the call site reads it, and
// catches only gl.exe - this DLL's own window calls resolve through its own
// imports. Detouring user32 would hit every window in the process.
//
// It has to happen *at* creation rather than after, because the windowed style
// includes WS_VISIBLE: the window is on screen the moment CreateWindowExA
// returns, so a corrective SetWindowPos would be a visible jump.
//
// Nothing undoes it afterwards. The binary's single SetWindowPos (in
// `SetVideoMode` @ 0x0046a0b0, the resolution-change path) passes
// `SWP_NOMOVE|SWP_NOZORDER`, so its X/Y arguments are discarded by Windows and a
// mode change resizes in place. `OnWindowMoved` @ 0x00470dd0 and
// `OnClientSizeChanged` @ 0x0046a560 - the DB called them `WindowMove` and
// `WindowResize` until the bodies were read - move and resize nothing; they are
// the WM_MOVE and WM_SIZE bookkeepers for the client-rect globals the mouse
// mapping reads. That bookkeeping is what makes this safe: WinMain re-derives
// GameWindowX/Y from the live window with ClientToScreen -> OnWindowMoved at
// 0x0046b91b, so the cursor stays aligned wherever the window ends up.
//
// The size is left exactly as the game computed it. Correcting the non-client
// margin (it undercounts by SM_CXPADDEDBORDER, which is where the 628x468 client
// in vulkan_renderer_notes.md comes from) would only change the windowed
// Present stretch, since the backbuffer is sized from ResolutionWidth/Height
// independently - and SetVideoMode would undo it on the next resolution change.

namespace gk {

// RAII, like every other *System: construct from entry.cpp. The IAT write is a
// plain memory store and needs no Detours transaction of its own, but the
// aggregate is built inside one anyway.
//
// `GKPLUS_WINDOW_PLACEMENT=raw` restores the stock behaviour (window at 0,0) for
// anyone who wants it; `work-area` is the default and the only other value.
class WindowPlacementSystem {
public:
  WindowPlacementSystem();
  ~WindowPlacementSystem();
  WindowPlacementSystem(const WindowPlacementSystem &) = delete;
  WindowPlacementSystem &operator=(const WindowPlacementSystem &) = delete;
};

} // namespace gk
