#include <cassert>
#include <string>

#include "Core.h"
#include "List.h"
#include "LuaEngine.h"
#include "Memory.h"
#include "Tokens.h"

namespace gk {
namespace {
// The 8-byte payload of a token list node, pool_alloc'd by CreateToken
// @ 0x004d35f0 and pool-freed by FreeTokens @ 0x004d3800.
struct TokenData {
  pool_string name;
  float value;
};
static_assert(sizeof(TokenData) == 8);

// The global token table @ 0x007b6af8 is a plain List - what was previously
// modelled as a two-field {head, count} struct is its first half, with the
// entry_pointers cache CreateToken frees at +0x08 making up the rest.
using Token = List_Member<pool_unique_ptr<TokenData>>;
using Tokens = List<pool_unique_ptr<TokenData>>;
static_assert(sizeof(Token) == 0x10);
static_assert(sizeof(Tokens) == 0x10);

Tokens *tokens;
using TCreateToken = ThisCall<void, Tokens *, const char *, float>;
TCreateToken CreateToken;

// The userdata holds the *base* node, so the sentinel can be carried around as
// the `pairs` control value without ever being mistaken for an entry - only
// entry_of() past an explicit sentinel check reaches `data`.
using TokenNode = List_Member_Base<pool_unique_ptr<TokenData>>;

int LuaTokenEq(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 2) {
    return luaL_error(L, "Two arguments expected");
  }

  auto a = static_cast<TokenNode **>(luaL_checkudata(L, 1, "Token"));
  auto b = static_cast<TokenNode **>(luaL_checkudata(L, 2, "Token"));

  lua_pushboolean(L, *a == *b);
  return 1;
}

int LuaTokenToString(lua_State *L) {
  auto a = static_cast<TokenNode **>(luaL_checkudata(L, 1, "Token"));
  if (*a == tokens->sentinel) {
    lua_pushstring(L, "<token list head>");
  } else {
    lua_pushstring(L, entry_of(*a)->data->name.get());
  }
  return 1;
}

void PushToken(lua_State *L, TokenNode *tok) {
  auto ud = static_cast<TokenNode **>(lua_newuserdatauv(L, sizeof(void *), 0));
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
  TokenNode *next;
  if (lua_isnil(L, 2)) {
    next = tokens->sentinel->next;
  } else {
    auto tok = static_cast<TokenNode **>(luaL_checkudata(L, 2, "Token"));
    next = (*tok)->next;
  }

  if (next == tokens->sentinel) {
    return 0;
  }
  PushToken(L, next);
  lua_pushnumber(L, entry_of(next)->data->value);
  return 2;
}

int LuaTokensIndex(lua_State *L) {
  std::string key = lua_tostring(L, 2);
  for (const auto &token : *tokens) {
    if (key == token->name.get()) {
      lua_pushnumber(L, token->value);
      return 1;
    }
  }
  return 0;
}

int LuaTokensNewIndex(lua_State *L) {
  std::string key = lua_tostring(L, 2);
  float value = lua_tonumber(L, 3);
  for (const auto &token : *tokens) {
    if (key == token->name.get()) {
      token->value = value;
      return 0;
    }
  }
  CreateToken(tokens, key.c_str(), value);
  return 0;
}

int LuaTokensPairs(lua_State *L) {
  lua_pushcfunction(L, LuaTokenNext);
  lua_pushnil(L);
  PushToken(L, tokens->sentinel);
  return 3;
}

int LuaTokensLen(lua_State *L) {
  lua_pushinteger(L, tokens->size());
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