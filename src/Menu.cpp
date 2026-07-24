#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "Core.h"
#include "LuaEngine.h"
#include "Menu.h"

namespace gk {
namespace {

// ---------------------------------------------------------------- game globals

std::array<Menu, 36> *Menus;
std::array<Menu, 7> *InGameMenus;

MenuIndex *ChosenMenu;
int *ChosenMenuItem;
int *InGameMenuIndex;
int *InGameMenuSelectedItem;

void *LocalizedStrings;

using TGetResourceString = FastCall<const char *, void *, unsigned>;
using TAddItem = ThisCall<void, Menu *, const char *>;
using TAddValueItem = ThisCall<void, Menu *, const char *, const char *, bool, bool>;
using TAddToggleItem = ThisCall<void, Menu *, const char *, int *>;
using TAddMultiValueItem = ThisCall<void, Menu *, const char *, int *, unsigned **>;
using TGetItemData = ThisCall<void *, Menu *, int>;
using TGoToMenu = FastCall<void, MenuIndex, bool>;
using TIsAnyInGameMenuOpen = FastCall<char>;
using TCloseInGameMenu = FastCall<void, int>;
using TSetupMenus = StdCall<void>;
using TOnMenuItemClicked = StdCall<void>;

TGetResourceString GetResourceString;
TAddItem AddItem;
TAddValueItem AddValueItem;
TAddToggleItem AddToggleItem;
TAddMultiValueItem AddMultiValueItem;
TGetItemData GetItemData;
TGoToMenu GoToMenu;
TIsAnyInGameMenuOpen IsAnyInGameMenuOpen;
TCloseInGameMenu CloseInGameMenuFn;
TSetupMenus SetupMenus;
TOnMenuItemClicked OnMenuItemClicked;

// ----------------------------------------------------------- owned Lua storage
//
// The game never copies the strings or takes ownership of the variables handed
// to Menu::Add*Item: labels are stored by pointer with label_is_static = 1, and
// toggle/choice items keep a raw pointer to the backing variable. Anything Lua
// supplies therefore has to outlive the menu, so it lives here. std::deque is
// used because push_back never invalidates pointers to existing elements.

std::deque<std::string> OwnedStrings;
std::deque<int> OwnedInts;
std::deque<std::vector<unsigned>> OwnedChoiceArrays;
std::deque<unsigned *> OwnedChoicePointers;

const char *OwnString(std::string_view s) {
  OwnedStrings.emplace_back(s);
  return OwnedStrings.back().c_str();
}

lua_Integer CallbackTable = LUA_NOREF;
lua_Integer OnSetup = LUA_NOREF;

} // namespace

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

const char *ResourceString(unsigned id) {
  if (!GetResourceString || !LocalizedStrings) {
    return "";
  }
  const char *s = GetResourceString(&LocalizedStrings, id);
  return s ? s : "";
}

namespace {

const char *MenuItemTypeName(MenuItemType t) {
  switch (t) {
  case MenuItemType::PlainLabel:
    return "plain";
  case MenuItemType::LabelWithValue:
    return "value";
  case MenuItemType::Toggle:
    return "toggle";
  case MenuItemType::MultiValue:
    return "choice";
  default:
    return "unknown";
  }
}

// Walks a menu's item list. The game iterates items_owner's list rather than the
// menu's own, because items_owner lets one menu render another menu's list.
template <typename F> void ForEachItem(Menu *menu, F &&fn) {
  Menu *owner = menu->items_owner ? menu->items_owner : menu;
  if (!owner->items.sentinel) {
    return;
  }
  for (auto it = owner->items.begin(); it != owner->items.end(); ++it) {
    fn(static_cast<MenuListItem *>(it.node));
  }
}

} // namespace

// -------------------------------------------------------------- MenuItemWrapper

MenuItemWrapper::MenuItemWrapper(MenuListItem *item) : item{item} {}

bool MenuItemWrapper::operator==(const MenuItemWrapper &other) const {
  return item == other.item;
}

int MenuItemWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "MenuItem(%d, %s, \"%s\")", item->data.index,
                  MenuItemTypeName(item->data.type),
                  item->data.label ? item->data.label : "");
  return 1;
}

