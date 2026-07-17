#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <d3d8to9.hpp>

#include <imgui.h>

#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include "Core.h"
#include "GUI.h"
#include "ImGuiBindings.h"
#include "LuaEngine.h"

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
StdCall<int> DoEvents;

bool ShowGUI = false;

lua_Integer OnDrawGui = LUA_NOREF;

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
  if (!*DirectXDevice || !ShowGUI) {
    return PresentScene();
  }
  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  auto L = Lua::GetEngine();
  lua_rawgeti(L, LUA_REGISTRYINDEX, OnDrawGui);
  if (!lua_isnil(L, -1)) {
    PushImgui(L);
    lua_call(L, 1, 0);
  } else {
    lua_pop(L, 1);
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

int __stdcall HookedDoEvents() { return DoEvents(); }

LRESULT WINAPI HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam,
                             LPARAM lParam) {
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

int LuaOnDrawGui(lua_State *L) {
  luaL_unref(L, LUA_REGISTRYINDEX, OnDrawGui);
  lua_pushvalue(L, 1);
  OnDrawGui = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}
} // namespace

GUIModule::GUIModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(WndProc, 0x0046aad0);
  GetObjectAtOffset(GameWindow, 0x006b02b8);
  GetObjectAtOffset(DirectXDevice, 0x007c121c);
  GetObjectAtOffset(InitDirectSound, 0x00574690);
  GetObjectAtOffset(CreateDirect3D, 0x0046a1e0);
  GetObjectAtOffset(ReleaseDirect3DDevice, 0x00574bc0);
  GetObjectAtOffset(PresentScene, 0x00574d30);
  GetObjectAtOffset(ResetD3D1, 0x00574ec0);
  GetObjectAtOffset(ResetD3D2, 0x00575160);
  GetObjectAtOffset(DoEvents, 0x0044dfc0);

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
  DetourAttach(&DoEvents, HookedDoEvents);
}

GUIModule::~GUIModule() {
  DetourDetach(&WndProc, HookedWndProc);
  DetourDetach(&InitDirectSound, HookedInitDirectSound);
  DetourDetach(&CreateDirect3D, HookedCreateDirect3D);
  DetourDetach(&ReleaseDirect3DDevice, HookedReleaseDirect3DDevice);
  DetourDetach(&PresentScene, HookedPresentScene);
  DetourDetach(&ResetD3D1, HookedResetD3D1);
  DetourDetach(&ResetD3D2, HookedResetD3D2);
  DetourDetach(&DoEvents, HookedDoEvents);
}

int GUIModule::Register(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, LuaOnDrawGui);
  lua_setfield(L, -2, "set_ondraw");

  lua_pushcfunction(L, ([](lua_State *L) {
                      auto file = Lua::to<std::string_view>(L, 1);
                      auto sz = Lua::check<float>(L, 2);
                      auto font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
                          file.data(), sz);
                      Lua::Create<GuiFont>(L, font);
                      return 1;
                    }));
  lua_setfield(L, -2, "load_font");
  return 1;
}
} // namespace gk