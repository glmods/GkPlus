#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include "Console.h"
#include "Core.h"
#include "LuaEngine.h"
#include "Varint.h"

#include <cassert>
#include <string>

namespace gk {
namespace {
using TCallback = FastCall<void, int, char *>;
using TRegisterConsoleCommand =
    FastCall<void, const char *, const char *, TCallback, int>;
TRegisterConsoleCommand RegisterConsoleCommand;

using TSetupConsoleCommands = StdCall<>;
TSetupConsoleCommands SetupConsoleCommands;

using TPrint = FastCall<void, const char *>;
TPrint Print;

using TExecuteCommandLine = FastCall<void, const char *>;
TExecuteCommandLine ExecuteCommandLine;
TExecuteCommandLine ExecuteCommand;

char *CommandLine;
unsigned *TextColor;
unsigned *CursorColor;

lua_Integer RegisteredCommandsTable = LUA_NOREF;
lua_Integer ConsolePrint = LUA_NOREF;
lua_Integer ConsoleSetup = LUA_NOREF;

void __fastcall HookedPrint(const char *what) {
  if (ConsolePrint == LUA_NOREF || ConsolePrint == LUA_REFNIL) {
    return Print(what);
  }

  lua_State *L = Lua::GetEngine();
  lua_rawgeti(L, LUA_REGISTRYINDEX, ConsolePrint);
  if (!lua_isnil(L, -1)) {
    lua_pushstring(L, what);
    lua_call(L, 1, 0);
  } else {
    lua_pop(L, 1);
  }
  Print(what);
}

void __fastcall CommandLua(int a, char *b) {
  lua_State *L = Lua::GetEngine();
  int res = luaL_dostring(L, CommandLine);
  if (res != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    Print(msg);
    lua_pop(L, 1);
  }
}

void __fastcall CommandLuaCustom(int len, char *name) {
  auto L = Lua::GetEngine();
  std::string name_str(name - 1, len);
  lua_rawgeti(L, LUA_REGISTRYINDEX, RegisteredCommandsTable);
  lua_getfield(L, -1, name_str.c_str());
  lua_pushstring(L, CommandLine);
  lua_call(L, 1, 0);
}

int LuaRegisterCommand(lua_State *L) {
  auto name = luaL_tolstring(L, 1, nullptr);
  auto help = luaL_tolstring(L, 2, nullptr);
  RegisterConsoleCommand(name, help, CommandLuaCustom, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, RegisteredCommandsTable);
  lua_pushvalue(L, 3);
  lua_setfield(L, -2, name);
  lua_pop(L, 2);
  return 0;
}

void __fastcall CommandDebugBreak(int a, char *b) { DebugBreak(); }

void __stdcall HookedSetupConsoleCommands() {
  RegisterConsoleCommand("LUA", "Executes Lua code", CommandLua, 1);
  RegisterConsoleCommand("DEBUGBREAK", "Enters the debugger", CommandDebugBreak,
                         1);

  auto L = Lua::GetEngine();
  lua_rawgeti(L, LUA_REGISTRYINDEX, ConsoleSetup);

  if (!lua_isnil(L, -1)) {
    lua_call(L, 0, 0);
  } else {
    lua_pop(L, 1);
  }

  SetupConsoleCommands();
}

int LuaPrint(lua_State *L) {
  int nargs = lua_gettop(L);
  for (int i = 1; i <= nargs; ++i) {
    Print(lua_tostring(L, i));
  }
  return 0;
}

int LuaSetTextColor(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 1) {
    return 0;
  }

  *TextColor = lua_tointeger(L, 1);
  return 0;
}

int LuaSetCursorColor(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 1) {
    return 0;
  }

  *CursorColor = lua_tointeger(L, 1);
  return 0;
}

int LuaExecuteCommand(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 1) {
    return 0;
  }

  ExecuteCommand(lua_tostring(L, 1));
  return 0;
}

