#include "Json.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <quickjs.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gk::json {
namespace {

// The codec is QuickJS, on a runtime of its own.
//
// Not the script host's: this runs on **both** game threads - the queue's
// producer hook and the four writer hooks are executor-side, the consumer is
// main-side - and a JSRuntime may only be used from one thread at a time. Its
// atom table and GC are runtime-wide, so borrowing the host's context here would
// race whatever a script was doing.
//
// A private runtime plus a lock instead. No modules, no scripts, no `gk` - it
// exists to call JS_ParseJSON and JS_JSONStringify, which is why nothing that
// happens in it can be observed by a script or vice versa. The operations are a
// few microseconds each and the two threads rarely queue at the same moment, so
// the lock is uncontended in practice.
//
// It is never freed. Freeing a JSRuntime at DLL detach, while another thread may
// still be inside it, is worse than leaking one for the process lifetime.
struct Codec {
  JSRuntime *runtime = nullptr;
  JSContext *context = nullptr;
  CRITICAL_SECTION lock{};

  Codec() {
    InitializeCriticalSection(&lock);
    runtime = JS_NewRuntime();
    if (runtime) {
      // The default is JS_DEFAULT_STACK_SIZE, a full megabyte - more than the
      // headroom either game thread has to spare by the time a payload is being
      // parsed. A quarter of that trips the guard while there is still stack
      // left to unwind through.
      JS_SetMaxStackSize(runtime, 256 * 1024);
      context = JS_NewContext(runtime);
    }
    if (!context) {
      DebugWrite("gkplus json: could not create the codec context");
    }
  }
};

Codec &TheCodec() {
  // A magic static, so the first caller wins whichever thread it is on.
  static Codec codec;
  return codec;
}

// Holds the codec lock for one operation. Every entry point below takes it; none
// of them nests, so the critical section's recursion is never exercised.
//
// **JS_UpdateStackTop is not optional here.** QuickJS's stack guard compares the
// current stack pointer against a base captured when the runtime was created, and
// this runtime is used from both game threads - so on whichever thread did not
// create it, the comparison is against an unrelated stack and the guard is
// meaningless. A deeply nested payload then recurses until the process dies,
// which is reachable from the network: update 0x67 carries a payload any peer can
// author. Re-basing per operation is what the header means by "should be called
// when changing thread"; it costs one register read.
struct Locked {
  Codec &codec;
  Locked() : codec(TheCodec()) {
    EnterCriticalSection(&codec.lock);
    if (codec.runtime) {
      JS_UpdateStackTop(codec.runtime);
    }
  }
  ~Locked() { LeaveCriticalSection(&codec.lock); }
  JSContext *ctx() const { return codec.context; }
};

// Drops a pending exception. The codec never reports one - a failed parse is
// Kind::Invalid and a failed encode is reported by its caller - but leaving one
// set would poison the next call on this context.
void ClearException(JSContext *ctx) {
  JSValue e = JS_GetException(ctx);
  JS_FreeValue(ctx, e);
}

// `value` as a JSON document. Frees `value`.
std::string StringifyAndFree(JSContext *ctx, JSValue value) {
  JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
  JS_FreeValue(ctx, value);
  if (JS_IsException(json)) {
    ClearException(ctx);
    JS_FreeValue(ctx, json);
    return {};
  }
  const char *text = JS_ToCString(ctx, json);
  JS_FreeValue(ctx, json);
  if (!text) {
    ClearException(ctx);
    return {};
  }
  std::string out = text;
  JS_FreeCString(ctx, text);
  return out;
}

// Which of the seven shapes a value already in hand has. `undefined` maps to
// Invalid, which is the right answer for both callers: for Classify nothing can
// parse to it, and for Document::KindAt a missing key and a value JSON cannot
// express are the same thing.
Kind KindOfValue(JSValueConst v) {
  if (JS_IsString(v)) {
    return Kind::String;
  }
  if (JS_IsNull(v)) {
    return Kind::Null;
  }
  if (JS_IsBool(v)) {
    return Kind::Bool;
  }
  if (JS_IsNumber(v)) {
    return Kind::Number;
  }
  if (JS_IsArray(v)) {
    return Kind::Array;
  }
  if (JS_IsObject(v)) {
    return Kind::Object;
  }
  return Kind::Invalid;
}

} // namespace

