#include "World.h"

#include "JsBindings.h"

#include <iterator>

namespace gk::js {
namespace {

// --- shared colour marshalling ------------------------------------------------

struct Color {
  float r, g, b, a;
};

JSValue NewColor(JSContext *ctx, const Color &c) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "r", JS_NewFloat64(ctx, c.r));
  JS_SetPropertyStr(ctx, obj, "g", JS_NewFloat64(ctx, c.g));
  JS_SetPropertyStr(ctx, obj, "b", JS_NewFloat64(ctx, c.b));
  JS_SetPropertyStr(ctx, obj, "a", JS_NewFloat64(ctx, c.a));
  return obj;
}

// Missing components keep whatever `out` already holds, the rule
// camera.position and camera.orientation follow.
bool ToColor(JSContext *ctx, JSValueConst v, Color *out) {
  if (!JS_IsObject(v)) {
    JS_ThrowTypeError(ctx, "expected a colour object with r, g, b and a");
    return false;
  }
  struct Field {
    const char *name;
    float *slot;
  } fields[] = {{"r", &out->r}, {"g", &out->g}, {"b", &out->b}, {"a", &out->a}};
  for (const Field &f : fields) {
    JSValue prop = JS_GetPropertyStr(ctx, v, f.name);
    if (JS_IsException(prop)) {
      return false;
    }
    if (JS_IsUndefined(prop)) {
      JS_FreeValue(ctx, prop);
      continue;
    }
    double d = 0.0;
    int failed = JS_ToFloat64(ctx, &d, prop);
    JS_FreeValue(ctx, prop);
    if (failed) {
      return false;
    }
    *f.slot = static_cast<float>(d);
  }
  return true;
}

// --- the sun ------------------------------------------------------------------

JSValue GetSunAngleJs(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetSunAngle());
}
JSValue SetSunAngleJs(JSContext *ctx, JSValueConst, JSValueConst v) {
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  SetSunAngle(static_cast<float>(d));
  return JS_UNDEFINED;
}

JSValue GetSunAngle2Js(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, GetSunAngle2());
}
JSValue SetSunAngle2Js(JSContext *ctx, JSValueConst, JSValueConst v) {
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  SetSunAngle2(static_cast<float>(d));
  return JS_UNDEFINED;
}

// A method rather than a property, because it is genuinely write-only:
// SetSunBrightness feeds the renderer and keeps no readable copy, and a getter
// returning a remembered value would lie the moment anything else set it. As a
// property it would also read back `undefined`, which TypeScript cannot express
// on an interface either - a set-only accessor there is still readable.
JSValue SetSunBrightnessJs(JSContext *ctx, JSValueConst, int argc,
                           JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "set_sun_brightness(color) expects one argument");
  }
  Color c{1.0f, 1.0f, 1.0f, 1.0f};
  if (!ToColor(ctx, argv[0], &c)) {
    return JS_EXCEPTION;
  }
  SetSunBrightness(c.r, c.g, c.b, c.a);
  return JS_UNDEFINED;
}

JSValue GetSunDirectionJs(JSContext *ctx, JSValueConst) {
  return NewVec3(ctx, GetSunDirection());
}

// Same story - the scene LightSet converts and stores it in its own form, so
// this is a method too. The alpha default of 1.0 is `AMBIENT`'s own.
//
// The JS name stays `set_ambient` deliberately. The native call underneath is
// LightSet_SetEmissiveColour and sets no ambient light (see World.h), but
// `world.set_ambient` is a published scripting API and renaming it would break
// every script using it. `AMBIENT` is also the console command it stands in for.
JSValue SetAmbientJs(JSContext *ctx, JSValueConst, int argc,
                     JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "set_ambient(color) expects one argument");
  }
  Color c{0.0f, 0.0f, 0.0f, 1.0f};
  if (!ToColor(ctx, argv[0], &c)) {
    return JS_EXCEPTION;
  }
  LightSet_SetEmissiveColour(c.r, c.g, c.b, c.a);
  return JS_UNDEFINED;
}