int MenuItemWrapper::get_index() { return item->data.index; }

std::string_view MenuItemWrapper::get_label() {
  return item->data.label ? item->data.label : "";
}

int MenuItemWrapper::get_type() { return static_cast<int>(item->data.type); }

std::string_view MenuItemWrapper::get_type_name() {
  return MenuItemTypeName(item->data.type);
}

bool MenuItemWrapper::get_is_current_value() {
  return item->data.is_current_value != 0;
}

// The string the game draws on the right-hand side, reproducing the renderer's
// own switch on itemType.
std::string_view MenuItemWrapper::get_value_text() {
  switch (item->data.type) {
  case MenuItemType::LabelWithValue:
    return item->data.value_text ? item->data.value_text : "";
  case MenuItemType::Toggle:
    if (!item->data.toggle_value) {
      return "";
    }
    // GL_TEXT_ON = 0x2ee0, GL_TEXT_OFF = 0x2ee1
    return ResourceString(*item->data.toggle_value != 0 ? 0x2ee0 : 0x2ee1);
  case MenuItemType::MultiValue:
    if (!item->data.multi_value_index || !item->data.multi_value_labels ||
        !*item->data.multi_value_labels) {
      return "";
    }
    return ResourceString((*item->data.multi_value_labels)[*item->data.multi_value_index]);
  default:
    return "";
  }
}

void MenuItemWrapper::get_rect(lua_State *L) {
  lua_pushnumber(L, item->data.rect_left);
  lua_setfield(L, -2, "left");
  lua_pushnumber(L, item->data.rect_top);
  lua_setfield(L, -2, "top");
  lua_pushnumber(L, item->data.rect_right);
  lua_setfield(L, -2, "right");
  lua_pushnumber(L, item->data.rect_bottom);
  lua_setfield(L, -2, "bottom");
}

int MenuItemWrapper::get_value(lua_State *L) {
  switch (item->data.type) {
  case MenuItemType::Toggle:
    if (!item->data.toggle_value) {
      return luaL_error(L, "toggle item has no bound variable");
    }
    lua_pushboolean(L, *item->data.toggle_value != 0);
    return 1;
  case MenuItemType::MultiValue:
    if (!item->data.multi_value_index) {
      return luaL_error(L, "choice item has no bound variable");
    }
    lua_pushinteger(L, *item->data.multi_value_index);
    return 1;
  default:
    return luaL_error(L, "item type '%s' has no bound value",
                      MenuItemTypeName(item->data.type));
  }
}

int MenuItemWrapper::set_value(lua_State *L) {
  switch (item->data.type) {
  case MenuItemType::Toggle:
    if (!item->data.toggle_value) {
      return luaL_error(L, "toggle item has no bound variable");
    }
    *item->data.toggle_value = lua_toboolean(L, 2) ? 1 : 0;
    return 0;
  case MenuItemType::MultiValue: {
    if (!item->data.multi_value_index) {
      return luaL_error(L, "choice item has no bound variable");
    }
    // The label array carries no length, so an out-of-range index would make the
    // renderer read past the end. Nothing here can validate it; refuse negatives
    // at least.
    lua_Integer v = luaL_checkinteger(L, 2);
    if (v < 0) {
      return luaL_error(L, "choice index must be >= 0");
    }
    *item->data.multi_value_index = static_cast<int>(v);
    return 0;
  }
  default:
    return luaL_error(L, "item type '%s' has no bound value",
                      MenuItemTypeName(item->data.type));
  }
}

void MenuItemWrapper::setup_metatable(lua_State *) {}

// ------------------------------------------------------------------ MenuWrapper

MenuWrapper::MenuWrapper(Menu *menu, int id, bool in_game)
    : menu{menu}, id{id}, in_game{in_game} {}

bool MenuWrapper::operator==(const MenuWrapper &other) const {
  return menu == other.menu;
}

int MenuWrapper::to_string(lua_State *L) const {
  if (in_game) {
    lua_pushfstring(L, "InGameMenu(%d)", id);
  } else {
    lua_pushfstring(L, "Menu(%d, %s)", id,
                    GetMenuName(static_cast<MenuIndex>(id)));
  }
  return 1;
}

