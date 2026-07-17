#pragma once

#include "Module.h"

namespace gk {
class TriggersModule final : public Module<TriggersModule> {
protected:
public:
  static constexpr const char *module_name = "gk.triggers";

  TriggersModule(lua_State *L);
  ~TriggersModule();
  int Register(lua_State *L);
};
} // namespace gk