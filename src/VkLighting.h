#pragma once

// Lighting maps: a companion `.dds` beside a `.RIM`, giving one texture a bump/metallic/roughness
// response the game never had.
//
// **The rule is a file name and nothing else.** When the renderer has an image named
// `bitmaps\lava.rim`, this looks for `bitmaps\lava lighting.dds` and, if a mod (or the install)
// provides one, uploads it into a bindless slot of its own and hands the slot to every material
// whose **stage 0** is that texture. Nothing registers anything, no script call is needed, and a
// texture with no companion costs one hash lookup, once, for the life of the session.
//
// The three channels, and the semantics are the caller's, not PBR's:
//
//   * **R - bump.** A height field. The normal is derived at draw time from its gradient against
//     a tangent frame taken from the fragment's own derivatives, so no tangents are needed in the
//     vertex format (which has none, and would cost 12 bytes of the canonical 48 to gain).
//   * **G - metallic.** The *intensity* of the highlight, straight through: 0 is no highlight at
//     all, 1 is the full one. It is not a metal/dielectric switch and nothing about the base
//     colour changes with it.
//   * **B - roughness.** The *sharpness* of the highlight, as the specular exponent: 0 is the
//     sharpest (`gloss_max`, 256 by default) and 1 the broadest (`gloss_min`, 4). Interpolated in
//     log2 so the knob behaves evenly across the range, which linear interpolation of an exponent
//     does not.
//
// The alpha channel is unread. DXT1 is the natural format for a map like this - three channels,
// no alpha, 8 bytes a block - and is what a modder should reach for first.
//
// --- Why it lives here, and not in the capture layer ------------------------------------------
//
// The capture layer records what D3D was asked to do; a lighting map is a texture D3D never sees.
// So this walks `TextureImages()` - the bindless registry, which already carries every image's
// `.rim` name (§4.14) - and creates its own images beside them. `TextureRegistryGeneration()` is
// the trigger, exactly as it is for the material override: an image created, destroyed or named
// is the only event that can change what a name-keyed table resolves to.
//
// That also makes this the first thing in the renderer to own a texture the capture layer never
// saw, which is what `vulkan_renderer_plan.md`'s §5 named as the missing half of a mod-facing
// material system.
//
// --- Where the file comes from ------------------------------------------------------------------
//
// A `.rim` name is relative to the `graphics` GLDir (`bitmaps\lava.rim`, `Ground\escape Ground
// vary 4.RIM`), so the companion is looked for at `graphics/<that>`, and at the bare name too for
// a texture acquired under another category. Both spellings of the suffix are accepted -
// `<stem> lighting.dds` and `<stem>_lighting.dds` - because a space in an asset name is normal in
// this game and awkward in a build script.
//
// **The mod VFS is consulted first, then the real file.** That is the order every other asset
// takes (`src/Vfs`), so a lighting map ships inside `gkplus/mods/<mod>.zip` under `graphics/...`
// like anything else - and dropping the file next to the `.RIM` works too, for authoring.
//
// The decoder is `src/Dds`, unchanged and shared with the engine-facing codec. That means DXT5 is
// refused **by name** here as well, which is stricter than Vulkan needs (BC3 exists) and is the
// right trade: one parser, one set of rules, and a file that works in a lighting map also works as
// a `BMPNAMES` texture.

#include <cstdint>
#include <string>

