#include "Triggers.h"

#include "JsBindings.h"
#include "Misc.h"
#include "ScriptQueue.h"

#include <cstdio>
#include <deque>
#include <iterator>
#include <string>

namespace gk::js {
namespace {

JSClassID TriggerClassId;

// A handle on one registered trigger. `ptr` is the engine's pool_alloc'd
// TriggerData, and it is only ever dereferenced after TriggerIsRegistered has
// confirmed it is still linked - EvaluateTriggers destroys a fired trigger
// itself and DeleteAllTriggers takes the lot at level teardown, both with no
// notification, so this is the Actor wrapper's re-derive rule applied to a
// structure that has no ids to re-derive from.
//
// `kind` and `script_name` are the recycle heuristic, not a proof. The pool
// recycles pages, so a later trigger can land on this exact address; a snapshot
// of the immutable kind plus the freshly-strdup'd script pointer makes that
// collision detectable in practice without pretending to be an identity. Stated
// rather than hidden: two triggers of one kind with no script, allocated at the
// same address, are indistinguishable here.
struct TriggerWrapper {
  TriggerData *ptr;
  TriggerKind kind;
  const void *script_name;
};

void TriggerFinalizer(JSRuntime *rt, JSValueConst val) {
  js_free_rt(rt, JS_GetOpaque(val, TriggerClassId));
}

const JSClassDef TriggerClass = {
    "Trigger", TriggerFinalizer, nullptr, nullptr, nullptr,
};

TriggerWrapper *WrapperOf(JSContext *ctx, JSValueConst self) {
  return static_cast<TriggerWrapper *>(
      JS_GetOpaque2(ctx, self, TriggerClassId));
}

// True when the handle still names the trigger it was made from. Takes the pause
// itself because the walk crosses a list the executor relinks.
bool StillLive(TriggerWrapper *w) {
  if (!w || !w->ptr) {
    return false;
  }
  ExecutorPause pause;
  if (!TriggerIsRegistered(w->ptr)) {
    return false;
  }
  return w->ptr->kind == w->kind && w->ptr->script_name.get() == w->script_name;
}

JSValue GetTriggerValid(JSContext *ctx, JSValueConst self) {
  TriggerWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, StillLive(w));
}

JSValue GetTriggerKind(JSContext *ctx, JSValueConst self) {
  TriggerWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  // From the snapshot, not the struct: the kind is immutable for a trigger's
  // lifetime, and reading it back is the one thing that stays answerable after
  // the engine has destroyed it.
  return JS_NewInt32(ctx, static_cast<int>(w->kind));
}

// Unregisters and destroys the trigger. Returns false when the handle is already
// stale, which is the normal outcome for a trigger that has fired - not an
// error, because nothing tells a script when that happened.
JSValue TriggerRemove(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  TriggerWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  if (!StillLive(w)) {
    // Clear it anyway: whatever is at that address now is not ours.
    w->ptr = nullptr;
    return JS_FALSE;
  }
  {
    ExecutorPause pause;
    // Re-tested inside this pause rather than trusting StillLive's: that one
    // released the pause before returning, and the executor could have fired
    // the trigger in between. RemoveTriggerFromGlobalList decrements
    // NumTriggers even when it finds nothing, so a lost race would corrupt the
    // count and double-free.
    if (TriggerIsRegistered(w->ptr)) {
      RemoveTriggerFromGlobalList(w->ptr, 1); // 1 = also free the 0x68 record
    }
  }
  w->ptr = nullptr;
  return JS_TRUE;
}

JSValue TriggerToString(JSContext *ctx, JSValueConst self, int,
                        JSValueConst *) {
  TriggerWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "[Trigger kind=%d%s]", static_cast<int>(w->kind),
                StillLive(w) ? "" : " removed");
  return JS_NewString(ctx, buf);
}

const JSCFunctionListEntry TriggerProto[] = {
    JS_CGETSET_DEF("valid", GetTriggerValid, nullptr),
    JS_CGETSET_DEF("kind", GetTriggerKind, nullptr),
    JS_CFUNC_DEF("remove", 0, TriggerRemove),
    JS_CFUNC_DEF("toString", 0, TriggerToString),
};

