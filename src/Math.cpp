#include "Math.h"

#include "Core.h"
#include "LuaEngine.h"

namespace gk {

static FastCall<void, Vec3 *, Vec3 *> CreateLaserFence;

MathModule::MathModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(CreateLaserFence, 0x0051c0f0);
}

bool Vec3::operator==(const Vec3 &) const = default;

int Vec3::to_string(lua_State *L) const {
  lua_pushfstring(L, "<Vec3 x: %f y: %f z: %f>", x, y, z);
  return 1;
}

void Vec3::setup_metatable(lua_State *L) {}

bool Vec4::operator==(const Vec4 &) const = default;

int Vec4::to_string(lua_State *L) const {
  lua_pushfstring(L, "<Vec4 x: %f y: %f z: %f w: %f>", x, y, z, w);
  return 1;
}

void Vec4::setup_metatable(lua_State *L) {}

int MathModule::Register(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, [](lua_State *L) {
    auto x = luaL_checknumber(L, 1);
    auto y = luaL_checknumber(L, 2);
    auto z = luaL_checknumber(L, 3);
    Lua::Create<Vec3>(L, x, y, z);
    return 1;
  });
  lua_setfield(L, -2, "vec3");

  lua_pushcfunction(L, [](lua_State *L) {
    auto x = luaL_checknumber(L, 1);
    auto y = luaL_checknumber(L, 2);
    auto z = luaL_checknumber(L, 3);
    auto w = luaL_checknumber(L, 4);
    Lua::Create<Vec4>(L, x, y, z, w);
    return 1;
  });
  lua_setfield(L, -2, "vec4");

  lua_pushcfunction(L, [](lua_State *L) {
    auto start = Lua::check<Vec3 *>(L, 1);
    auto end = Lua::check<Vec3 *>(L, 2);
    CreateLaserFence(start, end);
    return 0;
  });
  lua_setfield(L, -2, "create_laser_fence");

  return 1;
}
} // namespace gk