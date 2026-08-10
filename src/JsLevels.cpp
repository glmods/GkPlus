#include "CustomLevel.h"

#include "Js.h"
#include "JsBindings.h"
#include "Map.h"
#include "Menu.h"
#include "Misc.h"
#include "Roles.h"
#include "ScriptQueue.h"
#include "Session.h"
#include "Tokens.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

JSClassID LevelClassId;
JSClassID LevelsClassId;

// Like the Menu wrapper, a Level wrapper points at a registration CustomLevel.cpp
// never frees, so it needs no finalizer and cannot dangle.
CustomLevel *LevelOf(JSContext *ctx, JSValueConst self) {
  return static_cast<CustomLevel *>(JS_GetOpaque2(ctx, self, LevelClassId));
}

JSValue NewLevelWrapper(JSContext *ctx, CustomLevel *level) {
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(LevelClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetOpaque(obj, level);
  return obj;
}

// --- script callbacks ----------------------------------------------------------

// One per registered level, kept for the process lifetime for the same reason
// MenuItemBinding is: the wrapper handed to the load callbacks must be the same
// object levels.add() returned, so a script can hang state off it.
struct LevelBinding {
  JSContext *ctx;
  JSValue define;
  JSValue populate;
  JSValue setup;
  JSValue message_received;
  JSValue wrapper;
};

// The script hooks: the three a load runs, in that order, and then the inbox,
// which fires during play instead. One table so adding a fifth cannot get it
// read from the module but never released - LevelsAdd, its error paths and
// ReleaseLevelCallbacks all iterate this.
constexpr JSValue LevelBinding::*LevelHooks[] = {
    &LevelBinding::define,
    &LevelBinding::populate,
    &LevelBinding::setup,
    &LevelBinding::message_received,
};
constexpr const char *LevelHookNames[] = {"define", "populate", "setup",
                                          "message_received"};

std::vector<LevelBinding *> Bindings;

void CallHook(CustomLevel *level, void *user, JSValue LevelBinding::*hook,
              const char *name) {
  auto *binding = static_cast<LevelBinding *>(user);
  if (!binding || JS_IsUndefined(binding->*hook)) {
    return;
  }
  JSValueConst args[] = {binding->wrapper};
  JSValue result =
      JS_Call(binding->ctx, binding->*hook, JS_UNDEFINED, 1, args);
  if (JS_IsException(result)) {
    char where[192];
    std::snprintf(where, sizeof(where), "%s() for level '%s'", name,
                  CustomLevelTitle(level));
    ReportException(binding->ctx, where);
  }
  JS_FreeValue(binding->ctx, result);
}

void OnDefine(CustomLevel *level, void *user) {
  CallHook(level, user, &LevelBinding::define, "define");
}

void OnPopulate(CustomLevel *level, void *user) {
  CallHook(level, user, &LevelBinding::populate, "populate");
}

void OnSetup(CustomLevel *level, void *user) {
  CallHook(level, user, &LevelBinding::setup, "setup");
}

// Not CallHook: this one takes the message, and it is the only hook that runs
// outside a load - so `levels.current` is null while it runs, and the level has
// to arrive as an argument or a module-level hook could not reach its own Level
// at all. It comes *second* so that the documented signature stays
// `message_received(msg)`.
void OnMessage(CustomLevel *level, const char *json, void *user) {
  auto *binding = static_cast<LevelBinding *>(user);
  if (!binding || JS_IsUndefined(binding->message_received)) {
    return;
  }
  JSContext *ctx = binding->ctx;
  char where[192];
  std::snprintf(where, sizeof(where), "message_received() for level '%s'",
                CustomLevelTitle(level));

  // The queue guaranteed this is a well-formed document, so a failure here means
  // something the native codec accepts and QuickJS does not - worth reporting
  // rather than swallowing.
  JSValue message =
      JS_ParseJSON(ctx, json, std::strlen(json), "<gkplus message>");
  if (JS_IsException(message)) {
    ReportException(ctx, where);
    return;
  }
  JSValueConst args[] = {message, binding->wrapper};
  JSValue result =
      JS_Call(ctx, binding->message_received, JS_UNDEFINED, 2, args);
  JS_FreeValue(ctx, message);
  if (JS_IsException(result)) {
    ReportException(ctx, where);
  }
  JS_FreeValue(ctx, result);
}

// --- the description object ------------------------------------------------------

// Leaves *out alone when the property is absent, undefined or null, so the
// CustomLevelMap initialiser stays the default. Empty strings are meaningful -
// they are what makes an optional map field resolve to `none`.
bool GetStringProp(JSContext *ctx, JSValueConst obj, const char *name,
                   std::string *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  const char *s = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  if (!s) {
    return false;
  }
  *out = s;
  JS_FreeCString(ctx, s);
  return true;
}

bool GetDoubleProp(JSContext *ctx, JSValueConst obj, const char *name,
                   double *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  double d = 0;
  int rc = JS_ToFloat64(ctx, &d, v);
  JS_FreeValue(ctx, v);
  if (rc < 0) {
    return false;
  }
  *out = d;
  return true;
}

bool ToLevelMap(JSContext *ctx, JSValueConst obj, CustomLevelMap *out) {
  if (!JS_IsObject(obj)) {
    JS_ThrowTypeError(ctx, "the map description must be an object");
    return false;
  }
  double max_distance = out->max_camera_distance;
  int32_t max_vertices = out->max_vertices_per_section;
  if (!GetStringProp(ctx, obj, "rif", &out->rif_file) ||
      !GetStringProp(ctx, obj, "object", &out->object_name) ||
      !GetStringProp(ctx, obj, "bitmap", &out->bitmap) ||
      !GetStringProp(ctx, obj, "camera_plane", &out->camera_plane) ||
      !GetDoubleProp(ctx, obj, "max_camera_distance", &max_distance) ||
      !GetStringProp(ctx, obj, "max_camera_focus_height",
                     &out->max_camera_focus_height) ||
      !GetStringProp(ctx, obj, "min_camera_focus_height",
                     &out->min_camera_focus_height) ||
      !GetStringProp(ctx, obj, "shadow_object_rif", &out->shadow_object_rif) ||
      !GetStringProp(ctx, obj, "shadow_object_name", &out->shadow_object_name) ||
      !GetInt32Prop(ctx, obj, "max_vertices_per_section", &max_vertices)) {
    return false;
  }
  out->max_camera_distance = max_distance;
  out->max_vertices_per_section = max_vertices;

  if (out->rif_file.empty() || out->object_name.empty()) {
    JS_ThrowTypeError(ctx, "the map description needs both 'rif' and 'object'");
    return false;
  }
  return true;
}

bool ToIncludeList(JSContext *ctx, JSValueConst obj,
                   std::vector<std::string> *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, "includes");
  if (JS_IsException(v)) {
    return false;
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  // A bare string is the one-include case, which is common enough to be worth
  // not making people type brackets for.
  if (JS_IsString(v)) {
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) {
      return false;
    }
    out->emplace_back(s);
    JS_FreeCString(ctx, s);
    return true;
  }

  uint32_t length = 0;
  JSValue len = JS_GetPropertyStr(ctx, v, "length");
  int rc = JS_IsException(len) ? -1 : JS_ToUint32(ctx, &length, len);
  JS_FreeValue(ctx, len);
  if (rc < 0) {
    JS_FreeValue(ctx, v);
    return false;
  }
  for (uint32_t i = 0; i < length; ++i) {
    JSValue entry = JS_GetPropertyUint32(ctx, v, i);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, v);
      return false;
    }
    const char *s = JS_ToCString(ctx, entry);
    JS_FreeValue(ctx, entry);
    if (!s) {
      JS_FreeValue(ctx, v);
      return false;
    }
    out->emplace_back(s);
    JS_FreeCString(ctx, s);
  }
  JS_FreeValue(ctx, v);
  return true;
}

