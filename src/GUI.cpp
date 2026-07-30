#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <d3d8to9.hpp>

#include <imgui.h>

#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include "Core.h"
#include "GUI.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace gk {
namespace {
WNDPROC WndProc;
HWND *GameWindow;
Direct3DDevice8 **DirectXDevice;

FastCall<int, int> InitDirectSound;
StdCall<int> CreateDirect3D;
StdCall<> ReleaseDirect3DDevice;
StdCall<> PresentScene;
StdCall<int> ResetD3D1;
FastCall<int, int, int, int, int> ResetD3D2;

bool ShowGUI = false;

// The native seams (see GUI.h). Null until something installs a callback.
//
// If you are hunting for a function that ticks once per frame: 0x0044dfc0 is not
// it, whatever its name suggests. GkPlus called it DoEvents and detoured it on
// the strength of that name; the hook was a pure passthrough in every revision
// from the initial commit until it was deleted, and the detour only ever cost a
// trampoline. It is a per-thread monotonic clock accumulator (both game threads,
// many calls per frame from the render path), and reading it as a message pump
// cost real debugging time - it is named AccumulateThreadClock in the Ghidra DB
// now, with a plate comment. The two seams that do work are HookedPresentScene
// and, because the game stops presenting entirely whenever its window is
// inactive, the WM_TIMER handled in HookedWndProc below.
OverlayDrawCallback OverlayDraw = nullptr;
FrameCallback Frame = nullptr;
MessageLoopCallback MessageLoopWork = nullptr;

IDirect3DDevice9 *GetDX9Device() {
  return (*DirectXDevice)->GetProxyInterface();
}

int __fastcall HookedInitDirectSound(int arg) {
  ImGui_ImplWin32_Init(*GameWindow);
  return InitDirectSound(arg);
}

int __stdcall HookedCreateDirect3D() {
  if (CreateDirect3D()) {
    ImGui_ImplDX9_Init(GetDX9Device());
    ImGui_ImplDX9_CreateDeviceObjects();
    return 1;
  }

  return 0;
}

void __stdcall HookedReleaseDirect3DDevice() {
  ImGui_ImplDX9_Shutdown();
  ReleaseDirect3DDevice();
}

void __stdcall HookedPresentScene() {
  // Before the overlay check on purpose: this is the once-per-frame heartbeat,
  // and it has to keep ticking while the overlay is hidden. It covers in-game
  // and level loads only - the front end presents nothing, and nothing else
  // calls RunFrameCallback, so at the menus the WM_TIMER below is the *only*
  // driver. (A previous version of this comment credited CustomMenu.cpp with
  // covering the front end. It never has.)
  RunFrameCallback();

  if (!*DirectXDevice || !ShowGUI) {
    return PresentScene();
  }
  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  if (OverlayDraw) {
    OverlayDraw();
  }

  if (GetDX9Device()->BeginScene() >= 0) {
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    GetDX9Device()->EndScene();
  }

  PresentScene();
}

int __stdcall HookedResetD3D1() {
  ImGui_ImplDX9_InvalidateDeviceObjects();
  int res = ResetD3D1();
  ImGui_ImplDX9_CreateDeviceObjects();
  return res;
}

int __fastcall HookedResetD3D2(int arg1, int arg2, int arg3, int arg4) {
  ImGui_ImplDX9_InvalidateDeviceObjects();
  int res = ResetD3D2(arg1, arg2, arg3, arg4);
  ImGui_ImplDX9_CreateDeviceObjects();
  return res;
}

// Our own timer id, and the re-entrancy guard for the callback it drives - a
// script running inside the callback could in principle pump a message and land
// back here.
constexpr UINT_PTR FrameWakeupTimer = 0x476b01;
constexpr UINT FrameWakeupMs = 20;
bool InFrameWakeup = false;

// Our own message id, and the same guard for it. WM_APP is the range Windows
// reserves for an application's private messages, so nothing the game or a
// library sends can collide with it.
constexpr UINT WM_GKPLUS_MESSAGE_LOOP_WORK = WM_APP + 0x6b01;
bool InMessageLoopWork = false;

LRESULT WINAPI HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam,
                             LPARAM lParam) {
  // Ours, and the game has never heard of it: handled here and not forwarded.
  // This is the only heartbeat that survives the window losing focus - see
  // SetFrameWakeupEnabled in GUI.h for why that is not a nicety.
  if (msg == WM_TIMER && wParam == FrameWakeupTimer) {
    if (!InFrameWakeup) {
      InFrameWakeup = true;
      RunFrameCallback();
      InFrameWakeup = false;
    }
    return 0;
  }

  // Likewise ours. See SetMessageLoopCallback in GUI.h: this is the seam for
  // work that must NOT run inside the renderer, and the re-entrancy guard is
  // load-bearing rather than defensive - the callback reloads the world, and
  // LoadLevel pumps messages while it does.
  if (msg == WM_GKPLUS_MESSAGE_LOOP_WORK) {
    if (!InMessageLoopWork && MessageLoopWork) {
      InMessageLoopWork = true;
      MessageLoopWork();
      InMessageLoopWork = false;
    }
    return 0;
  }

  if (msg == WM_KEYDOWN && wParam == VK_F11) {
    ShowGUI = !ShowGUI;
  }

  if (ShowGUI) {
    auto &io = ImGui::GetIO();
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
      return true;
    } else if (io.WantCaptureKeyboard || io.WantCaptureMouse) {
      return DefWindowProc(hWnd, msg, wParam, lParam);
    }
  }
  return WndProc(hWnd, msg, wParam, lParam);
}
} // namespace

