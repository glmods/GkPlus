#include "CustomMenu.h"

#include "Js.h"
#include "JsBindings.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

JSClassID MenuClassId;
JSClassID MenuItemClassId;
JSClassID MenusClassId;

// Both wrappers hold a pointer that outlives every context, so neither needs a
// finalizer and neither can dangle - unlike the Actor and Role wrappers. A Menu
// wrapper points into `Menus[36]` (a .data array) and a MenuItem wrapper into a
// CustomMenuItem registration, which CustomMenu.cpp never frees.

Menu *MenuOf(JSContext *ctx, JSValueConst self) {
  return static_cast<Menu *>(JS_GetOpaque2(ctx, self, MenuClassId));
}

CustomMenuItem *ItemOf(JSContext *ctx, JSValueConst self) {
  return static_cast<CustomMenuItem *>(
      JS_GetOpaque2(ctx, self, MenuItemClassId));
}

MenuIndex IndexOf(const Menu *menu) {
  return static_cast<MenuIndex>(menu - GetMenus());
}

JSValue NewMenuWrapper(JSContext *ctx, Menu *menu) {
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(MenuClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetOpaque(obj, menu);
  return obj;
}

JSValue NewMenuItemWrapper(JSContext *ctx, CustomMenuItem *item) {
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(MenuItemClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetOpaque(obj, item);
  return obj;
}

bool EqualsIgnoreCase(const char *a, const char *b) {
  // Menu names are ASCII identifiers from Menus.inc.h, so this needs no locale
  // and deliberately does not use the CRT's _stricmp (which is locale-aware).
  for (; *a && *b; ++a, ++b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + ('a' - 'A') : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + ('a' - 'A') : *b;
    if (ca != cb) {
      return false;
    }
  }
  return *a == *b;
}

// --- script callbacks --------------------------------------------------------

// One per custom item. The registration owns this through its `user` pointer and
// keeps it for the process lifetime, so `wrapper` is a strong reference held
// forever - that is deliberate: the same MenuItem object must come back from
// add_item() and arrive at the callback, so `item === e` holds in a script.
struct MenuItemBinding {
  JSContext *ctx;
  JSValue callback;
  JSValue wrapper;
};

std::vector<MenuItemBinding *> Bindings;

void OnItemActivated(CustomMenuItem *item, void *user) {
  auto *binding = static_cast<MenuItemBinding *>(user);
  if (!binding || JS_IsUndefined(binding->callback)) {
    return;
  }
  JSValueConst args[] = {binding->wrapper};
  JSValue result =
      JS_Call(binding->ctx, binding->callback, JS_UNDEFINED, 1, args);
  if (JS_IsException(result)) {
    char where[128];
    std::snprintf(where, sizeof(where), "%s menu item '%s'",
                  GetMenuName(item->menu), item->label.c_str());
    ReportException(binding->ctx, where);
  }
  JS_FreeValue(binding->ctx, result);
}

// --- MenuItem ----------------------------------------------------------------

JSValue GetItemLabel(JSContext *ctx, JSValueConst self) {
  CustomMenuItem *item = ItemOf(ctx, self);
  return item ? JS_NewString(ctx, item->label.c_str()) : JS_EXCEPTION;
}

// -1 until the menu has been drawn once: an item is appended lazily, after the
// game's own populator has run (see CustomMenu.h).
JSValue GetItemIndex(JSContext *ctx, JSValueConst self) {
  CustomMenuItem *item = ItemOf(ctx, self);
  return item ? JS_NewInt32(ctx, item->index) : JS_EXCEPTION;
}

JSValue GetItemMenu(JSContext *ctx, JSValueConst self) {
  CustomMenuItem *item = ItemOf(ctx, self);
  if (!item) {
    return JS_EXCEPTION;
  }
  return NewMenuWrapper(ctx, &GetMenus()[static_cast<int>(item->menu)]);
}

// undefined rather than false for a plain item, so `if (e.value !== undefined)`
// is a working test for "is this a toggle".
JSValue GetItemValue(JSContext *ctx, JSValueConst self) {
  CustomMenuItem *item = ItemOf(ctx, self);
  if (!item) {
    return JS_EXCEPTION;
  }
  return item->kind == CustomMenuItemKind::Toggle
             ? JS_NewBool(ctx, item->value != 0)
             : JS_UNDEFINED;
}

JSValue SetItemValue(JSContext *ctx, JSValueConst self, JSValueConst v) {
  CustomMenuItem *item = ItemOf(ctx, self);
  if (!item) {
    return JS_EXCEPTION;
  }
  if (item->kind != CustomMenuItemKind::Toggle) {
    return JS_ThrowTypeError(ctx, "'%s' is not a toggle item",
                             item->label.c_str());
  }
  // The renderer reads this int through the pointer the item was created with,
  // so the ON/OFF text follows on the next frame with nothing else to do.
  item->value = JS_ToBool(ctx, v) ? 1 : 0;
  return JS_UNDEFINED;
}

JSValue MenuItemToString(JSContext *ctx, JSValueConst self, int,
                         JSValueConst *) {
  CustomMenuItem *item = ItemOf(ctx, self);
  if (!item) {
    return JS_EXCEPTION;
  }
  char buf[192];
  std::snprintf(buf, sizeof(buf), "[MenuItem %s.%d '%s']",
                GetMenuName(item->menu), item->index, item->label.c_str());
  return JS_NewString(ctx, buf);
}

const JSCFunctionListEntry MenuItemProto[] = {
    JS_CGETSET_DEF("label", GetItemLabel, nullptr),
    JS_CGETSET_DEF("index", GetItemIndex, nullptr),
    JS_CGETSET_DEF("menu", GetItemMenu, nullptr),
    JS_CGETSET_DEF("value", GetItemValue, SetItemValue),
    JS_CFUNC_DEF("toString", 0, MenuItemToString),
};

const JSClassDef MenuItemClass = {
    "MenuItem", nullptr, nullptr, nullptr, nullptr,
};

// --- Menu --------------------------------------------------------------------

JSValue GetMenuId(JSContext *ctx, JSValueConst self) {
  Menu *menu = MenuOf(ctx, self);
  return menu ? JS_NewInt32(ctx, static_cast<int>(IndexOf(menu)))
              : JS_EXCEPTION;
}

JSValue GetName(JSContext *ctx, JSValueConst self) {
  Menu *menu = MenuOf(ctx, self);
  return menu ? JS_NewString(ctx, GetMenuName(IndexOf(menu))) : JS_EXCEPTION;
}

// The localized title, resolved through the satellite string DLL - "" for a menu
// whose id has no string in the active language.
JSValue GetTitle(JSContext *ctx, JSValueConst self) {
  Menu *menu = MenuOf(ctx, self);
  return menu ? JS_NewString(ctx, ResourceString(menu->title_resource_id))
              : JS_EXCEPTION;
}

JSValue GetCount(JSContext *ctx, JSValueConst self) {
  Menu *menu = MenuOf(ctx, self);
  return menu ? JS_NewInt32(ctx, menu->num_items) : JS_EXCEPTION;
}

// A snapshot of what is on the menu right now, game items included: each entry
// is {index, label, type}. Labels are whatever the item holds - a resolved
// resource string for the game's own items.
JSValue GetItems(JSContext *ctx, JSValueConst self) {
  Menu *menu = MenuOf(ctx, self);
  if (!menu) {
    return JS_EXCEPTION;
  }
  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr) || !menu->items.sentinel) {
    return arr;
  }
  uint32_t n = 0;
  for (const MenuItemData &data : menu->items) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
    JS_SetPropertyStr(ctx, entry, "index", JS_NewInt32(ctx, n));
    JS_SetPropertyStr(ctx, entry, "label",
                      data.label ? JS_NewString(ctx, data.label) : JS_NULL);
    JS_SetPropertyStr(ctx, entry, "type",
                      JS_NewString(ctx, MenuItemTypeName(data.type)));
    if (JS_SetPropertyUint32(ctx, arr, n++, entry) < 0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }
  return arr;
}

