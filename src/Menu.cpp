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

// GetResourceString @ 0x00579000 opens `MOV EAX,dword ptr [ECX]`, so ECX is the
// address *of* the resource-table pointer global @ 0x00725664 - one level of
// indirection, not two. Measured: all 854 call sites in gl.exe load ECX with
// `MOV ECX,0x725664` and none dereferences it first. (The contrast that settles
// it is `FreeResourceStringTable` @ 0x00579020, which is handed the array base
// instead, via `MOV ECX,[0x00725664]` @ 0x0046aa72 - the binary distinguishes the
// two shapes and uses each with the function that wants it.)
//
// So `LocalizedStrings` is itself the slot address and is passed by value.
// Passing `&LocalizedStrings` would hand the game a pointer to *our* static,
// whose contents are 0x00725664, and the scan would run from the wrong base.
const char *ResourceString(unsigned id) {
  static void **LocalizedStrings;
  static FastCall<const char *, void **, unsigned> GetResourceString;
  if (!GetResourceString) {
    GetObjectAtOffset(LocalizedStrings, 0x00725664);
    GetObjectAtOffset(GetResourceString, 0x00579000);
  }
  // The table itself is null until `LoadResourceStringTable` @ 0x00578f30 fills it
  // - `WinMain` at 0x0046b355, and like the console that is *after* the engine's
  // first file open, so a script running from the first-open anchor is ahead of
  // it. GetResourceString scans 0x14-byte entries **with no bound**, so an
  // unloaded table does not fault, it walks .data until some dword matches the id.
  // Read fresh through the slot every call, because the whole point is that it
  // changes after the first call this might get.
  //
  // (The entries do carry an `is_last` flag at +0x10, which
  // `FreeResourceStringTable` honours and `GetResourceString` never tests - so
  // the walk is unbounded despite the terminator existing.)
  if (!*LocalizedStrings) {
    return "";
  }
  const char *s = GetResourceString(LocalizedStrings, id);
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

LevelList *GetLevelList() {
  LevelList *p;
  GetObjectAtOffset(p, 0x007b74dc);
  return p;
}

LevelList *GetMultiplayerLevelList() {
  LevelList *p;
  GetObjectAtOffset(p, 0x007b76b0);
  return p;
}

void AddLevel(const char *title, const char *script_file,
              const char *console_file) {
  FastCall<void, const char *, const char *, const char *> fn;
  GetObjectAtOffset(fn, 0x004efcc0);
  // All three are strdup'd; none may be null - AddLevel walks each one to
  // measure it before copying.
  fn(title ? title : "", script_file ? script_file : "",
     console_file ? console_file : "");
}

bool GetChooseLevelEnabled() {
  unsigned char *p;
  GetObjectAtOffset(p, 0x006b0173);
  return *p != 0;
}

void SetChooseLevelEnabled(bool enabled) {
  unsigned char *p;
  GetObjectAtOffset(p, 0x006b0173);
  *p = enabled ? 1 : 0;
}

int PlayUiSound(int sound_id) {
  FastCall<int, int> fn;
  GetObjectAtOffset(fn, 0x0058cdd0);
  return fn(sound_id);
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

void MenuClearItems(Menu *menu) {
  ThisCall<void, Menu *> fn;
  GetObjectAtOffset(fn, 0x004f7cd0);
  fn(menu);
}

void *GetMenuItemData(Menu *menu, int index) {
  ThisCall<void *, Menu *, int> fn;
  GetObjectAtOffset(fn, 0x004f7750);
  return fn(menu, index);
}
} // namespace gk