// --- Level ------------------------------------------------------------------------

JSValue GetLevelTitle(JSContext *ctx, JSValueConst self) {
  CustomLevel *level = LevelOf(ctx, self);
  return level ? JS_NewString(ctx, CustomLevelTitle(level)) : JS_EXCEPTION;
}

JSValue GetLevelScriptFile(JSContext *ctx, JSValueConst self) {
  CustomLevel *level = LevelOf(ctx, self);
  return level ? JS_NewString(ctx, CustomLevelScriptFile(level)) : JS_EXCEPTION;
}

JSValue GetLevelMap(JSContext *ctx, JSValueConst self) {
  CustomLevel *level = LevelOf(ctx, self);
  if (!level) {
    return JS_EXCEPTION;
  }
  const CustomLevelMap &map = CustomLevelDescription(level);
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "rif", JS_NewString(ctx, map.rif_file.c_str()));
  JS_SetPropertyStr(ctx, obj, "object",
                    JS_NewString(ctx, map.object_name.c_str()));
  JS_SetPropertyStr(ctx, obj, "bitmap", JS_NewString(ctx, map.bitmap.c_str()));
  JS_SetPropertyStr(ctx, obj, "camera_plane",
                    JS_NewString(ctx, map.camera_plane.c_str()));
  JS_SetPropertyStr(ctx, obj, "max_camera_distance",
                    JS_NewFloat64(ctx, map.max_camera_distance));
  JS_SetPropertyStr(ctx, obj, "max_camera_focus_height",
                    JS_NewString(ctx, map.max_camera_focus_height.c_str()));
  JS_SetPropertyStr(ctx, obj, "min_camera_focus_height",
                    JS_NewString(ctx, map.min_camera_focus_height.c_str()));
  JS_SetPropertyStr(ctx, obj, "shadow_object_rif",
                    JS_NewString(ctx, map.shadow_object_rif.c_str()));
  JS_SetPropertyStr(ctx, obj, "shadow_object_name",
                    JS_NewString(ctx, map.shadow_object_name.c_str()));
  JS_SetPropertyStr(ctx, obj, "max_vertices_per_section",
                    JS_NewInt32(ctx, map.max_vertices_per_section));
  return obj;
}