// Shared by add_item and add_toggle: validates the callback, builds the binding
// and registers with CustomMenu.cpp.
JSValue AddItem(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                bool is_toggle) {
  Menu *menu = MenuOf(ctx, self);
  if (!menu) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "%s expects a label",
                             is_toggle ? "add_toggle(label, initial, callback)"
                                       : "add_item(label, callback)");
  }

  int callback_arg = is_toggle ? 2 : 1;
  bool initial = false;
  if (is_toggle && argc > 1) {
    initial = JS_ToBool(ctx, argv[1]) != 0;
  }
  JSValueConst callback =
      argc > callback_arg ? argv[callback_arg] : JS_UNDEFINED;
  if (!JS_IsUndefined(callback) && !JS_IsFunction(ctx, callback)) {
    return JS_ThrowTypeError(ctx, "the callback must be a function");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  auto *binding = new MenuItemBinding{ctx, JS_DupValue(ctx, callback),
                                      JS_UNDEFINED};
  MenuIndex idx = IndexOf(menu);
  CustomMenuItem *item =
      is_toggle ? AddCustomMenuToggle(idx, label, initial, OnItemActivated,
                                      binding)
                : AddCustomMenuItem(idx, label, OnItemActivated, binding);
  JS_FreeCString(ctx, label);

  binding->wrapper = NewMenuItemWrapper(ctx, item);
  if (JS_IsException(binding->wrapper)) {
    // The registration survives with a null wrapper; make it inert rather than
    // let OnItemActivated dereference an exception value.
    JS_FreeValue(ctx, binding->callback);
    binding->callback = JS_UNDEFINED;
    binding->wrapper = JS_UNDEFINED;
    Bindings.push_back(binding);
    return JS_EXCEPTION;
  }
  Bindings.push_back(binding);
  return JS_DupValue(ctx, binding->wrapper);
}

JSValue MenuAddItemJs(JSContext *ctx, JSValueConst self, int argc,
                      JSValueConst *argv) {
  return AddItem(ctx, self, argc, argv, false);
}

