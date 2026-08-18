// The `settings` namespace: `<profile>\settings.json` as a live object tree
// (src/Settings.h).
//
// The point of exposing it is that the file is a *repository*, not GkPlus's own
// store - a mod keeps its settings in a section of its own beside `core`, and
// mods are scripts, so without this the "shared" half would be true only of C++.
//
// It reads and writes like an ordinary object:
//
//     settings.mymod = {window: {x: 10, y: 20}};   // a section of your own
//     settings.mymod.window.x = 40;                // in the document at once
//     if (settings.core.render.ao) { ... }         // and the menus see it too
//
// **Nothing here holds a copy.** Every property read goes to the store and every
// write goes straight through to it, so `settings` is not a snapshot that has to
// be pushed back: an object subtree hands out another node bound to its own path,
// and only a leaf ever becomes a JS value. That is what makes this shape correct
// rather than merely convenient - a plain object parsed once would be a *second*
// truth, and it would silently lose whichever of the two changed the same key
// last. The Advanced Graphics page writes `core.render.*` from the front end
// (src/RenderMenu.cpp) while a script holds this object, and the REPL's context
// builds a second `settings` on the same document; neither can diverge from what
// a script sees, because there is nothing to diverge from.
//
// **The file catches up on the way out.** A write marks the document dirty and
// `settings::SaveIfDirty()` at DLL detach writes it once (src/entry.cpp), so a
// script that changes a setting need not remember to save. A crash therefore
// loses the change - Gunlok does crash - so `save()` is still here for anything
// that must survive one, and the front-end page keeps calling it per click.
//
// Two limits are worth knowing, and both are deliberately loud rather than
// silent:
//
//   * **A key containing a dot cannot be addressed.** Paths are dot-separated all
//     the way down to json::Document, so `settings["my.mod"]` would mean the
//     nested `mod` inside `my` - a read of something else and a write to the
//     wrong place. Such a key reads as absent and refuses to be written.
//   * **An array is a value, and the one handed out is frozen.** Its elements are
//     not addressable by path, so there is nothing for `list.push(x)` to write
//     through; freezing turns that from a write that vanishes into a TypeError.
//     Assign the whole array to change it.
//
// The dotted-path methods (`get`, `set`, `remove`) stay on the root beside
// `save`, `reload`, `path` and `all`. `set` is still the way to create a deep
// path in one go - `settings.a.b = 1` needs `a` to exist first, exactly as it
// would for any object - and they are own properties, so a top-level section that
// happens to be named `save` shadows the method, the same trade `tokens.count`
// makes.

#include "Settings.h"

#include "JsBindings.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <new>
#include <string>
#include <vector>

