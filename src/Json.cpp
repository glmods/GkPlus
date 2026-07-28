#include "Json.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <quickjs.h>

#include <cstdio>
#include <cstring>

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

} // namespace gk::json