JSValue MenuAddToggleJs(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
  return AddItem(ctx, self, argc, argv, true);
}

// remember = push the current menu as this one's parent, which is what makes
// Back come back here. The game's own transitions pass false about as often as
// true, so it is not defaulted to either by accident.
JSValue MenuOpen(JSContext *ctx, JSValueConst self, int argc,
                 JSValueConst *argv) {
  Menu *menu = MenuOf(ctx, self);
  if (!menu) {
    return JS_EXCEPTION;
  }
  bool remember = argc > 0 && JS_ToBool(ctx, argv[0]);
  GoToMenu(IndexOf(menu), remember);
  return JS_UNDEFINED;
}

JSValue MenuToString(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  Menu *menu = MenuOf(ctx, self);
  if (!menu) {
    return JS_EXCEPTION;
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "[Menu %d %s]",
                static_cast<int>(IndexOf(menu)), GetMenuName(IndexOf(menu)));
  return JS_NewString(ctx, buf);
}

const JSCFunctionListEntry MenuProto[] = {
    JS_CGETSET_DEF("id", GetMenuId, nullptr),
    JS_CGETSET_DEF("name", GetName, nullptr),
    JS_CGETSET_DEF("title", GetTitle, nullptr),
    JS_CGETSET_DEF("count", GetCount, nullptr),
    JS_CGETSET_DEF("items", GetItems, nullptr),
    JS_CFUNC_DEF("add_item", 2, MenuAddItemJs),
    JS_CFUNC_DEF("add_toggle", 3, MenuAddToggleJs),
    JS_CFUNC_DEF("open", 1, MenuOpen),
    JS_CFUNC_DEF("toString", 0, MenuToString),
};

const JSClassDef MenuClass = {
    "Menu", nullptr, nullptr, nullptr, nullptr,
};

// --- the collection ----------------------------------------------------------

JSValue LookupMenuById(JSContext *ctx, int id) {
  if (id < 0 || id >= MenuCount) {
    return JS_UNDEFINED;
  }
  return NewMenuWrapper(ctx, &GetMenus()[id]);
}

// The Menus.inc.h identifier, case-insensitively: menus.Main, menus["main"].
// Unlike actors and roles this is a total mapping - every menu has a name - but
// ids stay the enumerated keys so `menus` reads like the other collections.
JSValue LookupMenuByName(JSContext *ctx, const char *name) {
  for (int i = 0; i < MenuCount; ++i) {
    if (EqualsIgnoreCase(GetMenuName(static_cast<MenuIndex>(i)), name)) {
      return NewMenuWrapper(ctx, &GetMenus()[i]);
    }
  }
  return JS_UNDEFINED;
}

void CollectMenuKeys(std::vector<std::string> *out) {
  char buf[16];
  out->reserve(MenuCount);
  for (int i = 0; i < MenuCount; ++i) {
    std::snprintf(buf, sizeof(buf), "%d", i);
    out->emplace_back(buf);
  }
}

unsigned CountMenus() { return MenuCount; }

// The menu the front end is showing. Meaningless while a level is running -
// ChosenMenu keeps whatever it last held - which is why there is no setter here
// and navigation goes through menu.open().
JSValue GetCurrent(JSContext *ctx, JSValueConst) {
  int id = static_cast<int>(GetChosenMenu());
  if (id < 0 || id >= MenuCount) {
    return JS_NULL;
  }
  return NewMenuWrapper(ctx, &GetMenus()[id]);
}

const JSCFunctionListEntry MenusProps[] = {
    JS_CGETSET_DEF("current", GetCurrent, nullptr),
};

const CollectionOps MenusOps = {
    .class_name = "Menus",
    .lookup_id = LookupMenuById,
    .lookup_name = LookupMenuByName,
    .collect_keys = CollectMenuKeys,
    .count = CountMenus,
    .assign = nullptr, // the 36 menus are fixed; add items through the wrapper
    .props = MenusProps,
    .props_len = static_cast<int>(std::size(MenusProps)),
};

} // namespace

void ReleaseMenuCallbacks(JSContext *ctx) {
  // Order matters: stop the game calling in first, then drop the values. The
  // registrations themselves stay - the menus hold pointers into them.
  ClearCustomMenuActions();
  for (auto it = Bindings.begin(); it != Bindings.end();) {
    MenuItemBinding *binding = *it;
    if (binding->ctx != ctx) {
      ++it;
      continue;
    }
    JS_FreeValue(ctx, binding->callback);
    JS_FreeValue(ctx, binding->wrapper);
    delete binding;
    it = Bindings.erase(it);
  }
}

JSValue NewMenusNamespace(JSContext *ctx) {
  if (!EnsureClass(ctx, &MenuClassId, &MenuClass, MenuProto,
                   static_cast<int>(std::size(MenuProto))) ||
      !EnsureClass(ctx, &MenuItemClassId, &MenuItemClass, MenuItemProto,
                   static_cast<int>(std::size(MenuItemProto)))) {
    return JS_EXCEPTION;
  }
  return NewCollection(ctx, &MenusClassId, &MenusOps);
}

} // namespace gk::js
