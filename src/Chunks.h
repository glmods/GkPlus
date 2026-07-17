#pragma once

#include "Module.h"

namespace gk {
class ChunksModule final : public Module<ChunksModule> {
public:
  ChunksModule(lua_State *L);
  ~ChunksModule();
};
} // namespace gk