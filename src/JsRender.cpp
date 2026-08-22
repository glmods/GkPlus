// The `render` namespace: the measurement surface for the Vulkan renderer
// (vulkan_renderer_notes.md section 4), plus `material_override`, which is the first piece of
// section 5's eventual shape (post-process pass registration, material override, draw-list
// introspection) to exist.
//
// Everything here reads or reconfigures **this renderer**, never the game: the capture layer
// forwards every call to the original runtime unchanged, so nothing in this namespace can alter
// what the game itself draws - `render.material_override` repaints the Vulkan frame and leaves
// `GKPLUS_RENDERER=d3d8` and `d3d9` exactly as they were. That is what keeps the A/B honest.

#include <string>

#include "D3D8Capture.h"
#include "JsBindings.h"
#include "VkCapture.h"
#include "VkContext.h"
#include "VkDraw.h"
#include "VkLighting.h"
#include "VkRenderer.h"
#include "MapLights.h"
#include "VertexFormat.h"
#include "VkResources.h"

#include <iterator>

namespace gk::js {
namespace {

// The vertex converter's layout census as an array of {layout, calls, vertices, specialized},
// most vertices first. An array rather than an object keyed by layout because the ORDER is the
// answer here - the top row is what the converter spends its time on.
JSValue LayoutCensusToArray(JSContext *ctx) {
  vulkan::LayoutCensusEntry entries[64];
  const uint32_t count =
      vulkan::ReadLayoutCensus(entries, static_cast<uint32_t>(std::size(entries)));
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (uint32_t i = 0; i < count; ++i) {
    JSValue row = JS_NewObject(ctx);
    if (JS_IsException(row)) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
    JS_SetPropertyStr(ctx, row, "layout", JS_NewUint32(ctx, entries[i].layout));
    JS_SetPropertyStr(ctx, row, "calls",
                      JS_NewInt64(ctx, static_cast<int64_t>(entries[i].calls)));
    JS_SetPropertyStr(ctx, row, "vertices",
                      JS_NewInt64(ctx, static_cast<int64_t>(entries[i].vertices)));
    JS_SetPropertyStr(ctx, row, "specialized", JS_NewBool(ctx, entries[i].specialized));
    // Which call path produced them. Four fixed names rather than an array, because the answer
    // this column exists for is read by a person: "buffered" and "user_pointer" have opposite
    // fixes, and an index into an enum nobody has open is not a measurement anyone acts on.
    static const char *const kSources[] = {"buffered", "version", "user_pointer", "other"};
    JSValue by_source = JS_NewObject(ctx);
    if (JS_IsException(by_source)) {
      JS_FreeValue(ctx, row);
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
    for (uint32_t s = 0; s < 4; ++s) {
      if (entries[i].calls_by_source[s] == 0) {
        continue;
      }
      JSValue pair = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, pair, "calls",
                        JS_NewInt64(ctx, static_cast<int64_t>(entries[i].calls_by_source[s])));
      JS_SetPropertyStr(ctx, pair, "vertices",
                        JS_NewInt64(ctx, static_cast<int64_t>(entries[i].vertices_by_source[s])));
      JS_SetPropertyStr(ctx, by_source, kSources[s], pair);
    }
    JS_SetPropertyStr(ctx, row, "by_source", by_source);
    if (JS_SetPropertyUint32(ctx, array, i, row) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// A std::map<uint32_t, T> as a JS object keyed by the decimal number. Keys are D3D enum
// values, so they stay numbers rather than being decoded to names - the whole point of the
// measurement is to find out which ones occur, and a name table would have to be invented
// before knowing that.
template <typename Map, typename Fn>
JSValue MapToObject(JSContext *ctx, const Map &map, Fn value_of) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  for (const auto &[key, value] : map) {
    JSValue v = value_of(ctx, value);
    if (JS_IsException(v) ||
        JS_SetPropertyUint32(ctx, obj, key, v) < 0) {
      JS_FreeValue(ctx, obj);
      return JS_EXCEPTION;
    }
  }
  return obj;
}

JSValue CountValue(JSContext *ctx, uint64_t n) {
  return JS_NewInt64(ctx, static_cast<int64_t>(n));
}

JSValue DistinctValue(JSContext *ctx, const std::set<uint32_t> &values) {
  return JS_NewInt32(ctx, static_cast<int32_t>(values.size()));
}

JSValue GetCaptured(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::DeviceCreated());
}

JSValue GetReport(JSContext *ctx, JSValueConst) {
  const std::string text = d3d8::FormatStats();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.topologies` - "", "strip", "line" or "all". A setter as well as a getter, because
// the only exact way to measure what these draws paint is to toggle them inside one paused
// frame; comparing two launches measures the scene drifting between them (§4.21).
JSValue GetTopologies(JSContext *ctx, JSValueConst) {
  bool strips = false;
  bool lines = false;
  d3d8::GetTopologies(strips, lines);
  const char *value = strips && lines ? "all" : strips ? "strip" : lines ? "line" : "";
  return JS_NewString(ctx, value);
}

JSValue SetTopologies(JSContext *ctx, JSValueConst, JSValueConst value) {
  const char *text = JS_ToCString(ctx, value);
  if (text == nullptr) {
    return JS_EXCEPTION;
  }
  const std::string want(text);
  JS_FreeCString(ctx, text);
  if (want != "" && want != "strip" && want != "line" && want != "all") {
    return JS_ThrowTypeError(ctx, "topologies must be \"\", \"strip\", \"line\" or \"all\"");
  }
  d3d8::SetTopologies(want == "strip" || want == "all", want == "line" || want == "all");
  return JS_UNDEFINED;
}

// `render.lighting` - the real per-vertex light sum, on or off. Same reason as `topologies`
// for being writable: false restores the previous build's material collapse, so the two states
// are one frame apart and the difference image is exactly what lighting paints (§4.26).
JSValue GetLighting(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::GetLightSum());
}

JSValue SetLighting(JSContext *ctx, JSValueConst, JSValueConst value) {
  d3d8::SetLightSum(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.skip_readonly_unlocks` - §4.84. See D3D8Capture.h.
//
// Writable for a reason the other knobs here do not have: this one is supposed to be *invisible*.
// A read-only lock changed nothing, so the two states differ only in work avoided - which makes
// flipping it mid-session both the A/B for the frame time and the check that it is sound, since a
// difference on screen would mean something is writing through a lock it declared read-only.
JSValue GetSkipReadOnlyUnlocks(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::SkipReadOnlyUnlocks());
}

JSValue SetSkipReadOnlyUnlocksValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  d3d8::SetSkipReadOnlyUnlocks(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.software_process_vertices` and `render.verify_process_vertices` - §4.85. See
// D3D8Capture.h for both, and read the verification report before trusting the first one.
JSValue GetSoftwareProcessVertices(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::SoftwareProcessVertices());
}

JSValue SetSoftwareProcessVerticesValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  d3d8::SetSoftwareProcessVertices(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetVerifyProcessVertices(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::VerifyProcessVertices());
}

JSValue SetVerifyProcessVerticesValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  d3d8::SetVerifyProcessVertices(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// The readback. A getter and not a call: it formats counters and touches no D3D.
JSValue GetProcessVerticesReport(JSContext *ctx, JSValueConst) {
  const std::string report = d3d8::FormatProcessVerticesVerification();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// `render.specular` - the specular term of that sum on its own. The mirror image of
// GKPLUS_NO_SPECULAR, which reaches only the forwarded call; with both, the term can be removed
// from one paused frame of each renderer and the two bases compared directly (§4.46).
JSValue GetSpecularValue(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::GetSpecular());
}

JSValue SetSpecularValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  d3d8::SetSpecular(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.half_pixel` - the D3D9 pixel-centre convention as a viewport offset (VkDraw.h).
// Writable for the same reason as `topologies` and `lighting`: it moves every pixel in the
// frame by half of one, so the only comparison fine enough to see it is two shots of the same
// paused frame.
JSValue GetHalfPixel(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::HalfPixel());
}

JSValue SetHalfPixelValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetHalfPixel(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.rhw_depth_raw` - take a pre-transformed vertex's z as the depth value, clamped to the
// viewport's slice, rather than running the viewport's depth range over it (VkDraw.h, §4.45).
// Writable for the same reason as `half_pixel`: it moves every screen-space draw in the frame
// along z, and a paused frame is the only place the before and after can be compared at a zero
// noise floor.
JSValue GetRhwDepthRaw(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::RhwDepthRaw());
}

JSValue SetRhwDepthRawValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetRhwDepthRaw(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.viewport_rect` - honour D3DVIEWPORT8's rectangle per draw (VkDraw.h, §4.47). Writable
// for the same reason as `rhw_depth_raw`, and it needs the toggle more than most: the only screen
// it changes is one the game will not hold still for a two-launch comparison, so the A/B has to
// happen inside one session.
JSValue GetViewportRect(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::ViewportRect());
}

JSValue SetViewportRectValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetViewportRect(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.offscreen` - rasterise the world at the game's backbuffer size and scale it onto the
// swapchain, rather than drawing straight into the swapchain (VkRenderer.h, §4.37). Writable for
// the same reason as `half_pixel`: it moves every pre-transformed pixel in the frame, so the only
// comparison fine enough to judge it is two shots of one paused frame.
JSValue GetOffscreen(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::Offscreen());
}

JSValue SetOffscreenValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetOffscreen(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.present_linear` - the filter for that final scale. False (nearest) is what the original
// appears to do, deduced from the probe quad keeping exactly sixteen distinct values through a
// 640->628 stretch; true is the A/B for that deduction.
JSValue GetPresentLinear(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::PresentLinear());
}

JSValue SetPresentLinearValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetPresentLinear(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.draw_state` - set it to a draw index, let a frame pass, then read back what D3D held
// at the moment that draw was issued, diffed against the shadow mirror (D3D8Capture.h). The
// per-draw half of `render.verify_state()`, and the half that can see a divergence which only
// exists mid-scene.
JSValue GetDrawState(JSContext *ctx, JSValueConst) {
  const std::string text = d3d8::DescribeWatchedDrawState();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue SetDrawStateValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  int64_t index = -1;
  if (JS_ToInt64(ctx, &index, value) != 0) {
    return JS_EXCEPTION;
  }
  d3d8::WatchDrawState(index);
  return JS_UNDEFINED;
}

// `render.draw_geometry` - what the same watched draw actually pulled out of the arena, beside
// what D3D holds in the game's own buffer for it (D3D8Capture.h). Set `render.draw_state` first;
// this reads back the geometry half of the same snapshot.
JSValue GetDrawGeometry(JSContext *ctx, JSValueConst) {
  const std::string text = d3d8::DescribeWatchedDrawGeometry();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.verify_state()` - the immediate form: read the fixed-function state back off the device
// and diff it against the mirror, now. Works in every renderer mode.
JSValue VerifyState(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const std::string text = d3d8::VerifyShadowState();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.draw_vertices` - set it to a draw index, let a frame pass, then read it back for the
// geometry that draw was actually handed. See WatchDrawVertices in VkDraw.h.
JSValue GetDrawVertices(JSContext *ctx, JSValueConst) {
  const std::string text = vulkan::DescribeWatchedVertices();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue SetDrawVerticesValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  uint32_t index = 0;
  if (JS_ToUint32(ctx, &index, value) != 0) {
    return JS_EXCEPTION;
  }
  vulkan::WatchDrawVertices(index);
  return JS_UNDEFINED;
}

// `render.force_lod` - the mip probe (VkDraw.h). -1 samples normally; 0 and up force every
// texture fetch to that level, which is how "we pick the wrong mip" is told apart from "we filter
// the right one wrongly".
JSValue GetForceLod(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, vulkan::ForceLod());
}

JSValue SetForceLodValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  double lod = -1.0;
  if (JS_ToFloat64(ctx, &lod, value) != 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetForceLod(static_cast<float>(lod));
  return JS_UNDEFINED;
}

// `render.depth_probe(armed, quad_z, clear_z, min_z, max_z)` - arm the depth probe
// (D3D8Capture.h). Drawn through the capture device itself, like `render.probe`, so d3d8, d3d9
// and vulkan are all asked the same question and the answer is a quad that is either there or
// not. The defaults are the discriminating case: quad z 0.8, viewport 0..0.5, depth cleared to
// 0.5, which the viewport transform makes visible and a raw z does not.
JSValue DepthProbe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const bool armed = argc < 1 || JS_ToBool(ctx, argv[0]) != 0;
  double values[4] = {0.8, 0.5, 0.0, 0.5};
  for (int i = 0; i < 4; ++i) {
    if (argc > i + 1 && JS_ToFloat64(ctx, &values[i], argv[i + 1]) != 0) {
      return JS_EXCEPTION;
    }
  }
  const std::string result =
      d3d8::ArmDepthProbe(armed, values[0], values[1], values[2], values[3]);
  return JS_NewStringLen(ctx, result.data(), result.size());
}

// `render.viewport_probe(armed, x, y, width, height)` - arm the viewport-rectangle probe
// (D3D8Capture.h). The depth probe's sibling: same delivery, same "read it in d3d8" rule, and it
// answers the other half of what a viewport does to a pre-transformed vertex.
JSValue ViewportProbe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const bool armed = argc < 1 || JS_ToBool(ctx, argv[0]) != 0;
  int32_t values[4] = {100, 60, 200, 150};
  for (int i = 0; i < 4; ++i) {
    if (argc > i + 1 && JS_ToInt32(ctx, &values[i], argv[i + 1]) != 0) {
      return JS_EXCEPTION;
    }
  }
  const std::string result = d3d8::ArmViewportProbe(
      armed, values[0], values[1], static_cast<uint32_t>(values[2]),
      static_cast<uint32_t>(values[3]));
  return JS_NewStringLen(ctx, result.data(), result.size());
}

// `render.probe(name, scale, mipmap)` - arm the synthetic quad (D3D8Capture.h). Drawn through
// the capture device itself, so d3d8, d3d9 and vulkan all get the same quad and the comparison
// stops depending on anything about the scene.
JSValue ProbeQuad(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  std::string name;
  if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
    const char *text = JS_ToCString(ctx, argv[0]);
    if (text == nullptr) {
      return JS_EXCEPTION;
    }
    name = text;
    JS_FreeCString(ctx, text);
  }
  double scale = 1.0;
  if (argc > 1 && JS_ToFloat64(ctx, &scale, argv[1]) != 0) {
    return JS_EXCEPTION;
  }
  const bool mipmap = argc > 2 && JS_ToBool(ctx, argv[2]) != 0;
  double offset = 0.0;
  if (argc > 3 && JS_ToFloat64(ctx, &offset, argv[3]) != 0) {
    return JS_EXCEPTION;
  }
  const bool alpha = argc > 4 && JS_ToBool(ctx, argv[4]) != 0;
  const std::string result = d3d8::ArmProbeQuad(name, scale, mipmap, offset, alpha);
  return JS_NewStringLen(ctx, result.data(), result.size());
}

// `render.material_override(name, spec)` - see SetMaterialOverride in VkDraw.h.
//
// `spec` null or absent removes the registration; anything else is
// `{texture, tint: [r, g, b, a?], hide}`. It **returns the readback** rather than undefined,
// because the one thing that can go wrong here is silent: a substring key that matches no live
// image, or matches more than the author meant, is not an error and cannot be detected from the
// call site. The answer says what it matched, now.
JSValue MaterialOverrideFn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "material_override(name, spec) needs a texture name");
  }
  const char *text = JS_ToCString(ctx, argv[0]);
  if (text == nullptr) {
    return JS_EXCEPTION;
  }
  const std::string name = text;
  JS_FreeCString(ctx, text);

  if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
    vulkan::RemoveMaterialOverride(name);
    const std::string result = vulkan::DescribeMaterialOverrides();
    return JS_NewStringLen(ctx, result.data(), result.size());
  }
  if (!JS_IsObject(argv[1])) {
    return JS_ThrowTypeError(ctx,
                             "material_override(name, spec): spec is {texture, tint, hide} "
                             "or null to remove");
  }

  vulkan::MaterialOverride over;
  JSValue texture = JS_GetPropertyStr(ctx, argv[1], "texture");
  if (JS_IsException(texture)) {
    return JS_EXCEPTION;
  }
  if (!JS_IsUndefined(texture) && !JS_IsNull(texture)) {
    const char *replacement = JS_ToCString(ctx, texture);
    if (replacement == nullptr) {
      JS_FreeValue(ctx, texture);
      return JS_EXCEPTION;
    }
    over.texture = replacement;
    JS_FreeCString(ctx, replacement);
  }
  JS_FreeValue(ctx, texture);

  JSValue tint = JS_GetPropertyStr(ctx, argv[1], "tint");
  if (JS_IsException(tint)) {
    return JS_EXCEPTION;
  }
  if (!JS_IsUndefined(tint) && !JS_IsNull(tint)) {
    // Three components or four; alpha defaults to opaque, since a tint given as a colour should
    // not silently make a surface transparent.
    for (int i = 0; i < 4; ++i) {
      JSValue component = JS_GetPropertyUint32(ctx, tint, uint32_t(i));
      if (JS_IsException(component)) {
        JS_FreeValue(ctx, tint);
        return JS_EXCEPTION;
      }
      if (JS_IsUndefined(component)) {
        JS_FreeValue(ctx, component);
        if (i < 3) {
          JS_FreeValue(ctx, tint);
          return JS_ThrowTypeError(ctx, "material_override: tint is [r, g, b] or [r, g, b, a]");
        }
        break;
      }
      double value = 1.0;
      const int failed = JS_ToFloat64(ctx, &value, component);
      JS_FreeValue(ctx, component);
      if (failed != 0) {
        JS_FreeValue(ctx, tint);
        return JS_EXCEPTION;
      }
      over.tint[i] = static_cast<float>(value);
    }
  }
  JS_FreeValue(ctx, tint);

  JSValue hide = JS_GetPropertyStr(ctx, argv[1], "hide");
  if (JS_IsException(hide)) {
    return JS_EXCEPTION;
  }
  over.hide = JS_ToBool(ctx, hide) != 0;
  JS_FreeValue(ctx, hide);

  vulkan::SetMaterialOverride(name, over);
  const std::string result = vulkan::DescribeMaterialOverrides();
  return JS_NewStringLen(ctx, result.data(), result.size());
}

// `render.material_overrides` - every registration, with the live images each key matches.
JSValue GetMaterialOverrides(JSContext *ctx, JSValueConst) {
  const std::string text = vulkan::DescribeMaterialOverrides();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue ClearMaterialOverridesFn(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  vulkan::ClearMaterialOverrides();
  return JS_UNDEFINED;
}

// `render.lighting_maps` - the companion `<texture> lighting.dds` feature (VkLighting.h). Off
// interns every material exactly as the build before it did, so this A/Bs the whole feature on one
// paused frame at a 0.000 noise floor - the same rule `render.lighting` and `render.shade_mode`
// follow.
JSValue GetLightingMaps(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::LightingMaps());
}

JSValue SetLightingMapsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetLightingMaps(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.lighting_map_report` - which names were probed, what was found, and the knobs. The
// readback matters more than for a diagnostic: a texture with no companion file is the *normal*
// case, so "nothing happened" is what both a working install and a misnamed file look like.
JSValue GetLightingMapReport(JSContext *ctx, JSValueConst) {
  const std::string text = vulkan::DescribeLightingMaps();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// The knobs, each a plain float on LightingMapParams. Separate accessors rather than one
// object so each can be swept from the REPL with `render.bump_scale = x` on a paused frame, which
// is the only comparison here with a noise floor worth having.
#define GK_LIGHTING_KNOB(name)                                                                  \
  JSValue Get##name(JSContext *ctx, JSValueConst) {                                             \
    return JS_NewFloat64(ctx, vulkan::LightingParams().name);                                   \
  }                                                                                             \
  JSValue Set##name##Value(JSContext *ctx, JSValueConst, JSValueConst value) {                  \
    double number = 0.0;                                                                        \
    if (JS_ToFloat64(ctx, &number, value) != 0) {                                               \
      return JS_EXCEPTION;                                                                      \
    }                                                                                           \
    vulkan::MutableLightingParams().name = static_cast<float>(number);                          \
    return JS_UNDEFINED;                                                                        \
  }

GK_LIGHTING_KNOB(bump_scale)
GK_LIGHTING_KNOB(bump_diffuse)
GK_LIGHTING_KNOB(bump_diffuse_limit)
GK_LIGHTING_KNOB(specular_scale)
GK_LIGHTING_KNOB(specular_from_diffuse)
GK_LIGHTING_KNOB(gloss_min)
GK_LIGHTING_KNOB(gloss_max)
GK_LIGHTING_KNOB(chrome_scale)
GK_LIGHTING_KNOB(chrome_blur)

#undef GK_LIGHTING_KNOB

// `render.chrome_texgen` - generate the chrome pass's texture coordinate from the bumped normal,
// or read the mesh's UV1 as the engine does. A bool rather than a knob because there is no
// meaningful blend between the two: they are different coordinates, and half of each is not a
// sphere map.
JSValue GetChromeTexgen(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::LightingParams().chrome_texgen);
}

JSValue SetChromeTexgen(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::MutableLightingParams().chrome_texgen = JS_ToBool(ctx, value) != 0;
  return JS_UNDEFINED;
}

// --- PN-triangle amplification (§4.71) --------------------------------------------------------
//
// Its own knob macro rather than a reuse of GK_LIGHTING_KNOB: these are not lighting-map
// parameters and filing them on LightingMapParams would have been convenient and wrong.
#define GK_TESS_KNOB(js_name, member)                                                           \
  JSValue Get##member(JSContext *ctx, JSValueConst) {                                           \
    return JS_NewFloat64(ctx, vulkan::TessParams().member);                                     \
  }                                                                                             \
  JSValue Set##member##Value(JSContext *ctx, JSValueConst, JSValueConst value) {                \
    double number = 0.0;                                                                        \
    if (JS_ToFloat64(ctx, &number, value) != 0) {                                               \
      return JS_EXCEPTION;                                                                      \
    }                                                                                           \
    vulkan::MutableTessParams().member = static_cast<float>(number);                            \
    return JS_UNDEFINED;                                                                        \
  }

GK_TESS_KNOB("tess_edge_pixels", edge_pixels)
GK_TESS_KNOB("tess_max", max_factor)
GK_TESS_KNOB("tess_min", min_factor)
GK_TESS_KNOB("pn_strength", pn_strength)
GK_TESS_KNOB("pn_flat_threshold", pn_flat_threshold)
GK_TESS_KNOB("pn_max_offset", pn_max_offset)
GK_TESS_KNOB("tess_shadow_factor", shadow_factor)

#undef GK_TESS_KNOB

// `render.tessellation` - off by default, because it changes the level's silhouette rather than
// reproducing D3D, and a default run has to keep the renderer's own claim true. Reads back
// **false on a device with no `tessellationShader`** even after being set, which is deliberate:
// the getter answers "is this happening" rather than "was it asked for".
JSValue GetTessellation(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::TessellationEnabled());
}

