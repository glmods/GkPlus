#pragma once

#include "Menu.h"

#include <string>

namespace gk {
struct CustomMenuItem;

// Runs *instead of* the game's dispatch when a GkPlus item is activated. `item`
// is the registration (already toggled, for a toggle item); `user` is whatever
// was handed to AddCustomMenuItem.
using CustomMenuAction = void (*)(CustomMenuItem *item, void *user);

// Runs once per reconcile - i.e. once a frame while the item's menu is the one
// on screen, before the game draws it - so a row can mirror state something
// else owns. A toggle refreshes `value`, a value item refreshes `value_text`
// (through SetCustomMenuValueText). Nothing else may be touched: `label` is held
// by the game by pointer and `index` belongs to the reconcile.
using CustomMenuRefresh = void (*)(CustomMenuItem *item, void *user);

// Whether the item should exist at all. Consulted only when the item is about to
// be *appended*, which makes it a registration-time filter and not a live one:
// an item already on a menu cannot be taken off again, because `Menu::ClearItems`
// is all-or-nothing. Use it for conditions that are constant for a run - which
// renderer resolved, what the device can do - not for anything that changes.
using CustomMenuAvailable = bool (*)(void *user);

enum class CustomMenuItemKind {
  Action, // plain label; the click is the whole behaviour
  Toggle, // ON / OFF, drawn by the game from `value`
  Value,  // arbitrary right-hand text, drawn by the game from `value_text`
};

// Who registered the item, and therefore whose teardown may silence it.
// ClearCustomMenuActions() is the script host dropping the JS callbacks its
// `user` pointers refer to; a Native registration's action is an ordinary
// function with process lifetime and must survive that.
enum class CustomMenuOwner {
  Script,
  Native,
};

// One front-end menu item owned by GkPlus rather than by the game.
//
// Registrations are address-stable and are never destroyed, which is a
// requirement rather than laziness: `Menu::AddItem`, `Menu::AddValueItem` and
// `Menu::AddToggleItem` store the label pointer with `label_is_static = 1`, store
// a value item's text pointer with `value_text_owned = 0`, and bind a toggle's
// `int *` by address; `Menu::ClearItems` frees none of the three - so the game
// keeps reading this struct's `label` characters, this struct's `value_text` and
// this struct's `value` for as long as the item is on screen. Moving or freeing
// it would dangle.
struct CustomMenuItem {
  MenuIndex menu;
  std::string label;
  CustomMenuItemKind kind;
  CustomMenuOwner owner;
  int value;           // Toggle state; the bound int*, rendered as ON / OFF
  char value_text[32]; // Value only; the char* the game renders, in place
  int index;           // live position within the menu, -1 while not applied
  CustomMenuAction action;
  CustomMenuRefresh refresh;
  CustomMenuAvailable available;
  void *user;
};

// Registers an item on a front-end menu. It is appended the next time that menu
// is *drawn*, never at registration time: the game's dispatch switches on the
// item index, so our item has to land after the game's own items, and for the
// dynamically populated menus that ordering only holds once the populator has
// run. Returns a pointer valid for the process lifetime.
CustomMenuItem *AddCustomMenuItem(MenuIndex menu, const char *label,
                                  CustomMenuAction action, void *user,
                                  CustomMenuOwner owner = CustomMenuOwner::Script);
CustomMenuItem *AddCustomMenuToggle(MenuIndex menu, const char *label,
                                    bool initial, CustomMenuAction action,
                                    void *user,
                                    CustomMenuOwner owner = CustomMenuOwner::Script);
// A `LabelWithValue` row - the one item type whose right-hand text is a plain
// `char *` rather than a `GL_RESOURCE_ID`, which is what makes text the string
// table does not contain ("4x", "Off") reachable at all. The click handler is
// expected to cycle the underlying setting; the text comes from the refresh.
CustomMenuItem *AddCustomMenuValue(MenuIndex menu, const char *label,
                                   CustomMenuAction action, void *user,
                                   CustomMenuOwner owner = CustomMenuOwner::Script);

void SetCustomMenuRefresh(CustomMenuItem *item, CustomMenuRefresh refresh);
void SetCustomMenuAvailable(CustomMenuItem *item, CustomMenuAvailable available);
// Copies `text` into the item's own buffer, truncating at 31 characters. The
// buffer never moves, which is the whole point: the game holds the pointer and
// re-measures the string every frame, so writing through this is how a value
// row changes.
void SetCustomMenuValueText(CustomMenuItem *item, const char *text);

// Declares `menu` a GkPlus page: the game's own items are cleared out of it once,
// the first time we are about to put an item there, and `title_resource_id`
// replaces its title if non-zero. Everything on the page is then ours, so the
// game's dispatch for it never runs.
//
// There is no spare slot in `Menus[36]` - it is a fixed `.data` array indexed by
// `ChosenMenu`, so a 37th id would be an out-of-bounds read on every draw. What
// makes this workable is that menu 19 (`Preferences`) is *dead*: `ChosenMenu` is
// written only by `GoToMenu` and `EnterMainMenuScreen`'s reset, and no `GoToMenu`
// call site passes 19, so nothing but us can reach it and nothing repopulates it
// (see menu_system_notes.md). Claiming a menu the game still navigates to would
// destroy that menu instead.
void ClaimCustomMenuPage(MenuIndex menu, unsigned title_resource_id);

// Makes every *script-owned* registration inert without unregistering it: the
// items stay on their menus as dead labels, because the game holds pointers into
// them. This is what a script host calls before it drops the callbacks those
// `user` pointers refer to. Native registrations are left alone - their actions
// are plain functions that outlive any script.
void ClearCustomMenuActions();

// Re-appends whatever the game dropped when it last rebuilt `menu`, and records
// each item's current index. Called from the per-frame front-end update hook,
// before the game draws.
void ReconcileCustomMenu(MenuIndex menu);

// Handles an activation if `index` is one of ours: plays the click sound, flips
// a toggle, runs the action. False means the click belongs to the game and its
// own dispatch must run.
bool DispatchCustomMenuClick(MenuIndex menu, int index);

// Hooks the two front-end entry points those two operations belong to:
// UpdateAndDrawMenuScreen (reconcile) and OnMenuItemClicked (dispatch). RAII,
// like every other *System - construct/destroy inside a Detours transaction.
class CustomMenuSystem {
public:
  CustomMenuSystem();
  ~CustomMenuSystem();
};
} // namespace gk
