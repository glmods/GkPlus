#include "Json.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <quickjs.h>

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

  Kind kind = Kind::Invalid;
  if (JS_IsString(parsed)) {
    kind = Kind::String;
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
  } else if (JS_IsNull(parsed)) {
    kind = Kind::Null;
  } else if (JS_IsBool(parsed)) {
    kind = Kind::Bool;
  } else if (JS_IsNumber(parsed)) {
    kind = Kind::Number;
  } else if (JS_IsArray(parsed)) {
    kind = Kind::Array;
  } else if (JS_IsObject(parsed)) {
    kind = Kind::Object;
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

// The container the last step lives in, or JS_UNDEFINED if any earlier step is
// missing or is not an object. Borrowed - the caller must not free it.
JSValue ParentOf(JSContext *ctx, JSValue root,
                 const std::vector<std::string> &steps) {
  JSValue node = root;
  for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
    if (!JS_IsObject(node) || JS_IsArray(node)) {
      return JS_UNDEFINED;
    }
    JSValue next = JS_GetPropertyStr(ctx, node, steps[i].c_str());
    if (JS_IsException(next)) {
      ClearException(ctx);
      JS_FreeValue(ctx, next);
      return JS_UNDEFINED;
    }
    // Borrowed, so the reference JS_GetPropertyStr took has to go back; the
    // parent still holds one, and the parent is alive for the whole walk.
    JS_FreeValue(ctx, next);
    node = next;
  }
  return JS_IsObject(node) && !JS_IsArray(node) ? node : JS_UNDEFINED;
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
  JSValue leaf = JS_GetPropertyStr(ctx, parent, steps.back().c_str());
  if (JS_IsException(leaf)) {
    ClearException(ctx);
    JS_FreeValue(ctx, leaf);
    return {};
  }
  // JSON cannot express undefined, so this is exactly "no such key".
  if (JS_IsUndefined(leaf)) {
    JS_FreeValue(ctx, leaf);
    return {};
  }
  return StringifyAndFree(ctx, leaf);
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
    JSValue next = JS_GetPropertyStr(ctx, node, steps[i].c_str());
    if (JS_IsException(next)) {
      ClearException(ctx);
      JS_FreeValue(ctx, next);
      next = JS_UNDEFINED;
    }
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
  JSAtom key = JS_NewAtom(ctx, steps.back().c_str());
  const int had = JS_HasProperty(ctx, parent, key);
  const int deleted = had > 0 ? JS_DeleteProperty(ctx, parent, key, 0) : 0;
  JS_FreeAtom(ctx, key);
  if (had < 0 || deleted < 0) {
    ClearException(ctx);
    return false;
  }
  return had > 0;
}

} // namespace gk::json
