#pragma once

#include "Module.h"

namespace gk {
class ConsoleModule final : public Module<ConsoleModule> {
public:
  static constexpr const char *module_name = "gk.console";

  ConsoleModule(lua_State *L);
  ~ConsoleModule();

  int Register(lua_State *L);
};
} // namespace gk