// True while any of this level's load callbacks is running: define, populate or
// setup. That is also the window locators() and spawn() accept - though during
// define there is no map yet, which they check for separately.
JSValue GetLevelLoading(JSContext *ctx, JSValueConst self) {
  CustomLevel *level = LevelOf(ctx, self);
  if (!level) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, CurrentCustomLevel() == level);
}

JSValue NewLocatorObject(JSContext *ctx, const CustomLevelLocator &loc) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "position", NewVec3(ctx, loc.position));
  JS_SetPropertyStr(ctx, obj, "orientation", NewVec4(ctx, loc.orientation));
  return obj;
}

// The `for "<rif object>"` half of a .gls `use` clause: every object of that
// name in the level rif, already converted to world space.
JSValue LevelLocators(JSContext *ctx, JSValueConst self, int argc,
                      JSValueConst *argv) {
  CustomLevel *level = LevelOf(ctx, self);
  if (!level) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "locators(name) expects an object name");
  }
  if (CurrentCustomLevel() != level) {
    return JS_ThrowTypeError(
        ctx, "locators() only works while the level is loading");
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }
  std::vector<CustomLevelLocator> found = LevelRifLocators(name);
  JS_FreeCString(ctx, name);

  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) {
    return arr;
  }
  uint32_t n = 0;
  for (const CustomLevelLocator &loc : found) {
    JSValue entry = NewLocatorObject(ctx, loc);
    if (JS_IsException(entry) || JS_SetPropertyUint32(ctx, arr, n++, entry) < 0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }
  return arr;
}

