#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include "Actors.h"
#include "Camera.h"
#include "Chunks.h"
#include "Console.h"
#include "Core.h"
#include "Debug.h"
#include "GUI.h"
#include "InputFix.h"
#include "LuaEngine.h"
#include "Map.h"
#include "Math.h"
#include "Memory.h"
#include "Menu.h"
#include "Misc.h"
#include "Music.h"
#include "Roles.h"
#include "Tokens.h"
#include "Triggers.h"

#include <cassert>
#include <memory>

namespace gk {
namespace {
int LuaAtPanic(lua_State *L) {
  auto message = lua_tostring(L, 1);
  luaL_traceback(L, L, message, 0);
  message = luaL_tolstring(L, -1, nullptr);
  MessageBox(nullptr, message, "Lua error", MB_OK | MB_ICONERROR);
  assert(0);
  return 0;
}

int LuaExit(lua_State *L) {
  exit(1);
  return 0;
}
} // namespace

struct Modules {
  ConsoleModule console;
  MenuModule menu;
  GUIModule gui;
  TokensModule tokens;
  MemoryModule memory;
  ActorsModule actors;
  MathModule math;
  RolesModule roles;
  MapModule map;
  CameraModule camera;
  MiscModule misc;
  MusicModule music;
  DebugModule debug;
  TriggersModule triggers;
  InputFixModule inputfix;
  // ChunksModule chunks;

  Modules(lua_State *L)
      : console{L}, menu{L}, gui{L}, tokens{L}, memory{L}, actors{L}, math{L},
        roles{L}, map{L}, camera{L}, misc{L}, music{L}, debug{L}, triggers{L},
        inputfix{L} /*, chunks{L} */ {}
};

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
  if (DetourIsHelperProcess()) {
    return TRUE;
  }
  static std::unique_ptr<Modules> modules;

  if (reason == DLL_PROCESS_ATTACH) {
    Lua::Init();

    lua_State *L = Lua::GetEngine();

    lua_atpanic(L, LuaAtPanic);

    luaL_openlibs(L);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    modules = std::make_unique<Modules>(L);
    assert(DetourTransactionCommit() == NO_ERROR);

    luaL_loadfile(L, "main.lua");
    if (lua_isstring(L, -1)) {
      auto err = lua_tostring(L, -1);
      MessageBox(nullptr, err, "Lua error", MB_OK | MB_ICONERROR);
      assert(0);
    }
    lua_call(L, 0, 0);

  } else if (reason == DLL_PROCESS_DETACH) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    modules = nullptr;
    assert(DetourTransactionCommit() == NO_ERROR);

    Lua::Close();
  }

  return TRUE;
}
} // namespace gk