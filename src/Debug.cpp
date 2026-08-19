#include "Debug.h"
#include "Core.h"

#include <cstdarg>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

namespace gk {
namespace {
// The GLS parser's three reporters, all __cdecl varargs ending in a bare RET.
// **All three genuinely return `int`**, so this signature is correct rather than
// merely harmless: the bodies are byte-identical apart from the counter they bump
// (0x00739a38 for Fatal and Error, 0x00739a3c for Warning), and EAX on exit is
// `___stdio_common_vsprintf`'s character count, which `__security_check_cookie`
// preserves. Separately, and independently: no call site of any of the three reads
// EAX (5 / 24 / 25 sites), so the value is also unobservable. The formatted text
// itself goes into a 2048-byte stack buffer and is discarded - see
// game_defects_notes.md on these reporters throwing their output away.
CDeclVarargs<int, char *> PrintParseError;
CDeclVarargs<int, char *> PrintParseWarning;
CDeclVarargs<int, char *> PrintFatalError;

// **Warnings are deliberately not redirected.** The GLS parser reports
// "default value assumed for '<field>'" through PrintParseWarning for every unset
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
  GetObjectAtOffset(PrintFatalError, 0x00476fb0);
  GetObjectAtOffset(PrintParseError, 0x00477000);
  GetObjectAtOffset(PrintParseWarning, 0x00477050);

  DetourAttach(&PrintFatalError, HookedDebugPrint);
  DetourAttach(&PrintParseError, HookedDebugPrint);
  if (RedirectWarnings) {
    DetourAttach(&PrintParseWarning, HookedDebugPrint);
  }
}

DebugSystem::~DebugSystem() {
  DetourDetach(&PrintFatalError, HookedDebugPrint);
  DetourDetach(&PrintParseError, HookedDebugPrint);
  // Must mirror the ctor exactly - detaching a hook that was never attached
  // fails the whole Detours transaction.
  if (RedirectWarnings) {
    DetourDetach(&PrintParseWarning, HookedDebugPrint);
  }
}
} // namespace gk