namespace gk::js {
namespace {

// The one place a JS value becomes text for the store. `undefined` has no JSON
// form - JSON.stringify answers with undefined rather than a document - and it is
// worth refusing loudly, because `settings.mymod = obj.missing` would otherwise
// read as a no-op that looks like a successful write.
bool ToJsonText(JSContext *ctx, JSValueConst value, std::string *out) {
  JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) {
    JS_FreeValue(ctx, json);
    return false; // a cyclic value, or a throwing toJSON: leave the exception
  }
  if (!JS_IsString(json)) {
    JS_FreeValue(ctx, json);
    JS_ThrowTypeError(ctx, "the value has no JSON form");
    return false;
  }
  const char *text = JS_ToCString(ctx, json);
  JS_FreeValue(ctx, json);
  if (!text) {
    return false;
  }
  *out = text;
  JS_FreeCString(ctx, text);
  return true;
}

// --- the node -----------------------------------------------------------------

JSClassID SettingsNodeClassId;

// A node holds the dotted path of the subtree it stands for and nothing else; the
// root's is empty. Two nodes for the same subtree are therefore interchangeable,
// and a node cannot go stale - what it names may stop existing, which reads as
// every key being absent.
const std::string *PathOf(JSValueConst obj) {
  return static_cast<const std::string *>(
      JS_GetOpaque(obj, SettingsNodeClassId));
}

void NodeFinalizer(JSRuntime *, JSValueConst val) {
  delete static_cast<std::string *>(JS_GetOpaque(val, SettingsNodeClassId));
}

JSValue NewNode(JSContext *ctx, const std::string &path) {
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(SettingsNodeClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  auto *owned = new (std::nothrow) std::string(path);
  if (!owned) {
    JS_FreeValue(ctx, obj);
    return JS_ThrowOutOfMemory(ctx);
  }
  JS_SetOpaque(obj, owned);
  return obj;
}

struct NodeKey {
  const char *text = nullptr; // owned; JS_FreeCString
  bool ours = false;          // a string key this node can address
  bool failed = false;        // an exception is pending
  bool dotted = false;        // a string key with a dot in it - unaddressable
};

NodeKey DecodeKey(JSContext *ctx, JSAtom prop) {
  NodeKey key;
  // Symbols are never ours, and must fall through to the ordinary lookup rather
  // than being reported absent - Symbol.toPrimitive and friends live on the
  // prototype.
  JSValue as_value = JS_AtomToValue(ctx, prop);
  const bool is_symbol = JS_IsSymbol(as_value);
  JS_FreeValue(ctx, as_value);
  if (is_symbol) {
    return key;
  }
  key.text = JS_AtomToCString(ctx, prop);
  if (!key.text) {
    key.failed = true; // out of memory, with an exception already pending
    return key;
  }
  key.dotted = std::strchr(key.text, '.') != nullptr;
  key.ours = !key.dotted;
  return key;
}

void FreeKey(JSContext *ctx, NodeKey *key) {
  if (key->text) {
    JS_FreeCString(ctx, key->text);
    key->text = nullptr;
  }
}

std::string ChildPath(const std::string &prefix, const char *key) {
  return prefix.empty() ? std::string(key) : prefix + "." + key;
}

// Object.freeze, recursively, over a value just parsed out of the store. Only
// arrays and their contents get here - an object subtree becomes a node instead -
// and an array's elements are not addressable by path, so there is nothing for a
// mutation to be written through to. See the header comment.
//
// The recursion is as deep as the value's nesting, which JS_ParseJSON has just
// walked on this same thread with a heavier frame per level, so anything that
// could overflow here has already been refused by its stack check.
bool FreezeDeep(JSContext *ctx, JSValueConst value) {
  if (!JS_IsObject(value)) {
    return true;
  }
  JSPropertyEnum *tab = nullptr;
  uint32_t len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, value, JS_GPN_STRING_MASK) < 0) {
    return false;
  }
  bool ok = true;
  for (uint32_t i = 0; ok && i < len; ++i) {
    JSValue child = JS_GetProperty(ctx, value, tab[i].atom);
    if (JS_IsException(child)) {
      ok = false;
      break;
    }
    ok = FreezeDeep(ctx, child);
    JS_FreeValue(ctx, child);
  }
  JS_FreePropertyEnum(ctx, tab, len);
  return ok && JS_FreezeObject(ctx, value) >= 0;
}

