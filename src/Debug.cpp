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

// **Warnings are deliberately not redirected.** The GLS parser reports
// "default value assumed for '<field>'" through DebugPrintWarning for every unset
// field of every section it parses - 13,000+ calls for one level load of the
// Training Level. Sending those to OutputDebugString makes the game unplayable
// whenever a debugger is attached, because each call is a synchronous round-trip
// to the debugger; `.outmask` does not help, since it only suppresses *display*,
// not delivery. Measured: the same build loads and exits in ~22s undebugged and
// cannot finish the parse in 40s under cdb.
//
// Unhooking costs nothing but visibility: the game's own PrintParseWarning
// @ 0x00477050 vsprintf's into a 2048-byte *local* and throws it away, bumping a
// counter at 0x00739a3c. The output call was compiled out of the shipped build, so
// this hook is the only thing that ever made those messages observable - and
// disabling it routes nothing to the console.
//
// Fatal and Error stay hooked - they are rare, and they are the ones worth
// seeing. Flip this to true to get the warning firehose back.
constexpr bool RedirectWarnings = false;

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
  if (RedirectWarnings) {
    DetourAttach(&DebugPrintWarning, HookedDebugPrint);
  }
}

DebugSystem::~DebugSystem() {
  DetourDetach(&DebugPrintFatal, HookedDebugPrint);
  DetourDetach(&DebugPrintError, HookedDebugPrint);
  // Must mirror the ctor exactly - detaching a hook that was never attached
  // fails the whole Detours transaction.
  if (RedirectWarnings) {
    DetourDetach(&DebugPrintWarning, HookedDebugPrint);
  }
}
} // namespace gk