// spawn(role, team, where[, options]) - `where` is a locator from locators(), or
// any {position, orientation} / {x, y, z} object. Returns the actor id, or -1.
JSValue LevelSpawn(JSContext *ctx, JSValueConst self, int argc,
                   JSValueConst *argv) {
  CustomLevel *level = LevelOf(ctx, self);
  if (!level) {
    return JS_EXCEPTION;
  }
  if (CurrentCustomLevel() != level) {
    return JS_ThrowTypeError(ctx,
                             "spawn() only works while the level is loading");
  }
  // Being the loading level is no longer enough on its own: define runs before
  // the map is converted, and setup runs even when the conversion failed (the
  // .gcs it stands in for would have run too). Both spawn factories reach into
  // TheMap, so without this a spawn from either would fault.
  if (!GetCurrentMap()) {
    return JS_ThrowTypeError(
        ctx, "spawn() needs the level geometry, which is not built yet");
  }
  if (argc < 3) {
    return JS_ThrowTypeError(ctx, "spawn(role, team, where[, options])");
  }

  const char *role_name = JS_ToCString(ctx, argv[0]);
  if (!role_name) {
    return JS_EXCEPTION;
  }
  Role *role = GetRoleByName(role_name);
  if (!role) {
    JSValue err = JS_ThrowTypeError(
        ctx, "no role named '%s' - is the .gsh that defines it in `includes`?",
        role_name);
    JS_FreeCString(ctx, role_name);
    return err;
  }
  JS_FreeCString(ctx, role_name);

  int32_t team = 0;
  if (JS_ToInt32(ctx, &team, argv[1]) < 0) {
    return JS_EXCEPTION;
  }

  Vec3 position{0, 0, 0};
  Vec4 orientation{0, 0, 0, 1};
  // A locator carries them in named sub-objects; anything else is read as the
  // position itself, so spawn(role, team, {x, y, z}) works too.
  JSValue nested = JS_GetPropertyStr(ctx, argv[2], "position");
  if (JS_IsException(nested)) {
    return JS_EXCEPTION;
  }
  bool is_locator = JS_IsObject(nested);
  if (is_locator) {
    bool ok = ToVec3(ctx, nested, &position);
    JS_FreeValue(ctx, nested);
    if (!ok) {
      return JS_EXCEPTION;
    }
    JSValue ori = JS_GetPropertyStr(ctx, argv[2], "orientation");
    if (JS_IsException(ori)) {
      return JS_EXCEPTION;
    }
    if (JS_IsObject(ori) && !ToVec4(ctx, ori, &orientation)) {
      JS_FreeValue(ctx, ori);
      return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ori);
  } else {
    JS_FreeValue(ctx, nested);
    if (!ToVec3(ctx, argv[2], &position)) {
      return JS_EXCEPTION;
    }
  }

  // Inserts into the actors hash while the executor walks it - see gk::ExecutorPause.
  int id;
  {
    ExecutorPause pause;
    id = MapSpawn(role, team, &position, &orientation);
  }

  // The `as "<token>"` clause: a token holding the new actor's id as a float,
  // which is how the engine names actors (see Tokens.h).
  if (argc > 3 && JS_IsObject(argv[3])) {
    std::string token;
    JSValue as = JS_GetPropertyStr(ctx, argv[3], "as");
    if (JS_IsException(as)) {
      return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(as) && !JS_IsNull(as)) {
      const char *s = JS_ToCString(ctx, as);
      if (s) {
        token = s;
        JS_FreeCString(ctx, s);
      }
    }
    JS_FreeValue(ctx, as);
    if (!token.empty() && id >= 0) {
      SetOrCreateToken(GetTokensTable(), token.c_str(),
                       static_cast<float>(id));
    }
  }
  return JS_NewInt32(ctx, id);
}

// send(message) - puts a payload on the engine's script queue, which is what a
// trigger firing does: this machine delivers it from the per-frame pop, every
// other player from update 0x67. A string sends the .gcs by name, so
// `send("wave2.gcs")` runs that file everywhere.
//
// Deliberately not level-scoped despite living here: the queue has one channel,
// so the payload arrives at whichever level is loaded when it is popped, which
// for anything sent during play is this one. It sits on Level because that is
// where message_received is, and because inside that hook the wrapper is the only
// handle a script has.
JSValue LevelSend(JSContext *ctx, JSValueConst self, int argc,
                  JSValueConst *argv) {
  if (!LevelOf(ctx, self)) {
    return JS_EXCEPTION;
  }
  if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
    return JS_ThrowTypeError(ctx, "send(message) expects a value");
  }
  // ToScriptPayload produces a complete envelope either way - kind "file" for a
  // string, kind "message" for anything else - so this queues it verbatim rather
  // than going through QueueScriptMessage, which would wrap a second time and
  // turn `send("wave2.gcs")` into a message whose body is an envelope.
  std::string payload;
  if (!ToScriptPayload(ctx, argv[0], &payload)) {
    return JS_EXCEPTION;
  }
  if (!QueueScriptPayload(payload.c_str())) {
    return JS_ThrowInternalError(ctx, "the script queue refused the message");
  }
  return JS_UNDEFINED;
}