JSValue SetTessellationValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetTessellationEnabled(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.tess_shadows` - whether the shadow passes tessellate with the colour pass. Separable
// because the bake is where the cost is, so a frame-time regression can be pinned to one half.
JSValue GetTessShadows(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::TessellationShadows());
}

JSValue SetTessShadowsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetTessellationShadows(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.pn_seam_fix` - zero the tangent term at a corner the mesh has split into vertices with
// different normals, which is what keeps a material boundary from tearing open (§4.71). On by
// default; the knob is the A/B that prices the rule, and turning it off reproduces the tear.
JSValue GetSeamFix(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::SplitCornerFix());
}

JSValue SetSeamFixValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetSplitCornerFix(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.tess_set` - "map" (the level mesh), "all" (props and units too) or "off". A string
// rather than a pair of booleans because the three are exclusive, and `render.normal_census` says
// the choice is a real one: more than half the curvature in a frame is in the props.
JSValue GetTessSet(JSContext *ctx, JSValueConst) {
  switch (vulkan::TessellationSet()) {
  case vulkan::TessSet::Map:
    return JS_NewString(ctx, "map");
  case vulkan::TessSet::All:
    return JS_NewString(ctx, "all");
  default:
    return JS_NewString(ctx, "off");
  }
}

JSValue SetTessSetValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  const char *text = JS_ToCString(ctx, value);
  if (text == nullptr) {
    return JS_EXCEPTION;
  }
  const std::string name = text;
  JS_FreeCString(ctx, text);
  if (name == "map") {
    vulkan::SetTessellationSet(vulkan::TessSet::Map);
  } else if (name == "all") {
    vulkan::SetTessellationSet(vulkan::TessSet::All);
  } else if (name == "off") {
    vulkan::SetTessellationSet(vulkan::TessSet::Off);
  } else {
    return JS_ThrowTypeError(ctx, "render.tess_set takes \"map\", \"all\" or \"off\"");
  }
  return JS_UNDEFINED;
}

// `render.shade_mode` - honour D3DRS_SHADEMODE, or interpolate everything (VkDraw.h). Writable
// for the same reason as the three above: on level02 it touches 2% of the draws and all of them
// are the stencil shadow, so nothing coarser than two shots of one paused frame can see it.
JSValue GetShadeMode(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::ShadeMode());
}

JSValue SetShadeModeValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetShadeMode(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.per_pixel_lighting` - **not a diagnostic, a feature** (VkDraw.h). D3D8's light sum,
// evaluated per pixel instead of per vertex. On by default, and off is the fixed-function
// reproduction bit-identically - which is the direction that makes it measurable: pause a frame,
// toggle it, and the difference image is exactly what Gouraud shading could not represent.
//
// The whole-frame MAD is the wrong reading for it. Per-vertex against per-pixel is a difference in
// SHAPE across a triangle, largest where a normal turns most and zero on anything flat, so it
// concentrates on curved and small geometry - a projectile, a unit's limbs - and averages to
// nearly nothing over a level's flat ground.
JSValue GetPerPixelLighting(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::PerPixelLighting());
}