int MenuWrapper::get_id() { return id; }
bool MenuWrapper::get_in_game() { return in_game; }

std::string_view MenuWrapper::get_name() {
  return in_game ? "InGame" : GetMenuName(static_cast<MenuIndex>(id));
}

std::string_view MenuWrapper::get_title() {
  return ResourceString(menu->title_resource_id);
}

int MenuWrapper::get_title_id() {
  return static_cast<int>(menu->title_resource_id);
}
int MenuWrapper::get_parent_id() { return menu->parent_menu_id; }
int MenuWrapper::get_num_items() { return menu->num_items; }
int MenuWrapper::get_num_nodes() { return menu->items.size(); }
int MenuWrapper::get_scroll_offset() { return menu->scroll_offset; }
void MenuWrapper::set_scroll_offset(int value) { menu->scroll_offset = value; }

void MenuWrapper::get_items(lua_State *L) {
  int n = 1;
  ForEachItem(menu, [&](MenuListItem *p) {
    Lua::Create<MenuItemWrapper>(L, p);
    lua_rawseti(L, -2, n++);
  });
}

int MenuWrapper::item(lua_State *L) {
  lua_Integer index = luaL_checkinteger(L, 2);
  // Menu__GetItemData has no bounds check, so do it here.
  if (index < 0 || index >= menu->items.size()) {
    lua_pushnil(L);
    return 1;
  }
  int i = 0;
  MenuListItem *found = nullptr;
  ForEachItem(menu, [&](MenuListItem *p) {
    if (i++ == index) {
      found = p;
    }
  });
  if (!found) {
    lua_pushnil(L);
    return 1;
  }
  Lua::Create<MenuItemWrapper>(L, found);
  return 1;
}

namespace {

// Registers a Lua callback for (menu, item index) in the module's callback
// table, which HookedOnMenuItemClicked consults before the game's own handler.
void StoreCallback(lua_State *L, Menu *menu, int index, int callback_idx) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, CallbackTable);
  lua_rawgeti(L, -1, reinterpret_cast<uintptr_t>(menu));
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, reinterpret_cast<uintptr_t>(menu));
  }
  lua_remove(L, -2);
  lua_pushvalue(L, callback_idx);
  lua_rawseti(L, -2, index);
  lua_pop(L, 1);
}

} // namespace

int MenuWrapper::add_item(lua_State *L) {
  const char *label = luaL_tolstring(L, 2, nullptr);
  if (!label) {
    return luaL_error(L, "expected a label");
  }
  // Copy: the game stores the pointer with label_is_static = 1 and never frees
  // or duplicates it, so a Lua-owned string would dangle once collected.
  const char *owned = OwnString(label);
  lua_pop(L, 1); // luaL_tolstring's pushed string

  int index = menu->num_items;
  if (!lua_isnoneornil(L, 3)) {
    StoreCallback(L, menu, index, 3);
  }
  AddItem(menu, owned);
  lua_pushinteger(L, index);
  return 1;
}

int MenuWrapper::add_value_item(lua_State *L) {
  const char *label = OwnString(luaL_checkstring(L, 2));
  const char *value = OwnString(luaL_checkstring(L, 3));
  int index = menu->num_items;
  if (!lua_isnoneornil(L, 4)) {
    StoreCallback(L, menu, index, 4);
  }
  // Both strings are ours and must outlive the menu, so tell the game they are
  // static: label_is_static = true, value_text_owned = false. Menu__ClearItems
  // then leaves both alone.
  AddValueItem(menu, label, value, true, false);
  lua_pushinteger(L, index);
  return 1;
}

int MenuWrapper::add_toggle(lua_State *L) {
  const char *label = OwnString(luaL_checkstring(L, 2));
  OwnedInts.push_back(lua_toboolean(L, 3) ? 1 : 0);
  int *slot = &OwnedInts.back();
  int index = menu->num_items;
  if (!lua_isnoneornil(L, 4)) {
    StoreCallback(L, menu, index, 4);
  }
  AddToggleItem(menu, label, slot);
  lua_pushinteger(L, index);
  return 1;
}

