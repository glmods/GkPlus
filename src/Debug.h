#pragma once

namespace gk {
// Redirects the game's DebugPrintFatal and DebugPrintError (@ 0x00476fb0 /
// 0x00477000) to OutputDebugString. **DebugPrintWarning @ 0x00477050 is
// deliberately left alone** - the GLS parser emits one warning per unset field
// per section, 13,000+ per level load, which makes the game unplayable under any
// debugger. See the `RedirectWarnings` flag in Debug.cpp to turn them back on.
//
// RAII: attaches in the ctor, detaches in the dtor - construct/destroy inside a
// Detours transaction.
class DebugSystem {
public:
  DebugSystem();
  ~DebugSystem();
};
} // namespace gk
