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

// The three Euler angles, in degrees. Each setter is the engine's own, so it
// rebuilds that axis' quaternion; the matrix rebuild is deferred to whichever
// accessor ran last, which is why each one ends with UpdateCameraMatrix.
JSValue GetYaw(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetCameraYaw());
}
JSValue GetRoll(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetCameraRoll());
}
JSValue GetPitch(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetCameraPitch());
}

using AngleSetterFn = void (*)(float);

JSValue SetAngle(JSContext *ctx, JSValueConst v, AngleSetterFn set) {
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  set(static_cast<float>(d));
  UpdateCameraMatrix();
  return JS_UNDEFINED;
}

JSValue SetYaw(JSContext *ctx, JSValueConst, JSValueConst v) {
  return SetAngle(ctx, v, SetCameraYaw);
}
JSValue SetRoll(JSContext *ctx, JSValueConst, JSValueConst v) {
  return SetAngle(ctx, v, SetCameraRoll);
}
JSValue SetPitch(JSContext *ctx, JSValueConst, JSValueConst v) {
  return SetAngle(ctx, v, SetCameraPitch);
}

// `SET CAMERA ORI`: all three at once, with a single matrix rebuild. Reads back
// as a plain object so `camera.orientation` round-trips through the setter.
JSValue GetOrientation(JSContext *ctx, JSValueConst) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "yaw", JS_NewFloat64(ctx, GetCameraYaw()));
  JS_SetPropertyStr(ctx, obj, "roll", JS_NewFloat64(ctx, GetCameraRoll()));
  JS_SetPropertyStr(ctx, obj, "pitch", JS_NewFloat64(ctx, GetCameraPitch()));
  return obj;
}

JSValue SetOrientation(JSContext *ctx, JSValueConst, JSValueConst v) {
  if (!JS_IsObject(v)) {
    return JS_ThrowTypeError(ctx, "camera.orientation takes an object");
  }
  // Omitted components keep their current value, matching camera.position.
  float yaw = GetCameraYaw();
  float roll = GetCameraRoll();
  float pitch = GetCameraPitch();
  struct Field {
    const char *name;
    float *out;
  } fields[] = {{"yaw", &yaw}, {"roll", &roll}, {"pitch", &pitch}};
  for (const Field &f : fields) {
    JSValue prop = JS_GetPropertyStr(ctx, v, f.name);
    if (JS_IsException(prop)) {
      return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(prop)) {
      double d = 0.0;
      int failed = JS_ToFloat64(ctx, &d, prop);
      JS_FreeValue(ctx, prop);
      if (failed) {
        return JS_EXCEPTION;
      }
      *f.out = static_cast<float>(d);
    } else {
      JS_FreeValue(ctx, prop);
    }
  }
  SetCameraOrientation(yaw, roll, pitch);
  return JS_UNDEFINED;
}

// `SET CAMERA FOCUS` / `FREE CAMERA FOCUS`. Reading gives null when no focus is
// latched, because the stale coordinates behind it mean nothing then.
JSValue GetFocus(JSContext *ctx, JSValueConst) {
  if (!IsCameraFocusSet()) {
    return JS_NULL;
  }
  return NewVec3(ctx, GetCameraFocus());
}

JSValue SetFocus(JSContext *ctx, JSValueConst, JSValueConst v) {
  if (JS_IsNull(v) || JS_IsUndefined(v)) {
    ClearCameraFocus();
    return JS_UNDEFINED;
  }
  Vec3 focus = IsCameraFocusSet() ? GetCameraFocus() : Vec3{};
  if (!ToVec3(ctx, v, &focus)) {
    return JS_EXCEPTION;
  }
  SetCameraFocus(focus);
  return JS_UNDEFINED;
}

JSValue GetTracking(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, IsCameraTracking());
}

JSValue StopTracking(JSContext *, JSValueConst, int, JSValueConst *) {
  StopCameraTracking();
  return JS_UNDEFINED;
}

// The interpolated moves. These stay command-backed rather than native because
// each one arms a dozen unnamed interpolation globals - start value, target,
// start time, duration, a mode flag - and the *handler* is where the pairing
// between them is written down. Reproducing that is how you get a camera that
// eases from the wrong place. See the header comment in JsCommands.cpp.
#define GK_CAMERA_COMMAND(fn, command)                                         \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunConsoleCommand(ctx, command, argc, argv);                        \
  }

GK_CAMERA_COMMAND(MoveTo, "SET REQUIRED POS")
GK_CAMERA_COMMAND(TurnTo, "SET REQUIRED ORI")
GK_CAMERA_COMMAND(ZoomTo, "SET REQUIRED DISTANCE")
GK_CAMERA_COMMAND(MoveAndZoomTo, "SET REQUIRED POSDIST")
GK_CAMERA_COMMAND(JerkyZoomTo, "SET JERKY DISTANCE")
GK_CAMERA_COMMAND(Rotate, "ROTATE CAMERA")
GK_CAMERA_COMMAND(Elevate, "ELEVATE CAMERA")
GK_CAMERA_COMMAND(Nudge, "ALTER CAMERA")
GK_CAMERA_COMMAND(CenterOn, "CENTRE")
GK_CAMERA_COMMAND(Track, "TRACK")
GK_CAMERA_COMMAND(BezierTrack, "CAMERA TRACK")

#undef GK_CAMERA_COMMAND

// Live accessors over the camera globals (Camera.h). Only ever handed to
// JS_SetPropertyFunctionList - see the note in JsBindings.h.
const JSCFunctionListEntry CameraProps[] = {
    JS_CGETSET_DEF2("position", GetPosition, SetPosition,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("orientation", GetOrientation, SetOrientation,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("yaw", GetYaw, SetYaw,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("roll", GetRoll, SetRoll,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("pitch", GetPitch, SetPitch,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("distance", GetDistance, SetDistance,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("max_distance", GetMaxDistance, SetMaxDistance,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("focus", GetFocus, SetFocus,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF("tracking", GetTracking, nullptr),
    JS_CFUNC_DEF("stop_tracking", 0, StopTracking),
    JS_CFUNC_DEF("move_to", 2, MoveTo),
    JS_CFUNC_DEF("turn_to", 4, TurnTo),
    JS_CFUNC_DEF("zoom_to", 2, ZoomTo),
    JS_CFUNC_DEF("move_and_zoom_to", 3, MoveAndZoomTo),
    JS_CFUNC_DEF("jerky_zoom_to", 1, JerkyZoomTo),
    JS_CFUNC_DEF("rotate", 2, Rotate),
    JS_CFUNC_DEF("elevate", 2, Elevate),
    JS_CFUNC_DEF("nudge", 2, Nudge),
    JS_CFUNC_DEF("center_on", 1, CenterOn),
    JS_CFUNC_DEF("track", 1, Track),
    JS_CFUNC_DEF("bezier_track", 4, BezierTrack),
};

} // namespace

JSValue NewCameraNamespace(JSContext *ctx) {
  return NewNamespace(ctx, CameraProps,
                      static_cast<int>(std::size(CameraProps)));
}

} // namespace gk::js