int MenuWrapper::add_choice(lua_State *L) {
  const char *label = OwnString(luaL_checkstring(L, 2));
  luaL_checktype(L, 3, LUA_TTABLE);

  lua_Integer n = luaL_len(L, 3);
  if (n <= 0) {
    return luaL_error(L, "choice list must not be empty");
  }
  OwnedChoiceArrays.emplace_back();
  auto &ids = OwnedChoiceArrays.back();
  ids.reserve(static_cast<size_t>(n));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 3, i);
    if (!lua_isinteger(L, -1)) {
      lua_pop(L, 1);
      return luaL_error(L, "choice list entry %d is not a GL_RESOURCE_ID", (int)i);
    }
    ids.push_back(static_cast<unsigned>(lua_tointeger(L, -1)));
    lua_pop(L, 1);
  }
  // The item stores GL_RESOURCE_ID** -- double indirect -- so we need a stable
  // pointer-to-pointer as well as a stable array.
  OwnedChoicePointers.push_back(ids.data());
  unsigned **labels = &OwnedChoicePointers.back();

  lua_Integer initial = luaL_optinteger(L, 4, 0);
  if (initial < 0 || initial >= n) {
    return luaL_error(L, "initial index out of range");
  }
  OwnedInts.push_back(static_cast<int>(initial));
  int *slot = &OwnedInts.back();

  int index = menu->num_items;
  if (!lua_isnoneornil(L, 5)) {
    StoreCallback(L, menu, index, 5);
  }
  AddMultiValueItem(menu, label, slot, labels);
  lua_pushinteger(L, index);
  return 1;
}

int MenuWrapper::activate(lua_State *L) {
  if (in_game) {
    return luaL_error(L, "activate() only applies to front-end menus");
  }
  bool remember = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
  GoToMenu(static_cast<MenuIndex>(id), remember);
  return 0;
}

void MenuWrapper::setup_metatable(lua_State *) {}

// ------------------------------------------------------------------- module API

namespace {

void PushMenu(lua_State *L, int id) {
  Lua::Create<MenuWrapper>(L, &(*Menus)[id], id, false);
}

void PushInGameMenu(lua_State *L, int id) {
  Lua::Create<MenuWrapper>(L, &(*InGameMenus)[id], id, true);
}

// Accepts either a numeric id or a menu name ("Main", "Audio", ...).
int ResolveMenuId(lua_State *L, int idx) {
  if (lua_isnumber(L, idx)) {
    lua_Integer v = lua_tointeger(L, idx);
    return (v < 0 || v > 35) ? -1 : static_cast<int>(v);
  }
  const char *name = lua_tostring(L, idx);
  if (!name) {
    return -1;
  }
  for (int i = 0; i <= 35; ++i) {
    if (std::string_view{GetMenuName(static_cast<MenuIndex>(i))} == name) {
      return i;
    }
  }
  return -1;
}

int LuaGetMenu(lua_State *L) {
  int id = ResolveMenuId(L, 1);
  if (id < 0) {
    lua_pushnil(L);
    return 1;
  }
  PushMenu(L, id);
  return 1;
}

int LuaGetInGameMenu(lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  if (id < 0 || id > 6) {
    lua_pushnil(L);
    return 1;
  }
  PushInGameMenu(L, static_cast<int>(id));
  return 1;
}

int LuaGoTo(lua_State *L) {
  int id = ResolveMenuId(L, 1);
  if (id < 0) {
    return luaL_error(L, "unknown menu");
  }
  bool remember = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
  GoToMenu(static_cast<MenuIndex>(id), remember);
  return 0;
}

int LuaResourceString(lua_State *L) {
  lua_pushstring(L, ResourceString(static_cast<unsigned>(luaL_checkinteger(L, 1))));
  return 1;
}

int LuaIsInGameMenuOpen(lua_State *L) {
  lua_pushboolean(L, IsAnyInGameMenuOpen() != 0);
  return 1;
}

int LuaCloseInGameMenu(lua_State *L) {
  CloseInGameMenuFn(static_cast<int>(luaL_checkinteger(L, 1)));
  return 0;
}

int LuaOnSetup(lua_State *L) {
  luaL_unref(L, LUA_REGISTRYINDEX, OnSetup);
  lua_pushvalue(L, 1);
  OnSetup = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

// gk.menu.menus -- indexable by id or name, and iterable with pairs().
int LuaMenusIndex(lua_State *L) {
  int id = ResolveMenuId(L, 2);
  if (id < 0) {
    lua_pushnil(L);
    return 1;
  }
  PushMenu(L, id);
  return 1;
}

int LuaMenusLen(lua_State *L) {
  lua_pushinteger(L, 36);
  return 1;
}

int LuaMenusNext(lua_State *L) {
  int id = lua_isnoneornil(L, 2) ? 0 : static_cast<int>(lua_tointeger(L, 2)) + 1;
  if (id > 35) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, id);
  PushMenu(L, id);
  return 2;
}

int LuaMenusPairs(lua_State *L) {
  lua_pushcfunction(L, LuaMenusNext);
  lua_pushvalue(L, 1);
  lua_pushnil(L);
  return 3;
}

int LuaGetChosenMenu(lua_State *L) {
  PushMenu(L, static_cast<int>(*ChosenMenu));
  return 1;
}

int LuaGetChosenMenuItem(lua_State *L) {
  lua_pushinteger(L, *ChosenMenuItem);
  return 1;
}

int LuaSetChosenMenuItem(lua_State *L) {
  *ChosenMenuItem = static_cast<int>(luaL_checkinteger(L, 1));
  return 0;
}

int LuaGetInGameMenuIndex(lua_State *L) {
  lua_pushinteger(L, *InGameMenuIndex);
  return 1;
}

int LuaGetInGameMenuSelectedItem(lua_State *L) {
  lua_pushinteger(L, *InGameMenuSelectedItem);
  return 1;
}

// ----------------------------------------------------------------------- hooks

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
  auto &MenusRef = *Menus;
  int menu_id = static_cast<int>(*ChosenMenu);
  int item_id = *ChosenMenuItem;

