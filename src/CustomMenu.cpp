#include "CustomMenu.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member
// pointers, and merely declaring them inside namespace gk hides the global
// templates that handle a plain function pointer.

#include <memory>
#include <vector>

namespace gk {
namespace {
StdCall<> UpdateAndDrawMenuScreen;
StdCall<> OnMenuItemClicked;

// unique_ptr, not CustomMenuItem by value: the game borrows &item->value and
// item->label.c_str(), so a vector reallocation would leave it reading freed
// memory (see CustomMenu.h).
std::vector<std::unique_ptr<CustomMenuItem>> Items;

Menu *MenuAt(MenuIndex idx) {
  int i = static_cast<int>(idx);
  if (i < 0 || i >= MenuCount) {
    return nullptr;
  }
  Menu *menu = &GetMenus()[i];
  // Menus[36] is a .data array; its list sentinels are only linked up by the
  // Menu constructors inside SetupMenus. Before that the list is unwalkable.
  return menu->items.sentinel ? menu : nullptr;
}

void Apply(CustomMenuItem *item, Menu *menu) {
  item->index = menu->num_items;
  if (item->is_toggle) {
    MenuAddToggleItem(menu, item->label.c_str(), &item->value);
  } else {
    MenuAddItem(menu, item->label.c_str());
  }
}

CustomMenuItem *FindByIndex(MenuIndex menu, int index) {
  for (const std::unique_ptr<CustomMenuItem> &item : Items) {
    if (item->menu == menu && item->index == index) {
      return item.get();
    }
  }
  return nullptr;
}

// Runs before the original, so our items are back in place for this frame's
// layout and hit-test. One menu is beyond reach this way: menu 11 (JoinGame)
// re-enumerates its sessions from *inside* UpdateAndDrawMenuScreen every frame,
// clearing the list after we have reconciled it.
void __stdcall HookedUpdateAndDrawMenuScreen() {
  ReconcileCustomMenu(GetChosenMenu());
  UpdateAndDrawMenuScreen();
}

void __stdcall HookedOnMenuItemClicked() {
  // The pseudo-values (0x100 none, 0x101 back, 0x102/3 scroll) never match a
  // real index, so they fall through to the game unexamined.
  if (!DispatchCustomMenuClick(GetChosenMenu(), GetChosenMenuItem())) {
    OnMenuItemClicked();
  }
}

CustomMenuItem *Register(MenuIndex menu, const char *label, bool is_toggle,
                         bool initial, CustomMenuAction action, void *user) {
  auto item = std::make_unique<CustomMenuItem>();
  item->menu = menu;
  item->label = label ? label : "";
  item->is_toggle = is_toggle;
  item->value = initial ? 1 : 0;
  item->index = -1;
  item->action = action;
  item->user = user;

  CustomMenuItem *raw = item.get();
  Items.push_back(std::move(item));
  return raw;
}
} // namespace

// Identity is the label *pointer*, not its text: our storage is stable and no
// game item can hold it, so pointer equality is exact even when a script adds
// two items with the same text.
void ReconcileCustomMenu(MenuIndex idx) {
  bool ours = false;
  for (const std::unique_ptr<CustomMenuItem> &item : Items) {
    if (item->menu == idx) {
      item->index = -1;
      ours = true;
    }
  }
  if (!ours) {
    return;
  }
  Menu *menu = MenuAt(idx);
  if (!menu) {
    return;
  }

  int position = 0;
  for (const MenuItemData &data : menu->items) {
    for (const std::unique_ptr<CustomMenuItem> &item : Items) {
      if (item->menu == idx && data.label == item->label.c_str()) {
        item->index = position;
      }
    }
    ++position;
  }

  for (const std::unique_ptr<CustomMenuItem> &item : Items) {
    if (item->menu == idx && item->index < 0) {
      Apply(item.get(), menu);
    }
  }
}

bool DispatchCustomMenuClick(MenuIndex menu, int index) {
  CustomMenuItem *item = FindByIndex(menu, index);
  if (!item) {
    return false;
  }
  PlayUiSound(UiSoundMenuSelect);
  // There is no generic toggle handler in the game - OnMenuItemClicked mutates
  // every bound variable explicitly - so a custom toggle has to flip its own.
  if (item->is_toggle) {
    item->value = !item->value;
  }
  if (item->action) {
    item->action(item, item->user);
  }
  return true;
}

CustomMenuItem *AddCustomMenuItem(MenuIndex menu, const char *label,
                                  CustomMenuAction action, void *user) {
  return Register(menu, label, false, false, action, user);
}

CustomMenuItem *AddCustomMenuToggle(MenuIndex menu, const char *label,
                                    bool initial, CustomMenuAction action,
                                    void *user) {
  return Register(menu, label, true, initial, action, user);
}

void ClearCustomMenuActions() {
  for (const std::unique_ptr<CustomMenuItem> &item : Items) {
    item->action = nullptr;
    item->user = nullptr;
  }
}

CustomMenuSystem::CustomMenuSystem() {
  GetObjectAtOffset(UpdateAndDrawMenuScreen, 0x004ea8e0);
  GetObjectAtOffset(OnMenuItemClicked, 0x004ecf10);

  DetourAttach(&UpdateAndDrawMenuScreen, HookedUpdateAndDrawMenuScreen);
  DetourAttach(&OnMenuItemClicked, HookedOnMenuItemClicked);
}

CustomMenuSystem::~CustomMenuSystem() {
  DetourDetach(&UpdateAndDrawMenuScreen, HookedUpdateAndDrawMenuScreen);
  DetourDetach(&OnMenuItemClicked, HookedOnMenuItemClicked);
}
} // namespace gk
