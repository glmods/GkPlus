#pragma once
#include "Module.h"

namespace gk {
class DebugModule final : public Module<DebugModule> {
public:
  static constexpr const char *module_name = "gk.debug";

  DebugModule(lua_State *L);
  ~DebugModule();
  int Register(lua_State *L);
};
} // namespace gk