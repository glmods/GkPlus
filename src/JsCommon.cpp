#include "JsBindings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace gk::js {
namespace {

// Reads one float component off an object, leaving *out alone when absent.
bool ReadComponent(JSContext *ctx, JSValueConst obj, const char *name,
                   float *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  double d = 0.0;
  int rc = JS_ToFloat64(ctx, &d, v);
  JS_FreeValue(ctx, v);
  if (rc) {
    return false;
  }
  *out = static_cast<float>(d);
  return true;
}

// Fetches a property, reporting "absent" for undefined and null alike so an
// options object can omit fields. Returns false with an exception pending.
bool GetOptionalProp(JSContext *ctx, JSValueConst obj, const char *name,
                     JSValue *out, bool *present) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    JS_FreeValue(ctx, v);
    *present = false;
    return true;
  }
  *out = v;
  *present = true;
  return true;
}

// A decoded property key. `text` is owned when `ours` is set. `failed` means the
// decode itself threw, which the caller must report rather than treat as a miss.
struct Key {
  const char *text = nullptr;
  int id = 0;
  bool is_id = false;
  bool ours = false;
  bool failed = false;
};

// Strict decimal parse. strtoll on its own would accept " 5" and "+5", which
// would make actors[" 5"] silently mean actors[5].
bool ParseId(const char *s, int *out) {
  const char *p = s;
  if (*p == '-') {
    ++p;
  }
  if (!*p) {
    return false;
  }
  for (const char *q = p; *q; ++q) {
    if (*q < '0' || *q > '9') {
      return false;
    }
  }
  long long v = std::strtoll(s, nullptr, 10);
  if (v < INT32_MIN || v > INT32_MAX) {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

Key DecodeKey(JSContext *ctx, JSAtom prop) {
  Key key;

  // Symbols are never ours: Symbol.iterator lives on the collection itself and
  // must fall through to the ordinary lookup.
  JSValue as_value = JS_AtomToValue(ctx, prop);
  bool is_symbol = JS_IsSymbol(as_value);
  JS_FreeValue(ctx, as_value);
  if (is_symbol) {
    return key;
  }

  key.text = JS_AtomToCString(ctx, prop);
  if (!key.text) {
    key.failed = true; // out of memory, with an exception already pending
    return key;
  }
  key.ours = true;
  key.is_id = ParseId(key.text, &key.id);
  return key;
}

void FreeKey(JSContext *ctx, Key *key) {
  if (key->text) {
    JS_FreeCString(ctx, key->text);
    key->text = nullptr;
  }
}

const CollectionOps *OpsOf(JSValueConst obj) {
  JSClassID id = 0;
  return static_cast<const CollectionOps *>(JS_GetAnyOpaque(obj, &id));
}

// JS_UNDEFINED for a miss, JS_EXCEPTION on a real error.
JSValue CollectionLookup(JSContext *ctx, const CollectionOps *ops,
                         const Key &key) {
  // A wholly-decimal key is an id, so actors[12] and actors["12"] are the same
  // property - which is what JS property semantics require anyway. A collection
  // with no integer keys leaves lookup_id null and takes everything by name.
  if (key.is_id && ops->lookup_id) {
    return ops->lookup_id(ctx, key.id);
  }
  return ops->lookup_name(ctx, key.text);
}

// Looks a key up from its string form, for the iterator.
JSValue CollectionLookupKey(JSContext *ctx, const CollectionOps *ops,
                            const std::string &text) {
  Key key;
  key.text = text.c_str();
  key.ours = true;
  key.is_id = ParseId(text.c_str(), &key.id);
  return CollectionLookup(ctx, ops, key); // does not own key.text
}

int CollectionGetOwnProperty(JSContext *ctx, JSPropertyDescriptor *desc,
                             JSValueConst obj, JSAtom prop) {
  const CollectionOps *ops = OpsOf(obj);
  if (!ops) {
    return 0;
  }
  Key key = DecodeKey(ctx, prop);
  if (key.failed) {
    return -1;
  }
  if (!key.ours) {
    return 0;
  }
  JSValue v = CollectionLookup(ctx, ops, key);
  FreeKey(ctx, &key);

  if (JS_IsException(v)) {
    return -1;
  }
  if (JS_IsUndefined(v)) {
    return 0;
  }
  if (!desc) {
    JS_FreeValue(ctx, v);
    return 1;
  }
  // Configurable matters: these properties vanish when the game destroys the
  // actor, and a non-configurable property that later disappears breaks the
  // object invariants the engine asserts on.
  desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
  desc->value = v;
  desc->getter = JS_UNDEFINED;
  desc->setter = JS_UNDEFINED;
  return 1;
}

int CollectionGetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab,
                                  uint32_t *plen, JSValueConst obj) {
  const CollectionOps *ops = OpsOf(obj);
  std::vector<std::string> keys;
  if (ops) {
    ops->collect_keys(&keys);
  }

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
    JSAtom atom = JS_NewAtom(ctx, key.c_str());
    if (atom == JS_ATOM_NULL) {
      JS_FreePropertyEnum(ctx, tab, n);
      return -1;
    }
    // Ignored for exotic objects - quickjs re-queries get_own_property for the
    // real flags, which is why that one returns JS_PROP_ENUMERABLE.
    tab[n].is_enumerable = true;
    tab[n].atom = atom;
    ++n;
  }

  *ptab = tab;
  *plen = n;
  return 0;
}