namespace gk {
namespace vulkan {

// Spelled the same as VkDraw.h's, which is what a resolved slot is compared against. Not included
// from there: this header is the lower half and nothing in VkDraw.h is needed to state its API.
constexpr uint32_t kNoLightingMap = 0xffffffffu;

// The global knobs. Every one is a *rendering* parameter rather than a per-material one, because
// the map's own channels carry everything that varies per surface - these say what the channels
// mean in world terms, which is the kind of thing a mod tunes once and a session A/Bs.
struct LightingMapParams {
  // How far the height gradient may tilt the normal. 1.0 makes a full-contrast step over one
  // texel a 45-degree slope; the scale is per *texel*, so a higher-resolution map is gentler for
  // the same artwork, which is what keeps the knob meaningful across mip levels.
  float bump_scale = 1.0f;
  // How much of the derived normal reaches the *diffuse* term, as a blend between the game's own
  // per-vertex lighting (0) and the per-pixel relighting the bumped normal implies (1). Highlights
  // alone leave a bump invisible wherever `metallic` is 0, which is most of a typical map - so
  // this defaults to on. It is applied as a ratio against the same sum computed with the geometric
  // normal, which is exact only where the material's ambient and diffuse colours agree; see
  // world.slang.
  float bump_diffuse = 1.0f;
  // A global multiplier on the added highlight.
  //
  // **0.25 and not 1.0, and the number comes from the game.** Gunlok over-drives its lights:
  // level02's key directional light has a diffuse colour of `4.0 4.0 4.0`, four times white. With
  // the highlight taking that colour (see `specular_from_diffuse`), a fully-metallic texel at 1.0
  // saturates to white over a whole floor - measured, it is a blown-out sheet rather than a
  // highlight. 0.25 is the reciprocal of that intensity, so `metallic = 1` reaches exactly 1.0 at
  // normal incidence instead of 4.0.
  float specular_scale = 0.25f;
  // Which colour the highlight reflects: the light's own **specular** colour at 0, its **diffuse**
  // colour at 1, and a blend between.
  //
  // Defaulting to the diffuse colour is not the faithful choice and is the useful one, because of
  // a measurement: on level02, *every* light reaching the ground authors `specular 0 0 0` - the
  // directional key light and all three of the point lights around it. Keyed on the authored
  // specular, the metallic channel would therefore do nothing at all over most of a level, which
  // reads as a broken feature rather than as a faithful one. (The 49 `light` sections in the
  // shipped `.gsh` set do all carry a specular colour, so this is about which lights a level
  // actually places, not about the format.)
  //
  // Set it to 0 for the game's own answer - which is what the fixed-function specular term in
  // world.slang uses, and is why the two can differ.
  float specular_from_diffuse = 1.0f;
  // The specular exponent at roughness 1 and at roughness 0.
  float gloss_min = 4.0f;
  float gloss_max = 256.0f;

  // --- the chrome pass ------------------------------------------------------------------------
  //
  // Gunlok's own sphere-map effect, which these three make answer to a lighting map. A
  // `reflective` role (48 of the shipped ones) is drawn a second time with `units\reflect.rim`
  // ADDSIGNED over its own texture; that pass's stage 0 is the same texture as the base pass, so
  // its lighting map resolves with no extra plumbing. See vulkan_renderer_notes.md.
  //
  // **All three are inert without a companion file.** A texture with no lighting map takes the
  // engine's own path exactly, which is what keeps a stock install looking stock.