JSValue SetPerPixelLightingValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetPerPixelLighting(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.msaa` - the world pass's sample count (VkDraw.h). 1 is off and is the default; 2, 4 and
// 8 are the counts a desktop device is likely to offer.
//
// **Writable at any time, and it costs one frame.** The setter only records the number - the
// rebuild it implies (a multisampled target, the depth image, and every cached world pipeline,
// since `rasterizationSamples` is not dynamic state) happens in `ReconcileRenderTarget` at the top
// of the next frame, under the wait-idle a resize takes anyway.
//
// It reads back **effective, not requested**, which is why a read straight after a write can
// answer the old value: the frame that adopts it has not started yet. A panel that binds a control
// to this must therefore keep its own pending value or it will fight itself for one frame - the
// same shape `render.tessellation` has on a device with no tessellation, one frame wide instead of
// forever. `render.status` prints both when they disagree.
JSValue GetMsaa(JSContext *ctx, JSValueConst) {
  return JS_NewUint32(ctx, vulkan::Msaa());
}

JSValue SetMsaaValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  uint32_t samples = 0;
  if (JS_ToUint32(ctx, &samples, value) != 0) {
    return JS_EXCEPTION;
  }
  // No range check and no throw: `SetMsaa` rounds down to a power of two the device offers, so
  // every number is meaningful and a 3 or a 1000 is a request for the nearest count rather than an
  // error. Throwing would make a slider that passes its raw position an exception generator.
  vulkan::SetMsaa(samples);
  return JS_UNDEFINED;
}

// --- HDR (VkDraw.h's "HDR: a linear-light pipeline with an SDR tonemap") ----------------------
//
// `render.hdr` - the world pass into `R16G16B16A16_SFLOAT` and a tonemap pass in place of the
// scale blit. Off by default, and in `render.stock`'s set.
//
// **Writable at any time and it costs one frame**, for exactly the reason `render.msaa` does and
// through the same machinery: the setter records the request and `ReconcileRenderTarget` rebuilds
// the targets and the world pipeline cache at the top of the next frame, under the wait-idle a
// resize takes anyway.
//
// Unlike `render.msaa` this one reads back **as requested, not as effective**, and that is
// deliberate rather than an inconsistency: the effective value is a `VkFormat` this side does not
// name, and the two things that can make it differ - a device with no tonemap pass, and the frame
// between the write and the reconcile - are both reported by `render.status` in words. A panel
// binding a checkbox here therefore does not have to keep a pending value of its own.
JSValue GetHdr(JSContext *ctx, JSValueConst) { return JS_NewBool(ctx, vulkan::Hdr()); }

JSValue SetHdrValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetHdr(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.linear_input` - sRGB-decode the fragment's FINAL colour, so the framebuffer blend and
// the tonemap run on light. On by default *within* HDR, and inert without it.
//
// It decodes the output and not the inputs, which is the conclusion of notes §4.94-§4.97: the
// shading runs exactly as Gunlok's fixed function runs it, because no partial decode of a
// display-referred lighting equation preserves what was balanced in it. What this buys is the
// blend - additive fires and flares accumulate past 1 instead of clipping - and a real over-range
// for the tonemap. An opaque draw is bit-exact against stock.
JSValue GetLinearInput(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::LinearInput());
}

