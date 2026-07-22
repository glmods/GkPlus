#pragma once

#include "Module.h"

namespace gk {
// Hook-only module (no Lua surface). Neutralizes the game's vestigial
// DirectInput keyboard device so dinput.dll never arms its WH_KEYBOARD_LL
// low-level hook - the thing that freezes system-wide keyboard input while the
// process is paused in a debugger. See InputFix.cpp for the full rationale.
class InputFixModule final : public Module<InputFixModule> {
public:
  InputFixModule(lua_State *L);
  ~InputFixModule();
};
} // namespace gk
