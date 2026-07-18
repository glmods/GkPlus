#pragma once
#include "LuaEngine.h"
#include "Module.h"

#include <cstddef>
#include <string_view>

namespace gk {

// Front-end menu ids. See src/Menus.inc.h and menu_system_notes.md.
enum class MenuIndex {
#define GUNLOK_MENU(NAME, ID, ...) NAME = ID,
#include "Menus.inc.h"
};

// MenuListItem::itemType. Only 0..3 exist -- verified by scanning every immediate
// stored to the itemType slot in the menu region. There is no slider, no dedicated
// text-entry type and no greyed-out state.
enum class MenuItemType : int {
  PlainLabel = 0,
  LabelWithValue = 1,
  Toggle = 2,
  MultiValue = 3,
};

// ChosenMenuItem pseudo-values. Real items use their 0-based index.
inline constexpr int MenuItemNone = 0x100;
inline constexpr int MenuItemBack = 0x101;
inline constexpr int MenuItemScrollUp = 0x102;
inline constexpr int MenuItemScrollDown = 0x103;

// Only six rows are ever visible; Menu::scroll_offset picks the first.
inline constexpr int MenuVisibleRows = 6;

// The game's menu entry: ListNode (0x10) + MenuItemData (0x68) at +0x10.
// MenuListItemVtable has exactly one slot (scalar deleting dtor) -- there is no
// virtual draw, all polymorphism is the `type` int.
struct MenuListItem {
  void *vtable;                  // 0x00
  MenuListItem *prev;            // 0x04
  MenuListItem *next;            // 0x08
  int pad_0c;                    // 0x0c
  int *toggle_value;             // 0x10 Toggle only; dereferenced as int*, not bool*
  char *value_text;              // 0x14 LabelWithValue only
  unsigned char value_text_owned;// 0x18 1 = heap, free it
  unsigned char pad_19[3];
  int *multi_value_index;        // 0x1c MultiValue only
  unsigned **multi_value_labels; // 0x20 MultiValue only; DOUBLE indirect
  unsigned char is_current_value;// 0x24 draw this row red instead of green
  unsigned char pad_25[3];
  char *label;                   // 0x28
  float rect_left;               // 0x2c normalized screen coords
  float rect_top;                // 0x30
  float rect_right;              // 0x34
  float rect_bottom;             // 0x38
  MenuItemType type;             // 0x3c
  unsigned char label_is_static; // 0x40 0 = heap, free it (opposite of value_text_owned)
  unsigned char pad_41[3];
  int index;                     // 0x44
  float glow_level;              // 0x48 ramps 0..0.5 while selected
  float reveal_rate;             // 0x4c chars per ms for the typewriter effect
  float unused_random;           // 0x50 written by InitAnimation, never read
  int label_chars_revealed;      // 0x54
  int value_chars_revealed;      // 0x58
  int pad_5c;                    // 0x5c
  long long label_reveal_start;  // 0x60
  long long value_reveal_start;  // 0x68
  void *extra_owned_buffer;      // 0x70
  int pad_74;                    // 0x74
};
static_assert(sizeof(MenuListItem) == 0x78);
static_assert(offsetof(MenuListItem, toggle_value) == 0x10);
static_assert(offsetof(MenuListItem, value_text) == 0x14);
static_assert(offsetof(MenuListItem, multi_value_index) == 0x1c);
static_assert(offsetof(MenuListItem, multi_value_labels) == 0x20);
static_assert(offsetof(MenuListItem, is_current_value) == 0x24);
static_assert(offsetof(MenuListItem, label) == 0x28);
static_assert(offsetof(MenuListItem, rect_left) == 0x2c);
static_assert(offsetof(MenuListItem, type) == 0x3c);
static_assert(offsetof(MenuListItem, label_is_static) == 0x40);
static_assert(offsetof(MenuListItem, index) == 0x44);
static_assert(offsetof(MenuListItem, glow_level) == 0x48);
static_assert(offsetof(MenuListItem, label_reveal_start) == 0x60);
static_assert(offsetof(MenuListItem, extra_owned_buffer) == 0x70);

// The game's menu container. Used for both Menus[36] and InGameMenus[7].
// The first four fields are an intrusive list header, the same template that
// backs LevelList and MultiplayerLevelList.
struct Menu {
  MenuListItem *sentinel;          // 0x00 circular list sentinel
  int num_nodes;                   // 0x04
  int *cached_items;               // 0x08 flat index -> MenuItemData* array
  bool cache_valid;                // 0x0c
  unsigned char pad_0d[3];
  MenuListItem *current_item;      // 0x10 mutable iteration cursor
  Menu *items_owner;               // 0x14 self-pointer; lets one menu render another's items
  int scroll_offset;               // 0x18 index of the first visible item
  int parent_menu_id;              // 0x1c the whole "back stack" is this one link
  unsigned title_resource_id;      // 0x20
  int num_items;                   // 0x24
  unsigned first_item_resource_id; // 0x28
};
static_assert(sizeof(Menu) == 0x2c);
static_assert(offsetof(Menu, current_item) == 0x10);
static_assert(offsetof(Menu, scroll_offset) == 0x18);
static_assert(offsetof(Menu, parent_menu_id) == 0x1c);
static_assert(offsetof(Menu, title_resource_id) == 0x20);
static_assert(offsetof(Menu, num_items) == 0x24);
static_assert(offsetof(Menu, first_item_resource_id) == 0x28);

const char *GetMenuName(MenuIndex idx);
// Resolves a GL_RESOURCE_ID through the localized string table. Returns "" for
// ids with no string in the active glres*.dll.
const char *ResourceString(unsigned id);

struct MenuItemWrapper final {
  static constexpr const char *metatable_name = "MenuItem";
  static void setup_metatable(lua_State *L);