// A leaf - anything that is not an object subtree - as a JS value. Parsed in the
// *host* runtime: the store's tree lives in the codec's private one and may never
// be handed out (see Json.h).
JSValue ReadLeaf(JSContext *ctx, const std::string &path) {
  const std::string json = settings::GetJson(path.c_str());
  if (json.empty()) {
    return JS_UNDEFINED;
  }
  JSValue value = JS_ParseJSON(ctx, json.c_str(), json.size(), "<setting>");
  if (JS_IsException(value)) {
    return JS_EXCEPTION;
  }
  if (!FreezeDeep(ctx, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

int NodeGetOwnProperty(JSContext *ctx, JSPropertyDescriptor *desc,
                       JSValueConst obj, JSAtom prop) {
  const std::string *prefix = PathOf(obj);
  if (!prefix) {
    return 0;
  }
  NodeKey key = DecodeKey(ctx, prop);
  if (key.failed) {
    return -1;
  }
  if (!key.ours) {
    // A symbol, or a key with a dot in it: not an own property. Reporting a
    // dotted key absent is the honest answer, since resolving it would read the
    // nested value of a different key.
    FreeKey(ctx, &key);
    return 0;
  }
  const std::string path = ChildPath(*prefix, key.text);
  FreeKey(ctx, &key);

  const json::Kind kind = settings::KindAt(path.c_str());
  if (kind == json::Kind::Invalid) {
    return 0; // nothing there - JSON has no undefined, so this is exactly absent
  }
  if (!desc) {
    // JS_HasProperty, or the enumerability re-query during a key walk.
    return 1;
  }
  JSValue value = kind == json::Kind::Object ? NewNode(ctx, path)
                                            : ReadLeaf(ctx, path);
  if (JS_IsException(value)) {
    return -1;
  }
  // Configurable because a section can be deleted, and enumerable because that
  // is what Object.keys and JSON.stringify read - quickjs ignores the flag it is
  // handed in the names list and re-queries this one for the truth.
  desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
  desc->value = value;
  desc->getter = JS_UNDEFINED;
  desc->setter = JS_UNDEFINED;
  return 1;
}

int NodeGetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab,
                            uint32_t *plen, JSValueConst obj) {
  const std::string *prefix = PathOf(obj);
  const std::vector<std::string> keys =
      prefix ? settings::Keys(prefix->c_str()) : std::vector<std::string>();

  JSPropertyEnum *tab = nullptr;
  if (!keys.empty()) {
    tab = static_cast<JSPropertyEnum *>(
        js_malloc(ctx, sizeof(JSPropertyEnum) * keys.size()));
    if (!tab) {
      return -1;
    }
  }

  uint32_t n = 0;
  for (const std::string &key : keys) {
    // A key with a dot in it is in the file - somebody hand-edited it, or an
    // older build wrote it - and cannot be addressed, so listing it would hand
    // out a name that reads back undefined.
    if (key.find('.') != std::string::npos) {
      continue;
    }
    JSAtom atom = JS_NewAtom(ctx, key.c_str());
    if (atom == JS_ATOM_NULL) {
      JS_FreePropertyEnum(ctx, tab, n);
      return -1;
    }
    tab[n].is_enumerable = true;
    tab[n].atom = atom;
    ++n;
  }

  *ptab = tab;
  *plen = n;
  return 0;
}

// `value` is borrowed - JS_SetPropertyInternal2 frees it after we return
// (quickjs.c:10212).
//
// This throws rather than returning false, for the reason the collections do
// (src/JsCommon.cpp): quickjs hands the hook's result straight back to the
// caller instead of turning a false into the strict-mode TypeError, so returning
// false would make the assignment a silent no-op.
int NodeSetProperty(JSContext *ctx, JSValueConst obj, JSAtom prop,
                    JSValueConst value, JSValueConst, int) {
  const std::string *prefix = PathOf(obj);
  if (!prefix) {
    JS_ThrowTypeError(ctx, "not a settings object");
    return -1;
  }
  NodeKey key = DecodeKey(ctx, prop);
  if (key.failed) {
    return -1;
  }
  if (!key.ours) {
    if (key.dotted) {
      JS_ThrowTypeError(ctx,
                        "a settings key cannot contain a dot ('%s'); it would "
                        "address a nested key instead",
                        key.text);
    } else {
      JS_ThrowTypeError(ctx, "settings keys must be strings");
    }
    FreeKey(ctx, &key);
    return -1;
  }
  const std::string path = ChildPath(*prefix, key.text);
  FreeKey(ctx, &key);

  std::string json;
  if (!ToJsonText(ctx, value, &json)) {
    return -1;
  }
  if (!settings::SetJson(path.c_str(), json.c_str())) {
    JS_ThrowTypeError(ctx, "'%s' cannot be written", path.c_str());
    return -1;
  }
  return 1;
}

int NodeDeleteProperty(JSContext *ctx, JSValueConst obj, JSAtom prop) {
  const std::string *prefix = PathOf(obj);
  if (!prefix) {
    return 0;
  }
  NodeKey key = DecodeKey(ctx, prop);
  if (key.failed) {
    return -1;
  }
  if (!key.ours) {
    FreeKey(ctx, &key);
    return 1; // there was nothing there to delete, which is a successful delete
  }
  const std::string path = ChildPath(*prefix, key.text);
  FreeKey(ctx, &key);
  settings::Remove(path.c_str());
  return 1;
}

const JSClassExoticMethods NodeExotic = {
    NodeGetOwnProperty,
    NodeGetOwnPropertyNames,
    NodeDeleteProperty,
    nullptr, // define_own_property - Object.defineProperty is not a settings API
    nullptr, // has_property        - emulated from get_own_property
    nullptr, // get_property        - ditto
    NodeSetProperty,
};

// --- the root's own members ---------------------------------------------------

// get(path, fallback?) - the stored value, or `fallback` (undefined by default)
// when there is nothing at that path. Still here because a path addresses a depth
// the tree cannot reach in one step, and because it takes a fallback.
JSValue GetJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "get(path, fallback) expects a path string");
  }
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  const std::string json = settings::GetJson(path);
  JS_FreeCString(ctx, path);
  if (json.empty()) {
    return argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
  }
  // A plain value, not a node: this is the snapshot-shaped half of the API, and
  // a caller passing a fallback is asking for a value either way.
  JSValue value = JS_ParseJSON(ctx, json.c_str(), json.size(), "<setting>");
  if (JS_IsException(value)) {
    JS_FreeValue(ctx, value);
    return argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
  }
  return value;
}