Kind Classify(const char *text, std::string *value) {
  if (value) {
    value->clear();
  }
  if (!text) {
    return Kind::Invalid;
  }
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx) {
    return Kind::Invalid;
  }

  // JS_ParseJSON is JSON.parse: strict, and it rejects trailing content
  // ("unexpected data at the end"), which is what keeps a file name like
  // `{a}.gcs` from reading as an object.
  JSValue parsed = JS_ParseJSON(ctx, text, std::strlen(text), "<payload>");
  if (JS_IsException(parsed)) {
    ClearException(ctx);
    JS_FreeValue(ctx, parsed);
    return Kind::Invalid;
  }

  Kind kind = KindOfValue(parsed);
  if (kind == Kind::String) {
    if (value) {
      const char *decoded = JS_ToCString(ctx, parsed);
      if (decoded) {
        *value = decoded;
        JS_FreeCString(ctx, decoded);
      } else {
        ClearException(ctx);
        kind = Kind::Invalid;
      }
    }
  }
  JS_FreeValue(ctx, parsed);
  return kind;
}

std::string Quote(const char *value) {
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx) {
    return "\"\"";
  }
  std::string out =
      StringifyAndFree(ctx, JS_NewString(ctx, value ? value : ""));
  // Callers rely on this always being a document - ScriptQueuePayload's whole
  // contract rests on it - so an allocation failure degrades to the empty string
  // rather than to something no parser accepts.
  return out.empty() ? std::string{"\"\""} : out;
}

std::string Envelope(const char *kind, const char *body_json) {
  // The last-resort value, for the two failures QuickJS can have here: no
  // context at all, and an allocation. Still an envelope, so a consumer sees a
  // kind it does not know rather than something no parser accepts.
  static const char *const Degraded = "{\"kind\":\"\",\"body\":null}";

  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx) {
    return Degraded;
  }

  // The body is parsed rather than pasted in as text. Concatenating would be
  // cheaper and would work for every caller here, but it would also let one bad
  // body escape as a document nobody can parse, and "always a document" is the
  // invariant the queue rests on.
  JSValue body = JS_NULL;
  if (body_json) {
    body = JS_ParseJSON(ctx, body_json, std::strlen(body_json), "<body>");
    if (JS_IsException(body)) {
      ClearException(ctx);
      JS_FreeValue(ctx, body);
      body = JS_NULL;
    }
  }

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    ClearException(ctx);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, body);
    return Degraded;
  }
  // Both setters consume the value they are given, body included.
  JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind ? kind : ""));
  JS_SetPropertyStr(ctx, obj, "body", body);

  std::string out = StringifyAndFree(ctx, obj);
  return out.empty() ? std::string{Degraded} : out;
}

