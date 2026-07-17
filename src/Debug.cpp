#include "Debug.h"
#include "Core.h"

#include <cstdarg>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

namespace gk {
static CDeclVarargs<int, char *> DebugPrintError;
static CDeclVarargs<int, char *> DebugPrintWarning;
static CDeclVarargs<int, char *> DebugPrintFatal;

int __cdecl HookedDebugPrint(char *fmt, ...) {
  char buffer[2048]{};
  va_list args;
  va_start(args, fmt);
  int res = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  OutputDebugString(buffer);
  OutputDebugString("\n");

  return res;
}

DebugModule::DebugModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(DebugPrintFatal, 0x00476fb0);
  GetObjectAtOffset(DebugPrintError, 0x00477000);
  GetObjectAtOffset(DebugPrintWarning, 0x00477050);

  DetourAttach(&DebugPrintFatal, HookedDebugPrint);
  DetourAttach(&DebugPrintError, HookedDebugPrint);
  DetourAttach(&DebugPrintWarning, HookedDebugPrint);
}

DebugModule::~DebugModule() {
  DetourDetach(&DebugPrintFatal, HookedDebugPrint);
  DetourDetach(&DebugPrintError, HookedDebugPrint);
  DetourDetach(&DebugPrintWarning, HookedDebugPrint);
}

int DebugModule::Register(lua_State *L) {
  lua_createtable(L, 0, 2);

  lua_pushcfunction(L, [](lua_State *L) {
    int nargs = lua_gettop(L);
    if (nargs < 0) {
      return 0;
    }

    for (int i = 1; i <= nargs; ++i) {
      OutputDebugString(lua_tostring(L, i));
      OutputDebugString("\t");
    }
    OutputDebugString("\n");

    return 0;
  });
  lua_setfield(L, -2, "log");

  lua_pushcfunction(L, [](lua_State *) {
    DebugBreak();
    return 0;
  });
  lua_setfield(L, -2, "dbgbreak");

  return 1;
}
} // namespace gk