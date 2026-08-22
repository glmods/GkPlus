#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "RenderSettings.h"

#include "D3D8Capture.h"
#include "Settings.h"
#include "VkDraw.h"
#include "VkLighting.h"

#include <cstring>
#include <iterator>
#include <string>

namespace gk::render_settings {
namespace {

// --- the enumerated knobs ----------------------------------------------------

struct NamedValue {
  const char *name;
  std::uint32_t value;
};

// Index order is the shader's operator order (vulkan_renderer_notes.md 4.93).
const NamedValue Tonemaps[] = {
    {"clamp", 0}, {"rolloff", 1}, {"reinhard", 2},
    {"aces", 3},  {"filmic", 4},  {"agx", 5},
};

const NamedValue TessSets[] = {
    {"off", static_cast<std::uint32_t>(vulkan::TessSet::Off)},
    {"map", static_cast<std::uint32_t>(vulkan::TessSet::Map)},
    {"all", static_cast<std::uint32_t>(vulkan::TessSet::All)},
};

const char *NameOf(const NamedValue *table, std::size_t count,
                   std::uint32_t value, const char *fallback) {
  for (std::size_t i = 0; i < count; ++i) {
    if (table[i].value == value) {
      return table[i].name;
    }
  }
  return fallback;
}

bool ValueOf(const NamedValue *table, std::size_t count, const char *name,
             std::uint32_t *out) {
  if (!name) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (std::strcmp(table[i].name, name) == 0) {
      *out = table[i].value;
      return true;
    }
  }
  return false;
}

// --- accessor thunks ---------------------------------------------------------
//
// One pair per knob, because the table stores plain function pointers and almost
// none of the natives has the signature it needs: the numeric ones return and
// take `float`, `int` or `uint32_t` where the table speaks `double`, and the
// parameter blocks are struct members, which no function pointer can name.

#define GK_BOOL(id, getter, setter)                                            \
  bool GetB_##id() { return getter(); }                                        \
  void SetB_##id(bool v) { setter(v); }

#define GK_NUM(id, getter, setter, T)                                          \
  double GetN_##id() { return static_cast<double>(getter()); }                 \
  void SetN_##id(double v) { setter(static_cast<T>(v)); }

#define GK_TESS_MEMBER(id, member)                                             \
  double GetN_##id() { return vulkan::TessParams().member; }                   \
  void SetN_##id(double v) {                                                   \
    vulkan::MutableTessParams().member = static_cast<float>(v);                \
  }

#define GK_LIGHT_MEMBER(id, member)                                            \
  double GetN_##id() { return vulkan::LightingParams().member; }               \
  void SetN_##id(double v) {                                                   \
    vulkan::MutableLightingParams().member = static_cast<float>(v);            \
  }

// The plain booleans.
GK_BOOL(specular, d3d8::GetSpecular, d3d8::SetSpecular)
// Reads back EFFECTIVE rather than requested - false on a device with no
// tessellationShader however it was set - so it is restored from the file and never
// written back. One launch on a machine that cannot do it would otherwise erase the
// preference for every machine that can.
GK_BOOL(tessellation, vulkan::TessellationEnabled, vulkan::SetTessellationEnabled)
GK_BOOL(tess_shadows, vulkan::TessellationShadows, vulkan::SetTessellationShadows)
GK_BOOL(pn_seam_fix, vulkan::SplitCornerFix, vulkan::SetSplitCornerFix)
GK_BOOL(hdr, vulkan::Hdr, vulkan::SetHdr)
GK_BOOL(linear_input, vulkan::LinearInput, vulkan::SetLinearInput)
GK_BOOL(bloom, vulkan::Bloom, vulkan::SetBloom)
GK_BOOL(per_pixel_lighting, vulkan::PerPixelLighting, vulkan::SetPerPixelLighting)
GK_BOOL(map_lighting, vulkan::MapLighting, vulkan::SetMapLighting)
GK_BOOL(map_lighting_all, vulkan::MapLightingAll, vulkan::SetMapLightingAll)
GK_BOOL(map_light_cull, vulkan::MapLightCull, vulkan::SetMapLightCull)
GK_BOOL(sun_shadows, vulkan::SunShadows, vulkan::SetSunShadows)
GK_BOOL(stencil_shadow, vulkan::StencilShadow, vulkan::SetStencilShadow)
GK_BOOL(shadow_soft_blur, vulkan::ShadowSoftBlur, vulkan::SetShadowSoftBlur)
GK_BOOL(map_shadows, vulkan::MapShadows, vulkan::SetMapShadows)
GK_BOOL(map_shadow_indirect, vulkan::MapShadowIndirectEnabled, vulkan::SetMapShadowIndirect)
GK_BOOL(ao, vulkan::AmbientOcclusion, vulkan::SetAmbientOcclusion)
GK_BOOL(ao_map_only, vulkan::AoMapOnly, vulkan::SetAoMapOnly)
GK_BOOL(lighting_maps, vulkan::LightingMaps, vulkan::SetLightingMaps)
GK_BOOL(local_lights, vulkan::LocalLights, vulkan::SetLocalLights)
GK_BOOL(local_shadows, vulkan::LocalShadows, vulkan::SetLocalShadows)
GK_BOOL(dynamic_shadows, vulkan::DynamicShadows, vulkan::SetDynamicShadows)