JSValue LevelToString(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  CustomLevel *level = LevelOf(ctx, self);
  if (!level) {
    return JS_EXCEPTION;
  }
  char buf[192];
  std::snprintf(buf, sizeof(buf), "[Level '%s']", CustomLevelTitle(level));
  return JS_NewString(ctx, buf);
}

// Defined with the rest of the start machinery further down, because it shares
// ReadDifficulty and QueueStart with levels.start().
JSValue LevelStart(JSContext *ctx, JSValueConst self, int argc,
                   JSValueConst *argv);

const JSCFunctionListEntry LevelProto[] = {
    JS_CGETSET_DEF("title", GetLevelTitle, nullptr),
    JS_CGETSET_DEF("script_file", GetLevelScriptFile, nullptr),
    JS_CGETSET_DEF("map", GetLevelMap, nullptr),
    JS_CGETSET_DEF("loading", GetLevelLoading, nullptr),
    JS_CFUNC_DEF("locators", 1, LevelLocators),
    JS_CFUNC_DEF("spawn", 4, LevelSpawn),
    JS_CFUNC_DEF("send", 1, LevelSend),
    JS_CFUNC_DEF("start", 1, LevelStart),
    JS_CFUNC_DEF("toString", 0, LevelToString),
};

const JSClassDef LevelClass = {
    "Level", nullptr, nullptr, nullptr, nullptr,
};

// --- the collection ----------------------------------------------------------------

// Registered levels, in the order they were added - which is also the order they
// appear in Choose Level, because AddLevel appends to both lists at once.
std::vector<CustomLevel *> Registered;

JSValue LookupLevelById(JSContext *ctx, int id) {
  if (id < 0 || static_cast<size_t>(id) >= Registered.size()) {
    return JS_UNDEFINED;
  }
  return NewLevelWrapper(ctx, Registered[id]);
}

JSValue LookupLevelByName(JSContext *ctx, const char *name) {
  for (CustomLevel *level : Registered) {
    if (std::strcmp(CustomLevelTitle(level), name) == 0) {
      return NewLevelWrapper(ctx, level);
    }
  }
  return JS_UNDEFINED;
}

void CollectLevelKeys(std::vector<std::string> *out) {
  char buf[16];
  out->reserve(Registered.size());
  for (size_t i = 0; i < Registered.size(); ++i) {
    std::snprintf(buf, sizeof(buf), "%zu", i);
    out->emplace_back(buf);
  }
}

unsigned CountLevels() { return static_cast<unsigned>(Registered.size()); }

// Where the map fields and the hooks were found. They are the same object for a
// flat description and two different ones when the map is nested, so the rest of
// add() works off the pair rather than re-deciding.
struct Description {
  JSValue map_source = JS_UNDEFINED;     // owned
  JSValueConst container = JS_UNDEFINED; // borrowed: includes/define/... come
                                         // from here

  void reset(JSContext *ctx) {
    JS_FreeValue(ctx, map_source);
    map_source = JS_UNDEFINED;
    container = JS_UNDEFINED;
  }
};

// `source` is the description object, in either of the two shapes a level is
// naturally written in:
//
//   levels.add("Arena", { rif: "...", object: "Land", populate })  // flat
//   import * as arena from "./levels/arena.mjs";
//   levels.add("Arena", arena)                                     // nested map
//
// The second is what makes a module namespace a valid description with no host
// help: `export const map = {...}` lands as a `map` property beside the exported
// hooks. Telling them apart is unambiguous because no map field is itself called
// `map` - see CustomLevelMap.
bool ResolveDescription(JSContext *ctx, JSValueConst source, Description *out) {
  if (!JS_IsObject(source)) {
    JS_ThrowTypeError(ctx, "add(title, source) takes a description object - for "
                           "a level module, `import * as m` and pass m");
    return false;
  }
  JSValue nested = JS_GetPropertyStr(ctx, source, "map");
  if (JS_IsException(nested)) {
    return false;
  }
  if (JS_IsUndefined(nested) || JS_IsNull(nested)) {
    JS_FreeValue(ctx, nested);
    out->map_source = JS_DupValue(ctx, source); // flat
  } else if (JS_IsObject(nested)) {
    out->map_source = nested; // the ref from JS_GetPropertyStr moves here
  } else {
    // Not "no map, look at the object itself" - a `map` that is present but not
    // an object is a mistake, and saying so beats the "needs both 'rif' and
    // 'object'" the flat reading would otherwise produce.
    JS_FreeValue(ctx, nested);
    JS_ThrowTypeError(ctx, "`map` must be an object - a level module declares it "
                           "as `export const map = {...}`");
    return false;
  }
  out->container = source;
  return true;
}