// --- fog ----------------------------------------------------------------------

JSValue GetFogAvailable(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, HasFog());
}

JSValue GetFogEnabledJs(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, GetFogMode() != 0);
}
JSValue SetFogEnabledJs(JSContext *ctx, JSValueConst, JSValueConst v) {
  SetFogEnabled(JS_ToBool(ctx, v) != 0);
  return JS_UNDEFINED;
}

JSValue GetFogModeJs(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, GetFogMode());
}

using FloatGetter = float (*)();
using FloatSetter = void (*)(float);

JSValue FloatSet(JSContext *ctx, JSValueConst v, FloatSetter set) {
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  set(static_cast<float>(d));
  return JS_UNDEFINED;
}

#define GK_FLOAT_ACCESSORS(js_get, js_set, get_fn, set_fn)                     \
  JSValue js_get(JSContext *ctx, JSValueConst) {                               \
    return JS_NewFloat64(ctx, get_fn());                                       \
  }                                                                            \
  JSValue js_set(JSContext *ctx, JSValueConst, JSValueConst v) {               \
    return FloatSet(ctx, v, set_fn);                                           \
  }

GK_FLOAT_ACCESSORS(GetFogValueJs, SetFogValueJs, GetFogValue, SetFogValue)
GK_FLOAT_ACCESSORS(GetFogUpdateJs, SetFogUpdateJs, GetFogUpdateRate,
                   SetFogUpdateRate)
GK_FLOAT_ACCESSORS(GetFogTransitionJs, SetFogTransitionJs, GetFogTransition,
                   SetFogTransition)

#undef GK_FLOAT_ACCESSORS

JSValue GetFogColorJs(JSContext *ctx, JSValueConst) {
  float rgba[4];
  GetFogColor(rgba);
  return NewColor(ctx, Color{rgba[0], rgba[1], rgba[2], rgba[3]});
}
JSValue SetFogColorJs(JSContext *ctx, JSValueConst, JSValueConst v) {
  float rgba[4];
  GetFogColor(rgba); // omitted components keep their current value
  Color c{rgba[0], rgba[1], rgba[2], rgba[3]};
  if (!ToColor(ctx, v, &c)) {
    return JS_EXCEPTION;
  }
  SetFogColor(c.r, c.g, c.b, c.a);
  return JS_UNDEFINED;
}

const JSCFunctionListEntry FogProps[] = {
    JS_CGETSET_DEF("available", GetFogAvailable, nullptr),
    JS_CGETSET_DEF2("enabled", GetFogEnabledJs, SetFogEnabledJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF("mode", GetFogModeJs, nullptr),
    JS_CGETSET_DEF2("value", GetFogValueJs, SetFogValueJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("update_rate", GetFogUpdateJs, SetFogUpdateJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("transition", GetFogTransitionJs, SetFogTransitionJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("color", GetFogColorJs, SetFogColorJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

// `fog` is a nested object rather than a `fog_*` prefix on seven properties.
// JS_DEF_OBJECT nests safely - it routes through JS_NewObjectProtoList to
// JS_SetPropertyFunctionList (quickjs.c:40081), which is the one path that
// honours JS_DEF_CGETSET. See the QuickJS conventions in CLAUDE.md.
const JSCFunctionListEntry WorldProps[] = {
    JS_CGETSET_DEF2("sun_angle", GetSunAngleJs, SetSunAngleJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("sun_angle2", GetSunAngle2Js, SetSunAngle2Js,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF("set_sun_brightness", 1, SetSunBrightnessJs),
    JS_CGETSET_DEF("sun_direction", GetSunDirectionJs, nullptr),
    JS_CFUNC_DEF("set_ambient", 1, SetAmbientJs),
    JS_OBJECT_DEF("fog", FogProps, static_cast<int>(std::size(FogProps)),
                  JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

} // namespace

JSValue NewWorldNamespace(JSContext *ctx) {
  return NewNamespace(ctx, WorldProps, static_cast<int>(std::size(WorldProps)));
}

} // namespace gk::js
