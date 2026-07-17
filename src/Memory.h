#pragma once

#include "Module.h"

namespace gk {
class MemoryModule final : public Module<MemoryModule> {
public:
  static constexpr const char *module_name = "gk.memory";

  MemoryModule(lua_State *L);
  int Register(lua_State *L);
};
} // namespace gk