// add(title, {...}) - the whole API. The object supplies the map fields, the
// .gls/.gsh `includes` the level's roles come from, and the define/populate/setup
// hooks; a level module's namespace is one such object.
JSValue LevelsAdd(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "add(title, source) expects a description object");
  }
  const char *title = JS_ToCString(ctx, argv[0]);
  if (!title) {
    return JS_EXCEPTION;
  }

  Description desc;
  if (!ResolveDescription(ctx, argv[1], &desc)) {
    JS_FreeCString(ctx, title);
    return JS_EXCEPTION;
  }

  CustomLevelMap map;
  std::vector<std::string> includes;
  auto *binding = new LevelBinding{ctx,          JS_UNDEFINED, JS_UNDEFINED,
                                   JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
  auto release_hooks = [&] {
    for (JSValue LevelBinding::*hook : LevelHooks) {
      JS_FreeValue(ctx, binding->*hook);
      binding->*hook = JS_UNDEFINED;
    }
  };
  auto fail = [&](JSValue err) {
    release_hooks();
    delete binding;
    desc.reset(ctx);
    JS_FreeCString(ctx, title);
    return err;
  };
  for (size_t i = 0; i < std::size(LevelHooks); ++i) {
    JSValue LevelBinding::*hook = LevelHooks[i];
    const char *name = LevelHookNames[i];
    binding->*hook = JS_GetPropertyStr(ctx, desc.container, name);
    if (JS_IsException(binding->*hook)) {
      binding->*hook = JS_UNDEFINED;
      return fail(JS_EXCEPTION);
    }
    if (JS_IsNull(binding->*hook)) {
      JS_FreeValue(ctx, binding->*hook);
      binding->*hook = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(binding->*hook) &&
        !JS_IsFunction(ctx, binding->*hook)) {
      return fail(JS_ThrowTypeError(ctx, "%s must be a function", name));
    }
  }
  if (!ToLevelMap(ctx, desc.map_source, &map) ||
      !ToIncludeList(ctx, desc.container, &includes)) {
    return fail(JS_EXCEPTION);
  }
  desc.reset(ctx);

  CustomLevel *level = AddCustomLevel(title, map, includes, OnDefine, OnPopulate,
                                      OnSetup, OnMessage, binding);
  if (!level) {
    // AddCustomLevel has already said why on the console.
    JSValue err = JS_ThrowTypeError(ctx, "could not register the level '%s'",
                                    title);
    release_hooks();
    delete binding;
    JS_FreeCString(ctx, title);
    return err;
  }
  JS_FreeCString(ctx, title);

  binding->wrapper = NewLevelWrapper(ctx, level);
  if (JS_IsException(binding->wrapper)) {
    // The level survives - it will still load, it just will not call back.
    release_hooks();
    binding->wrapper = JS_UNDEFINED;
    Bindings.push_back(binding);
    Registered.push_back(level);
    return JS_EXCEPTION;
  }
  Bindings.push_back(binding);
  Registered.push_back(level);
  return JS_DupValue(ctx, binding->wrapper);
}

// The level being loaded right now, or null outside a load callback. The same
// object add() returned.
JSValue GetCurrent(JSContext *ctx, JSValueConst) {
  CustomLevel *level = CurrentCustomLevel();
  if (!level) {
    return JS_NULL;
  }
  for (LevelBinding *binding : Bindings) {
    // A binding can carry an undefined wrapper: add() keeps the registration
    // when NewLevelWrapper fails, since the level still loads. JS_GetOpaque
    // reads a JSObject out of the value without checking it is one, so the
    // non-object has to be rejected here rather than by the class-id test.
    if (JS_IsObject(binding->wrapper) &&
        JS_GetOpaque(binding->wrapper, LevelClassId) == level) {
      return JS_DupValue(ctx, binding->wrapper);
    }
  }
  return NewLevelWrapper(ctx, level);
}

// --- starting a level ----------------------------------------------------------