// chrome_texgen is a bool on the lighting parameter block, so it needs its own pair.
bool GetB_chrome_texgen() { return vulkan::LightingParams().chrome_texgen; }
void SetB_chrome_texgen(bool v) {
  vulkan::MutableLightingParams().chrome_texgen = v;
}

// The numeric knobs.
// MsaaWanted, not Msaa: a write is only adopted at the top of the next frame and the
// count is clamped to what the device offers, so reading the effective value back would
// persist the clamp instead of the request.
GK_NUM(msaa, vulkan::MsaaWanted, vulkan::SetMsaa, std::uint32_t)
GK_NUM(shadow_cascades, vulkan::ShadowCascades, vulkan::SetShadowCascades, int)
GK_NUM(shadow_soft_taps, vulkan::ShadowSoftTaps, vulkan::SetShadowSoftTaps, int)
GK_NUM(map_shadow_rate, vulkan::MapShadowRate, vulkan::SetMapShadowRate, int)
GK_NUM(ao_taps, vulkan::AoTaps, vulkan::SetAoTaps, int)
GK_NUM(local_shadow_taps, vulkan::LocalShadowTaps, vulkan::SetLocalShadowTaps, int)
GK_NUM(shadow_bias, vulkan::ShadowBias, vulkan::SetShadowBias, float)
GK_NUM(shadow_strength, vulkan::ShadowStrength, vulkan::SetShadowStrength, float)
GK_NUM(shadow_extent, vulkan::ShadowExtent, vulkan::SetShadowExtent, float)
GK_NUM(shadow_softness, vulkan::ShadowSoftness, vulkan::SetShadowSoftness, float)
GK_NUM(shadow_soft_min, vulkan::ShadowSoftMin, vulkan::SetShadowSoftMin, float)
GK_NUM(shadow_soft_max, vulkan::ShadowSoftMax, vulkan::SetShadowSoftMax, float)
GK_NUM(map_shadow_bias, vulkan::MapShadowBias, vulkan::SetMapShadowBias, float)
GK_NUM(dynamic_shadow_bias, vulkan::DynamicShadowBias, vulkan::SetDynamicShadowBias, float)
GK_NUM(ao_radius, vulkan::AoRadius, vulkan::SetAoRadius, float)
GK_NUM(ao_screen_radius, vulkan::AoScreenRadius, vulkan::SetAoScreenRadius, float)
GK_NUM(ao_bias, vulkan::AoBias, vulkan::SetAoBias, float)
GK_NUM(ao_strength, vulkan::AoStrength, vulkan::SetAoStrength, float)
GK_NUM(ao_direct, vulkan::AoDirect, vulkan::SetAoDirect, float)
GK_NUM(exposure, vulkan::Exposure, vulkan::SetExposure, float)
GK_NUM(tonemap_knee, vulkan::TonemapKnee, vulkan::SetTonemapKnee, float)
GK_NUM(tonemap_white, vulkan::TonemapWhite, vulkan::SetTonemapWhite, float)
GK_NUM(map_light_gain, vulkan::MapLightGain, vulkan::SetMapLightGain, float)

