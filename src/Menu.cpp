#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <array>
#include <cassert>

#include "Core.h"
#include "LuaEngine.h"
#include "Menu.h"

namespace gk {
namespace {
struct Menu {
  int a, b, c, d, e, f;
  char *label;
  int h;
  int idxTitle;
  int nItems;
  int k;
};

std::array<Menu, 36> *Menus;
lua_Integer CallbackTable = LUA_NOREF;
lua_Integer OnSetup = LUA_NOREF;

using TAddMenu = ThisCall<void, Menu *, const char *>;

TAddMenu AddMenu;

using TSetupMenus = StdCall<void>;

TSetupMenus SetupMenus;

enum class MenuIndex {
#define GUNLOK_MENU(NAME, ID, ...) NAME = ID,
#include "Menus.inc.h"
};

const char *GetMenuName(MenuIndex idx) {
  switch (idx) {
  default:
    return "Unknown";
#define GUNLOK_MENU(NAME, ...)                                                 \
  case MenuIndex::NAME:                                                        \
    return #NAME;
#include "Menus.inc.h"
  }
}

MenuIndex *ChosenMenu;
int *ChosenMenuItem;

using TOnMenuItemClicked = StdCall<void>;
TOnMenuItemClicked OnMenuItemClicked;

int LuaAddItem(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 3) {
    return luaL_error(L, "Three arguments expected");
  }

  auto menu = static_cast<Menu **>(luaL_checkudata(L, 1, "Menu"));

  const char *label = luaL_tolstring(L, 2, nullptr);
  if (!label) {
    return 0;
  }

  auto new_idx = (*menu)->nItems;

  lua_rawgeti(L, LUA_REGISTRYINDEX, CallbackTable);

  lua_rawgeti(L, -1, reinterpret_cast<uintptr_t>(*menu));

  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, reinterpret_cast<uintptr_t>(*menu));
  }

  lua_remove(L, -2);
  lua_pushvalue(L, 3);
  lua_rawseti(L, -2, new_idx);
  lua_pop(L, 1);

  AddMenu(*menu, label);

  return 0;
}

int LuaMenuEq(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 2) {
    return luaL_error(L, "Two arguments expected");
  }

  auto a = static_cast<Menu **>(luaL_checkudata(L, 1, "Menu"));
  auto b = static_cast<Menu **>(luaL_checkudata(L, 2, "Menu"));

  lua_pushboolean(L, *a == *b);
  return 1;
}

void PushMenu(lua_State *L, Menu *menu) {
  auto ud = static_cast<Menu **>(lua_newuserdatauv(L, sizeof(void *), 0));
  *ud = menu;
  if (luaL_newmetatable(L, "Menu")) {
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, LuaMenuEq);
    lua_setfield(L, -2, "__eq");

    lua_pushcfunction(L, LuaAddItem);
    lua_setfield(L, -2, "add_item");
  }
  lua_setmetatable(L, -2);
}

int LuaGetMenu(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 1) {
    return 0;
  }

  int index = lua_tointeger(L, 1);
  if (index < 0 || index > 35) {
    return 0;
  }

  std::array<Menu, 36> &MenusRef = *Menus;
  PushMenu(L, &MenusRef[index]);
  return 1;
}

void __stdcall HookedSetupMenus() {
  SetupMenus();

  if (OnSetup == LUA_NOREF || OnSetup == LUA_REFNIL) {
    return;
  }

  lua_State *L = Lua::GetEngine();
  lua_rawgeti(L, LUA_REGISTRYINDEX, OnSetup);

  if (!lua_isnil(L, -1)) {
    lua_newtable(L);
    lua_pushcfunction(L, LuaGetMenu);
    lua_setfield(L, -2, "get");

    lua_call(L, 1, 0);
  } else {
    lua_pop(L, 1);
  }
}

void __stdcall HookedOnMenuItemClicked() {
  std::array<Menu, 36> &MenusRef = *Menus;

  lua_State *L = Lua::GetEngine();
  lua_newtable(L);

  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "propagate");

  PushMenu(L, &MenusRef[(int)*ChosenMenu]);
  lua_setfield(L, -2, "menu");

  lua_pushinteger(L, *ChosenMenuItem);
  lua_setfield(L, -2, "item");

  lua_rawgeti(L, LUA_REGISTRYINDEX, CallbackTable);
  lua_rawgeti(L, -1, reinterpret_cast<uintptr_t>(&MenusRef[(int)*ChosenMenu]));

  lua_remove(L, -2);

  if (lua_isnil(L, -1)) {
    lua_pop(L, 2);
    return OnMenuItemClicked();
  }

  auto res = lua_rawgeti(L, -1, *ChosenMenuItem);

  lua_remove(L, -2);

  if (res == LUA_TNIL) {
    lua_pop(L, 2);
    return OnMenuItemClicked();
  }

  lua_pushvalue(L, -2);
  lua_call(L, 1, 0);

  lua_getfield(L, -1, "propagate");
  bool propagate = lua_toboolean(L, -1);
  lua_pop(L, 2);

  if (propagate) {
    OnMenuItemClicked();
  }
}

struct LevelInfo {
  void *dtor;
  LevelInfo *prev, *next;
  char *title, *script, *console;
};

struct LevelList {
  LevelInfo *first;
  int numLevels;
};

LevelList *levelList;

int LuaOnSetup(lua_State *L) {
  luaL_unref(L, LUA_REGISTRYINDEX, OnSetup);
  lua_pushvalue(L, 1);
  OnSetup = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}
} // namespace

MenuModule::MenuModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(Menus, 0x007b76d0);
  GetObjectAtOffset(AddMenu, 0x004f7a60);
  GetObjectAtOffset(SetupMenus, 0x004e95e0);

  GetObjectAtOffset(ChosenMenu, 0x007b732c);
  GetObjectAtOffset(ChosenMenuItem, 0x006a7d6c);
  GetObjectAtOffset(OnMenuItemClicked, 0x004ecf10);

  GetObjectAtOffset(levelList, 0x007b74dc);

  lua_newtable(L);
  CallbackTable = luaL_ref(L, LUA_REGISTRYINDEX);
  DetourAttach(&SetupMenus, HookedSetupMenus);
  DetourAttach(&OnMenuItemClicked, HookedOnMenuItemClicked);
}

MenuModule::~MenuModule() {
  DetourDetach(&SetupMenus, HookedSetupMenus);
  DetourDetach(&OnMenuItemClicked, HookedOnMenuItemClicked);
}

int MenuModule::Register(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, LuaOnSetup);
  lua_setfield(L, -2, "set_onsetup");
  return 1;
}
} // namespace gk