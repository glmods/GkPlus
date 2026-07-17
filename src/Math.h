#pragma once

#include "Module.h"

#include "LuaEngine.h"

namespace gk {
struct Vec3 final {
  float x, y, z;

  bool operator==(const Vec3 &) const;

  int to_string(lua_State *L) const;

  static constexpr const char *metatable_name = "Vec3";

  static void setup_metatable(lua_State *L);

  using fields =
      Lua::Fields<Lua::ROSlot<"x", &Vec3::x>, Lua::ROSlot<"y", &Vec3::y>,
                  Lua::ROSlot<"z", &Vec3::z>>;
};

struct Vec4 final {
  float x, y, z, w;

  bool operator==(const Vec4 &) const;

  int to_string(lua_State *L) const;

  static constexpr const char *metatable_name = "Vec4";

  static void setup_metatable(lua_State *L);

  using fields =
      Lua::Fields<Lua::ROSlot<"x", &Vec4::x>, Lua::ROSlot<"y", &Vec4::y>,
                  Lua::ROSlot<"z", &Vec4::z>, Lua::ROSlot<"w", &Vec4::w>>;
};

class MathModule final : public Module<MathModule> {
public:
  static constexpr const char *module_name = "gk.math";

  MathModule(lua_State *L);
  int Register(lua_State *L);
};
} // namespace gk