#include "CustomMenu.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member
// pointers, and merely declaring them inside namespace gk hides the global
// templates that handle a plain function pointer.

#include <cstring>
#include <memory>
#include <vector>

namespace gk {
namespace {
StdCall<> UpdateAndDrawMenuScreen;
StdCall<> OnMenuItemClicked;

// unique_ptr, not CustomMenuItem by value: the game borrows &item->value,
// item->value_text and item->label.c_str(), so a vector reallocation would leave
// it reading freed memory (see CustomMenu.h).
std::vector<std::unique_ptr<CustomMenuItem>> Items;

// A menu GkPlus owns outright. `cleared` latches the one-time ClearItems, which
// is deferred until we actually have something to put there - a page whose every
// item is unavailable is left as the game built it rather than emptied.
struct CustomMenuPage {
  MenuIndex menu;
  unsigned title_resource_id;
  bool cleared;
};
std::vector<CustomMenuPage> Pages;

CustomMenuPage *FindPage(MenuIndex idx) {
  for (CustomMenuPage &page : Pages) {
    if (page.menu == idx) {
      return &page;
    }
  }
  return nullptr;
}

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

bool IsAvailable(const CustomMenuItem *item) {
  return !item->available || item->available(item->user);
}

void Apply(CustomMenuItem *item, Menu *menu) {
  item->index = menu->num_items;
  switch (item->kind) {
  case CustomMenuItemKind::Toggle:
    MenuAddToggleItem(menu, item->label.c_str(), &item->value);
    break;
  case CustomMenuItemKind::Value:
    // label_is_static, and *not* value_text_owned: both buffers are ours and
    // ClearItems must free neither.
    MenuAddValueItem(menu, item->label.c_str(), item->value_text, true, false);
    break;
  case CustomMenuItemKind::Action:
    MenuAddItem(menu, item->label.c_str());
    break;
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

CustomMenuItem *Register(MenuIndex menu, const char *label,
                         CustomMenuItemKind kind, bool initial,
                         CustomMenuAction action, void *user,
                         CustomMenuOwner owner) {
  auto item = std::make_unique<CustomMenuItem>();
  item->menu = menu;
  item->label = label ? label : "";
  item->kind = kind;
  item->owner = owner;
  item->value = initial ? 1 : 0;
  item->value_text[0] = '\0';
  item->index = -1;
  item->action = action;
  item->refresh = nullptr;
  item->available = nullptr;
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

  // A refresh runs before the matching pass, so a value item's text is already
  // correct on the frame it is first appended rather than one frame later.
  for (const std::unique_ptr<CustomMenuItem> &item : Items) {
    if (item->menu == idx && item->refresh) {
      item->refresh(item.get(), item->user);
    }
  }

  if (CustomMenuPage *page = FindPage(idx); page && !page->cleared) {
    bool any = false;
    for (const std::unique_ptr<CustomMenuItem> &item : Items) {
      any = any || (item->menu == idx && IsAvailable(item.get()));
    }
    if (any) {
      MenuClearItems(menu);
      if (page->title_resource_id) {
        menu->title_resource_id = page->title_resource_id;
      }
      page->cleared = true;
    }
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
    if (item->menu == idx && item->index < 0 && IsAvailable(item.get())) {
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
  // every bound variable explicitly - so a custom toggle has to flip its own. A
  // value item's text is not touched here: cycling is the action's job, and the
  // next reconcile is what puts the result on screen.
  if (item->kind == CustomMenuItemKind::Toggle) {
    item->value = !item->value;
  }
  if (item->action) {
    item->action(item, item->user);
  }
  return true;
}

CustomMenuItem *AddCustomMenuItem(MenuIndex menu, const char *label,
                                  CustomMenuAction action, void *user,
                                  CustomMenuOwner owner) {
  return Register(menu, label, CustomMenuItemKind::Action, false, action, user,
                  owner);
}

CustomMenuItem *AddCustomMenuToggle(MenuIndex menu, const char *label,
                                    bool initial, CustomMenuAction action,
                                    void *user, CustomMenuOwner owner) {
  return Register(menu, label, CustomMenuItemKind::Toggle, initial, action, user,
                  owner);
}

CustomMenuItem *AddCustomMenuValue(MenuIndex menu, const char *label,
                                   CustomMenuAction action, void *user,
                                   CustomMenuOwner owner) {
  return Register(menu, label, CustomMenuItemKind::Value, false, action, user,
                  owner);
}

void SetCustomMenuRefresh(CustomMenuItem *item, CustomMenuRefresh refresh) {
  if (item) {
    item->refresh = refresh;
  }
}

void SetCustomMenuAvailable(CustomMenuItem *item,
                            CustomMenuAvailable available) {
  if (item) {
    item->available = available;
  }
}

void SetCustomMenuValueText(CustomMenuItem *item, const char *text) {
  if (!item) {
    return;
  }
  const char *src = text ? text : "";
  std::size_t n = std::strlen(src);
  if (n >= sizeof(item->value_text)) {
    n = sizeof(item->value_text) - 1;
  }
  std::memcpy(item->value_text, src, n);
  item->value_text[n] = '\0';
}

void ClaimCustomMenuPage(MenuIndex menu, unsigned title_resource_id) {
  if (CustomMenuPage *page = FindPage(menu)) {
    page->title_resource_id = title_resource_id;
    return;
  }
  Pages.push_back(CustomMenuPage{menu, title_resource_id, false});
}

void ClearCustomMenuActions() {
  for (const std::unique_ptr<CustomMenuItem> &item : Items) {
    if (item->owner != CustomMenuOwner::Script) {
      continue;
    }
    item->action = nullptr;
    item->refresh = nullptr;
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
