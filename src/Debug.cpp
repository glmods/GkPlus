#include "Debug.h"
#include "Core.h"

#include <cstdarg>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

namespace gk {
namespace {
CDeclVarargs<int, char *> DebugPrintError;
CDeclVarargs<int, char *> DebugPrintWarning;
CDeclVarargs<int, char *> DebugPrintFatal;

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
} // namespace

DebugSystem::DebugSystem() {
  GetObjectAtOffset(DebugPrintFatal, 0x00476fb0);
  GetObjectAtOffset(DebugPrintError, 0x00477000);
  GetObjectAtOffset(DebugPrintWarning, 0x00477050);

  DetourAttach(&DebugPrintFatal, HookedDebugPrint);
  DetourAttach(&DebugPrintError, HookedDebugPrint);
  DetourAttach(&DebugPrintWarning, HookedDebugPrint);
}

DebugSystem::~DebugSystem() {
  DetourDetach(&DebugPrintFatal, HookedDebugPrint);
  DetourDetach(&DebugPrintError, HookedDebugPrint);
  DetourDetach(&DebugPrintWarning, HookedDebugPrint);
}
} // namespace gk
