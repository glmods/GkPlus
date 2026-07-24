#pragma once

namespace gk {
// Redirects the game's DebugPrintFatal/Error/Warning (@ 0x00476fb0 / 0x00477000
// / 0x00477050) to OutputDebugString. RAII: attaches in the ctor, detaches in the
// dtor - construct/destroy inside a Detours transaction.
class DebugSystem {
public:
  DebugSystem();
  ~DebugSystem();
};
} // namespace gk