  // How much of the chrome stage's result survives, before the metallic channel scales it. 1.0 is
  // the engine's own strength; 0 removes the reflection and leaves the base pass alone, which is
  // the A/B.
  float chrome_scale = 1.0f;
  // How far the roughness channel may blur the reflection, in mip levels. B already means the
  // highlight's *sharpness*, and a rough surface reflecting less sharply is that same statement
  // made in the sphere map. 0 samples the top level whatever B says.
  //
  // **DEFAULTS TO 0 BECAUSE IT DOES NOT WORK YET, and the reason is not established.** Measured on
  // level02 at the settled camera, against a 0.010/255 repeatability floor: sweeping this 0 -> 8
  // moves 0.006, and 0 -> 20 moves 0.012. Both are noise. What has been ruled out:
  //
  //   * the texture - `units\reflect.RIM` is 256x256 and carries all 5 of its mip levels;
  //   * the channel - the unit maps' B averages 0.58, so the LOD being asked for is ~2.3 at a
  //     `chrome_blur` of 4, not 0;
  //   * the push constant - `chrome_scale` and `chrome_texgen` sit either side of it in the same
  //     block and both demonstrably work, so the block's layout is not adrift;
  //   * the branch - `chrome_texgen` only takes effect when the shade ran, and it works, which
  //     proves the same `shade.roughness` this reads is being written.
  //
  // What is left, and where to start: the chrome stage's sampler is D3DTEXF_NONE (`render.state`
  // shows it as `filt 220`), which AcquireSampler reproduces as `maxLod = 0.25`, so a bias cannot
  // reach a second level. `MippedSamplerFor` below exists to hand that stage a mipping variant and
  // *should* fix it - but the bindless table still shows **4 samplers** with this set to 20, so
  // that swap is not allocating one and nobody has yet found out why. Turn it on and check the
  // sampler count first.
  float chrome_blur = 0.0f;
  // Whether the chrome stage's texture coordinate is *generated* from the bumped normal rather
  // than read from the mesh's second UV set.
  //
  // On, because the generated coordinate is the one that responds to the height field at all - an
  // authored UV1 is fixed to the surface and cannot. The formula is the engine's own: its
  // map-wide chrome variant asks for D3DTSS_TCI_CAMERASPACENORMAL with no texture transform, so
  // this is what Gunlok already does for that path, applied per pixel and to a normal the map has
  // tilted. Off falls back to UV1 and reproduces the engine's per-unit path exactly.
  bool chrome_texgen = true;
};

struct LightingMapStats {
  uint64_t names_probed = 0;    // distinct `.rim` names looked up, ever
  uint64_t maps_found = 0;      // ... of which a file was found for
  uint64_t load_failures = 0;   // found, and refused or undecodable. See DescribeLightingMaps
  uint64_t images_created = 0;
  uint64_t bytes_uploaded = 0;
  uint64_t resolves = 0;        // rebuilds of the base-image -> lighting-image table
  // Draws whose material carried a lighting map, cumulative. Per draw and not per interned
  // material, for the reason `overridden_draws` is: the question it answers is "did the frame
  // paint anything with this", and a map on an asset the camera cannot see resolves and reports
  // itself exactly like one that is on screen (§4.44).
  uint64_t materials_lit = 0;
  // Draws recognised as the engine's chrome pass *and* carrying a lighting map, cumulative and
  // per draw for the same reason `materials_lit` is. A reflective unit whose texture ships no map
  // is not counted, because nothing about its draw changed.
  uint64_t chrome_draws = 0;
};

// On by default. Off makes every material intern exactly as it did before this existed, so the
// A/B on a paused frame is the feature and nothing else - the same rule `render.shade_mode` and
// `render.lighting` follow. Turning it back on forces a resolve, since nothing was loaded while
// it was off.
void SetLightingMaps(bool enabled);
bool LightingMaps();

const LightingMapParams &LightingParams();
LightingMapParams &MutableLightingParams();

// The lighting image for a base image's bindless slot, or kNoLightingMap. Cheap: a bounds check
// and an array read.
uint32_t LightingMapFor(uint32_t texture_index);

// Whether a bindless slot is the engine's sphere-map texture, `units\reflect.rim` - i.e. whether
// a material sampling it at stage 1 is Gunlok's chrome pass. That name is the whole interface:
// nothing registers a chrome material, and the two the engine builds are the only things that
// ever sample it (`ChromeMaterialUnit` and `ChromeMaterialMap`, both built once from WinMain).
//
// False whenever lighting maps are off, so `render.lighting_maps = false` reverts the chrome pass
// to the engine's own behaviour along with everything else - the A/B stays one switch.
bool IsChromeTexture(uint32_t texture_index);

// Whether a bindless slot is one of *ours*. The material override resolves its keys by walking
// every live image's name, and a lighting map's name contains its base texture's - so without
// this, `render.material_override("lava", ...)` would find `bitmaps\lava lighting.dds` as
// readily as `bitmaps\lava.rim` and could swap one in as a replacement texture.
bool IsLightingImage(uint32_t texture_index);

// Rebuilds the table if the texture registry has changed since the last one. Called per draw, and
// costs a comparison against a counter until something is created, destroyed or named.
void EnsureLightingMapsResolved();

// Destroys every image this owns. Called from ShutdownDraw, before the resources go.
void ShutdownLightingMaps();

const LightingMapStats &LightingMapCounters();
// The draw path counts into these, the same way it does into MutableDrawStats().
LightingMapStats &MutableLightingCounters();

// Every name probed and what came of it, plus the knobs. The readback matters for the same reason
// the material override's does: a map that was never found and a map that is found and paints
// nothing look identical from outside, and the failure here is *silent by design* - a texture
// with no companion file is the normal case, not an error.
std::string DescribeLightingMaps();

} // namespace vulkan
} // namespace gk