// The PN-triangle parameter block.
GK_TESS_MEMBER(tess_edge_pixels, edge_pixels)
GK_TESS_MEMBER(tess_max, max_factor)
GK_TESS_MEMBER(tess_min, min_factor)
GK_TESS_MEMBER(pn_strength, pn_strength)
GK_TESS_MEMBER(pn_flat_threshold, pn_flat_threshold)
GK_TESS_MEMBER(pn_max_offset, pn_max_offset)
GK_TESS_MEMBER(tess_shadow_factor, shadow_factor)

// The lighting-map parameter block.
GK_LIGHT_MEMBER(bump_scale, bump_scale)
GK_LIGHT_MEMBER(bump_diffuse, bump_diffuse)
GK_LIGHT_MEMBER(bump_diffuse_limit, bump_diffuse_limit)
GK_LIGHT_MEMBER(specular_scale, specular_scale)
GK_LIGHT_MEMBER(specular_from_diffuse, specular_from_diffuse)
GK_LIGHT_MEMBER(gloss_min, gloss_min)
GK_LIGHT_MEMBER(gloss_max, gloss_max)
GK_LIGHT_MEMBER(chrome_scale, chrome_scale)
GK_LIGHT_MEMBER(chrome_blur, chrome_blur)

// The two enumerated knobs, as strings - which is what belongs in the file, since
// an index would silently mean something else if the operator list ever grew.
const char *GetS_tonemap() {
  return NameOf(Tonemaps, std::size(Tonemaps), vulkan::Tonemap(), "clamp");
}

bool SetS_tonemap(const char *name) {
  std::uint32_t op = 0;
  if (!ValueOf(Tonemaps, std::size(Tonemaps), name, &op)) {
    return false;
  }
  vulkan::SetTonemap(op);
  return true;
}

const char *GetS_tess_set() {
  return NameOf(TessSets, std::size(TessSets),
                static_cast<std::uint32_t>(vulkan::TessellationSet()), "off");
}

bool SetS_tess_set(const char *name) {
  std::uint32_t set = 0;
  if (!ValueOf(TessSets, std::size(TessSets), name, &set)) {
    return false;
  }
  vulkan::SetTessellationSet(static_cast<vulkan::TessSet>(set));
  return true;
}

// --- the bloom layers --------------------------------------------------------
//
// Fifteen values behind one function rather than fifteen knobs, so they need
// their own thunks: `render.bloom_layer(i, spec)` reads and writes a whole
// `BloomLayer` at a time. Flattened to `core.render.bloom_layer.<i>.<field>`
// rather than stored as an array, because an array is a leaf in
// `json::Document` - there is nothing a single field's write could go through
// (see src/JsSettings.cpp, where an array handed to a script is frozen for
// exactly that reason), so a flat key per field is what makes a partial update
// expressible at all.
//
// This is also what let `examples/render-panel.mjs` stop carrying its own
// restore-and-persist pass: it was the last thing outside this file that owned a
// renderer setting.

const NamedValue BloomBlends[] = {
    {"off", static_cast<std::uint32_t>(vulkan::BloomBlend::Off)},
    {"add", static_cast<std::uint32_t>(vulkan::BloomBlend::Add)},
    {"screen", static_cast<std::uint32_t>(vulkan::BloomBlend::Screen)},
    {"max", static_cast<std::uint32_t>(vulkan::BloomBlend::Max)},
};

#define GK_BLOOM_FLOAT(idx, field)                                             \
  double GetN_bloom##idx##_##field() {                                         \
    return vulkan::BloomLayerAt(idx).field;                                    \
  }                                                                            \
  void SetN_bloom##idx##_##field(double v) {                                   \
    vulkan::BloomLayer layer = vulkan::BloomLayerAt(idx);                      \
    layer.field = static_cast<float>(v);                                       \
    vulkan::SetBloomLayer(idx, layer);                                         \
  }