JSValue SetLinearInputValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetLinearInput(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.tonemap` - the operator, as a name rather than a number, because a number here would be
// unreadable in a REPL session and every one of the four is a different look.
//
// **No operator reaches the 2D half of the game**: the menus, briefing screens, HUD and inventory
// are drawn after the tonemap, so what is chosen here applies to the world alone (notes §4.92).
// `rolloff` is the default because it is the conservative one - exactly the identity below
// `render.tonemap_knee`, so it only touches what genuinely exceeds it - and not because anything
// depends on it any more.
const char *TonemapName(uint32_t op) {
  switch (op) {
  case 1: return "rolloff";
  case 2: return "reinhard";
  case 3: return "aces";
  case 4: return "filmic";
  case 5: return "agx";
  default: return "clamp";
  }
}

JSValue GetTonemap(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, TonemapName(vulkan::Tonemap()));
}

JSValue SetTonemapValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  const char *name = JS_ToCString(ctx, value);
  if (name == nullptr) {
    return JS_EXCEPTION;
  }
  const std::string text = name;
  JS_FreeCString(ctx, name);
  // **Refused rather than approximated**, the same rule `GKPLUS_VK_PRESENT_MODE` follows: the
  // whole point of naming an operator is knowing which one ran, and a typo silently behaving as
  // `clamp` would read as "HDR does nothing on this machine".
  if (text == "clamp") {
    vulkan::SetTonemap(0);
  } else if (text == "rolloff") {
    vulkan::SetTonemap(1);
  } else if (text == "reinhard") {
    vulkan::SetTonemap(2);
  } else if (text == "aces") {
    vulkan::SetTonemap(3);
  } else if (text == "filmic") {
    vulkan::SetTonemap(4);
  } else if (text == "agx") {
    vulkan::SetTonemap(5);
  } else {
    return JS_ThrowTypeError(
        ctx, "render.tonemap must be clamp, rolloff, reinhard, aces, filmic or agx");
  }
  return JS_UNDEFINED;
}

// `render.exposure` - a linear multiplier applied before the operator, which is the only place it
// can go: after it, it would brighten an already-compressed image and undo the compression.
JSValue GetExposure(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, vulkan::Exposure());
}

JSValue SetExposureValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  double v = 0.0;
  if (JS_ToFloat64(ctx, &v, value) != 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetExposure(static_cast<float>(v));
  return JS_UNDEFINED;
}

// `render.tonemap_knee` - where `rolloff` stops being the identity. Read by that operator alone.
JSValue GetTonemapKnee(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, vulkan::TonemapKnee());
}

JSValue SetTonemapKneeValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  double v = 0.0;
  if (JS_ToFloat64(ctx, &v, value) != 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetTonemapKnee(static_cast<float>(v));
  return JS_UNDEFINED;
}

// `render.tonemap_white` - what `reinhard` maps to exactly 1.0. Read by that operator alone.
JSValue GetTonemapWhite(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, vulkan::TonemapWhite());
}

JSValue SetTonemapWhiteValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  double v = 0.0;
  if (JS_ToFloat64(ctx, &v, value) != 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetTonemapWhite(static_cast<float>(v));
  return JS_UNDEFINED;
}

// --- bloom (VkDraw.h's "bloom: three layers over the HDR target") -----------------------------
//
// `render.bloom` - the master switch. Off by default, in `render.stock`'s set, and **inert without
// `render.hdr`**: a threshold is a statement about light and an 8-bit target has none to make. It
// reads back as REQUESTED for the same reason `render.hdr` does, so a checkbox can bind to it
// directly; `render.bloom_layers` says in words when a request is not being served.
JSValue GetBloom(JSContext *ctx, JSValueConst) { return JS_NewBool(ctx, vulkan::Bloom()); }

JSValue SetBloomValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetBloom(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

const char *BlendName(vulkan::BloomBlend blend) {
  switch (blend) {
  case vulkan::BloomBlend::Add: return "add";
  case vulkan::BloomBlend::Screen: return "screen";
  case vulkan::BloomBlend::Max: return "max";
  default: return "off";
  }
}

// `render.bloom_layer(index, spec?)` - one layer's five parameters.
//
// **A function taking an object rather than fifteen flat accessors**, which is the one place this
// namespace departs from its own convention (`ao_radius`, `tess_edge_pixels`, ... are all flat).
// Three layers times five parameters is where that convention stops paying: `bloom1_threshold`
// through `bloom3_blend` is fifteen names to remember and no way to read a layer as one thing.
// `render.material_override` is the precedent for the shape - a spec object, partial, with the
// readback as the return value.
//
// It **returns the layer as it now stands**, so a caller sees what the clamps did, and it reads
// without writing when the spec is absent. Every field of the spec is optional and an absent one is
// left alone, which is what makes `bloom_layer(0, {intensity: 0.8})` a one-parameter edit rather
// than a four-parameter reset.
JSValue BloomLayerFn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  uint32_t index = 0;
  if (argc < 1 || JS_ToUint32(ctx, &index, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx, "bloom_layer(index, spec) needs a layer index (0, 1 or 2)");
  }
  if (index >= vulkan::kBloomLayers) {
    return JS_ThrowRangeError(ctx, "bloom_layer: index must be 0, 1 or 2");
  }
  vulkan::BloomLayer layer = vulkan::BloomLayerAt(index);

  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!JS_IsObject(argv[1])) {
      return JS_ThrowTypeError(ctx, "bloom_layer(index, spec): spec is "
                                    "{threshold, knee, radius, intensity, blend}");
    }
    // The four numbers, read through one helper so an absent key and a bad value cannot be
    // confused: absent leaves the current value, present and unconvertible throws.
    struct NumberField {
      const char *name;
      float *target;
    };
    const NumberField numbers[] = {{"threshold", &layer.threshold},
                                   {"knee", &layer.knee},
                                   {"radius", &layer.radius},
                                   {"intensity", &layer.intensity}};
    for (const NumberField &field : numbers) {
      JSValue value = JS_GetPropertyStr(ctx, argv[1], field.name);
      if (JS_IsException(value)) {
        return JS_EXCEPTION;
      }
      if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        continue;
      }
      double number = 0.0;
      const int failed = JS_ToFloat64(ctx, &number, value);
      JS_FreeValue(ctx, value);
      if (failed != 0) {
        return JS_EXCEPTION;
      }
      *field.target = static_cast<float>(number);
    }

    JSValue blend = JS_GetPropertyStr(ctx, argv[1], "blend");
    if (JS_IsException(blend)) {
      return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(blend) && !JS_IsNull(blend)) {
      const char *name = JS_ToCString(ctx, blend);
      if (name == nullptr) {
        JS_FreeValue(ctx, blend);
        return JS_EXCEPTION;
      }
      const std::string text = name;
      JS_FreeCString(ctx, name);
      // **Refused rather than approximated**, the same rule `render.tonemap` follows: a typo
      // silently behaving as `off` would read as "bloom does nothing on this machine", which is the
      // one diagnosis that sends someone looking in the wrong place.
      if (text == "off") {
        layer.blend = vulkan::BloomBlend::Off;
      } else if (text == "add") {
        layer.blend = vulkan::BloomBlend::Add;
      } else if (text == "screen") {
        layer.blend = vulkan::BloomBlend::Screen;
      } else if (text == "max") {
        layer.blend = vulkan::BloomBlend::Max;
      } else {
        JS_FreeValue(ctx, blend);
        return JS_ThrowTypeError(ctx, "bloom_layer: blend must be off, add, screen or max");
      }
    }
    JS_FreeValue(ctx, blend);
    vulkan::SetBloomLayer(index, layer);
    layer = vulkan::BloomLayerAt(index);
  }

  JSValue out = JS_NewObject(ctx);
  if (JS_IsException(out)) {
    return out;
  }
  JS_SetPropertyStr(ctx, out, "threshold", JS_NewFloat64(ctx, layer.threshold));
  JS_SetPropertyStr(ctx, out, "knee", JS_NewFloat64(ctx, layer.knee));
  JS_SetPropertyStr(ctx, out, "radius", JS_NewFloat64(ctx, layer.radius));
  JS_SetPropertyStr(ctx, out, "intensity", JS_NewFloat64(ctx, layer.intensity));
  JS_SetPropertyStr(ctx, out, "blend", JS_NewString(ctx, BlendName(layer.blend)));
  return out;
}

// `render.bloom_layers` - all three layers, the size each is running at, and whether the pass is
// actually doing anything. A string and not a structure, for the reason `material_overrides` is one:
// the interesting part is what a caller cannot compute from the knobs - the layer extents, the
// sigma in texels, whether a kernel hit its cap, and which of "off", "unavailable" and "inert
// without hdr" is the case.
JSValue GetBloomLayers(JSContext *ctx, JSValueConst) {
  const std::string text = vulkan::DescribeBloom();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.stock` - all twelve deliberate departures from D3D8 at once, and back again (VkDraw.h,
// notes §4.87). `true` is the setup a comparison against `GKPLUS_RENDERER=d3d8` needs; `false`
// restores what the session had, not the build's defaults.
//
// **The point of it is that the A/B is one write on a paused frame.** Twelve by hand is twelve
// frames of drift on anything that moves, and the comparison this exists to serve is the one with
// the zero noise floor.
//
// It reads back derived rather than as a mode flag - `render.ao = true` while stock is applied
// makes `render.stock` false on the next read, because it is - so a panel binding a checkbox to it
// stays honest with no state of its own.
JSValue GetStock(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::Stock());
}

JSValue SetStockValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetStock(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.map_lighting` - replace the level's BAKED per-vertex colour with a per-pixel evaluation
// of its own light rig (VkDraw.h, notes §4.54). **Off by default on performance grounds**: it is
// brute force over every light in the level per pixel until phase 2's culling exists.
//
// Judge it on level04 or level05, not only on level02. The fitted model reaches r 0.93-0.96 there
// and only 0.37 on level02, whose 51 lights have ranges so long that none dominates anywhere - so
// the feature looks least like the bake exactly on the level every other measurement uses.
JSValue GetMapLighting(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::MapLighting());
}

JSValue SetMapLightingValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetMapLighting(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.map_lighting_all` - substitute on every lit draw, not just the map's own geometry.
// Off by default; see SetMapLightingAll in VkDraw.h for why that is a measurement.
JSValue GetMapLightingAll(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::MapLightingAll());
}

JSValue SetMapLightingAllValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetMapLightingAll(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.map_light_cull` - bin the map lights into a world-space grid instead of looping all of
// them per fragment (VkDraw.h). On by default, and **off must be bit-identical**: the grid drops
// only lights whose range cannot reach a cell, so it is exact. That A/B is the only thing that can
// catch a cell quietly missing a light, which otherwise looks like art rather than like a bug.
JSValue GetMapLightCull(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::MapLightCull());
}

JSValue SetMapLightCullValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetMapLightCull(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.sun_shadows` and its three knobs (VkDraw.h). A real shadow map from the sun, which is
// the first thing here the game never had at all - its own shadows are stencil volumes under the
// units and nothing else.
//
// `shadow_bias` and `shadow_extent` are knobs rather than constants on purpose: acne and
// peter-panning trade against each other, so sweeping them on a paused frame is how you find one.
// Their defaults are a sweep and not a guess (§4.59) - and `shadow_bias` is in **shadow texels**,
// which is what makes one value hold across every cascade, level and extent.
//
// `render.shadow_cascades` (1..4) is the A/B for cascading itself: 1 is §4.58's single map at the
// same texel density, so it toggles on one paused frame.
// `render.stencil_shadow` - draw the game's OWN blob shadow as well as the sun's map. Off, since
// otherwise a unit has both. Its three passes are identified by stencil being enabled, which
// §4.31 measured is exact on level01 and level02; this knob is what checks that on a level those
// measurements never covered.
JSValue GetStencilShadow(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::StencilShadow());
}

JSValue SetStencilShadowValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetStencilShadow(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetSunShadows(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::SunShadows());
}

JSValue SetSunShadowsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetSunShadows(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

#define GK_SHADOW_KNOB(name, setter)                                                               JSValue Get##setter(JSContext *ctx, JSValueConst) {                                                return JS_NewFloat64(ctx, vulkan::setter());                                                   }                                                                                                JSValue Set##setter##Value(JSContext *ctx, JSValueConst, JSValueConst value) {                     double v = 0.0;                                                                                  if (JS_ToFloat64(ctx, &v, value) < 0) {                                                            return JS_EXCEPTION;                                                                           }                                                                                                vulkan::Set##setter(static_cast<float>(v));                                                      return JS_UNDEFINED;                                                                           }

GK_SHADOW_KNOB(shadow_bias, ShadowBias)
GK_SHADOW_KNOB(shadow_strength, ShadowStrength)
GK_SHADOW_KNOB(shadow_extent, ShadowExtent)
// PCSS (VkDraw.h, notes 4.100). `render.shadow_softness` is the sun's angular radius as a
// tangent, **not** a filter width: the penumbra's world radius per world unit between a fragment
// and its blocker, so one value holds at any map resolution, cascade count or `shadow_extent`.
// 0 is off, and off takes the 3x3 path bit-identically.
GK_SHADOW_KNOB(shadow_softness, ShadowSoftness)
GK_SHADOW_KNOB(shadow_soft_min, ShadowSoftMin)
GK_SHADOW_KNOB(shadow_soft_max, ShadowSoftMax)
GK_SHADOW_KNOB(map_shadow_bias, MapShadowBias)
GK_SHADOW_KNOB(dynamic_shadow_bias, DynamicShadowBias)
// Ambient occlusion's five float knobs (§4.86). Same macro, because they are the same shape - a
// float in, a float out, clamped on the C++ side where a clamp is a real requirement.
GK_SHADOW_KNOB(ao_radius, AoRadius)
GK_SHADOW_KNOB(ao_screen_radius, AoScreenRadius)
GK_SHADOW_KNOB(ao_bias, AoBias)
GK_SHADOW_KNOB(ao_strength, AoStrength)
GK_SHADOW_KNOB(ao_direct, AoDirect)

// `render.ao` and its knobs (VkDraw.h, §4.86) - screen-space ambient occlusion with **no blur
// pass**, because its kernel is one fixed disc rather than a per-pixel randomised one. Off by
// default: the game never had it, so nothing here is a fidelity improvement and off is
// bit-identical to the build before it existed.
//
// `render.ao_debug` is not optional when tuning: the radius and the tap count are invisible in a
// shaded frame and obvious in the term itself.
JSValue GetAo(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::AmbientOcclusion());
}

JSValue SetAoValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetAmbientOcclusion(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetAoMapOnly(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::AoMapOnly());
}

JSValue SetAoMapOnlyValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetAoMapOnly(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetAoDebug(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::AoDebug());
}

JSValue SetAoDebugValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetAoDebug(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetAoTaps(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, vulkan::AoTaps());
}

JSValue SetAoTapsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  int32_t taps = 16;
  if (JS_ToInt32(ctx, &taps, value) < 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetAoTaps(taps);
  return JS_UNDEFINED;
}

// `render.map_shadows` and its two knobs (VkDraw.h, §4.61) - the static shadow atlas for the
// level's own STDLIGHT rig. Off by default, and the atlas is baked whether or not it is sampled,
// so this A/Bs on one paused frame at a 0.000 floor.
//
// `render.map_shadow_report` is not optional reading: a level with no map lights, an atlas that
// could not be created and a bake that has not finished all look identical from the screen.
JSValue GetMapShadows(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::MapShadows());
}

JSValue SetMapShadowsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetMapShadows(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetMapShadowReport(JSContext *ctx, JSValueConst) {
  const std::string report = vulkan::MapShadowReport();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

JSValue GetMapShadowIndirect(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::MapShadowIndirectEnabled());
}

JSValue SetMapShadowIndirectValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetMapShadowIndirect(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetMapShadowRate(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, vulkan::MapShadowRate());
}

JSValue SetMapShadowRateValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  int32_t rate = 1;
  if (JS_ToInt32(ctx, &rate, value) < 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetMapShadowRate(rate);
  return JS_UNDEFINED;
}

// `render.shadow_soft_blur` (VkDraw.h, notes 4.101) - the rotated screen-space mask against the
// inline lattice. It reads back **what was asked for** rather than what the device could build,
// because unlike `sun_shadow_indirect` the two paths do not produce the same image.
JSValue GetShadowSoftBlur(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::ShadowSoftBlur());
}

JSValue SetShadowSoftBlurValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetShadowSoftBlur(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetShadowSoftTaps(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, vulkan::ShadowSoftTaps());
}

JSValue SetShadowSoftTapsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  int32_t taps = 0;
  if (JS_ToInt32(ctx, &taps, value) < 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetShadowSoftTaps(taps);
  return JS_UNDEFINED;
}

JSValue GetShadowCascades(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, vulkan::ShadowCascades());
}

JSValue SetShadowCascadesValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  int32_t count = 1;
  if (JS_ToInt32(ctx, &count, value) < 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetShadowCascades(count);
  return JS_UNDEFINED;
}

// `render.map_light_gain` - the model's one free parameter. The fit puts it at 0.9 on level01 and
// 1.35 on level04 and level05, so 1.0 is the middle rather than an identity.
JSValue GetMapLightGain(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, vulkan::MapLightGain());
}