// The game's LevelList @ 0x007b74dc holds every startable level: the 15-mission
// campaign EnterMainMenuScreen seeds, anything ADD MISSION added, and - because
// AddCustomLevel registers through the same AddLevel - every script-defined
// level too. So one lookup serves all three and `startable` is the honest
// inventory of what start() will accept.
bool FindListedLevel(const char *name, LevelStartRequest *out) {
  LevelList *list = GetLevelList();
  if (!list) {
    return false;
  }
  for (const LevelInfo &info : *list) {
    if (info.title && _stricmp(info.title.get(), name) == 0) {
      out->script = info.script ? info.script.get() : "";
      out->console = info.console ? info.console.get() : "";
      return true;
    }
  }
  return false;
}

// `difficulty` accepts the same names game.difficulty does, or the raw 0..3.
// Absent leaves the request's default (medium), which is what the New Game
// menu's first item produces.
bool ReadDifficulty(JSContext *ctx, JSValueConst options, int *out) {
  if (!JS_IsObject(options)) {
    return true;
  }
  JSValue v = JS_GetPropertyStr(ctx, options, "difficulty");
  if (JS_IsException(v)) {
    return false;
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  if (JS_IsNumber(v)) {
    int32_t d = 0;
    int rc = JS_ToInt32(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (rc < 0) {
      return false;
    }
    *out = d;
    return true;
  }
  const char *name = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  if (!name) {
    return false;
  }
  for (int i = static_cast<int>(Difficulty::Easy);
       i <= static_cast<int>(Difficulty::Extreme); ++i) {
    if (_stricmp(name, DifficultyName(i)) == 0) {
      JS_FreeCString(ctx, name);
      *out = i;
      return true;
    }
  }
  JS_ThrowRangeError(ctx, "difficulty must be easy, medium, hard or extreme");
  JS_FreeCString(ctx, name);
  return false;
}

// Turns start()'s first argument into a request. A Level wrapper and a title
// both resolve through LevelList; an object with `script` is the escape hatch
// for a .gls that was never registered.
bool ReadStartTarget(JSContext *ctx, JSValueConst target,
                     LevelStartRequest *out) {
  if (JS_IsString(target)) {
    const char *name = JS_ToCString(ctx, target);
    if (!name) {
      return false;
    }
    bool found = FindListedLevel(name, out);
    if (!found) {
      // Registered but not listed yet: listing waits for the first
      // EnterMainMenuScreen so the campaign gets the empty LevelList it
      // insists on, and a script can call start() before that has happened.
      if (CustomLevel *level = CustomLevelByTitle(name)) {
        out->script = CustomLevelScriptFile(level);
        out->console = "";
        found = true;
      }
    }
    if (!found) {
      JS_ThrowRangeError(ctx, "no level named '%s' - see levels.startable",
                         name);
    }
    JS_FreeCString(ctx, name);
    return found;
  }
  if (JS_IsObject(target)) {
    // A Level wrapper: start it by the virtual script name it registered under,
    // which is exactly what the Choose Level item would have set.
    if (auto *level = static_cast<CustomLevel *>(
            JS_GetOpaque(target, LevelClassId))) {
      out->script = CustomLevelScriptFile(level);
      out->console = "";
      return true;
    }
    JSValue script = JS_GetPropertyStr(ctx, target, "script");
    if (JS_IsException(script)) {
      return false;
    }
    if (!JS_IsUndefined(script)) {
      const char *text = JS_ToCString(ctx, script);
      JS_FreeValue(ctx, script);
      if (!text) {
        return false;
      }
      out->script = text;
      JS_FreeCString(ctx, text);
      JSValue console = JS_GetPropertyStr(ctx, target, "console");
      if (JS_IsException(console)) {
        return false;
      }
      if (!JS_IsUndefined(console) && !JS_IsNull(console)) {
        const char *ctext = JS_ToCString(ctx, console);
        JS_FreeValue(ctx, console);
        if (!ctext) {
          return false;
        }
        out->console = ctext;
        JS_FreeCString(ctx, ctext);
      } else {
        JS_FreeValue(ctx, console);
      }
      return true;
    }
    JS_FreeValue(ctx, script);
  }
  JS_ThrowTypeError(ctx, "start(level) takes a Level, a title, or {script, "
                         "console}");
  return false;
}

JSValue QueueStart(JSContext *ctx, const LevelStartRequest &request) {
  if (const char *refusal = LevelStartRefusal(request)) {
    return JS_ThrowTypeError(ctx, "%s", refusal);
  }
  return JS_NewBool(ctx, QueueLevelStart(request));
}

JSValue LevelsStart(JSContext *ctx, JSValueConst, int argc,
                    JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "start(level, options?)");
  }
  LevelStartRequest request;
  if (!ReadStartTarget(ctx, argv[0], &request) ||
      !ReadDifficulty(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                      &request.difficulty)) {
    return JS_EXCEPTION;
  }
  return QueueStart(ctx, request);
}

