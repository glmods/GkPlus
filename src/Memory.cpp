#include "Memory.h"

#include "Core.h"
#include "LuaEngine.h"

#include <cassert>

namespace gk {
namespace {

CDecl<void *, size_t> pool_alloc_ptr;
StdCall<void, void *> pool_free_ptr;

template <typename T> int LuaReadInt(lua_State *L) {
  auto addr = Lua::check<int>(L, 1);
  T *obj;
  GetObjectAtOffset(obj, addr);
  lua_pushinteger(L, *obj);
  return 1;
}

int LuaReadFloat(lua_State *L) {
  auto addr = Lua::check<int>(L, 1);
  float *obj;
  GetObjectAtOffset(obj, addr);
  lua_pushnumber(L, *obj);
  return 1;
}

int LuaReadString(lua_State *L) {
  auto addr = Lua::check<int>(L, 1);
  char **obj;
  GetObjectAtOffset(obj, addr);
  lua_pushstring(L, *obj);
  return 1;
}

template <typename T> int LuaWriteInt(lua_State *L) {
  auto addr = Lua::check<int>(L, 1);
  auto value = Lua::check<int>(L, 2);
  T *obj;
  GetObjectAtOffset(obj, addr);
  *obj = value;
  return 0;
}

int LuaWriteFloat(lua_State *L) {
  auto addr = Lua::check<int>(L, 1);
  auto value = Lua::check<int>(L, 2);
  float *obj;
  GetObjectAtOffset<float *>(obj, addr);
  *obj = value;
  return 0;
}
} // namespace

void *pool_alloc(size_t sz) { return pool_alloc_ptr(sz); }
void pool_free(void *ptr) { pool_free_ptr(ptr); }

MemoryModule::MemoryModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(pool_alloc_ptr, 0x00571470);
  GetObjectAtOffset(pool_free_ptr, 0x005715b0);
}

int MemoryModule::Register(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, LuaReadInt<int8_t>);
  lua_setfield(L, -2, "read_int8");

  lua_pushcfunction(L, LuaReadInt<uint8_t>);
  lua_setfield(L, -2, "read_uint8");

  lua_pushcfunction(L, LuaReadInt<int16_t>);
  lua_setfield(L, -2, "read_int16");

  lua_pushcfunction(L, LuaReadInt<uint16_t>);
  lua_setfield(L, -2, "read_uint16");

  lua_pushcfunction(L, LuaReadInt<int32_t>);
  lua_setfield(L, -2, "read_int32");

  lua_pushcfunction(L, LuaReadInt<uint32_t>);
  lua_setfield(L, -2, "read_uint32");

  lua_pushcfunction(L, LuaReadFloat);
  lua_setfield(L, -2, "read_float");

  lua_pushcfunction(L, LuaReadString);
  lua_setfield(L, -2, "read_string");

  lua_pushcfunction(L, LuaWriteInt<int8_t>);
  lua_setfield(L, -2, "write_int8");

  lua_pushcfunction(L, LuaWriteInt<uint8_t>);
  lua_setfield(L, -2, "write_uint8");

  lua_pushcfunction(L, LuaWriteInt<int16_t>);
  lua_setfield(L, -2, "write_int16");

  lua_pushcfunction(L, LuaWriteInt<uint16_t>);
  lua_setfield(L, -2, "write_uint16");

  lua_pushcfunction(L, LuaWriteInt<int32_t>);
  lua_setfield(L, -2, "write_int32");

  lua_pushcfunction(L, LuaWriteInt<uint32_t>);
  lua_setfield(L, -2, "write_uint32");

  lua_pushcfunction(L, LuaWriteFloat);
  lua_setfield(L, -2, "write_float");

  return 1;
}
} // namespace gk