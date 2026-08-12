// The `settings` namespace: the shared JSON file under `gkplus\` (src/Settings.h).
//
// The point of exposing it is that the file is a *repository*, not GkPlus's own
// store - a mod keeps its settings in a section of its own beside `core`, and
// mods are scripts, so without this the "shared" half would be true only of C++.
//
// The surface is four calls over a dotted path, because the alternative - handing
// out a live object and reflecting mutations - cannot work: a JS object here would
// be a copy of a subtree, and writing to a copy is exactly the bug the shape is
// meant to prevent. Values cross as JSON text, which is also what makes an
// unknown section survive a rewrite: nothing here builds a schema.

#include "Settings.h"

#include "JsBindings.h"

#include <iterator>
#include <string>

namespace gk::js {
namespace {

// The one place a JS value becomes text for the store. `undefined` has no JSON
// form - JSON.stringify answers with undefined rather than a document - and it is
// worth refusing loudly, because `set(path, obj.missing)` would otherwise read as
// a no-op that looks like a successful write.
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

// get(path, fallback?) - the stored value, or `fallback` (undefined by default)
// when there is nothing at that path.
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
  // Parsed in the *host* runtime. The store's tree lives in the codec's private
  // one and may never be handed out - see Json.h.
  JSValue value = JS_ParseJSON(ctx, json.c_str(), json.size(), "<setting>");
  if (JS_IsException(value)) {
    JS_FreeValue(ctx, value);
    return argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
  }
  return value;
}

// set(path, value) - in memory only; save() is what reaches the disk.
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

// The whole document, as a fresh object each time. A snapshot, like `mods` -
// mutating it changes nothing, which is why set() exists.
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

const JSCFunctionListEntry SettingsProps[] = {
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
  return NewNamespace(ctx, SettingsProps,
                      static_cast<int>(std::size(SettingsProps)));
}

} // namespace gk::js
