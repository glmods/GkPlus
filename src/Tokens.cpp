#include <cassert>
#include <string>

#include "Core.h"
#include "LuaEngine.h"
#include "Tokens.h"

namespace gk {
namespace {
struct Token {
  void *vtbl;
  Token *prev, *next;
  struct {
    char *name;
    float value;
  } *data;
};

struct Tokens final {
  Token *token_list;
  int token_count;
};

Tokens *tokens;
using TCreateToken = ThisCall<void, Tokens *, const char *, float>;
TCreateToken CreateToken;

int LuaTokenEq(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 2) {
    return luaL_error(L, "Two arguments expected");
  }

  auto a = static_cast<Token **>(luaL_checkudata(L, 1, "Token"));
  auto b = static_cast<Token **>(luaL_checkudata(L, 2, "Token"));

  lua_pushboolean(L, *a == *b);
  return 1;
}

int LuaTokenToString(lua_State *L) {
  auto a = static_cast<Token **>(luaL_checkudata(L, 1, "Token"));
  lua_pushstring(L, (*a)->data->name);
  return 1;
}

void PushToken(lua_State *L, Token *tok) {
  auto ud = static_cast<Token **>(lua_newuserdatauv(L, sizeof(void *), 0));
  *ud = tok;
  if (luaL_newmetatable(L, "Token")) {
    lua_pushcfunction(L, LuaTokenEq);
    lua_setfield(L, -2, "__eq");

    lua_pushcfunction(L, LuaTokenToString);
    lua_setfield(L, -2, "__tostring");
  }
  lua_setmetatable(L, -2);
}

int LuaTokenNext(lua_State *L) {
  Token *next;
  if (lua_isnil(L, 2)) {
    next = tokens->token_list->next;
  } else {
    auto tok = static_cast<Token **>(luaL_checkudata(L, 2, "Token"));
    next = (*tok)->next;
  }

  if (next == tokens->token_list) {
    return 0;
  }
  PushToken(L, next);
  lua_pushnumber(L, next->data->value);
  return 2;
}

int LuaTokensIndex(lua_State *L) {
  std::string key = lua_tostring(L, 2);
  for (auto tok = tokens->token_list->next; tok != tokens->token_list;
       tok = tok->next) {
    if (key == tok->data->name) {
      lua_pushnumber(L, tok->data->value);
      return 1;
    }
  }
  return 0;
}

int LuaTokensNewIndex(lua_State *L) {
  std::string key = lua_tostring(L, 2);
  float value = lua_tonumber(L, 3);
  for (auto tok = tokens->token_list->next; tok != tokens->token_list;
       tok = tok->next) {
    if (key == tok->data->name) {
      tok->data->value = value;
      return 0;
    }
  }
  CreateToken(tokens, key.c_str(), value);
  return 0;
}

int LuaTokensPairs(lua_State *L) {
  lua_pushcfunction(L, LuaTokenNext);
  lua_pushnil(L);
  PushToken(L, tokens->token_list);
  return 3;
}

int LuaTokensLen(lua_State *L) {
  lua_pushinteger(L, tokens->token_count);
  return 1;
}
} // namespace

TokensModule::TokensModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(tokens, 0x007b6af8);
  GetObjectAtOffset(CreateToken, 0x004d35f0);
}

int TokensModule::Register(lua_State *L) {
  auto ud = lua_newuserdatauv(L, sizeof(void *), 0);
  if (luaL_newmetatable(L, "Tokens")) {
    lua_pushcfunction(L, LuaTokensIndex);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, LuaTokensNewIndex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, LuaTokensPairs);
    lua_setfield(L, -2, "__pairs");

    lua_pushcfunction(L, LuaTokensLen);
    lua_setfield(L, -2, "__len");
  }
  lua_setmetatable(L, -2);
  return 1;
}

} // namespace gk