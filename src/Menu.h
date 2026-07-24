#pragma once
#include "List.h"
#include "Memory.h"

#include <cstddef>

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

// The payload half of a menu entry -- the "MenuItemData (0x68)" that sits at
// +0x10 of the 0x78-byte node. MenuListItemVtable has exactly one slot (scalar
// deleting dtor) -- there is no virtual draw, all polymorphism is the `type` int.
//
// Of the three buffers Menu::ClearItems @ 0x004f7cd0 reclaims, only
// extra_owned_buffer is owned unconditionally; label and value_text are each
// gated on a sibling flag, so neither can be a pool_unique_ptr. The bound value
// pointers point at globals and are never freed.
struct MenuItemData {
  int *toggle_value;             // 0x00 Toggle only; dereferenced as int*, not bool*
  char *value_text;              // 0x04 LabelWithValue only; owned iff value_text_owned
  unsigned char value_text_owned;// 0x08 1 = heap, free it
  unsigned char pad_09[3];
  int *multi_value_index;        // 0x0c MultiValue only
  unsigned **multi_value_labels; // 0x10 MultiValue only; DOUBLE indirect
  unsigned char is_current_value;// 0x14 draw this row red instead of green
  unsigned char pad_15[3];
  char *label;                   // 0x18 owned iff label_is_static == 0
  float rect_left;               // 0x1c normalized screen coords
  float rect_top;                // 0x20
  float rect_right;              // 0x24
  float rect_bottom;             // 0x28
  MenuItemType type;             // 0x2c
  unsigned char label_is_static; // 0x30 0 = heap, free it (opposite of value_text_owned)
  unsigned char pad_31[3];
  int index;                     // 0x34
  float glow_level;              // 0x38 ramps 0..0.5 while selected
  float reveal_rate;             // 0x3c chars per ms for the typewriter effect
  float unused_random;           // 0x40 written by InitAnimation, never read
  int label_chars_revealed;      // 0x44
  int value_chars_revealed;      // 0x48
  int pad_4c;                    // 0x4c alignment gap before the two timestamps
  long long label_reveal_start;  // 0x50
  long long value_reveal_start;  // 0x58
  pool_unique_ptr<void> extra_owned_buffer; // 0x60
  int pad_64;                    // 0x64
};
static_assert(sizeof(MenuItemData) == 0x68);
static_assert(offsetof(MenuItemData, multi_value_index) == 0x0c);
static_assert(offsetof(MenuItemData, multi_value_labels) == 0x10);
static_assert(offsetof(MenuItemData, is_current_value) == 0x14);
static_assert(offsetof(MenuItemData, label) == 0x18);
static_assert(offsetof(MenuItemData, rect_left) == 0x1c);
static_assert(offsetof(MenuItemData, type) == 0x2c);
static_assert(offsetof(MenuItemData, label_is_static) == 0x30);
static_assert(offsetof(MenuItemData, index) == 0x34);
static_assert(offsetof(MenuItemData, glow_level) == 0x38);
static_assert(offsetof(MenuItemData, label_reveal_start) == 0x50);
static_assert(offsetof(MenuItemData, extra_owned_buffer) == 0x60);

// The entry is a plain List_Member. This is what the previously unexplained
// `pad_0c` was: the gap List_Member<T> leaves between the 0xc-byte base and an
// 8-aligned payload. 0xc rounded up to 0x10, plus 0x68, is exactly the 0x78 the
// item's destructor pool-frees.
using MenuListItem = List_Member<MenuItemData>;
static_assert(alignof(MenuItemData) == 8);
static_assert(sizeof(MenuListItem) == 0x78);

// The game's menu container. Used for both Menus[36] and InGameMenus[7]. It opens
// with an embedded List<MenuItemData> -- the same template that backs LevelList
// and MultiplayerLevelList.
struct Menu {
  List<MenuItemData> items;        // 0x00
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

// --- Native API over the menu system ----------------------------------------

// English identifier for a front-end menu id (e.g. MenuIndex::Audio -> "Audio").
const char *GetMenuName(MenuIndex idx);
// The renderer's name for an item type ("plain"/"value"/"toggle"/"choice").
const char *MenuItemTypeName(MenuItemType t);
// Resolves a GL_RESOURCE_ID through the localized string table (LocalizedStrings
// @ 0x00725664 via GetResourceString @ 0x00579000). Returns "" for ids with no
// string in the active glres*.dll.
const char *ResourceString(unsigned id);

// Menus[36] @ 0x007b76d0 and InGameMenus[7] @ 0x007b7578.
Menu *GetMenus();
Menu *GetInGameMenus();

// ChosenMenu @ 0x007b732c / ChosenMenuItem @ 0x006a7d6c (see the MenuItem* pseudo-
// values). InGameMenuIndex @ 0x007b7270 / InGameMenuSelectedItem @ 0x006a89b4.
MenuIndex GetChosenMenu();
int GetChosenMenuItem();
void SetChosenMenuItem(int item);
int GetInGameMenuIndex();
int GetInGameMenuSelectedItem();

// GoToMenu @ 0x004fbfa0 (remember = push the current menu as parent).
void GoToMenu(MenuIndex target, bool remember);
// IsAnyInGameMenuOpen @ 0x00569550.
bool IsAnyInGameMenuOpen();
// CloseInGameMenu @ 0x005691f0 (kind 0/1/2/3/0x41/0x42/0x43).
void CloseInGameMenu(int kind);

// Menu item builders. The game stores label/value pointers without copying and
// binds the toggle/choice variables by pointer, so everything passed here must
// outlive the menu. AddItem @ 0x004f7a60, AddValueItem @ 0x004f7ae0,
// AddToggleItem @ 0x004f7950, AddMultiValueItem @ 0x004f79d0.
void MenuAddItem(Menu *menu, const char *label);
void MenuAddValueItem(Menu *menu, const char *label, const char *value,
                      bool label_is_static, bool value_text_owned);
void MenuAddToggleItem(Menu *menu, const char *label, int *value);
void MenuAddMultiValueItem(Menu *menu, const char *label, int *index,
                           unsigned **labels);
// GetItemData @ 0x004f7750 - cached, NO bounds check.
void *GetMenuItemData(Menu *menu, int index);
} // namespace gk