void SetOverlayDrawCallback(OverlayDrawCallback callback) {
  OverlayDraw = callback;
}

void SetFrameCallback(FrameCallback callback) { Frame = callback; }

void SetMessageLoopCallback(MessageLoopCallback callback) {
  MessageLoopWork = callback;
}

bool PostMessageLoopWork() {
  if (!GameWindow || !*GameWindow) {
    return false; // GUISystem has not resolved the window yet
  }
  return ::PostMessageW(*GameWindow, WM_GKPLUS_MESSAGE_LOOP_WORK, 0, 0) != 0;
}

void RunFrameCallback() {
  if (Frame) {
    Frame();
  }
}

void SetFrameWakeupEnabled(bool enabled) {
  if (!GameWindow || !*GameWindow) {
    return; // GUISystem has not resolved the window yet
  }
  if (enabled) {
    ::SetTimer(*GameWindow, FrameWakeupTimer, FrameWakeupMs, nullptr);
  } else {
    ::KillTimer(*GameWindow, FrameWakeupTimer);
  }
}

GUISystem::GUISystem() {
  GetObjectAtOffset(WndProc, 0x0046aad0);
  GetObjectAtOffset(GameWindow, 0x006b02b8);
  GetObjectAtOffset(DirectXDevice, 0x007c121c);
  GetObjectAtOffset(InitDirectSound, 0x00574690);
  GetObjectAtOffset(CreateDirect3D, 0x0046a1e0);
  GetObjectAtOffset(ReleaseDirect3DDevice, 0x00574bc0);
  GetObjectAtOffset(PresentScene, 0x00574d30);
  GetObjectAtOffset(ResetD3D1, 0x00574ec0);
  GetObjectAtOffset(ResetD3D2, 0x00575160);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  auto &io = ImGui::GetIO();
  io.IniFilename = nullptr;

  ImGui::StyleColorsDark();

  DetourAttach(&WndProc, HookedWndProc);
  DetourAttach(&InitDirectSound, HookedInitDirectSound);
  DetourAttach(&CreateDirect3D, HookedCreateDirect3D);
  DetourAttach(&ReleaseDirect3DDevice, HookedReleaseDirect3DDevice);
  DetourAttach(&PresentScene, HookedPresentScene);
  DetourAttach(&ResetD3D1, HookedResetD3D1);
  DetourAttach(&ResetD3D2, HookedResetD3D2);
}

GUISystem::~GUISystem() {
  DetourDetach(&WndProc, HookedWndProc);
  DetourDetach(&InitDirectSound, HookedInitDirectSound);
  DetourDetach(&CreateDirect3D, HookedCreateDirect3D);
  DetourDetach(&ReleaseDirect3DDevice, HookedReleaseDirect3DDevice);
  DetourDetach(&PresentScene, HookedPresentScene);
  DetourDetach(&ResetD3D1, HookedResetD3D1);
  DetourDetach(&ResetD3D2, HookedResetD3D2);
}
} // namespace gk
