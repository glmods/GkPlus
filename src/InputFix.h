#pragma once

namespace gk {
// Hook-only subsystem (no scripting surface). Neutralizes the game's vestigial
// DirectInput keyboard device so dinput.dll never arms its WH_KEYBOARD_LL
// low-level hook - the thing that freezes system-wide keyboard input while the
// process is paused in a debugger. See InputFix.cpp for the full rationale.
class InputFixSystem {
public:
  InputFixSystem();
  ~InputFixSystem();
};
} // namespace gk