#define GK_BLOOM_BLEND(idx)                                                    \
  const char *GetS_bloom##idx##_blend() {                                      \
    return NameOf(BloomBlends, std::size(BloomBlends),                         \
                  static_cast<std::uint32_t>(vulkan::BloomLayerAt(idx).blend), \
                  "add");                                                      \
  }                                                                            \
  bool SetS_bloom##idx##_blend(const char *name) {                             \
    std::uint32_t blend = 0;                                                   \
    if (!ValueOf(BloomBlends, std::size(BloomBlends), name, &blend)) {          \
      return false;                                                            \
    }                                                                          \
    vulkan::BloomLayer layer = vulkan::BloomLayerAt(idx);                      \
    layer.blend = static_cast<vulkan::BloomBlend>(blend);                      \
    vulkan::SetBloomLayer(idx, layer);                                         \
    return true;                                                               \
  }

#define GK_BLOOM_LAYER(idx)                                                    \
  GK_BLOOM_FLOAT(idx, threshold)                                               \
  GK_BLOOM_FLOAT(idx, knee)                                                    \
  GK_BLOOM_FLOAT(idx, radius)                                                  \
  GK_BLOOM_FLOAT(idx, intensity)                                               \
  GK_BLOOM_BLEND(idx)

GK_BLOOM_LAYER(0)
GK_BLOOM_LAYER(1)
GK_BLOOM_LAYER(2)

#undef GK_BLOOM_LAYER
#undef GK_BLOOM_BLEND
#undef GK_BLOOM_FLOAT
#undef GK_BOOL
#undef GK_NUM
#undef GK_TESS_MEMBER
#undef GK_LIGHT_MEMBER

// --- the table ---------------------------------------------------------------

enum class Kind { Bool, Number, String };

struct Knob {
  const char *name; // the leaf under `core.render.`, spelled as `render.<name>`
  Kind kind;
  // A launch-time override that outranks the file. Checked here rather than in
  // each setter so the rule cannot depend on which setter latched its own
  // env-read flag.
  const char *env;
  // False for a knob whose getter reports the EFFECTIVE value rather than the
  // requested one: restore it from the file, never write it back.
  bool sync;
  bool (*get_bool)();
  void (*set_bool)(bool);
  double (*get_number)();
  void (*set_number)(double);
  const char *(*get_string)();
  bool (*set_string)(const char *);
};