bool OpenEnvelope(const char *text, std::string *kind, std::string *body_json) {
  if (kind) {
    kind->clear();
  }
  if (body_json) {
    body_json->clear();
  }
  if (!text) {
    return false;
  }
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx) {
    return false;
  }

  JSValue parsed = JS_ParseJSON(ctx, text, std::strlen(text), "<payload>");
  if (JS_IsException(parsed)) {
    ClearException(ctx);
    JS_FreeValue(ctx, parsed);
    return false;
  }
  // JS_IsObject is true for an array too, and an array is not one of ours - it
  // is a message body a peer might send at the top level by mistake, and it
  // should take the residual path rather than be opened.
  if (!JS_IsObject(parsed) || JS_IsArray(parsed)) {
    JS_FreeValue(ctx, parsed);
    return false;
  }

  JSValue kind_value = JS_GetPropertyStr(ctx, parsed, "kind");
  if (JS_IsException(kind_value)) {
    ClearException(ctx);
    JS_FreeValue(ctx, kind_value);
    JS_FreeValue(ctx, parsed);
    return false;
  }
  if (!JS_IsString(kind_value)) {
    JS_FreeValue(ctx, kind_value);
    JS_FreeValue(ctx, parsed);
    return false;
  }
  const char *kind_text = JS_ToCString(ctx, kind_value);
  JS_FreeValue(ctx, kind_value);
  if (!kind_text) {
    ClearException(ctx);
    JS_FreeValue(ctx, parsed);
    return false;
  }
  std::string kind_out = kind_text;
  JS_FreeCString(ctx, kind_text);

  JSValue body = JS_GetPropertyStr(ctx, parsed, "body");
  JS_FreeValue(ctx, parsed);
  if (JS_IsException(body)) {
    ClearException(ctx);
    JS_FreeValue(ctx, body);
    return false;
  }
  // A body must be *present*. JSON cannot express undefined, so this only fires
  // for an object that has no `body` at all - which is not an envelope.
  if (JS_IsUndefined(body)) {
    JS_FreeValue(ctx, body);
    return false;
  }

  // Consumes `body`. The only value this can refuse is one JSON.stringify
  // rejects, and nothing that came back out of JS_ParseJSON is such a value.
  std::string body_out = StringifyAndFree(ctx, body);
  if (body_out.empty()) {
    return false;
  }

  if (kind) {
    *kind = std::move(kind_out);
  }
  if (body_json) {
    *body_json = std::move(body_out);
  }
  return true;
}

// --- Document ----------------------------------------------------------------

namespace {

// The path split, done once per operation. Empty for an empty or malformed path,
// which every caller below treats as "no such value" rather than as the root -
// addressing the root by "" would make an accidental empty string replace the
// whole file.
std::vector<std::string> SplitPath(const char *path) {
  std::vector<std::string> steps;
  if (!path || !*path) {
    return steps;
  }
  const char *begin = path;
  for (const char *p = path;; ++p) {
    if (*p == '.' || *p == '\0') {
      if (p == begin) { // "a..b" or a leading/trailing dot
        return {};
      }
      steps.emplace_back(begin, static_cast<std::size_t>(p - begin));
      if (*p == '\0') {
        break;
      }
      begin = p + 1;
    }
  }
  return steps;
}

// **An own property, never an inherited one.** Every walk below goes through these
// two rather than JS_GetPropertyStr/JS_HasProperty, and it is not a refinement:
// the tree is made of ordinary JS objects, so every node inherits
// Object.prototype, and a step named `toString` or `constructor` would otherwise
// resolve to a function nobody put in the document. That made `KindAt("toString")`
// answer Object, `Set("constructor.x", ...)` write a property into Object itself,
// and `Remove("core.toString")` report a deletion it had not made. JSON has no
// prototypes, so an inherited key is never part of the document by definition.
//
// The value comes back **owned**; the walks free it immediately and keep
// borrowing, exactly as they did before.
JSValue GetOwnStr(JSContext *ctx, JSValueConst obj, const char *name) {
  if (!JS_IsObject(obj)) {
    return JS_UNDEFINED;
  }
  JSAtom atom = JS_NewAtom(ctx, name);
  if (atom == JS_ATOM_NULL) {
    ClearException(ctx);
    return JS_UNDEFINED;
  }
  JSPropertyDescriptor desc;
  const int found = JS_GetOwnProperty(ctx, &desc, obj, atom);
  JS_FreeAtom(ctx, atom);
  if (found <= 0) {
    if (found < 0) {
      ClearException(ctx);
    }
    return JS_UNDEFINED;
  }
  // A parsed JSON document holds nothing but plain data properties, so these two
  // are always undefined; freeing them is what keeps that an assumption rather
  // than a leak if it ever stops being true.
  JS_FreeValue(ctx, desc.getter);
  JS_FreeValue(ctx, desc.setter);
  return desc.value;
}

bool HasOwnStr(JSContext *ctx, JSValueConst obj, const char *name) {
  if (!JS_IsObject(obj)) {
    return false;
  }
  JSAtom atom = JS_NewAtom(ctx, name);
  if (atom == JS_ATOM_NULL) {
    ClearException(ctx);
    return false;
  }
  // A null descriptor asks only whether it is there (quickjs.c:9276).
  const int found = JS_GetOwnProperty(ctx, nullptr, obj, atom);
  JS_FreeAtom(ctx, atom);
  if (found < 0) {
    ClearException(ctx);
  }
  return found > 0;
}

// The container the last step lives in, or JS_UNDEFINED if any earlier step is
// missing or is not an object. Borrowed - the caller must not free it.
JSValue ParentOf(JSContext *ctx, JSValue root,
                 const std::vector<std::string> &steps) {
  JSValue node = root;
  for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
    if (!JS_IsObject(node) || JS_IsArray(node)) {
      return JS_UNDEFINED;
    }
    JSValue next = GetOwnStr(ctx, node, steps[i].c_str());
    // Borrowed, so the reference GetOwnStr took has to go back; the parent still
    // holds one, and the parent is alive for the whole walk.
    JS_FreeValue(ctx, next);
    node = next;
  }
  return JS_IsObject(node) && !JS_IsArray(node) ? node : JS_UNDEFINED;
}

