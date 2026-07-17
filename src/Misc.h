#pragma once
#include "Module.h"

namespace gk {
class MiscModule final : public Module<MiscModule> {
public:
  static constexpr const char *module_name = "gk.misc";

  MiscModule(lua_State *L);
  int Register(lua_State *L);
};
} // namespace gk