const Knob Knobs[] = {
    {.name = "specular",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_specular,
     .set_bool = SetB_specular},
    {.name = "tess.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = false,
     .get_bool = GetB_tessellation,
     .set_bool = SetB_tessellation},
    {.name = "tess.shadows",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_tess_shadows,
     .set_bool = SetB_tess_shadows},
    {.name = "tess.seam_fix",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_pn_seam_fix,
     .set_bool = SetB_pn_seam_fix},
    {.name = "hdr.enabled",
     .kind = Kind::Bool,
     .env = "GKPLUS_VK_HDR",
     .sync = true,
     .get_bool = GetB_hdr,
     .set_bool = SetB_hdr},
    {.name = "hdr.linear_input",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_linear_input,
     .set_bool = SetB_linear_input},
    {.name = "bloom.enabled",
     .kind = Kind::Bool,
     .env = "GKPLUS_VK_BLOOM",
     .sync = true,
     .get_bool = GetB_bloom,
     .set_bool = SetB_bloom},
    {.name = "per_pixel_lighting",
     .kind = Kind::Bool,
     .env = "GKPLUS_VK_PER_PIXEL_LIGHTING",
     .sync = true,
     .get_bool = GetB_per_pixel_lighting,
     .set_bool = SetB_per_pixel_lighting},
    {.name = "map_light.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_map_lighting,
     .set_bool = SetB_map_lighting},
    {.name = "map_light.all",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_map_lighting_all,
     .set_bool = SetB_map_lighting_all},
    {.name = "map_light.cull",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_map_light_cull,
     .set_bool = SetB_map_light_cull},
    {.name = "sun_shadow.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_sun_shadows,
     .set_bool = SetB_sun_shadows},
    {.name = "stencil_shadow",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_stencil_shadow,
     .set_bool = SetB_stencil_shadow},
    {.name = "sun_shadow.soft_blur",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_shadow_soft_blur,
     .set_bool = SetB_shadow_soft_blur},
    {.name = "map_shadow.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_map_shadows,
     .set_bool = SetB_map_shadows},
    {.name = "map_shadow.indirect",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_map_shadow_indirect,
     .set_bool = SetB_map_shadow_indirect},
    {.name = "ao.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_ao,
     .set_bool = SetB_ao},
    {.name = "ao.map_only",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_ao_map_only,
     .set_bool = SetB_ao_map_only},
    {.name = "lighting_map.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_lighting_maps,
     .set_bool = SetB_lighting_maps},
    {.name = "local_light.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_local_lights,
     .set_bool = SetB_local_lights},
    {.name = "local_light.shadows",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_local_shadows,
     .set_bool = SetB_local_shadows},
    {.name = "dynamic_shadow.enabled",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_dynamic_shadows,
     .set_bool = SetB_dynamic_shadows},
    {.name = "lighting_map.chrome_texgen",
     .kind = Kind::Bool,
     .env = nullptr,
     .sync = true,
     .get_bool = GetB_chrome_texgen,
     .set_bool = SetB_chrome_texgen},
    {.name = "msaa",
     .kind = Kind::Number,
     .env = "GKPLUS_VK_MSAA",
     .sync = true,
     .get_number = GetN_msaa,
     .set_number = SetN_msaa},
    {.name = "sun_shadow.cascades",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_cascades,
     .set_number = SetN_shadow_cascades},
    {.name = "sun_shadow.soft_taps",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_soft_taps,
     .set_number = SetN_shadow_soft_taps},
    {.name = "map_shadow.rate",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_map_shadow_rate,
     .set_number = SetN_map_shadow_rate},
    {.name = "ao.taps",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_ao_taps,
     .set_number = SetN_ao_taps},
    {.name = "local_light.shadow_taps",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_local_shadow_taps,
     .set_number = SetN_local_shadow_taps},
    {.name = "sun_shadow.bias",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_bias,
     .set_number = SetN_shadow_bias},
    {.name = "sun_shadow.strength",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_strength,
     .set_number = SetN_shadow_strength},
    {.name = "sun_shadow.extent",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_extent,
     .set_number = SetN_shadow_extent},
    {.name = "sun_shadow.softness",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_softness,
     .set_number = SetN_shadow_softness},
    {.name = "sun_shadow.soft_min",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_soft_min,
     .set_number = SetN_shadow_soft_min},
    {.name = "sun_shadow.soft_max",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_shadow_soft_max,
     .set_number = SetN_shadow_soft_max},
    {.name = "map_shadow.bias",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_map_shadow_bias,
     .set_number = SetN_map_shadow_bias},
    {.name = "dynamic_shadow.bias",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_dynamic_shadow_bias,
     .set_number = SetN_dynamic_shadow_bias},
    {.name = "ao.radius",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_ao_radius,
     .set_number = SetN_ao_radius},
    {.name = "ao.screen_radius",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_ao_screen_radius,
     .set_number = SetN_ao_screen_radius},
    {.name = "ao.bias",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_ao_bias,
     .set_number = SetN_ao_bias},
    {.name = "ao.strength",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_ao_strength,
     .set_number = SetN_ao_strength},
    {.name = "ao.direct",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_ao_direct,
     .set_number = SetN_ao_direct},
    {.name = "hdr.exposure",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_exposure,
     .set_number = SetN_exposure},
    {.name = "hdr.knee",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_tonemap_knee,
     .set_number = SetN_tonemap_knee},
    {.name = "hdr.white",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_tonemap_white,
     .set_number = SetN_tonemap_white},
    {.name = "map_light.gain",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_map_light_gain,
     .set_number = SetN_map_light_gain},
    {.name = "tess.edge_pixels",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_tess_edge_pixels,
     .set_number = SetN_tess_edge_pixels},
    {.name = "tess.max",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_tess_max,
     .set_number = SetN_tess_max},
    {.name = "tess.min",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_tess_min,
     .set_number = SetN_tess_min},
    {.name = "tess.pn_strength",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_pn_strength,
     .set_number = SetN_pn_strength},
    {.name = "tess.pn_flat_threshold",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_pn_flat_threshold,
     .set_number = SetN_pn_flat_threshold},
    {.name = "tess.pn_max_offset",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_pn_max_offset,
     .set_number = SetN_pn_max_offset},
    {.name = "tess.shadow_factor",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_tess_shadow_factor,
     .set_number = SetN_tess_shadow_factor},
    {.name = "lighting_map.bump_scale",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bump_scale,
     .set_number = SetN_bump_scale},
    {.name = "lighting_map.bump_diffuse",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bump_diffuse,
     .set_number = SetN_bump_diffuse},
    {.name = "lighting_map.bump_diffuse_limit",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bump_diffuse_limit,
     .set_number = SetN_bump_diffuse_limit},
    {.name = "lighting_map.specular_scale",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_specular_scale,
     .set_number = SetN_specular_scale},
    {.name = "lighting_map.specular_from_diffuse",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_specular_from_diffuse,
     .set_number = SetN_specular_from_diffuse},
    {.name = "lighting_map.gloss_min",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_gloss_min,
     .set_number = SetN_gloss_min},
    {.name = "lighting_map.gloss_max",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_gloss_max,
     .set_number = SetN_gloss_max},
    {.name = "lighting_map.chrome_scale",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_chrome_scale,
     .set_number = SetN_chrome_scale},
    {.name = "lighting_map.chrome_blur",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_chrome_blur,
     .set_number = SetN_chrome_blur},
    {.name = "hdr.tonemap",
     .kind = Kind::String,
     .env = nullptr,
     .sync = true,
     .get_string = GetS_tonemap,
     .set_string = SetS_tonemap},
    {.name = "tess.set",
     .kind = Kind::String,
     .env = nullptr,
     .sync = true,
     .get_string = GetS_tess_set,
     .set_string = SetS_tess_set},
    {.name = "bloom_layer.0.threshold",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom0_threshold,
     .set_number = SetN_bloom0_threshold},
    {.name = "bloom_layer.0.knee",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom0_knee,
     .set_number = SetN_bloom0_knee},
    {.name = "bloom_layer.0.radius",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom0_radius,
     .set_number = SetN_bloom0_radius},
    {.name = "bloom_layer.0.intensity",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom0_intensity,
     .set_number = SetN_bloom0_intensity},
    {.name = "bloom_layer.0.blend",
     .kind = Kind::String,
     .env = nullptr,
     .sync = true,
     .get_string = GetS_bloom0_blend,
     .set_string = SetS_bloom0_blend},
    {.name = "bloom_layer.1.threshold",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom1_threshold,
     .set_number = SetN_bloom1_threshold},
    {.name = "bloom_layer.1.knee",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom1_knee,
     .set_number = SetN_bloom1_knee},
    {.name = "bloom_layer.1.radius",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom1_radius,
     .set_number = SetN_bloom1_radius},
    {.name = "bloom_layer.1.intensity",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom1_intensity,
     .set_number = SetN_bloom1_intensity},
    {.name = "bloom_layer.1.blend",
     .kind = Kind::String,
     .env = nullptr,
     .sync = true,
     .get_string = GetS_bloom1_blend,
     .set_string = SetS_bloom1_blend},
    {.name = "bloom_layer.2.threshold",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom2_threshold,
     .set_number = SetN_bloom2_threshold},
    {.name = "bloom_layer.2.knee",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom2_knee,
     .set_number = SetN_bloom2_knee},
    {.name = "bloom_layer.2.radius",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom2_radius,
     .set_number = SetN_bloom2_radius},
    {.name = "bloom_layer.2.intensity",
     .kind = Kind::Number,
     .env = nullptr,
     .sync = true,
     .get_number = GetN_bloom2_intensity,
     .set_number = SetN_bloom2_intensity},
    {.name = "bloom_layer.2.blend",
     .kind = Kind::String,
     .env = nullptr,
     .sync = true,
     .get_string = GetS_bloom2_blend,
     .set_string = SetS_bloom2_blend},
};

