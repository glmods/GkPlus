#pragma once

#include "Menu.h"

#include <string>

namespace gk {
struct CustomMenuItem;

// Runs *instead of* the game's dispatch when a GkPlus item is activated. `item`
// is the registration (already toggled, for a toggle item); `user` is whatever
// was handed to AddCustomMenuItem.
using CustomMenuAction = void (*)(CustomMenuItem *item, void *user);

// One front-end menu item owned by GkPlus rather than by the game.
//
// Registrations are address-stable and are never destroyed, which is a
// requirement rather than laziness: `Menu::AddItem` and `Menu::AddToggleItem`
// store the label pointer with `label_is_static = 1` and the toggle's `int *` by
// address, and `Menu::ClearItems` frees neither - so the game keeps reading this
// struct's `label` characters and this struct's `value` for as long as the item
// is on screen. Moving or freeing it would dangle.
struct CustomMenuItem {
  MenuIndex menu;
  std::string label;
  bool is_toggle;
  int value; // toggle state; the bound int*, rendered as ON / OFF
  int index; // live position within the menu, -1 while not applied
  CustomMenuAction action;
  void *user;
};

// Registers an item on a front-end menu. It is appended the next time that menu
// is *drawn*, never at registration time: the game's dispatch switches on the
// item index, so our item has to land after the game's own items, and for the
// dynamically populated menus that ordering only holds once the populator has
// run. Returns a pointer valid for the process lifetime.
CustomMenuItem *AddCustomMenuItem(MenuIndex menu, const char *label,
                                  CustomMenuAction action, void *user);
CustomMenuItem *AddCustomMenuToggle(MenuIndex menu, const char *label,
                                    bool initial, CustomMenuAction action,
                                    void *user);

// Makes every registration inert without unregistering it: the items stay on
// their menus as dead labels, because the game holds pointers into them. This is
// what a script host calls before it drops the callbacks those `user` pointers
// refer to.
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
