#include "LuaEngine.h"

static lua_State *lua;

namespace gk::Lua {
void Init() { lua = luaL_newstate(); }

void Close() { lua_close(lua); }

lua_State *GetEngine() { return lua; }

int detail::interop<int>::check(lua_State *L, int idx) {
  return luaL_checkinteger(L, idx);
}
int detail::interop<int>::opt(lua_State *L, int idx, int def) {
  return luaL_optinteger(L, idx, def);
}

float detail::interop<float>::check(lua_State *L, int idx) {
  return luaL_checknumber(L, idx);
}
float detail::interop<float>::opt(lua_State *L, int idx, float def) {
  return luaL_optnumber(L, idx, def);
}

double detail::interop<double>::check(lua_State *L, int idx) {
  return luaL_checknumber(L, idx);
}
double detail::interop<double>::opt(lua_State *L, int idx, double def) {
  return luaL_optnumber(L, idx, def);
}

bool detail::interop<bool>::to(lua_State *L, int idx) {
  return lua_toboolean(L, idx);
}

std::string_view detail::interop<std::string_view>::to(lua_State *L, int idx) {
  std::size_t l;
  auto s = luaL_tolstring(L, idx, &l);
  return {s, l};
}
std::string_view detail::interop<std::string_view>::check(lua_State *L,
                                                          int idx) {
  std::size_t l;
  auto s = luaL_checklstring(L, idx, &l);
  return {s, l};
}
std::string_view detail::interop<std::string_view>::opt(lua_State *L, int idx,
                                                        std::string_view def) {
  std::size_t l;
  auto s = luaL_optlstring(L, idx, def.data(), &l);
  return {s, l};
}

const char *detail::interop<const char *>::check(lua_State *L, int idx) {
  return luaL_checkstring(L, idx);
}
const char *detail::interop<const char *>::to(lua_State *L, int idx) {
  return luaL_tolstring(L, idx, nullptr);
}
const char *detail::interop<const char *>::opt(lua_State *L, int idx,
                                               const char *def) {
  return luaL_optstring(L, idx, def);
}

void detail::interop<int>::push(lua_State *L, int value) {
  lua_pushinteger(L, value);
}
void detail::interop<float>::push(lua_State *L, float value) {
  lua_pushnumber(L, value);
}
void detail::interop<double>::push(lua_State *L, double value) {
  lua_pushnumber(L, value);
}
void detail::interop<bool>::push(lua_State *L, bool value) {
  lua_pushboolean(L, value);
}
void detail::interop<std::string_view>::push(lua_State *L,
                                             std::string_view value) {
  lua_pushlstring(L, value.data(), value.size());
}
void detail::interop<const char *>::push(lua_State *L, const char *value) {
  lua_pushstring(L, value);
}
} // namespace gk::Lua