struct Command {
  void *dtor;
  Command *prev, *next;
  const char *command;
};

struct CommandList {
  Command *first;
  int num;
};

static CommandList *List;

static void __fastcall HookedExecuteCommandLine(const char *cmdline) {
  if (ConsolePrint != LUA_NOREF && ConsolePrint != LUA_REFNIL) {
    lua_State *L = Lua::GetEngine();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ConsolePrint);
    if (!lua_isnil(L, -1)) {
      lua_pushstring(L, "> ");
      lua_pushstring(L, cmdline);
      lua_concat(L, 2);
      lua_call(L, 1, 0);
    } else {
      lua_pop(L, 1);
    }
  }

  ExecuteCommandLine(cmdline);
}

using TExecuteCommandFile = FastCall<int, unsigned char *>;
static TExecuteCommandFile ExecuteCommandFile;

static int __fastcall HookedExecuteCommandFile(unsigned char *file) {
  if (file && (*file & 0x80)) {
    lua_Integer ref = DecodeVarint(file);
    auto L = Lua::GetEngine();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_call(L, 0, 0);
    return 1;
  }
  int result = ExecuteCommandFile(file);
  return result;
}

int LuaOnPrint(lua_State *L) {
  luaL_unref(L, LUA_REGISTRYINDEX, ConsolePrint);

  lua_pushvalue(L, 1);
  ConsolePrint = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

int LuaOnSetup(lua_State *L) {
  luaL_unref(L, LUA_REGISTRYINDEX, ConsoleSetup);

  lua_pushvalue(L, 1);
  ConsoleSetup = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}
} // namespace

ConsoleModule::ConsoleModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(RegisterConsoleCommand, 0x004d5d50);
  GetObjectAtOffset(SetupConsoleCommands, 0x0043c800);
  GetObjectAtOffset(Print, 0x004d4b50);
  GetObjectAtOffset(ExecuteCommandLine, 0x004d59e0);
  GetObjectAtOffset(CommandLine, 0x007b6958);
  GetObjectAtOffset(TextColor, 0x007b6950);
  GetObjectAtOffset(CursorColor, 0x007c149c);
  GetObjectAtOffset(ExecuteCommandFile, 0x0043f250);
  GetObjectAtOffset(ExecuteCommand, 0x004d6090);
  GetObjectAtOffset(List, 0x007b6aa8);

  lua_newtable(L);
  RegisteredCommandsTable = luaL_ref(L, LUA_REGISTRYINDEX);

  DetourAttach(&SetupConsoleCommands, HookedSetupConsoleCommands);
  DetourAttach(&ExecuteCommandLine, HookedExecuteCommandLine);
  DetourAttach(&ExecuteCommandFile, HookedExecuteCommandFile);
  DetourAttach(&Print, HookedPrint);
}

ConsoleModule::~ConsoleModule() {
  DetourDetach(&SetupConsoleCommands, HookedSetupConsoleCommands);
  DetourDetach(&ExecuteCommandLine, HookedExecuteCommandLine);
  DetourDetach(&ExecuteCommandFile, HookedExecuteCommandFile);
  DetourDetach(&Print, HookedPrint);
}

int ConsoleModule::Register(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, LuaPrint);
  lua_setfield(L, -2, "print");

  lua_pushcfunction(L, LuaSetTextColor);
  lua_setfield(L, -2, "set_text_color");

  lua_pushcfunction(L, LuaSetCursorColor);
  lua_setfield(L, -2, "set_cursor_color");

  lua_pushcfunction(L, LuaExecuteCommand);
  lua_setfield(L, -2, "execute");

  lua_pushcfunction(L, LuaRegisterCommand);
  lua_setfield(L, -2, "register_command");

  lua_pushcfunction(L, LuaOnSetup);
  lua_setfield(L, -2, "set_onsetup");

  lua_pushcfunction(L, LuaOnPrint);
  lua_setfield(L, -2, "set_onprint");

  return 1;
}
} // namespace gk