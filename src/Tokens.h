#pragma once

#include "Module.h"

namespace gk {
class TokensModule final : public Module<TokensModule> {
protected:
public:
  static constexpr const char *module_name = "gk.tokens";

  TokensModule(lua_State *L);
  int Register(lua_State *L);
};
} // namespace gk