// set(path, value) - creates every intermediate object, which is what makes it
// worth keeping beside the tree: `settings.set("mymod.window.x", 5)` needs
// neither `mymod` nor `window` to exist.
JSValue SetJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "set(path, value) expects a path string");
  }
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  std::string json;
  if (!ToJsonText(ctx, argv[1], &json)) {
    JS_FreeCString(ctx, path);
    return JS_EXCEPTION;
  }
  const bool ok = settings::SetJson(path, json.c_str());
  JS_FreeCString(ctx, path);
  if (!ok) {
    return JS_ThrowTypeError(ctx, "that path cannot be written");
  }
  return JS_UNDEFINED;
}

// remove(path) -> whether there was anything there.
JSValue RemoveJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "remove(path) expects a path string");
  }
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  const bool removed = settings::Remove(path);
  JS_FreeCString(ctx, path);
  return JS_NewBool(ctx, removed);
}

JSValue SaveJs(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewBool(ctx, settings::Save());
}

JSValue ReloadJs(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewBool(ctx, settings::Reload());
}

// The whole document as a plain object, detached from the store - the tree itself
// is the live view, so this is for a caller that wants a copy that will *not*
// change under it.
JSValue GetAllJs(JSContext *ctx, JSValueConst) {
  const std::string text = settings::Text();
  JSValue value = JS_ParseJSON(ctx, text.c_str(), text.size(), "<settings>");
  if (JS_IsException(value)) {
    JS_FreeValue(ctx, value);
    return JS_NewObject(ctx);
  }
  return value;
}

JSValue GetPathJs(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, settings::Path().c_str());
}

// Own properties of the root node, and non-enumerable so Object.keys(settings)
// lists sections and nothing else. Own properties win over the exotic hooks
// (quickjs.c:8734 for a read, :10137 for a write), so a top-level section named
// like one of these is shadowed by it - the same trade `tokens.count` makes.
const JSCFunctionListEntry RootProps[] = {
    JS_CFUNC_DEF("get", 2, GetJs),
    JS_CFUNC_DEF("set", 2, SetJs),
    JS_CFUNC_DEF("remove", 1, RemoveJs),
    JS_CFUNC_DEF("save", 0, SaveJs),
    JS_CFUNC_DEF("reload", 0, ReloadJs),
    JS_CGETSET_DEF("all", GetAllJs, nullptr),
    JS_CGETSET_DEF("path", GetPathJs, nullptr),
};

} // namespace

JSValue NewSettingsNamespace(JSContext *ctx) {
  JSClassDef def = {};
  def.class_name = "SettingsNode";
  def.finalizer = NodeFinalizer;
  def.exotic = const_cast<JSClassExoticMethods *>(&NodeExotic);
  // An empty prototype chained to Object.prototype, so hasOwnProperty and
  // friends work on a node - unlike the collections, which want none.
  if (!EnsureClass(ctx, &SettingsNodeClassId, &def, nullptr, 0, JS_UNDEFINED)) {
    return JS_ThrowInternalError(ctx, "could not register SettingsNode");
  }
  JSValue root = NewNode(ctx, std::string());
  if (JS_IsException(root)) {
    return root;
  }
  if (JS_SetPropertyFunctionList(ctx, root, RootProps,
                                 static_cast<int>(std::size(RootProps))) < 0) {
    JS_FreeValue(ctx, root);
    return JS_EXCEPTION;
  }
  return root;
}

} // namespace gk::js