  // Pseudo-items (none / back / scroll) are the game's own chrome; never divert
  // them to Lua.
  if (item_id >= MenuItemNone) {
    return OnMenuItemClicked();
  }

  lua_State *L = Lua::GetEngine();
  lua_newtable(L);

  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "propagate");

  PushMenu(L, menu_id);
  lua_setfield(L, -2, "menu");

  lua_pushinteger(L, item_id);
  lua_setfield(L, -2, "item");

  lua_rawgeti(L, LUA_REGISTRYINDEX, CallbackTable);
  lua_rawgeti(L, -1, reinterpret_cast<uintptr_t>(&MenusRef[menu_id]));
  lua_remove(L, -2);

  if (lua_isnil(L, -1)) {
    lua_pop(L, 2);
    return OnMenuItemClicked();
  }

  auto res = lua_rawgeti(L, -1, item_id);
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

} // namespace

// ------------------------------------------------------------------ MenuModule

MenuModule::MenuModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(Menus, 0x007b76d0);
  GetObjectAtOffset(InGameMenus, 0x007b7578);

  GetObjectAtOffset(ChosenMenu, 0x007b732c);
  GetObjectAtOffset(ChosenMenuItem, 0x006a7d6c);
  GetObjectAtOffset(InGameMenuIndex, 0x007b7270);
  GetObjectAtOffset(InGameMenuSelectedItem, 0x006a89b4);

  GetObjectAtOffset(LocalizedStrings, 0x00725664);
  GetObjectAtOffset(GetResourceString, 0x00579000);

  GetObjectAtOffset(AddItem, 0x004f7a60);
  GetObjectAtOffset(AddValueItem, 0x004f7ae0);
  GetObjectAtOffset(AddToggleItem, 0x004f7950);
  GetObjectAtOffset(AddMultiValueItem, 0x004f79d0);
  GetObjectAtOffset(GetItemData, 0x004f7750);
  GetObjectAtOffset(GoToMenu, 0x004fbfa0);

  GetObjectAtOffset(IsAnyInGameMenuOpen, 0x00569550);
  GetObjectAtOffset(CloseInGameMenuFn, 0x005691f0);

  GetObjectAtOffset(SetupMenus, 0x004e95e0);
  GetObjectAtOffset(OnMenuItemClicked, 0x004ecf10);

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
  lua_pushcfunction(L, LuaGetMenu);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, LuaGetInGameMenu);
  lua_setfield(L, -2, "get_ingame");
  lua_pushcfunction(L, LuaGoTo);
  lua_setfield(L, -2, "goto_menu");
  lua_pushcfunction(L, LuaResourceString);
  lua_setfield(L, -2, "resource_string");
  lua_pushcfunction(L, LuaIsInGameMenuOpen);
  lua_setfield(L, -2, "is_ingame_menu_open");
  lua_pushcfunction(L, LuaCloseInGameMenu);
  lua_setfield(L, -2, "close_ingame_menu");
  lua_pushcfunction(L, LuaGetChosenMenu);
  lua_setfield(L, -2, "chosen_menu");
  lua_pushcfunction(L, LuaGetChosenMenuItem);
  lua_setfield(L, -2, "chosen_item");
  lua_pushcfunction(L, LuaSetChosenMenuItem);
  lua_setfield(L, -2, "set_chosen_item");
  lua_pushcfunction(L, LuaGetInGameMenuIndex);
  lua_setfield(L, -2, "ingame_menu_index");
  lua_pushcfunction(L, LuaGetInGameMenuSelectedItem);
  lua_setfield(L, -2, "ingame_selected_item");

  // gk.menu.menus: menus[3], menus.Audio, pairs(menus), #menus
  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, LuaMenusIndex);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, LuaMenusLen);
  lua_setfield(L, -2, "__len");
  lua_pushcfunction(L, LuaMenusPairs);
  lua_setfield(L, -2, "__pairs");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "menus");

  // gk.menu.ids.Audio == 25, gk.menu.names[25] == "Audio"
  lua_newtable(L);
