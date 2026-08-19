#pragma once

namespace gk {
// Redirects the game's PrintFatalError and PrintParseError (@ 0x00476fb0 /
// 0x00477000) to OutputDebugString. **PrintParseWarning @ 0x00477050 is
// deliberately left alone** - the GLS parser emits one warning per unset field
// per section, 13,000+ per level load, which makes the game unplayable under any
// debugger. See the `RedirectWarnings` flag in Debug.cpp to turn them back on.
//
// All three are the GLS parser's own reporters, not a general debug-print
// family: the 25 callers of 0x00477050 are ParseGSH (18), GSHTokenize (3),
// PopFileFromParserStack (2) and FinalizeAndRegisterObject (1), and nothing
// outside the parser calls it. They were named DebugPrintFatal /
// DebugPrintError / DebugPrintWarning here until that was measured.
//
// RAII: attaches in the ctor, detaches in the dtor - construct/destroy inside a
// Detours transaction.
class DebugSystem {
public:
  DebugSystem();
  ~DebugSystem();
};
} // namespace gk
