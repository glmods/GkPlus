#include "Camera.h"

#include "JsBindings.h"

#include <iterator>

namespace gk::js {
namespace {

JSValue GetPosition(JSContext *ctx, JSValueConst) {
  return NewVec3(ctx, GetCameraPosition());
}

JSValue SetPosition(JSContext *ctx, JSValueConst, JSValueConst v) {
  Vec3 pos = GetCameraPosition(); // omitted components keep their current value
  if (!ToVec3(ctx, v, &pos)) {
    return JS_EXCEPTION;
  }
  SetCameraPosition(pos);
  return JS_UNDEFINED;
}

JSValue GetDistance(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetCameraDistance());
}

JSValue SetDistance(JSContext *ctx, JSValueConst, JSValueConst v) {
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  SetCameraDistance(static_cast<float>(d));
  return JS_UNDEFINED;
}

JSValue GetMaxDistance(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetMaxCameraDistance());
}

JSValue SetMaxDistance(JSContext *ctx, JSValueConst, JSValueConst v) {
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  SetMaxCameraDistance(static_cast<float>(d));
  return JS_UNDEFINED;
}

// Live accessors over the three camera globals (Camera.h:6-7). Only ever handed
// to JS_SetPropertyFunctionList - see the note in JsBindings.h.
const JSCFunctionListEntry CameraProps[] = {
    JS_CGETSET_DEF2("position", GetPosition, SetPosition,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("distance", GetDistance, SetDistance,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("max_distance", GetMaxDistance, SetMaxDistance,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

} // namespace

JSValue NewCameraNamespace(JSContext *ctx) {
  return NewNamespace(ctx, CameraProps,
                      static_cast<int>(std::size(CameraProps)));
}

} // namespace gk::js