// The value at `path` itself rather than its container, borrowed on the same
// terms as ParentOf. An **empty path is the root** here - the two callers are the
// ones that may address it (see Document::KindAt) - while a malformed one is
// JS_UNDEFINED, exactly like a missing key.
JSValue NodeAt(JSContext *ctx, JSValue root, const char *path) {
  if (!path || !*path) {
    return root;
  }
  const std::vector<std::string> steps = SplitPath(path);
  if (steps.empty()) {
    return JS_UNDEFINED;
  }
  JSValue parent = ParentOf(ctx, root, steps);
  if (JS_IsUndefined(parent)) {
    return JS_UNDEFINED;
  }
  JSValue leaf = GetOwnStr(ctx, parent, steps.back().c_str());
  // Borrowed: the reference GetOwnStr took goes back, and the parent holds one for
  // as long as the caller's lock is held.
  JS_FreeValue(ctx, leaf);
  return leaf;
}

} // namespace

Document::Document() : root_(nullptr) {
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx) {
    return;
  }
  root_ = new JSValue(JS_NewObject(ctx));
}

Document::~Document() {
  if (!root_) {
    return;
  }
  Locked locked;
  if (JSContext *ctx = locked.ctx()) {
    JS_FreeValue(ctx, *static_cast<JSValue *>(root_));
  }
  delete static_cast<JSValue *>(root_);
  root_ = nullptr;
}

bool Document::Parse(const char *text) {
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return false;
  }
  JSValue *root = static_cast<JSValue *>(root_);

  JSValue parsed = text ? JS_ParseJSON(ctx, text, std::strlen(text), "<document>")
                        : JS_EXCEPTION;
  if (JS_IsException(parsed)) {
    ClearException(ctx);
    JS_FreeValue(ctx, parsed);
    parsed = JS_UNDEFINED;
  }
  const bool ok = JS_IsObject(parsed) && !JS_IsArray(parsed);
  if (!ok) {
    JS_FreeValue(ctx, parsed);
    parsed = JS_NewObject(ctx);
  }
  JS_FreeValue(ctx, *root);
  *root = parsed;
  return ok;
}

std::string Document::Stringify(bool pretty) const {
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return "{}";
  }
  JSValue space = pretty ? JS_NewString(ctx, "  ") : JS_UNDEFINED;
  JSValue json = JS_JSONStringify(ctx, *static_cast<JSValue *>(root_),
                                  JS_UNDEFINED, space);
  // JS_JSONStringify takes its arguments as JSValueConst - it consumes none of
  // them, root included, which is what makes this a const operation at all.
  JS_FreeValue(ctx, space);
  if (JS_IsException(json)) {
    ClearException(ctx);
    JS_FreeValue(ctx, json);
    return "{}";
  }
  const char *text = JS_ToCString(ctx, json);
  JS_FreeValue(ctx, json);
  if (!text) {
    ClearException(ctx);
    return "{}";
  }
  std::string out = text;
  JS_FreeCString(ctx, text);
  return out;
}

