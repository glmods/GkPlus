#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <d3d8to9.hpp>

#include <imgui.h>

#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include "Core.h"
#include "D3D8Capture.h"
#include "GUI.h"
#include "VkRenderer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace gk {
namespace {
WNDPROC WndProc;
HWND *GameWindow;
// The game's own `direct3d_device` global. Typed as the *interface*, not d3d8to9's concrete
// Direct3DDevice8: with the capture layer installed (src/D3D8Capture.h) this holds a
// CaptureDevice instead, and a static cast to the wrong concrete class would read garbage.
// d3d8::ResolveD3D9Device unwraps whichever it is.
IDirect3DDevice8 **DirectXDevice;

FastCall<void, int> OnActivateApp;
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
  return d3d8::ResolveD3D9Device(*DirectXDevice);
}

// Whether the ImGui DX9 backend is the one drawing the overlay, which is neither of the two
// modes that do not have a D3D9 device: Vulkan draws its own overlay on its own backend, and the
// d3d8 passthrough has no D3D9 device at all because it forwards to Windows' own D3D8.
//
// One predicate rather than the test spelled out at each of the five sites - which is how the
// first version of the passthrough crashed, having gated Init and missed NewFrame.
bool Dx9Overlay() {
  return !vulkan::RendererRequested() && !d3d8::PassthroughToSystemD3D8();
}

// --- keeping the game rendering while unfocused (GKPLUS_RENDER_UNFOCUSED) ----------------
//
// Gunlok renders and presents NOTHING while its window is inactive: RenderSceneAndPresent
// @ 0x00574c50 wraps its whole body in `if (DAT_007c1230 != 0)`, and OnActivateApp clears
// that gate on focus loss by calling ReleaseD3DResources @ 0x00574960.
//
// The gate is a "D3D resources are valid" flag, not a "should I draw" flag - it is cleared
// *because* the textures, vertex buffers and cached state have just been released. So
// forcing it back to 1 draws through released objects. The only safe patch is to skip the
// release, which leaves the resources alive and the gate set.
//
// Suppressing ONLY the teardown is not enough, and that is the mistake this comment exists
// to record. OnActivateApp's two branches are not symmetric around ReleaseD3DResources: the
// focus-GAIN branch also runs restore work with no counterpart on the loss side. This
// comment used to pair FUN_005a1d60 against the loss branch's FUN_005a1ca0 as a
// release/reload couple; they are not one. 0x005a1ca0 is TextureManager_ReleaseAll (a COM
// `Release` per texture); 0x005a1d60 flips a creation flag in a 0x20-byte record and
// reloads nothing; the actual restore is TextureManager_RecreateAll @ 0x005a1b80, which
// RestoreD3DResources calls and which the old text omitted. So the gain branch runs
// RestoreD3DResources -> TextureManager_RecreateAll plus
// FUN_00468dc0/FUN_00557210/FUN_004b10f0. Skip the release and that restore still runs,
// against objects that were never released - so the conclusion below is unchanged.
//
// If TextureManager is ever mirrored: **its list at +0x1c is NOT a List<T>.** The node is
// {data, prev, next} with no vptr and the payload at +0x00, which is not List_Member<T>'s
// {vptr, prev, next, data} layout, so src/List.h does not describe it.
//
// So the whole handler is skipped instead. Nothing is torn down, nothing is rebuilt, and
// DAT_007c1230 keeps the value it had - which is what makes RenderSceneAndPresent keep
// running. The cost is that `HasWindowFocus` @ 0x006a3744 stays set and the DirectInput
// devices stay acquired while the window is inactive; harmless, because keyboard input
// arrives as WM_KEYDOWN to a window that is not receiving any (input_notes.md), and the
// DirectInput keyboard is the vestigial one InputFixSystem already suppresses.
//
// Off by default, and deliberately so: this is only sound in WINDOWED mode. In exclusive
// fullscreen the device really is lost on Alt-Tab, and keeping the gate set just means
// every Present fails instead of being skipped.
bool RenderUnfocused = false;