// --- the properties every collection gets ------------------------------------

JSValue CollectionCount(JSContext *ctx, JSValueConst self) {
  const CollectionOps *ops = OpsOf(self);
  return JS_NewUint32(ctx, ops ? ops->count() : 0);
}

// Snapshots the keys, resolves each, and hands back the resulting array's own
// iterator - so `[...actors]` and `for (const a of actors)` cannot be
// invalidated by the game mutating the table mid-loop. Yields exactly what
// Object.values() would: wrappers for actors and roles, values for tokens.
JSValue CollectionIterator(JSContext *ctx, JSValueConst self, int,
                           JSValueConst *) {
  const CollectionOps *ops = OpsOf(self);
  std::vector<std::string> keys;
  if (ops) {
    ops->collect_keys(&keys);
  }

  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) {
    return arr;
  }
  uint32_t n = 0;
  for (const std::string &key : keys) {
    JSValue entry = CollectionLookupKey(ctx, ops, key);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
    if (JS_IsUndefined(entry)) {
      continue; // vanished between the snapshot and the lookup
    }
    if (JS_SetPropertyUint32(ctx, arr, n++, entry) < 0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }

  JSValue values = JS_GetPropertyStr(ctx, arr, "values");
  if (JS_IsException(values)) {
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
  }
  JSValue iter = JS_Call(ctx, values, arr, 0, nullptr);
  JS_FreeValue(ctx, values);
  JS_FreeValue(ctx, arr);
  return iter;
}

const JSCFunctionListEntry CollectionSharedProps[] = {
    JS_CGETSET_DEF("count", CollectionCount, nullptr),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, CollectionIterator),
};

// `val` is borrowed - JS_SetPropertyInternal2 frees it after we return
// (quickjs.c:10212).
//
// This throws rather than returning false. quickjs hands the exotic hook's
// result straight back to the caller (quickjs.c:10209-10213) instead of turning
// a false into the strict-mode TypeError the ordinary read-only path raises, so
// returning false would make `actors[1] = x` a silent no-op. Silently swallowing
// a write is the worst outcome for a modding API; naming the collection in a
// TypeError is the useful one.
int CollectionSetProperty(JSContext *ctx, JSValueConst obj, JSAtom atom,
                          JSValueConst value, JSValueConst, int) {
  const CollectionOps *ops = OpsOf(obj);
  if (!ops) {
    return false;
  }
  if (!ops->assign) {
    JS_ThrowTypeError(ctx, "%s is a read-only collection", ops->class_name);
    return -1;
  }
  Key key = DecodeKey(ctx, atom);
  if (key.failed) {
    return -1;
  }
  if (!key.ours) {
    JS_ThrowTypeError(ctx, "%s keys must be strings", ops->class_name);
    return -1;
  }
  int rc = ops->assign(ctx, key.text, value);
  FreeKey(ctx, &key);
  return rc;
}