// The same thing spelled on a Level wrapper, so a script that just registered
// one can start it without naming it again.
JSValue LevelStart(JSContext *ctx, JSValueConst self, int argc,
                   JSValueConst *argv) {
  CustomLevel *level = LevelOf(ctx, self);
  if (!level) {
    return JS_EXCEPTION;
  }
  LevelStartRequest request;
  request.script = CustomLevelScriptFile(level);
  if (!ReadDifficulty(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                      &request.difficulty)) {
    return JS_EXCEPTION;
  }
  return QueueStart(ctx, request);
}

JSValue LevelsQuit(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_NewBool(ctx, QueueReturnToMainMenu());
}

JSValue GetPending(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, LevelStartPending());
}

// Everything start() will take by name, in the order the Choose Level menu
// shows it - the game keeps item n aligned with list entry n.
JSValue GetStartable(JSContext *ctx, JSValueConst) {
  JSValue out = JS_NewArray(ctx);
  if (JS_IsException(out)) {
    return out;
  }
  LevelList *list = GetLevelList();
  if (!list) {
    return out;
  }
  uint32_t i = 0;
  for (const LevelInfo &info : *list) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, out);
      return JS_EXCEPTION;
    }
    JS_SetPropertyStr(ctx, entry, "index", JS_NewInt32(ctx, i));
    JS_SetPropertyStr(ctx, entry, "title",
                      JS_NewString(ctx, info.title ? info.title.get() : ""));
    JS_SetPropertyStr(ctx, entry, "script",
                      JS_NewString(ctx, info.script ? info.script.get() : ""));
    JS_SetPropertyStr(ctx, entry, "console",
                      JS_NewString(ctx, info.console ? info.console.get() : ""));
    JS_SetPropertyUint32(ctx, out, i, entry);
    ++i;
  }
  return out;
}

const JSCFunctionListEntry LevelsProps[] = {
    JS_CFUNC_DEF("add", 2, LevelsAdd),
    JS_CGETSET_DEF("current", GetCurrent, nullptr),
    JS_CFUNC_DEF("start", 2, LevelsStart),
    JS_CFUNC_DEF("quit", 0, LevelsQuit),
    JS_CGETSET_DEF("startable", GetStartable, nullptr),
    JS_CGETSET_DEF("start_pending", GetPending, nullptr),
};

const CollectionOps LevelsOps = {
    .class_name = "Levels",
    .lookup_id = LookupLevelById,
    .lookup_name = LookupLevelByName,
    .collect_keys = CollectLevelKeys,
    .count = CountLevels,
    .assign = nullptr, // levels are added through add(), never by assignment
    .props = LevelsProps,
    .props_len = static_cast<int>(std::size(LevelsProps)),
};

} // namespace

void ReleaseLevelCallbacks(JSContext *ctx) {
  ClearCustomLevelActions();
  for (auto it = Bindings.begin(); it != Bindings.end();) {
    LevelBinding *binding = *it;
    if (binding->ctx != ctx) {
      ++it;
      continue;
    }
    for (JSValue LevelBinding::*hook : LevelHooks) {
      JS_FreeValue(ctx, binding->*hook);
    }
    JS_FreeValue(ctx, binding->wrapper);
    delete binding;
    it = Bindings.erase(it);
  }
}

JSValue NewLevelsNamespace(JSContext *ctx) {
  if (!EnsureClass(ctx, &LevelClassId, &LevelClass, LevelProto,
                   static_cast<int>(std::size(LevelProto)))) {
    return JS_EXCEPTION;
  }
  return NewCollection(ctx, &LevelsClassId, &LevelsOps);
}

} // namespace gk::js