std::string PathOf(const Knob &knob) {
  return std::string("core.render.") + knob.name;
}

bool Overridden(const Knob &knob) {
  if (knob.env == nullptr) {
    return false;
  }
  // GetEnvironmentVariableA rather than getenv, matching the rest of the
  // codebase: the CRT copies the environment block at startup, so getenv would
  // miss a variable set after that - and this runs from an intercepted file open
  // rather than from DllMain.
  char value[8];
  return ::GetEnvironmentVariableA(knob.env, value, sizeof value) > 0;
}

} // namespace

void ApplyStored() {
  for (const Knob &knob : Knobs) {
    // An environment variable outranks the file, and it does so per knob rather
    // than wholesale: GKPLUS_VK_MSAA pins the sample count and says nothing
    // about HDR.
    if (Overridden(knob)) {
      continue;
    }
    const std::string path = PathOf(knob);
    switch (knob.kind) {
    case Kind::Bool:
      // The live value as the fallback, so a knob with no stored setting keeps
      // the build's default rather than being reset to false.
      knob.set_bool(settings::GetBool(path.c_str(), knob.get_bool()));
      break;
    case Kind::Number:
      knob.set_number(settings::GetNumber(path.c_str(), knob.get_number()));
      break;
    case Kind::String: {
      const std::string stored =
          settings::GetString(path.c_str(), knob.get_string());
      // The setter refuses a name it does not know rather than approximating
      // one, and the refusal is dropped here on purpose: an unrecognised
      // operator is more likely a newer GkPlus's setting than a typo, and the
      // knob keeps its default instead of the launch failing over it.
      knob.set_string(stored.c_str());
      break;
    }
    }
  }
}