JSValue SetMapLightGainValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  double gain = 1.0;
  if (JS_ToFloat64(ctx, &gain, value) < 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetMapLightGain(static_cast<float>(gain));
  return JS_UNDEFINED;
}

// `render.map_light_report` - the level's own `STDLIGHT` rig, in world space (src/MapLights.h).
// Nothing renders from it yet; this is the reading that says the loader found the right file and
// put the lights in the right place.
//
// **The bounds comparison is the check that matters.** A light set whose world bounds sit inside
// the map's own is one whose unit scale and origin were applied correctly; get either wrong and
// the positions land orders of magnitude out, which no per-light number would make obvious.
JSValue GetMapLightReport(JSContext *ctx, JSValueConst) {
  const std::string text = MapLightReport();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.draw_range` - [first, last] of the frame's draw list to record, inclusive. The bisect
// for "which draw painted that pixel"; see SetDrawRange in VkDraw.h. Only meaningful on a paused
// frame, since an index is a position in a list the game rebuilds every frame.
JSValue GetDrawRange(JSContext *ctx, JSValueConst) {
  uint32_t first = 0;
  uint32_t last = 0;
  vulkan::GetDrawRange(first, last);
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  if (JS_SetPropertyUint32(ctx, array, 0, JS_NewInt64(ctx, first)) < 0 ||
      JS_SetPropertyUint32(ctx, array, 1, JS_NewInt64(ctx, last)) < 0) {
    JS_FreeValue(ctx, array);
    return JS_EXCEPTION;
  }
  return array;
}

JSValue SetDrawRangeValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  // null restores "all of them", so a session can get back without spelling out UINT32_MAX.
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    vulkan::SetDrawRange(0, UINT32_MAX);
    return JS_UNDEFINED;
  }
  uint32_t first = 0;
  uint32_t last = 0;
  JSValue a = JS_GetPropertyUint32(ctx, value, 0);
  JSValue b = JS_GetPropertyUint32(ctx, value, 1);
  const int ok = JS_ToUint32(ctx, &first, a) == 0 && JS_ToUint32(ctx, &last, b) == 0;
  JS_FreeValue(ctx, a);
  JS_FreeValue(ctx, b);
  if (!ok) {
    return JS_ThrowTypeError(ctx, "draw_range takes [first, last] or null");
  }
  vulkan::SetDrawRange(first, last);
  return JS_UNDEFINED;
}

// `render.draw_hide` - [first, last] to leave out, everything else drawn. See SetDrawHide.
JSValue GetDrawHide(JSContext *ctx, JSValueConst) {
  uint32_t first = 0;
  uint32_t last = 0;
  vulkan::GetDrawHide(first, last);
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  if (JS_SetPropertyUint32(ctx, array, 0, JS_NewInt64(ctx, first)) < 0 ||
      JS_SetPropertyUint32(ctx, array, 1, JS_NewInt64(ctx, last)) < 0) {
    JS_FreeValue(ctx, array);
    return JS_EXCEPTION;
  }
  return array;
}

JSValue SetDrawHideValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    vulkan::SetDrawHide(1, 0); // a window no index can fall in
    return JS_UNDEFINED;
  }
  uint32_t first = 0;
  uint32_t last = 0;
  JSValue a = JS_GetPropertyUint32(ctx, value, 0);
  JSValue b = JS_GetPropertyUint32(ctx, value, 1);
  const int ok = JS_ToUint32(ctx, &first, a) == 0 && JS_ToUint32(ctx, &last, b) == 0;
  JS_FreeValue(ctx, a);
  JS_FreeValue(ctx, b);
  if (!ok) {
    return JS_ThrowTypeError(ctx, "draw_hide takes [first, last] or null");
  }
  vulkan::SetDrawHide(first, last);
  return JS_UNDEFINED;
}

// `render.ref_range` / `render.ref_hide` - the same pair pointed at the runtime the capture
// layer forwards to, so the ORIGINAL can be bisected. See SetRefRange in D3D8Capture.h; the
// whole value is that these work in `d3d8` mode, where the Vulkan draw list does not exist.
JSValue MakePair(JSContext *ctx, uint32_t first, uint32_t last) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  if (JS_SetPropertyUint32(ctx, array, 0, JS_NewInt64(ctx, first)) < 0 ||
      JS_SetPropertyUint32(ctx, array, 1, JS_NewInt64(ctx, last)) < 0) {
    JS_FreeValue(ctx, array);
    return JS_EXCEPTION;
  }
  return array;
}

// Reads [first, last] out of `value`, or reports that it is not a pair. `null` is the caller's
// to interpret, because "all of them" and "none of them" are different defaults.
bool ReadPair(JSContext *ctx, JSValueConst value, uint32_t &first, uint32_t &last) {
  JSValue a = JS_GetPropertyUint32(ctx, value, 0);
  JSValue b = JS_GetPropertyUint32(ctx, value, 1);
  const bool ok = JS_ToUint32(ctx, &first, a) == 0 && JS_ToUint32(ctx, &last, b) == 0;
  JS_FreeValue(ctx, a);
  JS_FreeValue(ctx, b);
  return ok;
}

JSValue GetRefRange(JSContext *ctx, JSValueConst) {
  uint32_t first = 0;
  uint32_t last = 0;
  d3d8::GetRefRange(first, last);
  return MakePair(ctx, first, last);
}

JSValue SetRefRangeValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    d3d8::SetRefRange(0, UINT32_MAX);
    return JS_UNDEFINED;
  }
  uint32_t first = 0;
  uint32_t last = 0;
  if (!ReadPair(ctx, value, first, last)) {
    return JS_ThrowTypeError(ctx, "ref_range takes [first, last] or null");
  }
  d3d8::SetRefRange(first, last);
  return JS_UNDEFINED;
}

JSValue GetRefHide(JSContext *ctx, JSValueConst) {
  uint32_t first = 0;
  uint32_t last = 0;
  d3d8::GetRefHide(first, last);
  return MakePair(ctx, first, last);
}

JSValue SetRefHideValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    d3d8::SetRefHide(1, 0); // a window no index can fall in
    return JS_UNDEFINED;
  }
  uint32_t first = 0;
  uint32_t last = 0;
  if (!ReadPair(ctx, value, first, last)) {
    return JS_ThrowTypeError(ctx, "ref_hide takes [first, last] or null");
  }
  d3d8::SetRefHide(first, last);
  return JS_UNDEFINED;
}

// `render.frame_draws()` or `render.frame_draws(first, last)` - the capture layer's own list of
// the last complete frame's draws. Mirror-side, so it works in `d3d8` mode where there is no
// Vulkan draw list; see FormatFrameDraws in D3D8Capture.h.
JSValue FrameDraws(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  uint32_t first = 0;
  uint32_t last = UINT32_MAX;
  if (argc >= 1 && JS_ToUint32(ctx, &first, argv[0]) != 0) {
    return JS_EXCEPTION;
  }
  if (argc >= 2 && JS_ToUint32(ctx, &last, argv[1]) != 0) {
    return JS_EXCEPTION;
  }
  const std::string text = d3d8::FormatFrameDraws(first, last);
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.frame_lights` - the last complete frame's D3D lights, deduplicated by contents, with
// how many draws each reached and how many frames it has survived. See FormatFrameLights in
// D3D8Capture.h for what the two "distinct" counts mean together.
//
// **Unlike `frame_draws`, this is NOT mirror-side: it reads empty under `GKPLUS_RENDERER=d3d8`
// and `=d3d9`.** The comment here claimed otherwise until it was used to answer a question about
// the reference and reported `0 distinct lights` on a level with eight of them. The census is fed
// from `ResolveLightRun`, which allocates the frame's Vulkan light scratch and therefore only runs
// on the Vulkan draw path - the *mirror* (`State.lights`) is filled by `SetLight` in every mode,
// but nothing walks it otherwise. That is a limitation of the instrument and not of the game: the
// values are the game's own, so a reading taken under `vulkan` describes what `d3d8` was handed too.
JSValue GetFrameLights(JSContext *ctx, JSValueConst) {
  const std::string text = d3d8::FormatFrameLights();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.local_lights` - on by default. Off drops D3D's point and spot lights from the sum, so a
// paused A/B is exactly the pixels they reach: the ceiling on what shadowing them could be worth.
JSValue GetLocalLights(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::LocalLights());
}

JSValue SetLocalLightsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetLocalLights(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.local_light_window` - on by default (§4.70). Off restores D3D8's hard cutoff at Range,
// so the rim it draws round every point light can be A/B'd inside one paused frame.
JSValue GetLocalLightWindow(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::LocalLightWindow());
}

JSValue SetLocalLightWindowValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetLocalLightWindow(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.local_shadows` - a feature, on by default (§4.65). Shadows from D3D's own point and spot
// lights, out of the same static atlas the map lights use. Independent of `render.map_shadows`
// because they are two light systems sharing one image.
//
// `render.local_shadow_report` is not optional reading: a light that moves, a light the sixteen
// reserved slots had no room for, and a cube that has not been baked yet all look identical on
// screen, and the first of those is the design working rather than failing.
JSValue GetLocalShadows(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::LocalShadows());
}

JSValue SetLocalShadowsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetLocalShadows(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetLocalShadowReport(JSContext *ctx, JSValueConst) {
  const std::string report = vulkan::LocalShadowReport();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// `render.dynamic_shadows` - a feature, on by default (§4.66). The per-frame atlas: every D3D
// point and spot light in the frame gets a cube, cast by every opaque thing in the frame, so a
// light that MOVES casts and a unit or a barrel is a caster. It supersedes `local_shadows` for any
// light it has room for; that one is the fallback for the rest.
JSValue GetDynamicShadows(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::DynamicShadows());
}

JSValue SetDynamicShadowsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetDynamicShadows(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetDynArenaOnly(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::DynamicShadowArenaOnly());
}

JSValue SetDynArenaOnlyValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetDynamicShadowArenaOnly(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

// `render.local_shadow_taps` - the PCF radius for D3D's point and spot lights (§4.69). The one
// knob here that trades frame time directly for the artefact play reported, so it is worth being
// able to A/B in a session rather than only in a build.
JSValue GetLocalShadowTaps(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, vulkan::LocalShadowTaps());
}

JSValue SetLocalShadowTapsValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  int32_t radius = 0;
  if (JS_ToInt32(ctx, &radius, value) < 0) {
    return JS_EXCEPTION;
  }
  vulkan::SetLocalShadowTaps(radius);
  return JS_UNDEFINED;
}

JSValue GetDynMapOnly(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::DynamicShadowMapOnly());
}

JSValue SetDynMapOnlyValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetDynamicShadowMapOnly(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetDynSample(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::DynamicShadowSample());
}

JSValue SetDynSampleValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetDynamicShadowSample(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetDynamicShadowReport(JSContext *ctx, JSValueConst) {
  const std::string report = vulkan::DynamicShadowReport();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// Three of the four bisect knobs (§4.66), `dynamic_shadow_indirect` below being the fourth. A
// capture of a bake that hangs cannot exist, so the route to one is to walk the bake DOWN until it
// survives - and these are the axes it can be walked down. They are what proved the bake innocent:
// all lights, all faces and ONE caster runs at the control's frame rate, which is the whole pass
// structure with nothing in it. Kept because the next hang will want them.
#define GK_DYN_CAP(name, setter)                                                                   \
  JSValue Get##setter(JSContext *ctx, JSValueConst) {                                              \
    return JS_NewInt32(ctx, vulkan::setter());                                                     \
  }                                                                                                \
  JSValue Set##setter##Value(JSContext *ctx, JSValueConst, JSValueConst value) {                   \
    int32_t v = 0;                                                                                 \
    if (JS_ToInt32(ctx, &v, value) < 0) {                                                          \
      return JS_EXCEPTION;                                                                         \
    }                                                                                              \
    vulkan::Set##setter(v);                                                                        \
    return JS_UNDEFINED;                                                                           \
  }

GK_DYN_CAP(dynamic_shadow_max_lights, DynamicShadowMaxLights)
GK_DYN_CAP(dynamic_shadow_max_faces, DynamicShadowMaxFaces)
GK_DYN_CAP(dynamic_shadow_max_casters, DynamicShadowMaxCasters)
#undef GK_DYN_CAP

JSValue GetDynIndirect(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::DynamicShadowIndirect());
}

JSValue SetDynIndirectValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetDynamicShadowIndirect(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetSunCull(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::SunShadowCull());
}

JSValue SetSunCullValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetSunShadowCull(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetSunIndirect(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::SunShadowIndirect());
}

JSValue SetSunIndirectValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetSunShadowIndirect(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue GetSunShadowReport(JSContext *ctx, JSValueConst) {
  const std::string report = vulkan::SunShadowReport();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

JSValue GetDynCull(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, vulkan::DynamicShadowCull());
}

JSValue SetDynCullValue(JSContext *ctx, JSValueConst, JSValueConst value) {
  vulkan::SetDynamicShadowCull(JS_ToBool(ctx, value) != 0);
  return JS_UNDEFINED;
}

JSValue DrawInfo(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  uint32_t index = 0;
  if (argc < 1 || JS_ToUint32(ctx, &index, argv[0]) < 0) {
    return JS_ThrowTypeError(ctx, "draw_info(i) takes a draw index");
  }
  const std::string text = vulkan::DescribeDraw(index);
  if (text.empty()) {
    return JS_NULL;
  }
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.normal_census()` - how much of the frame carries smooth normals, which is what decides
// whether a PN-triangle stage can reach anything at all. See DescribeNormalCensus in VkDraw.h.
JSValue NormalCensus(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const std::string text = vulkan::DescribeNormalCensus();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// `render.seam_census()` - where two triangles meet, do their two PN patches agree? The
// measurement behind "tessellation tears the level mesh". See DescribeSeamCensus in VkDraw.h.
JSValue SeamCensus(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const std::string text = vulkan::DescribeSeamCensus();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue GetShadowState(JSContext *ctx, JSValueConst) {
  const std::string text = d3d8::FormatShadowState();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// Structured form, so a REPL session can do arithmetic on it rather than reading prose -
// e.g. `Object.keys(render.stats.fvfs).length` to size the vertex-format work.
JSValue GetStats(JSContext *ctx, JSValueConst) {
  const d3d8::CaptureStats &s = d3d8::Stats();

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }

  // Sets the property or unwinds the whole object; a partially-built stats object would be
  // worse than an exception, because it reads as a real measurement.
  bool failed = false;
  auto put = [&](const char *name, JSValue v) {
    if (failed) {
      JS_FreeValue(ctx, v);
      return;
    }
    if (JS_IsException(v) || JS_SetPropertyStr(ctx, obj, name, v) < 0) {
      failed = true;
    }
  };

  put("frames", JS_NewInt64(ctx, static_cast<int64_t>(s.frames)));
  put("draws_buffered", JS_NewInt64(ctx, static_cast<int64_t>(s.draws_buffered)));
  put("draws_user_ptr", JS_NewInt64(ctx, static_cast<int64_t>(s.draws_user_ptr)));
  put("max_draws_per_frame",
      JS_NewInt64(ctx, static_cast<int64_t>(s.max_draws_per_frame)));
  put("textures", JS_NewInt64(ctx, static_cast<int64_t>(s.textures_created)));
  put("vertex_buffers",
      JS_NewInt64(ctx, static_cast<int64_t>(s.vertex_buffers_created)));
  put("index_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.index_buffers_created)));
  put("vertex_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.vertex_bytes)));
  put("index_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.index_bytes)));
  put("max_texture_stage", JS_NewInt32(ctx, static_cast<int32_t>(s.max_stage_used)));
  put("max_light_index", JS_NewInt32(ctx, static_cast<int32_t>(s.max_light_index)));
  put("blocks_recorded", JS_NewInt64(ctx, static_cast<int64_t>(s.blocks_recorded)));
  put("blocks_opaque", JS_NewInt64(ctx, static_cast<int64_t>(s.blocks_opaque)));
  put("block_applies", JS_NewInt64(ctx, static_cast<int64_t>(s.block_applies)));
  put("block_states_total",
      JS_NewInt64(ctx, static_cast<int64_t>(s.block_states_total)));
  put("max_block_states", JS_NewInt64(ctx, static_cast<int64_t>(s.max_block_states)));
  put("distinct_materials", JS_NewInt64(ctx, static_cast<int64_t>(s.distinct_materials)));
  put("distinct_pipelines", JS_NewInt64(ctx, static_cast<int64_t>(s.distinct_pipelines)));
  put("max_materials_per_frame",
      JS_NewInt64(ctx, static_cast<int64_t>(s.max_materials_per_frame)));
  put("max_active_stages", JS_NewInt64(ctx, static_cast<int64_t>(s.max_active_stages)));
  put("opaque_block_applies",
      JS_NewInt64(ctx, static_cast<int64_t>(s.opaque_block_applies)));
  put("live_vertex_buffers",
      JS_NewInt64(ctx, static_cast<int64_t>(s.live_vertex_buffers)));
  put("live_index_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.live_index_buffers)));
  put("live_vertex_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.live_vertex_bytes)));
  put("live_index_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.live_index_bytes)));
  put("peak_live_vertex_bytes",
      JS_NewInt64(ctx, static_cast<int64_t>(s.peak_live_vertex_bytes)));
  put("peak_live_index_bytes",
      JS_NewInt64(ctx, static_cast<int64_t>(s.peak_live_index_bytes)));
  put("peak_live_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.peak_live_buffers)));
  put("locks", JS_NewInt64(ctx, static_cast<int64_t>(s.locks)));
  put("max_locked_bytes_per_frame",
      JS_NewInt64(ctx, static_cast<int64_t>(s.max_locked_bytes_per_frame)));
  put("foreign_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.foreign_buffers)));
  put("failed_uploads", JS_NewInt64(ctx, static_cast<int64_t>(s.failed_uploads)));
  put("unconvertible_buffers",
      JS_NewInt64(ctx, static_cast<int64_t>(s.unconvertible_buffers)));
  // §4.84. `readonly_unlock_vertices` is the one to read - these arrive in thousand-vertex runs,
  // so a count of unlocks understates what the skip avoids by three orders of magnitude.
  put("process_vertices", JS_NewInt64(ctx, static_cast<int64_t>(s.process_vertices)));
  put("process_vertices_vertices",
      JS_NewInt64(ctx, static_cast<int64_t>(s.process_vertices_vertices)));
  put("readonly_unlocks", JS_NewInt64(ctx, static_cast<int64_t>(s.readonly_unlocks)));
  put("readonly_unlock_vertices",
      JS_NewInt64(ctx, static_cast<int64_t>(s.readonly_unlock_vertices)));

  // The texture pixel path. The last three are the Phase 2c-iv question: each is a way
  // pixels reach a texture without IDirect3DTexture8::LockRect seeing them, so all three
  // being 0 is what says the bindless upload can be built on LockRect alone.
  put("live_textures", JS_NewInt64(ctx, static_cast<int64_t>(s.live_textures)));
  put("texture_lock_rects", JS_NewInt64(ctx, static_cast<int64_t>(s.texture_lock_rects)));
  put("texture_surface_levels",
      JS_NewInt64(ctx, static_cast<int64_t>(s.texture_surface_levels)));
  put("live_surfaces", JS_NewInt64(ctx, static_cast<int64_t>(s.live_surfaces)));
  put("surface_lock_rects", JS_NewInt64(ctx, static_cast<int64_t>(s.surface_lock_rects)));
  put("surface_texture_lock_rects",
      JS_NewInt64(ctx, static_cast<int64_t>(s.surface_texture_lock_rects)));
  put("surface_copy_rects", JS_NewInt64(ctx, static_cast<int64_t>(s.surface_copy_rects)));
  put("copy_rects_untracked",
      JS_NewInt64(ctx, static_cast<int64_t>(s.copy_rects_untracked)));
  put("copy_rects_partial", JS_NewInt64(ctx, static_cast<int64_t>(s.copy_rects_partial)));
  put("texture_render_targets",
      JS_NewInt64(ctx, static_cast<int64_t>(s.texture_render_targets)));
  put("texture_read_failures",
      JS_NewInt64(ctx, static_cast<int64_t>(s.texture_read_failures)));
  put("images_seeded", JS_NewInt64(ctx, static_cast<int64_t>(s.images_seeded)));
  put("textures_named", JS_NewInt64(ctx, static_cast<int64_t>(s.textures_named)));
  put("rim_records", JS_NewInt64(ctx, static_cast<int64_t>(s.rim_records)));
  put("rim_records_bound", JS_NewInt64(ctx, static_cast<int64_t>(s.rim_records_bound)));
  put("texture_updates", JS_NewInt64(ctx, static_cast<int64_t>(s.texture_updates)));
  put("resource_get_devices",
      JS_NewInt64(ctx, static_cast<int64_t>(s.resource_get_devices)));

  // fvfs and primitive_types map value -> how many draws used it; the state maps go to the
  // COUNT of distinct values seen, which is what says whether a state is a constant.
  put("fvfs", MapToObject(ctx, s.fvf_counts, CountValue));
  // `fvfs` above is keyed on the SetVertexShader handle. `converted_layouts` is what the vertex
  // converter was actually handed, which is a different set - the buffered path converts with
  // the FVF CreateVertexBuffer was given - and `specialized` says whether that layout has a
  // dispatch of its own or fell to the generic loop. See vulkan_renderer_notes.md §4.82.
  put("converted_layouts", LayoutCensusToArray(ctx));
  put("primitive_types", MapToObject(ctx, s.primitive_type_counts, CountValue));
  put("texture_formats", MapToObject(ctx, s.texture_formats, CountValue));
  put("texture_pools", MapToObject(ctx, s.texture_pools, CountValue));
  put("render_states", MapToObject(ctx, s.render_states, DistinctValue));
  put("stage_states", MapToObject(ctx, s.stage_states, DistinctValue));

  JSValue transforms = JS_NewArray(ctx);
  if (JS_IsException(transforms)) {
    failed = true;
  } else {
    uint32_t i = 0;
    for (const uint32_t state : s.transform_states) {
      if (JS_SetPropertyUint32(ctx, transforms, i++, JS_NewInt32(ctx, state)) < 0) {
        failed = true;
        break;
      }
    }
    put("transform_states", transforms);
  }

  if (failed) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue Reset(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  d3d8::ResetStats();
  return JS_UNDEFINED;
}

// Every texture image, with the `.rim` path it was acquired under. This is the surface a mod
// will eventually address the bindless table through - an asset name is the only identity a
// texture has that survives a restart and that someone can write down.
JSValue GetTextures(JSContext *ctx, JSValueConst) {
  const std::vector<vulkan::TextureImageInfo> images = vulkan::TextureImages();
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  uint32_t i = 0;
  for (const vulkan::TextureImageInfo &info : images) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, array);
      return entry;
    }
    JS_SetPropertyStr(ctx, entry, "index", JS_NewInt32(ctx, info.index));
    JS_SetPropertyStr(ctx, entry, "name",
                      JS_NewStringLen(ctx, info.name.c_str(), info.name.size()));
    JS_SetPropertyStr(ctx, entry, "width", JS_NewInt32(ctx, info.width));
    JS_SetPropertyStr(ctx, entry, "height", JS_NewInt32(ctx, info.height));
    JS_SetPropertyStr(ctx, entry, "levels", JS_NewInt32(ctx, info.levels));
    JS_SetPropertyStr(ctx, entry, "format", JS_NewInt64(ctx, info.d3d_format));
    JS_SetPropertyStr(ctx, entry, "bytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(info.bytes)));
    if (JS_SetPropertyUint32(ctx, array, i++, entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// The only check that the texture images hold the RIGHT bytes rather than merely holding
// bytes. Stalls the GPU once per mip level, so it is deliberately a call rather than a getter.
// `render.vertex_buffer_load` - see D3D8Capture.h. A getter rather than a call: it walks the live
// wrapper set and touches no D3D, so it is cheap enough to read from a panel.
JSValue GetVertexBufferLoad(JSContext *ctx, JSValueConst) {
  const std::string report = d3d8::FormatVertexBufferLoad(16);
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

JSValue GetDrawReport(JSContext *ctx, JSValueConst) {
  const std::string report = vulkan::FormatDrawStats();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// Arms a RenderDoc capture of the next frame. Needs GKPLUS_RENDERDOC set at launch, because
// RenderDoc has to be loaded before the Vulkan instance exists.
JSValue Capture(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  if (!vulkan::TriggerCapture()) {
    return JS_ThrowTypeError(
        ctx, "renderdoc is not loaded - relaunch with GKPLUS_RENDERDOC=1");
  }
  return JS_UNDEFINED;
}

JSValue GetCaptureStatus(JSContext *ctx, JSValueConst) {
  const std::string status = vulkan::FormatCaptureStatus();
  return JS_NewStringLen(ctx, status.c_str(), status.size());
}

JSValue VerifyTextures(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const std::string report = d3d8::VerifyTextureImages();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// `render.capture_batch(n)` - see CaptureStagingBatch in VkCapture.h. The unit is a staging
// batch rather than a frame because the uploads worth capturing happen during a level load,
// which presents nothing.
JSValue CaptureBatch(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  uint32_t batch = 0;
  if (argc < 1 || JS_ToUint32(ctx, &batch, argv[0]) < 0) {
    return JS_ThrowTypeError(ctx, "capture_batch(n) takes a batch index");
  }
  vulkan::CaptureStagingBatch(batch);
  return JS_UNDEFINED;
}

JSValue GetStagingWatch(JSContext *ctx, JSValueConst) {
  const std::string &log = vulkan::StagingWatchLog();
  return JS_NewStringLen(ctx, log.c_str(), log.size());
}

JSValue VerifyBuffers(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const std::string report = d3d8::VerifyBufferSlots();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// The layer's own words, not just a count. See the note on ValidationMessages in VkContext.h:
// DebugWrite is OutputDebugString, and a debugger makes Gunlok unusable, so the REPL is the
// only practical way to read these.
JSValue GetValidationMessages(JSContext *ctx, JSValueConst) {
  const std::vector<std::string> &messages = vulkan::ValidationMessages();
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  uint32_t i = 0;
  for (const std::string &message : messages) {
    JSValue text = JS_NewStringLen(ctx, message.data(), message.size());
    if (JS_IsException(text) || JS_SetPropertyUint32(ctx, array, i++, text) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue ClearValidation(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  vulkan::ClearValidationMessages();
  return JS_UNDEFINED;
}

// --- the Vulkan device -------------------------------------------------------
//
// Reading any of these brings the device up if it is not up already. That is deliberate:
// initialization cannot happen at DLL load (it does LoadLibrary("vulkan-1.dll"), which
// deadlocks under the loader lock), so *something* has to trigger it from the main thread,
// and a REPL query is the earliest useful trigger during bring-up.

JSValue GetVulkanReport(JSContext *ctx, JSValueConst) {
  const std::string text = vulkan::FormatCaps() + vulkan::FormatStats() +
                           vulkan::FormatResourceStats();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue GetVulkan(JSContext *ctx, JSValueConst) {
  vulkan::Initialize();

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  bool failed = false;
  auto put = [&](const char *name, JSValue v) {
    if (failed) {
      JS_FreeValue(ctx, v);
      return;
    }
    if (JS_IsException(v) || JS_SetPropertyStr(ctx, obj, name, v) < 0) {
      failed = true;
    }
  };

  put("status", JS_NewString(ctx, vulkan::InitResultName(vulkan::Status())));
  put("available", JS_NewBool(ctx, vulkan::Available()));
  const std::string &error = vulkan::LastError();
  put("error", error.empty() ? JS_NULL
                             : JS_NewStringLen(ctx, error.data(), error.size()));

  if (vulkan::Available()) {
    const vulkan::DeviceCaps &c = vulkan::Caps();
    put("device", JS_NewString(ctx, c.device_name.c_str()));
    put("discrete", JS_NewBool(ctx, c.discrete));
    put("api_version", JS_NewInt64(ctx, c.api_version));
    put("max_bindless_textures",
        JS_NewInt64(ctx, static_cast<int64_t>(c.max_bindless_textures)));
    put("max_push_constants",
        JS_NewInt64(ctx, static_cast<int64_t>(c.max_push_constants)));
    put("device_local_mb",
        JS_NewInt64(ctx, static_cast<int64_t>(c.device_local_bytes >> 20)));
    put("host_visible_device_local_mb",
        JS_NewInt64(ctx, static_cast<int64_t>(c.host_visible_device_local_bytes >> 20)));
    put("descriptor_indexing", JS_NewBool(ctx, c.descriptor_indexing));
    put("runtime_descriptor_array", JS_NewBool(ctx, c.runtime_descriptor_array));
    put("partially_bound", JS_NewBool(ctx, c.partially_bound));
    put("variable_descriptor_count", JS_NewBool(ctx, c.variable_descriptor_count));
    put("update_after_bind", JS_NewBool(ctx, c.update_after_bind));
    put("non_uniform_indexing", JS_NewBool(ctx, c.non_uniform_indexing));
    put("buffer_device_address", JS_NewBool(ctx, c.buffer_device_address));
    put("dynamic_rendering", JS_NewBool(ctx, c.dynamic_rendering));
    put("synchronization2", JS_NewBool(ctx, c.synchronization2));
  }

  const vulkan::RendererStats &r = vulkan::Stats();
  put("renderer_requested", JS_NewBool(ctx, vulkan::RendererRequested()));
  put("renderer_ready", JS_NewBool(ctx, r.ready));
  put("width", JS_NewInt64(ctx, r.width));
  put("height", JS_NewInt64(ctx, r.height));
  put("swapchain_images", JS_NewInt64(ctx, r.image_count));
  put("present_mode", JS_NewInt64(ctx, r.present_mode));
  put("frames_presented", JS_NewInt64(ctx, static_cast<int64_t>(r.frames_presented)));
  put("swapchain_rebuilds", JS_NewInt64(ctx, static_cast<int64_t>(r.swapchain_rebuilds)));
  put("acquire_failures", JS_NewInt64(ctx, static_cast<int64_t>(r.acquire_failures)));

  const vulkan::ResourceStats &m = vulkan::Resources();
  put("arenas_ready", JS_NewBool(ctx, m.ready));
  put("vertex_arena_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.vertex_arena_bytes)));
  put("index_arena_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.index_arena_bytes)));
  put("staging_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_bytes)));
  put("vertex_used", JS_NewInt64(ctx, static_cast<int64_t>(m.vertex_used)));
  put("index_used", JS_NewInt64(ctx, static_cast<int64_t>(m.index_used)));
  put("uploads", JS_NewInt64(ctx, static_cast<int64_t>(m.uploads)));
  put("uploaded_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.uploaded_bytes)));
  put("staging_wraps", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_wraps)));
  put("staging_flushes", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_flushes)));
  put("staging_stalls", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_stalls)));
  put("staging_reclaims", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_reclaims)));
  // Read these two as a DIFFERENCE across a window, not as session totals: whether the ring's
  // blocking matters depends on when it happens, not on how often (§4.63).
  put("staging_stall_us", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_stall_us)));
  put("staging_flush_us", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_flush_us)));
  put("dropped_uploads", JS_NewInt64(ctx, static_cast<int64_t>(m.dropped_uploads)));
  put("arena_exhausted", JS_NewInt64(ctx, static_cast<int64_t>(m.arena_exhausted)));
  put("images_live", JS_NewInt64(ctx, static_cast<int64_t>(m.images_live)));
  put("images_created", JS_NewInt64(ctx, static_cast<int64_t>(m.images_created)));
  put("image_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.image_bytes)));
  put("image_peak_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.image_peak_bytes)));
  put("image_uploads", JS_NewInt64(ctx, static_cast<int64_t>(m.image_uploads)));
  put("image_uploaded_bytes",
      JS_NewInt64(ctx, static_cast<int64_t>(m.image_uploaded_bytes)));
  put("unsupported_formats", JS_NewInt64(ctx, static_cast<int64_t>(m.unsupported_formats)));
  put("unaligned_rects", JS_NewInt64(ctx, static_cast<int64_t>(m.unaligned_rects)));
  put("image_uploads_dropped",
      JS_NewInt64(ctx, static_cast<int64_t>(m.image_uploads_dropped)));
  put("descriptor_capacity",
      JS_NewInt64(ctx, static_cast<int64_t>(m.descriptor_capacity)));
  put("descriptors_written",
      JS_NewInt64(ctx, static_cast<int64_t>(m.descriptors_written)));
  put("samplers_live", JS_NewInt64(ctx, static_cast<int64_t>(m.samplers_live)));
  put("descriptors_out_of_range",
      JS_NewInt64(ctx, static_cast<int64_t>(m.descriptors_out_of_range)));

  if (failed) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

const JSCFunctionListEntry RenderProps[] = {
    JS_CGETSET_DEF("captured", GetCaptured, nullptr),
    JS_CGETSET_DEF("report", GetReport, nullptr),
    JS_CGETSET_DEF("stats", GetStats, nullptr),
    JS_CGETSET_DEF("state", GetShadowState, nullptr),
    JS_CGETSET_DEF("topologies", GetTopologies, SetTopologies),
    JS_CGETSET_DEF("lighting", GetLighting, SetLighting),
    JS_CGETSET_DEF("skip_readonly_unlocks", GetSkipReadOnlyUnlocks,
                   SetSkipReadOnlyUnlocksValue),
    JS_CGETSET_DEF("software_process_vertices", GetSoftwareProcessVertices,
                   SetSoftwareProcessVerticesValue),
    JS_CGETSET_DEF("verify_process_vertices", GetVerifyProcessVertices,
                   SetVerifyProcessVerticesValue),
    JS_CGETSET_DEF("process_vertices_report", GetProcessVerticesReport, nullptr),
    JS_CGETSET_DEF("specular", GetSpecularValue, SetSpecularValue),
    JS_CGETSET_DEF("half_pixel", GetHalfPixel, SetHalfPixelValue),
    JS_CGETSET_DEF("rhw_depth_raw", GetRhwDepthRaw, SetRhwDepthRawValue),
    JS_CGETSET_DEF("viewport_rect", GetViewportRect, SetViewportRectValue),
    JS_CGETSET_DEF("offscreen", GetOffscreen, SetOffscreenValue),
    JS_CGETSET_DEF("present_linear", GetPresentLinear, SetPresentLinearValue),
    JS_CGETSET_DEF("shade_mode", GetShadeMode, SetShadeModeValue),
    // The PN-triangle amplification pass (§4.71).
    JS_CGETSET_DEF("tessellation", GetTessellation, SetTessellationValue),
    JS_CGETSET_DEF("tess_shadows", GetTessShadows, SetTessShadowsValue),
    JS_CGETSET_DEF("pn_seam_fix", GetSeamFix, SetSeamFixValue),
    JS_CGETSET_DEF("tess_set", GetTessSet, SetTessSetValue),
    JS_CGETSET_DEF("tess_edge_pixels", Getedge_pixels, Setedge_pixelsValue),
    JS_CGETSET_DEF("tess_max", Getmax_factor, Setmax_factorValue),
    JS_CGETSET_DEF("tess_min", Getmin_factor, Setmin_factorValue),
    JS_CGETSET_DEF("pn_strength", Getpn_strength, Setpn_strengthValue),
    JS_CGETSET_DEF("pn_flat_threshold", Getpn_flat_threshold, Setpn_flat_thresholdValue),
    JS_CGETSET_DEF("pn_max_offset", Getpn_max_offset, Setpn_max_offsetValue),
    JS_CGETSET_DEF("tess_shadow_factor", Getshadow_factor, Setshadow_factorValue),
    JS_CGETSET_DEF("msaa", GetMsaa, SetMsaaValue),
    JS_CGETSET_DEF("hdr", GetHdr, SetHdrValue),
    JS_CGETSET_DEF("linear_input", GetLinearInput, SetLinearInputValue),
    JS_CGETSET_DEF("tonemap", GetTonemap, SetTonemapValue),
    JS_CGETSET_DEF("exposure", GetExposure, SetExposureValue),
    JS_CGETSET_DEF("tonemap_knee", GetTonemapKnee, SetTonemapKneeValue),
    JS_CGETSET_DEF("tonemap_white", GetTonemapWhite, SetTonemapWhiteValue),
    JS_CGETSET_DEF("bloom", GetBloom, SetBloomValue),
    JS_CFUNC_DEF("bloom_layer", 2, BloomLayerFn),
    JS_CGETSET_DEF("bloom_layers", GetBloomLayers, nullptr),
    JS_CGETSET_DEF("stock", GetStock, SetStockValue),
    JS_CGETSET_DEF("per_pixel_lighting", GetPerPixelLighting, SetPerPixelLightingValue),
    JS_CGETSET_DEF("map_light_report", GetMapLightReport, nullptr),
    JS_CGETSET_DEF("map_lighting", GetMapLighting, SetMapLightingValue),
    JS_CGETSET_DEF("map_light_gain", GetMapLightGain, SetMapLightGainValue),
    JS_CGETSET_DEF("map_lighting_all", GetMapLightingAll, SetMapLightingAllValue),
    JS_CGETSET_DEF("map_light_cull", GetMapLightCull, SetMapLightCullValue),
    JS_CGETSET_DEF("sun_shadows", GetSunShadows, SetSunShadowsValue),
    JS_CGETSET_DEF("stencil_shadow", GetStencilShadow, SetStencilShadowValue),
    JS_CGETSET_DEF("shadow_bias", GetShadowBias, SetShadowBiasValue),
    JS_CGETSET_DEF("shadow_strength", GetShadowStrength, SetShadowStrengthValue),
    JS_CGETSET_DEF("shadow_extent", GetShadowExtent, SetShadowExtentValue),
    JS_CGETSET_DEF("shadow_cascades", GetShadowCascades, SetShadowCascadesValue),
    JS_CGETSET_DEF("shadow_softness", GetShadowSoftness, SetShadowSoftnessValue),
    JS_CGETSET_DEF("shadow_soft_min", GetShadowSoftMin, SetShadowSoftMinValue),
    JS_CGETSET_DEF("shadow_soft_max", GetShadowSoftMax, SetShadowSoftMaxValue),
    JS_CGETSET_DEF("shadow_soft_taps", GetShadowSoftTaps, SetShadowSoftTapsValue),
    JS_CGETSET_DEF("shadow_soft_blur", GetShadowSoftBlur, SetShadowSoftBlurValue),
    JS_CGETSET_DEF("map_shadows", GetMapShadows, SetMapShadowsValue),
    JS_CGETSET_DEF("map_shadow_bias", GetMapShadowBias, SetMapShadowBiasValue),
    JS_CGETSET_DEF("map_shadow_rate", GetMapShadowRate, SetMapShadowRateValue),
    JS_CGETSET_DEF("map_shadow_indirect", GetMapShadowIndirect, SetMapShadowIndirectValue),
    JS_CGETSET_DEF("map_shadow_report", GetMapShadowReport, nullptr),
    JS_CGETSET_DEF("ao", GetAo, SetAoValue),
    JS_CGETSET_DEF("ao_radius", GetAoRadius, SetAoRadiusValue),
    JS_CGETSET_DEF("ao_screen_radius", GetAoScreenRadius, SetAoScreenRadiusValue),
    JS_CGETSET_DEF("ao_bias", GetAoBias, SetAoBiasValue),
    JS_CGETSET_DEF("ao_strength", GetAoStrength, SetAoStrengthValue),
    JS_CGETSET_DEF("ao_direct", GetAoDirect, SetAoDirectValue),
    JS_CGETSET_DEF("ao_taps", GetAoTaps, SetAoTapsValue),
    JS_CGETSET_DEF("ao_map_only", GetAoMapOnly, SetAoMapOnlyValue),
    JS_CGETSET_DEF("ao_debug", GetAoDebug, SetAoDebugValue),
    JS_CGETSET_DEF("force_lod", GetForceLod, SetForceLodValue),
    JS_CGETSET_DEF("lighting_maps", GetLightingMaps, SetLightingMapsValue),
    JS_CGETSET_DEF("lighting_map_report", GetLightingMapReport, nullptr),
    JS_CGETSET_DEF("bump_scale", Getbump_scale, Setbump_scaleValue),
    JS_CGETSET_DEF("bump_diffuse", Getbump_diffuse, Setbump_diffuseValue),
    JS_CGETSET_DEF("bump_diffuse_limit", Getbump_diffuse_limit,
                   Setbump_diffuse_limitValue),
    JS_CGETSET_DEF("specular_scale", Getspecular_scale, Setspecular_scaleValue),
    JS_CGETSET_DEF("specular_from_diffuse", Getspecular_from_diffuse,
                   Setspecular_from_diffuseValue),
    JS_CGETSET_DEF("gloss_min", Getgloss_min, Setgloss_minValue),
    JS_CGETSET_DEF("gloss_max", Getgloss_max, Setgloss_maxValue),
    JS_CGETSET_DEF("chrome_scale", Getchrome_scale, Setchrome_scaleValue),
    JS_CGETSET_DEF("chrome_blur", Getchrome_blur, Setchrome_blurValue),
    JS_CGETSET_DEF("chrome_texgen", GetChromeTexgen, SetChromeTexgen),
    JS_CFUNC_DEF("probe", 5, ProbeQuad),
    JS_CFUNC_DEF("depth_probe", 5, DepthProbe),
    JS_CFUNC_DEF("viewport_probe", 5, ViewportProbe),
    JS_CFUNC_DEF("material_override", 2, MaterialOverrideFn),
    JS_CGETSET_DEF("material_overrides", GetMaterialOverrides, nullptr),
    JS_CFUNC_DEF("clear_material_overrides", 0, ClearMaterialOverridesFn),
    JS_CGETSET_DEF("draw_range", GetDrawRange, SetDrawRangeValue),
    JS_CGETSET_DEF("draw_hide", GetDrawHide, SetDrawHideValue),
    JS_CGETSET_DEF("ref_range", GetRefRange, SetRefRangeValue),
    JS_CGETSET_DEF("ref_hide", GetRefHide, SetRefHideValue),
    JS_CFUNC_DEF("draw_info", 1, DrawInfo),
    JS_CFUNC_DEF("normal_census", 0, NormalCensus),
    JS_CFUNC_DEF("seam_census", 0, SeamCensus),
    JS_CFUNC_DEF("frame_draws", 2, FrameDraws),
    JS_CGETSET_DEF("frame_lights", GetFrameLights, nullptr),
    JS_CGETSET_DEF("local_lights", GetLocalLights, SetLocalLightsValue),
    JS_CGETSET_DEF("local_light_window", GetLocalLightWindow,
                   SetLocalLightWindowValue),
    JS_CGETSET_DEF("local_shadows", GetLocalShadows, SetLocalShadowsValue),
    JS_CGETSET_DEF("dynamic_shadows", GetDynamicShadows, SetDynamicShadowsValue),
    JS_CGETSET_DEF("dynamic_shadow_arena_only", GetDynArenaOnly, SetDynArenaOnlyValue),
    JS_CGETSET_DEF("dynamic_shadow_map_only", GetDynMapOnly, SetDynMapOnlyValue),
    JS_CGETSET_DEF("local_shadow_taps", GetLocalShadowTaps, SetLocalShadowTapsValue),
    JS_CGETSET_DEF("dynamic_shadow_sample", GetDynSample, SetDynSampleValue),
    JS_CGETSET_DEF("dynamic_shadow_bias", GetDynamicShadowBias, SetDynamicShadowBiasValue),
    JS_CGETSET_DEF("dynamic_shadow_report", GetDynamicShadowReport, nullptr),
    JS_CGETSET_DEF("dynamic_shadow_indirect", GetDynIndirect, SetDynIndirectValue),
    JS_CGETSET_DEF("dynamic_shadow_cull", GetDynCull, SetDynCullValue),
    JS_CGETSET_DEF("sun_shadow_cull", GetSunCull, SetSunCullValue),
    JS_CGETSET_DEF("sun_shadow_indirect", GetSunIndirect, SetSunIndirectValue),
    JS_CGETSET_DEF("sun_shadow_report", GetSunShadowReport, nullptr),
    JS_CGETSET_DEF("dynamic_shadow_max_lights", GetDynamicShadowMaxLights,
                   SetDynamicShadowMaxLightsValue),
    JS_CGETSET_DEF("dynamic_shadow_max_faces", GetDynamicShadowMaxFaces,
                   SetDynamicShadowMaxFacesValue),
    JS_CGETSET_DEF("dynamic_shadow_max_casters", GetDynamicShadowMaxCasters,
                   SetDynamicShadowMaxCastersValue),
    JS_CGETSET_DEF("local_shadow_report", GetLocalShadowReport, nullptr),
    JS_CGETSET_DEF("draw_vertices", GetDrawVertices, SetDrawVerticesValue),
    JS_CGETSET_DEF("draw_state", GetDrawState, SetDrawStateValue),
    JS_CGETSET_DEF("draw_geometry", GetDrawGeometry, nullptr),
    JS_CFUNC_DEF("verify_state", 0, VerifyState),
    JS_CGETSET_DEF("vulkan", GetVulkan, nullptr),
    JS_CGETSET_DEF("vulkan_report", GetVulkanReport, nullptr),
    JS_CGETSET_DEF("validation", GetValidationMessages, nullptr),
    JS_CGETSET_DEF("textures", GetTextures, nullptr),
    JS_CGETSET_DEF("draws", GetDrawReport, nullptr),
    JS_CGETSET_DEF("vertex_buffer_load", GetVertexBufferLoad, nullptr),
    JS_CFUNC_DEF("reset", 0, Reset),
    JS_CFUNC_DEF("clear_validation", 0, ClearValidation),
    JS_CFUNC_DEF("verify_textures", 0, VerifyTextures),
    JS_CFUNC_DEF("verify_buffers", 0, VerifyBuffers),
    JS_CFUNC_DEF("capture_batch", 1, CaptureBatch),
    JS_CGETSET_DEF("staging_watch", GetStagingWatch, nullptr),
    JS_CGETSET_DEF("renderdoc", GetCaptureStatus, nullptr),
    JS_CFUNC_DEF("capture", 0, Capture),
};

} // namespace

JSValue NewRenderNamespace(JSContext *ctx) {
  return NewNamespace(ctx, RenderProps, static_cast<int>(std::size(RenderProps)));
}

} // namespace gk::js