// --- class registration ------------------------------------------------------

// Class ids are per-runtime; prototypes are per-context. This half is the
// runtime half, and is a no-op the second time round.
bool RegisterClass(JSContext *ctx, JSClassID *class_id, const JSClassDef *def) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  // A no-op once the id has been handed out, so this is safe to re-run for a
  // second context in the same runtime.
  JS_NewClassID(rt, class_id);
  return JS_IsRegisteredClass(rt, *class_id) ||
         JS_NewClass(rt, *class_id, def) >= 0;
}

// The per-context half. `proto_props` may be null for an empty prototype, and
// `parent_proto` may be anything non-object to mean "leave Object.prototype".
bool InstallProto(JSContext *ctx, JSClassID class_id,
                  const JSCFunctionListEntry *proto_props, int proto_len,
                  JSValueConst parent_proto) {
  JSValue proto = JS_NewObject(ctx);
  if (JS_IsException(proto)) {
    return false;
  }
  if (JS_IsObject(parent_proto) &&
      JS_SetPrototype(ctx, proto, parent_proto) < 0) {
    JS_FreeValue(ctx, proto);
    return false;
  }
  if (proto_props &&
      JS_SetPropertyFunctionList(ctx, proto, proto_props, proto_len) < 0) {
    JS_FreeValue(ctx, proto);
    return false;
  }
  JS_SetClassProto(ctx, class_id, proto); // consumes proto
  return true;
}

// JS_NewClass stores this by pointer, so it has to outlive every runtime.
const JSClassExoticMethods CollectionExotic = {
    CollectionGetOwnProperty,
    CollectionGetOwnPropertyNames,
    nullptr, // delete_property - nothing here supports removal
    nullptr, // define_own_property
    nullptr, // has_property    - emulated from get_own_property
    nullptr, // get_property
    CollectionSetProperty,
};

} // namespace

JSValue NewVec3(JSContext *ctx, const Vec3 &v) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
  JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, v.y));
  JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, v.z));
  return obj;
}

JSValue NewVec4(JSContext *ctx, const Vec4 &v) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
  JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, v.y));
  JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, v.z));
  JS_SetPropertyStr(ctx, obj, "w", JS_NewFloat64(ctx, v.w));
  return obj;
}

bool ToVec3(JSContext *ctx, JSValueConst v, Vec3 *out) {
  if (!JS_IsObject(v)) {
    JS_ThrowTypeError(ctx, "expected an object with x/y/z");
    return false;
  }
  return ReadComponent(ctx, v, "x", &out->x) &&
         ReadComponent(ctx, v, "y", &out->y) &&
         ReadComponent(ctx, v, "z", &out->z);
}

bool ToVec4(JSContext *ctx, JSValueConst v, Vec4 *out) {
  if (!JS_IsObject(v)) {
    JS_ThrowTypeError(ctx, "expected an object with x/y/z/w");
    return false;
  }
  return ReadComponent(ctx, v, "x", &out->x) &&
         ReadComponent(ctx, v, "y", &out->y) &&
         ReadComponent(ctx, v, "z", &out->z) &&
         ReadComponent(ctx, v, "w", &out->w);
}

bool GetInt32Prop(JSContext *ctx, JSValueConst obj, const char *name,
                  int32_t *out) {
  JSValue v = JS_UNDEFINED;
  bool present = false;
  if (!GetOptionalProp(ctx, obj, name, &v, &present)) {
    return false;
  }
  if (!present) {
    return true;
  }
  int rc = JS_ToInt32(ctx, out, v);
  JS_FreeValue(ctx, v);
  return rc == 0;
}

