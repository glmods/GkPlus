#pragma once

#include "Module.h"

namespace gk {
class GUIModule final : public Module<GUIModule> {
public:
  static constexpr const char *module_name = "gk.gui";

  GUIModule(lua_State *L);
  ~GUIModule();
  int Register(lua_State *L);
};
} // namespace gk