std::string Document::Get(const char *path) const {
  const std::vector<std::string> steps = SplitPath(path);
  if (steps.empty()) {
    return {};
  }
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return {};
  }
  JSValue parent = ParentOf(ctx, *static_cast<JSValue *>(root_), steps);
  if (JS_IsUndefined(parent)) {
    return {};
  }
  JSValue leaf = GetOwnStr(ctx, parent, steps.back().c_str());
  // JSON cannot express undefined, so this is exactly "no such key".
  if (JS_IsUndefined(leaf)) {
    JS_FreeValue(ctx, leaf);
    return {};
  }
  return StringifyAndFree(ctx, leaf);
}

Kind Document::KindAt(const char *path) const {
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return Kind::Invalid;
  }
  return KindOfValue(NodeAt(ctx, *static_cast<JSValue *>(root_), path));
}

std::vector<std::string> Document::Keys(const char *path) const {
  std::vector<std::string> out;
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return out;
  }
  JSValue node = NodeAt(ctx, *static_cast<JSValue *>(root_), path);
  if (!JS_IsObject(node) || JS_IsArray(node)) {
    return out;
  }
  JSPropertyEnum *tab = nullptr;
  uint32_t len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, node,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    ClearException(ctx);
    return out;
  }
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    const char *key = JS_AtomToCString(ctx, tab[i].atom);
    if (key) {
      out.emplace_back(key);
      JS_FreeCString(ctx, key);
    } else {
      ClearException(ctx);
    }
  }
  JS_FreePropertyEnum(ctx, tab, len);
  return out;
}

bool Document::Set(const char *path, const char *json) {
  const std::vector<std::string> steps = SplitPath(path);
  if (steps.empty() || !json) {
    return false;
  }
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return false;
  }

  JSValue value = JS_ParseJSON(ctx, json, std::strlen(json), "<value>");
  if (JS_IsException(value)) {
    ClearException(ctx);
    JS_FreeValue(ctx, value);
    return false;
  }

  // The walk creates as it goes, so it cannot use ParentOf.
  JSValue node = *static_cast<JSValue *>(root_);
  for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
    JSValue next = GetOwnStr(ctx, node, steps[i].c_str());
    if (!JS_IsObject(next) || JS_IsArray(next)) {
      JS_FreeValue(ctx, next);
      next = JS_NewObject(ctx);
      if (JS_IsException(next)) {
        ClearException(ctx);
        JS_FreeValue(ctx, next);
        JS_FreeValue(ctx, value);
        return false;
      }
      // Consumes one reference; JS_DupValue keeps ours for the next round.
      JS_SetPropertyStr(ctx, node, steps[i].c_str(), JS_DupValue(ctx, next));
    }
    JS_FreeValue(ctx, next);
    node = next; // borrowed: its parent holds it, and the parent outlives the walk
  }

  return JS_SetPropertyStr(ctx, node, steps.back().c_str(), value) >= 0;
}

bool Document::Remove(const char *path) {
  const std::vector<std::string> steps = SplitPath(path);
  if (steps.empty()) {
    return false;
  }
  Locked locked;
  JSContext *ctx = locked.ctx();
  if (!ctx || !root_) {
    return false;
  }
  JSValue parent = ParentOf(ctx, *static_cast<JSValue *>(root_), steps);
  if (JS_IsUndefined(parent)) {
    return false;
  }
  if (!HasOwnStr(ctx, parent, steps.back().c_str())) {
    return false;
  }
  JSAtom key = JS_NewAtom(ctx, steps.back().c_str());
  const int deleted = JS_DeleteProperty(ctx, parent, key, 0);
  JS_FreeAtom(ctx, key);
  if (deleted < 0) {
    ClearException(ctx);
    return false;
  }
  return true;
}

} // namespace gk::json