  MenuListItem *item;
  explicit MenuItemWrapper(MenuListItem *item);

  bool operator==(const MenuItemWrapper &) const;
  int to_string(lua_State *L) const;

  int get_index();
  std::string_view get_label();
  int get_type();
  std::string_view get_type_name();
  std::string_view get_value_text();
  bool get_is_current_value();
  void get_rect(lua_State *L);

  // Reads/writes the bound variable for Toggle (bool) and MultiValue (int)
  // items. Errors for the other two types, which have no binding.
  int get_value(lua_State *L);
  int set_value(lua_State *L);

  using type = MenuItemWrapper;
  using fields = Lua::Fields<
      Lua::Getter<"index", &type::get_index>,
      Lua::Getter<"label", &type::get_label>,
      Lua::Getter<"type", &type::get_type>,
      Lua::Getter<"type_name", &type::get_type_name>,
      Lua::Getter<"value_text", &type::get_value_text>,
      Lua::Getter<"is_current_value", &type::get_is_current_value>,
      Lua::TableGetter<"rect", &type::get_rect>,
      Lua::Function<"get_value", type, &type::get_value>,
      Lua::Function<"set_value", type, &type::set_value>>;
};

struct MenuWrapper final {
  static constexpr const char *metatable_name = "Menu";
  static void setup_metatable(lua_State *L);

  Menu *menu;
  int id;        // index within its array
  bool in_game;  // false = Menus[36], true = InGameMenus[7]

  MenuWrapper(Menu *menu, int id, bool in_game);

  bool operator==(const MenuWrapper &) const;
  int to_string(lua_State *L) const;

  int get_id();
  bool get_in_game();
  std::string_view get_name();
  std::string_view get_title();
  int get_title_id();
  int get_parent_id();
  int get_num_items();
  int get_num_nodes();
  int get_scroll_offset();
  void set_scroll_offset(int value);
  void get_items(lua_State *L);

  int item(lua_State *L);
  int add_item(lua_State *L);
  int add_value_item(lua_State *L);
  int add_toggle(lua_State *L);
  int add_choice(lua_State *L);
  int activate(lua_State *L);

  using type = MenuWrapper;
  using fields = Lua::Fields<
      Lua::Getter<"id", &type::get_id>,
      Lua::Getter<"in_game", &type::get_in_game>,
      Lua::Getter<"name", &type::get_name>,
      Lua::Getter<"title", &type::get_title>,
      Lua::Getter<"title_id", &type::get_title_id>,
      Lua::Getter<"parent_id", &type::get_parent_id>,
      Lua::Getter<"num_items", &type::get_num_items>,
      Lua::Getter<"num_nodes", &type::get_num_nodes>,
      Lua::GetterSetter<"scroll_offset", &type::get_scroll_offset,
                        &type::set_scroll_offset>,
      Lua::TableGetter<"items", &type::get_items>,
      Lua::Function<"item", type, &type::item>,
      Lua::Function<"add_item", type, &type::add_item>,
      Lua::Function<"add_value_item", type, &type::add_value_item>,
      Lua::Function<"add_toggle", type, &type::add_toggle>,
      Lua::Function<"add_choice", type, &type::add_choice>,
      Lua::Function<"activate", type, &type::activate>>;
};

class MenuModule final : public Module<MenuModule> {
public:
  static constexpr const char *module_name = "gk.menu";

  MenuModule(lua_State *L);
  ~MenuModule();
  int Register(lua_State *L);
};
} // namespace gk
