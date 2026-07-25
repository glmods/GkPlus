#include "Menu.h"

#include "Core.h"

namespace gk {
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

// GetResourceString @ 0x00579000 takes ECX = &LocalizedStrings (the address of
// the resource-table pointer global @ 0x00725664), so the pointer must live at a
// stable address - a function-local static, resolved once.
const char *ResourceString(unsigned id) {
  static void *LocalizedStrings;
  static FastCall<const char *, void *, unsigned> GetResourceString;
  if (!GetResourceString) {
    GetObjectAtOffset(LocalizedStrings, 0x00725664);
    GetObjectAtOffset(GetResourceString, 0x00579000);
  }
  if (!LocalizedStrings) {
    return "";
  }
  const char *s = GetResourceString(&LocalizedStrings, id);
  return s ? s : "";
}

Menu *GetMenus() {
  Menu *m;
  GetObjectAtOffset(m, 0x007b76d0);
  return m;
}

Menu *GetInGameMenus() {
  Menu *m;
  GetObjectAtOffset(m, 0x007b7578);
  return m;
}

MenuIndex GetChosenMenu() {
  MenuIndex *p;
  GetObjectAtOffset(p, 0x007b732c);
  return *p;
}

int GetChosenMenuItem() {
  int *p;
  GetObjectAtOffset(p, 0x006a7d6c);
  return *p;
}

void SetChosenMenuItem(int item) {
  int *p;
  GetObjectAtOffset(p, 0x006a7d6c);
  *p = item;
}

int GetInGameMenuIndex() {
  int *p;
  GetObjectAtOffset(p, 0x007b7270);
  return *p;
}

int GetInGameMenuSelectedItem() {
  int *p;
  GetObjectAtOffset(p, 0x006a89b4);
  return *p;
}

void GoToMenu(MenuIndex target, bool remember) {
  FastCall<void, MenuIndex, bool> fn;
  GetObjectAtOffset(fn, 0x004fbfa0);
  fn(target, remember);
}

void PlayUiSound(int sound_id) {
  FastCall<void, int> fn;
  GetObjectAtOffset(fn, 0x0058cdd0);
  fn(sound_id);
}

bool IsAnyInGameMenuOpen() {
  FastCall<char> fn;
  GetObjectAtOffset(fn, 0x00569550);
  return fn() != 0;
}

void CloseInGameMenu(int kind) {
  FastCall<void, int> fn;
  GetObjectAtOffset(fn, 0x005691f0);
  fn(kind);
}

void MenuAddItem(Menu *menu, const char *label) {
  ThisCall<void, Menu *, const char *> fn;
  GetObjectAtOffset(fn, 0x004f7a60);
  fn(menu, label);
}

void MenuAddValueItem(Menu *menu, const char *label, const char *value,
                      bool label_is_static, bool value_text_owned) {
  ThisCall<void, Menu *, const char *, const char *, bool, bool> fn;
  GetObjectAtOffset(fn, 0x004f7ae0);
  fn(menu, label, value, label_is_static, value_text_owned);
}

void MenuAddToggleItem(Menu *menu, const char *label, int *value) {
  ThisCall<void, Menu *, const char *, int *> fn;
  GetObjectAtOffset(fn, 0x004f7950);
  fn(menu, label, value);
}

void MenuAddMultiValueItem(Menu *menu, const char *label, int *index,
                           unsigned **labels) {
  ThisCall<void, Menu *, const char *, int *, unsigned **> fn;
  GetObjectAtOffset(fn, 0x004f79d0);
  fn(menu, label, index, labels);
}

void *GetMenuItemData(Menu *menu, int index) {
  ThisCall<void *, Menu *, int> fn;
  GetObjectAtOffset(fn, 0x004f7750);
  return fn(menu, index);
}
} // namespace gk