JSValue NewTriggerWrapper(JSContext *ctx, TriggerData *trigger) {
  if (!TriggerClassId) {
    return JS_ThrowInternalError(ctx, "the Trigger class is not registered");
  }
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(TriggerClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  auto *w =
      static_cast<TriggerWrapper *>(js_malloc(ctx, sizeof(TriggerWrapper)));
  if (!w) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  w->ptr = trigger;
  w->kind = trigger->kind;
  w->script_name = trigger->script_name.get();
  JS_SetOpaque(obj, w);
  return obj;
}

const JSCFunctionListEntry TriggerKinds[] = {
    JS_PROP_INT32_DEF("death", static_cast<int>(TriggerKind::Death),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("location", static_cast<int>(TriggerKind::Location),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("location_specified",
                      static_cast<int>(TriggerKind::LocationSpecified),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("location_all",
                      static_cast<int>(TriggerKind::LocationAll),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("location_timed",
                      static_cast<int>(TriggerKind::LocationTimed),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("instant_death",
                      static_cast<int>(TriggerKind::InstantDeath),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("instant_displace",
                      static_cast<int>(TriggerKind::InstantDisplace),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("time", static_cast<int>(TriggerKind::Time),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("escort", static_cast<int>(TriggerKind::Escort),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("proximity", static_cast<int>(TriggerKind::Proximity),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("door", static_cast<int>(TriggerKind::Door),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("door_once", static_cast<int>(TriggerKind::DoorOnce),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("doors_either",
                      static_cast<int>(TriggerKind::DoorsEither),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("four_doors", static_cast<int>(TriggerKind::FourDoors),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("light_up", static_cast<int>(TriggerKind::LightUp),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("defog", static_cast<int>(TriggerKind::Defog),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("shot", static_cast<int>(TriggerKind::Shot),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("being_attacked",
                      static_cast<int>(TriggerKind::BeingAttacked),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("frag_score", static_cast<int>(TriggerKind::FragScore),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("time_limit", static_cast<int>(TriggerKind::TimeLimit),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("time_if_alive",
                      static_cast<int>(TriggerKind::TimeIfAlive),
                      JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("been_alerted",
                      static_cast<int>(TriggerKind::BeenAlerted),
                      JS_PROP_ENUMERABLE),
};

// Registers a trigger. Every ownership decision below is load-bearing and was
// read out of AddTriggerToGlobalList @ 0x0043e240 and CreateTrigger
// @ 0x0044e8c0:
//
//   * The engine copies every string it is handed - strdup for the script name,
//     malloc+strcpy per actor name - so JS_ToCString buffers are correct and
//     pool_alloc'ing them would leak.
//   * CreateTrigger stores the `const char *` it is given without copying, which
//     is why it takes its address; the buffer only has to outlive the
//     AddTriggerToGlobalList call.
//   * AddTriggerToGlobalList CONSUMES the list: its last act on every path is
//     DeleteTriggers on the by-value copy, which frees the sentinel InitList
//     allocated. Calling DeleteList after it would double-free.
//   * It reads coords[0..3] with no null and no length check, so the array is
//     always four entries and always zero-filled first.
//
// AddTriggerToGlobalList early-outs (still deleting the list) when the executor
// is not running, so calling this outside a live level registers nothing. That
// used to be a silent no-op; it now throws, because the handle this returns has
// to name something and there is no honest handle for a trigger that was never
// created.
JSValue TriggersCreate(JSContext *ctx, JSValueConst, int argc,
                       JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "create(options) expects an object");
  }
  JSValueConst opts = argv[0];

  int32_t kind = 0;
  if (!GetInt32Prop(ctx, opts, "kind", &kind)) {
    return JS_EXCEPTION;
  }
  if (kind < 0 || kind > static_cast<int>(TriggerKind::BeenAlerted)) {
    return JS_ThrowRangeError(ctx, "unknown trigger kind %d", kind);
  }

  Vec3 coords[4] = {};
  if (!GetVec3ArrayProp(ctx, opts, "coords", coords, 4)) {
    return JS_EXCEPTION;
  }

  // Overloaded per kind: a radius for the location/proximity kinds, a tick
  // deadline for the time ones. JS_ToInt64Ext takes a Number or a BigInt.
  int64_t value = 0;
  if (!GetInt64Prop(ctx, opts, "value", &value)) {
    return JS_EXCEPTION;
  }

  int32_t team = 0;
  if (!GetInt32Prop(ctx, opts, "team", &team)) {
    return JS_EXCEPTION;
  }

  std::string script;
  bool has_script = false;
  // A deque, not a vector: CreateTrigger stores the `const char *` it is handed
  // WITHOUT copying, and only AddTriggerToGlobalList strdups it - so every name
  // pushed here has to keep its address until that call. A vector<string> would
  // move its elements on reallocation and invalidate the pointers already in the
  // target list, and for a short (SSO) name the character buffer moves with the
  // object, so it is not just the object address that shifts. Deque guarantees
  // stable element references across push_back.
  std::deque<std::string> target_names;
  TriggerList targets{};
  InitList(&targets);


  // A string is a .gcs name, as it always was; an object is a message that
  // reaches the level's message_received when the trigger fires, on every
  // machine. ToScriptPayload encodes the second and leaves the first alone - see
  // ScriptQueue.h for what the engine then does with each.
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "script");
    if (JS_IsException(v)) {
      DeleteList(&targets);
      return JS_EXCEPTION;
    }
    has_script = !JS_IsUndefined(v) && !JS_IsNull(v);
    bool ok = !has_script || ToScriptPayload(ctx, v, &script);
    JS_FreeValue(ctx, v);
    if (!ok) {
      DeleteList(&targets);
      return JS_EXCEPTION;
    }
  }

  {
    JSValue list = JS_GetPropertyStr(ctx, opts, "targets");
    if (JS_IsException(list)) {
      DeleteList(&targets);
      return JS_EXCEPTION;
    }
    int64_t count = 0;
    if (!JS_IsUndefined(list) && !JS_IsNull(list) &&
        JS_GetLength(ctx, list, &count) < 0) {
      JS_FreeValue(ctx, list);
      DeleteList(&targets);
      return JS_EXCEPTION;
    }
    for (int64_t i = 0; i < count; ++i) {
      JSValue entry = JS_GetPropertyUint32(ctx, list, static_cast<uint32_t>(i));
      if (JS_IsException(entry)) {
        JS_FreeValue(ctx, list);
        DeleteList(&targets);
        return JS_EXCEPTION;
      }
      // Actors, not names. The engine's target list holds `char *` copies that
      // EvaluateTriggers matches by string, so a name is what actually goes in -
      // but that is this layer's business, not the script's, and it is why an
      // actor with no token is refused here rather than silently dropped.
      Actor *actor = ActorFromValue(ctx, entry);
      std::string name;
      bool ok = actor != nullptr && ActorTokenName(ctx, actor, &name);
      JS_FreeValue(ctx, entry);
      if (!ok) {
        JS_FreeValue(ctx, list);
        DeleteList(&targets);
        return JS_EXCEPTION;
      }
      // Owned by us until AddTriggerToGlobalList has copied it, which it does by
      // strdup - so this has to stay put until the call below, and the deque is
      // what guarantees that.
      target_names.push_back(name);
      // CreateTrigger takes the address OF the pointer and stores the pointer
      // itself, so `slot` only has to outlive this call while the string it
      // points at has to outlive the whole function.
      const char *slot = target_names.back().c_str();
      CreateTrigger(&targets, &slot);
    }
    JS_FreeValue(ctx, list);
  }

  TriggerData *created = nullptr;
  {
    // ToScriptPayload has already encoded it, so the AddTriggerToGlobalList
    // hook must not quote it a second time.
    EncodedPayloadScope encoded;
    // Trigger registration appends to the lists EvaluateTriggers walks on the
    // executor thread, so it takes the pause the engine's own handlers take. See
    // gk::ExecutorPause in Misc.h.
    ExecutorPause pause;
    AddTriggerToGlobalList(static_cast<TriggerKind>(kind), coords, value,
                           targets,
                           has_script ? reinterpret_cast<const unsigned char *>(
                                            script.c_str())
                                      : nullptr,
                           team);
    // Inside the same pause: the insert is at the tail, and reading the tail
    // after resuming would race an executor that had already fired and removed
    // it. AddTriggerToGlobalList returns void, so the tail is the only route to
    // what it just made.
    created = LastRegisteredTrigger();
  }
  // No DeleteList here - AddTriggerToGlobalList already consumed `targets`.
  if (!created) {
    return JS_ThrowInternalError(
        ctx, "no trigger was registered - AddTriggerToGlobalList early-outs "
             "when the executor is not running, so this needs a live level");
  }
  return NewTriggerWrapper(ctx, created);
}

// The engine's own `REMOVE TRIGGER`, and its reach is far narrower than the
// command name suggests: CommandRemoveTrigger @ 0x00444500 parses one word,
// requires it to be exactly "SHOT" (`__mbsicmp`, so anything else does nothing
// at all), then matches a TRIGGER_SHOT trigger by comparing the parsed position
// against `coords[1]` and `coords[2]`. So it removes shot triggers by position
// and nothing else.
//
// This exists for triggers GkPlus did not create - a level's own `.gcs` shot
// triggers. Anything registered through `create` has a handle, and
// `trigger.remove()` is both exact and cheaper.
JSValue TriggersRemoveShot(JSContext *ctx, JSValueConst, int argc,
                           JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(
        ctx, "remove_shot(position) expects an {x, y, z} object");
  }
  // Validated here rather than left to the formatter: ConsoleParsePosition also
  // accepts a *name* out of MapAuxObjectList, so a stray string would reach the
  // handler and silently match some map marker instead of failing.
  Vec3 position{};
  if (!ToVec3(ctx, argv[0], &position)) {
    return JS_EXCEPTION;
  }
  JSValue args[] = {JS_NewString(ctx, "SHOT"), argv[0]};
  if (JS_IsException(args[0])) {
    return JS_EXCEPTION;
  }
  JSValue result = RunConsoleCommand(ctx, "REMOVE TRIGGER", 2, args);
  JS_FreeValue(ctx, args[0]);
  return result;
}

const JSCFunctionListEntry TriggersProps[] = {
    JS_CFUNC_DEF2("create", 1, TriggersCreate,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("remove_shot", 1, TriggersRemoveShot,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_OBJECT_DEF("kind", TriggerKinds,
                  static_cast<int>(std::size(TriggerKinds)),
                  JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

} // namespace

JSValue NewTriggersNamespace(JSContext *ctx) {
  if (!EnsureClass(ctx, &TriggerClassId, &TriggerClass, TriggerProto,
                   static_cast<int>(std::size(TriggerProto)))) {
    return JS_EXCEPTION;
  }
  return NewNamespace(ctx, TriggersProps,
                      static_cast<int>(std::size(TriggersProps)));
}

} // namespace gk::js