#define GUNLOK_MENU(NAME, ID, ...)                                             \
  lua_pushinteger(L, ID);                                                      \
  lua_setfield(L, -2, #NAME);
#include "Menus.inc.h"
  lua_setfield(L, -2, "ids");

  lua_newtable(L);
#define GUNLOK_MENU(NAME, ID, ...)                                             \
  lua_pushstring(L, #NAME);                                                    \
  lua_rawseti(L, -2, ID);
#include "Menus.inc.h"
  lua_setfield(L, -2, "names");

  // gk.menu.titles[25] == "Audio Menu" (the shipped English text, for reference;
  // the live title comes from the active language DLL via Menu.title)
  lua_newtable(L);
#define GUNLOK_MENU(NAME, ID, TITLE_ID, TITLE_TEXT, ...)                       \
  lua_pushstring(L, TITLE_TEXT);                                               \
  lua_rawseti(L, -2, ID);
#include "Menus.inc.h"
  lua_setfield(L, -2, "titles");

  // gk.menu.item_type.toggle == 2
  lua_newtable(L);
  lua_pushinteger(L, static_cast<int>(MenuItemType::PlainLabel));
  lua_setfield(L, -2, "plain");
  lua_pushinteger(L, static_cast<int>(MenuItemType::LabelWithValue));
  lua_setfield(L, -2, "value");
  lua_pushinteger(L, static_cast<int>(MenuItemType::Toggle));
  lua_setfield(L, -2, "toggle");
  lua_pushinteger(L, static_cast<int>(MenuItemType::MultiValue));
  lua_setfield(L, -2, "choice");
  lua_setfield(L, -2, "item_type");

  // gk.menu.pseudo_item.back == 0x101
  lua_newtable(L);
  lua_pushinteger(L, MenuItemNone);
  lua_setfield(L, -2, "none");
  lua_pushinteger(L, MenuItemBack);
  lua_setfield(L, -2, "back");
  lua_pushinteger(L, MenuItemScrollUp);
  lua_setfield(L, -2, "scroll_up");
  lua_pushinteger(L, MenuItemScrollDown);
  lua_setfield(L, -2, "scroll_down");
  lua_setfield(L, -2, "pseudo_item");

  lua_pushinteger(L, MenuVisibleRows);
  lua_setfield(L, -2, "visible_rows");

  return 1;
}
} // namespace gk
