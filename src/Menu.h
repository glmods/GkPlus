#pragma once
#include "Module.h"

namespace gk {
class MenuModule final : public Module<MenuModule> {
public:
  static constexpr const char *module_name = "gk.menu";

  MenuModule(lua_State *L);
  ~MenuModule();
  int Register(lua_State *L);
};
} // namespace gk