bool GetInt64Prop(JSContext *ctx, JSValueConst obj, const char *name,
                  int64_t *out) {
  JSValue v = JS_UNDEFINED;
  bool present = false;
  if (!GetOptionalProp(ctx, obj, name, &v, &present)) {
    return false;
  }
  if (!present) {
    return true;
  }
  // JS_ToInt64Ext accepts a Number or a BigInt. Radii and tick counts are small
  // integers, so demanding a BigInt for the 64-bit field would be hostile.
  int rc = JS_ToInt64Ext(ctx, out, v);
  JS_FreeValue(ctx, v);
  return rc == 0;
}

bool GetFloatProp(JSContext *ctx, JSValueConst obj, const char *name,
                  float *out) {
  JSValue v = JS_UNDEFINED;
  bool present = false;
  if (!GetOptionalProp(ctx, obj, name, &v, &present)) {
    return false;
  }
  if (!present) {
    return true;
  }
  double d = 0.0;
  int rc = JS_ToFloat64(ctx, &d, v);
  JS_FreeValue(ctx, v);
  if (rc) {
    return false;
  }
  *out = static_cast<float>(d);
  return true;
}

bool GetVec3ArrayProp(JSContext *ctx, JSValueConst obj, const char *name,
                      Vec3 *out, int count) {
  JSValue list = JS_UNDEFINED;
  bool present = false;
  if (!GetOptionalProp(ctx, obj, name, &list, &present)) {
    return false;
  }
  if (!present) {
    return true;
  }

  int64_t len = 0;
  if (JS_GetLength(ctx, list, &len) < 0) {
    JS_FreeValue(ctx, list);
    return false;
  }
  if (len > count) {
    len = count;
  }
  for (int64_t i = 0; i < len; ++i) {
    JSValue e = JS_GetPropertyUint32(ctx, list, static_cast<uint32_t>(i));
    if (JS_IsException(e)) {
      JS_FreeValue(ctx, list);
      return false;
    }
    if (JS_IsUndefined(e) || JS_IsNull(e)) {
      JS_FreeValue(ctx, e);
      continue;
    }
    bool ok = ToVec3(ctx, e, &out[i]);
    JS_FreeValue(ctx, e);
    if (!ok) {
      JS_FreeValue(ctx, list);
      return false;
    }
  }
  JS_FreeValue(ctx, list);
  return true;
}

JSValue NewNamespace(JSContext *ctx, const JSCFunctionListEntry *props,
                     int len) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  if (JS_SetPropertyFunctionList(ctx, obj, props, len) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

bool EnsureClass(JSContext *ctx, JSClassID *class_id, const JSClassDef *def,
                 const JSCFunctionListEntry *proto_props, int proto_len) {
  if (!RegisterClass(ctx, class_id, def)) {
    return false;
  }
  if (!proto_props) {
    return true; // no prototype at all
  }
  return InstallProto(ctx, *class_id, proto_props, proto_len, JS_UNDEFINED);
}

bool EnsureClass(JSContext *ctx, JSClassID *class_id, const JSClassDef *def,
                 const JSCFunctionListEntry *proto_props, int proto_len,
                 JSValueConst parent_proto) {
  if (!RegisterClass(ctx, class_id, def)) {
    return false;
  }
  return InstallProto(ctx, *class_id, proto_props, proto_len, parent_proto);
}

JSValue NewCollection(JSContext *ctx, JSClassID *class_id,
                      const CollectionOps *ops) {
  JSClassDef def = {};
  def.class_name = ops->class_name;
  def.exotic = const_cast<JSClassExoticMethods *>(&CollectionExotic);
  if (!EnsureClass(ctx, class_id, &def, nullptr, 0)) {
    return JS_EXCEPTION;
  }

  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(*class_id));
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetOpaque(obj, const_cast<CollectionOps *>(ops));
  // The shared properties first, so a namespace could deliberately override one.
  if (JS_SetPropertyFunctionList(ctx, obj, CollectionSharedProps,
                                 static_cast<int>(
                                     std::size(CollectionSharedProps))) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  if (ops->props &&
      JS_SetPropertyFunctionList(ctx, obj, ops->props, ops->props_len) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

} // namespace gk::js
