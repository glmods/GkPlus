#include "Triggers.h"

#include "JsBindings.h"
#include "Misc.h"
#include "ScriptQueue.h"

#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

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
// read out of RegisterTriggers @ 0x0043e240 and CreateTrigger @ 0x0044e8c0:
//
//   * The engine copies every string it is handed - strdup for the script name,
//     malloc+strcpy per actor name - so JS_ToCString buffers are correct and
//     pool_alloc'ing them would leak.
//   * CreateTrigger stores the `const char *` it is given without copying, which
//     is why it takes its address; the buffer only has to outlive the
//     RegisterTriggers call.
//   * RegisterTriggers CONSUMES the list: its last act on every path is
//     DeleteTriggers on the by-value copy, which frees the sentinel InitList
//     allocated. Calling DeleteList after it would double-free.
//   * It reads coords[0..3] with no null and no length check, so the array is
//     always four entries and always zero-filled first.
//
// Caveat with no fix in this layer: RegisterTriggers early-outs (still deleting
// the list) when the executor is not running, so calling this outside a live
// level silently registers nothing. Distinguishing that needs IsExecutorRunning
// @ 0x00502da0, which this change deliberately does not pull in.
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
  std::vector<const char *> target_names;
  TriggerList targets{};
  InitList(&targets);

  auto release = [&] {
    for (const char *name : target_names) {
      JS_FreeCString(ctx, name);
    }
  };

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
      release();
      return JS_EXCEPTION;
    }
    int64_t count = 0;
    if (!JS_IsUndefined(list) && !JS_IsNull(list) &&
        JS_GetLength(ctx, list, &count) < 0) {
      JS_FreeValue(ctx, list);
      DeleteList(&targets);
      release();
      return JS_EXCEPTION;
    }
    for (int64_t i = 0; i < count; ++i) {
      JSValue entry = JS_GetPropertyUint32(ctx, list, static_cast<uint32_t>(i));
      const char *name = JS_IsException(entry) ? nullptr : JS_ToCString(ctx, entry);
      JS_FreeValue(ctx, entry);
      if (!name) {
        JS_FreeValue(ctx, list);
        DeleteList(&targets);
        release();
        return JS_EXCEPTION;
      }
      target_names.push_back(name);
      // The address of the local, not of the vector element: CreateTrigger
      // dereferences immediately, but handing it the address of a reallocating
      // container reads as a bug.
      const char *slot = name;
      CreateTrigger(&targets, &slot);
    }
    JS_FreeValue(ctx, list);
  }

  {
    // ToScriptPayload has already encoded it, so the RegisterTriggers hook must
    // not quote it a second time.
    EncodedPayloadScope encoded;
    // Trigger registration appends to the lists EvaluateTriggers walks on the
    // executor thread, so it takes the pause the engine's own handlers take. See
    // gk::ExecutorPause in Misc.h.
    ExecutorPause pause;
    RegisterTriggers(static_cast<TriggerKind>(kind), coords, value, targets,
                     has_script ? reinterpret_cast<const unsigned char *>(
                                      script.c_str())
                                : nullptr,
                     team);
  }
  // No DeleteList here - RegisterTriggers already consumed `targets`.
  release();
  return JS_UNDEFINED;
}

const JSCFunctionListEntry TriggersProps[] = {
    JS_CFUNC_DEF2("create", 1, TriggersCreate,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_OBJECT_DEF("kind", TriggerKinds,
                  static_cast<int>(std::size(TriggerKinds)),
                  JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

} // namespace

JSValue NewTriggersNamespace(JSContext *ctx) {
  return NewNamespace(ctx, TriggersProps,
                      static_cast<int>(std::size(TriggersProps)));
}

} // namespace gk::js