void __fastcall HookedOnActivateApp(int wParam) {
  if (RenderUnfocused) {
    return;
  }
  OnActivateApp(wParam);
}

int __fastcall HookedInitDirectSound(int arg) {
  ImGui_ImplWin32_Init(*GameWindow);
  return InitDirectSound(arg);
}

int __stdcall HookedCreateDirect3D() {
  if (CreateDirect3D()) {
    // Under GKPLUS_RENDERER=vulkan the overlay is drawn by src/VkRenderer.cpp on the
    // Vulkan backend instead. The ImGui context and the Win32 backend are still ours -
    // only the rendering half differs - so nothing else here changes.
    // ... and under GKPLUS_RENDERER=d3d8 there is no D3D9 device at all, because the capture
    // layer is forwarding to Windows' own D3D8. That mode exists to be the reference in an A/B,
    // so it renders the game and not the overlay; the REPL is how it is driven anyway.
    if (Dx9Overlay()) {
      ImGui_ImplDX9_Init(GetDX9Device());
      ImGui_ImplDX9_CreateDeviceObjects();
    }
    return 1;
  }

  return 0;
}

void __stdcall HookedReleaseDirect3DDevice() {
  if (Dx9Overlay()) {
    ImGui_ImplDX9_Shutdown();
  }
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

  // In Vulkan mode the overlay is built and recorded inside DrawFrame, which runs from
  // CaptureDevice::Present further down this same call. Doing it here as well would open a
  // second ImGui frame per frame.
  if (!*DirectXDevice || !ShowGUI || !Dx9Overlay()) {
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
  if (!Dx9Overlay()) {
    return ResetD3D1();
  }
  ImGui_ImplDX9_InvalidateDeviceObjects();
  int res = ResetD3D1();
  ImGui_ImplDX9_CreateDeviceObjects();
  return res;
}

int __fastcall HookedResetD3D2(int arg1, int arg2, int arg3, int arg4) {
  if (!Dx9Overlay()) {
    return ResetD3D2(arg1, arg2, arg3, arg4);
  }
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

bool IsOverlayVisible() { return ShowGUI; }

void RunOverlayDrawCallback() {
  if (OverlayDraw) {
    OverlayDraw();
  }
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
  {
    // Only "1" enables it; anything else (including "0") leaves the game's own behaviour
    // alone, so a stale variable set to 0 does not silently turn it on.
    char value[8] = {};
    const DWORD len =
        ::GetEnvironmentVariableA("GKPLUS_RENDER_UNFOCUSED", value, sizeof(value));
    RenderUnfocused = len == 1 && value[0] == '1';
  }

  GetObjectAtOffset(OnActivateApp, 0x0046f400);
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

  DetourAttach(&OnActivateApp, HookedOnActivateApp);
  DetourAttach(&WndProc, HookedWndProc);
  DetourAttach(&InitDirectSound, HookedInitDirectSound);
  DetourAttach(&CreateDirect3D, HookedCreateDirect3D);
  DetourAttach(&ReleaseDirect3DDevice, HookedReleaseDirect3DDevice);
  DetourAttach(&PresentScene, HookedPresentScene);
  DetourAttach(&ResetD3D1, HookedResetD3D1);
  DetourAttach(&ResetD3D2, HookedResetD3D2);
}

GUISystem::~GUISystem() {
  DetourDetach(&OnActivateApp, HookedOnActivateApp);
  DetourDetach(&WndProc, HookedWndProc);
  DetourDetach(&InitDirectSound, HookedInitDirectSound);
  DetourDetach(&CreateDirect3D, HookedCreateDirect3D);
  DetourDetach(&ReleaseDirect3DDevice, HookedReleaseDirect3DDevice);
  DetourDetach(&PresentScene, HookedPresentScene);
  DetourDetach(&ResetD3D1, HookedResetD3D1);
  DetourDetach(&ResetD3D2, HookedResetD3D2);
}
} // namespace gk