void SyncToSettings() {
  for (const Knob &knob : Knobs) {
    if (!knob.sync || Overridden(knob)) {
      continue;
    }
    const std::string path = PathOf(knob);
    switch (knob.kind) {
    case Kind::Bool: {
      const bool live = knob.get_bool();
      // Compared before writing, so an unchanged frame dirties nothing. Without
      // this the store would be dirty on every frame and SaveSettled's
      // fifteen-second cap would turn into a file write every fifteen seconds
      // for the life of the process. The fallback is the opposite of `live`, so
      // an absent key always reads as a difference and gets written once.
      if (settings::GetBool(path.c_str(), !live) != live) {
        settings::SetBool(path.c_str(), live);
      }
      break;
    }
    case Kind::Number: {
      const double live = knob.get_number();
      // An exact compare rather than an epsilon: both sides came from the same
      // double, so a knob nothing has touched reproduces its own bits.
      if (settings::GetNumber(path.c_str(), live + 1.0) != live) {
        settings::SetNumber(path.c_str(), live);
      }
      break;
    }
    case Kind::String: {
      const char *live = knob.get_string();
      if (settings::GetString(path.c_str(), "") != live) {
        settings::SetString(path.c_str(), live);
      }
      break;
    }
    }
  }
}

const char *TonemapName(std::uint32_t op) {
  return NameOf(Tonemaps, std::size(Tonemaps), op, "clamp");
}

bool TonemapFromName(const char *name, std::uint32_t *out) {
  return ValueOf(Tonemaps, std::size(Tonemaps), name, out);
}

const char *BloomBlendName(vulkan::BloomBlend blend) {
  return NameOf(BloomBlends, std::size(BloomBlends),
                static_cast<std::uint32_t>(blend), "add");
}

bool BloomBlendFromName(const char *name, vulkan::BloomBlend *out) {
  std::uint32_t value = 0;
  if (!ValueOf(BloomBlends, std::size(BloomBlends), name, &value)) {
    return false;
  }
  *out = static_cast<vulkan::BloomBlend>(value);
  return true;
}

const char *TessSetName(vulkan::TessSet set) {
  return NameOf(TessSets, std::size(TessSets), static_cast<std::uint32_t>(set),
                "off");
}

bool TessSetFromName(const char *name, vulkan::TessSet *out) {
  std::uint32_t value = 0;
  if (!ValueOf(TessSets, std::size(TessSets), name, &value)) {
    return false;
  }
  *out = static_cast<vulkan::TessSet>(value);
  return true;
}

} // namespace gk::render_settings
