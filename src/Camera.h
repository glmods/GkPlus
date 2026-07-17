#pragma once

#include "Module.h"

namespace gk {
class CameraModule final : public Module<CameraModule> {
public:
  static constexpr const char *module_name = "gk.camera";

  CameraModule(lua_State *L);
  int Register(lua_State *L);
};
} // namespace gk