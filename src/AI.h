#pragma once

#include "Module.h"

namespace gk {
class AIModule final : public Module<AIModule> {
public:
  static constexpr const char *module_name = "gk.ai";

  AIModule(lua_State *L);

  int Register(lua_State *L);
};
} // namespace gk