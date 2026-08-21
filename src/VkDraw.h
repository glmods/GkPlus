#pragma once

// Phase 3: the draws. The capture layer reduces each of the game's draw calls to a DrawItem and
// pushes it here; the renderer walks the list once per frame.
//
// This is the first thing that READS what Phase 2 built - the vertex arena, the index arena, the
// canonical vertex layout, the texture images, the samplers and the bindless set. Everything
// before it was verified by counters and a readback; this is verified by looking at the screen.
//
// Two shapes are deliberately absent from the first cut, and both are counted rather than
// silently dropped (see DrawStats):
//
//   * user-pointer draws (DrawPrimitiveUP / DrawIndexedPrimitiveUP), which have no buffer to
//     pull from at all - they are text, particles and the in-game menus;
//   * non-indexed and non-triangle-list primitives.
//
// vulkan_renderer_notes.md section 2 is the design this converged on, and both halves are here:
// a draw's per-draw data lives in a GpuDrawRecord array (§4.26) and its texture stages in a
// GpuMaterial array (§4.30), each interned once a frame. The push constants carry four
// addresses and three indices, and nothing else.

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace gk {
namespace vulkan {

// --- the shader ABI ---------------------------------------------------------------------------
//
// Three arrays a frame - lights, draw records and materials - all in host-visible scratch that
// rotates with the vertex scratch, all reached by device address. They exist because the
// 128-byte push constant block was full at 120 (§4.19) and the light sum needs a per-draw
// matrix, five material colours and a variable number of lights - none of which fits.
//
// **Every field is a float4 or an array of them**, for the layout reason the vertex struct
// already documents: a float4 sits at a 16-byte stride under std140, std430 and scalar rules
// alike, so the C++ struct and the Slang one agree structurally rather than by betting on which
// rule Slang picked. The four trailing uints are the one exception and they are last, where
// every rule puts them at the same four offsets.

// One D3D8 light, as the vertex shader consumes it. `spot.w` carries D3DLIGHTTYPE as a float -
// 1 point, 2 spot, 3 directional - rather than a uint, so the whole struct is one kind of
// component and the layout question does not arise a second time.
struct GpuLight {
  // xyz in world space. **w is this light's slot in the PER-FRAME atlas** (§4.66), or -1. It wins
  // over `direction[3]` below where both are set, because the per-frame cube is strictly the
  // better answer - it holds the units and props the static one cannot - and a light with only
  // the static one falls back to it.
  float position[4];
  // xyz in world space, already normalised. **w is this light's slot in the static shadow atlas**
  // (§4.65), or -1 for a light that has none - which is the normal case for a light that moves,
  // for one the atlas had no room for, and for every light before the bake reaches it. Carried as
  // a float in the same spare lane `GpuMapLight::axis[3]` uses, and read back with a cast.
  float direction[4];
  float diffuse[4];
  float specular[4];
  float ambient[4];
  float attenuation[4]; // att0, att1, att2, range
  float spot[4];        // cos(theta/2), cos(phi/2), falloff, type
};
static_assert(sizeof(GpuLight) == 112, "the scratch stride is part of the shader ABI");

// One of the LEVEL's own lights - the `STDLIGHT` rig that baked its per-vertex colours, which the
// shipped engine loads and never reads (src/MapLights.h). Nothing to do with `GpuLight` above:
// that is a D3D light the game set this frame, this is authoring data D3D has never seen.
//
// Everything is premultiplied on the CPU so the shader does the least possible per light per
// pixel, and it is three float4s for the layout reason the rest of this ABI is.
//
// The model each field serves is fitted, not chosen - see vulkan_renderer_notes.md §4.54, which
// is also why `spread` is absent: as a cone exponent it makes the fit *worse* on every level, so
// carrying it would be carrying a field to ignore.
struct GpuMapLight {
  float position[4];  // xyz world space, w = range in world units
  // rgb = the light's colour times its brightness, already multiplied. w carries
  // `engine_light_flags` as a float - only bit 0x4 (LFlag_Omni) is read, and it decides whether
  // the cone below applies at all.
  float colour[4];
  // xyz = **row 2** of the orientation matrix, which is the light's axis. Rows 0 and 1 both fit
  // worse and negating this one collapses the fit, which is what identifies it (§4.54). Unused
  // on an omni light.
  //
  // w is this light's slot in the **static shadow atlas** (§4.61), or -1 for a light that has
  // none - which is the normal case for a level with more lights than the atlas holds, and for
  // every light before the bake reaches it. Carried as a float because the whole struct is
  // float4s for the layout reason above, and read back with a cast.
  float axis[4];
};
static_assert(sizeof(GpuMapLight) == 48, "the scratch stride is part of the shader ABI");

// The static shadow atlas for the map lights (§4.61): one cube per light, six 90-degree faces in
// a square grid of tiles.
//
// **The atlas size is what costs memory and the face size is what buys capacity**, which is the
// arithmetic that picks both. 4096 is the `maxImageDimension2D` every Vulkan device guarantees and
// `D16_UNORM` is a mandatory depth format, so the image is **32 MB** whatever the face size; 64
// then leaves room for 682 lights, against level01's 686 - the most of any shipped level. A level
// with more than the atlas holds shadows the most influential and leaves the rest as they were.
constexpr uint32_t kMapShadowFace = 64;
constexpr uint32_t kMapShadowAtlas = 4096;
constexpr uint32_t kMapShadowTilesPerRow = kMapShadowAtlas / kMapShadowFace;
constexpr uint32_t kMapShadowSlots = kMapShadowTilesPerRow * kMapShadowTilesPerRow / 6;

// The last few slots of that atlas belong to **D3D's own point and spot lights** (§4.65) - the
// game's runtime lights, which are a different system from the `STDLIGHT` rig above and had no
// shadow at all. They are a handful: 5 on level02's start, 12 at its fire camera, 4 on level04,
// 1 on level05 and none on prison, measured with `render.frame_lights`.
//
// **Sixteen, and they come off the map lights' budget** rather than out of a second image. Only
// level01 notices - it is the one level with more `STDLIGHT`s than the atlas holds (686 against
// 682), so it goes from refusing 4 of them to refusing 20, all at the bottom of the
// `brightness * range^3` order. Its D3D lights are worth nothing measurable on screen and its map
// lights are worth 0.195 MAD, so trading sixteen of the weakest for the whole feature is the
// right way round.
constexpr uint32_t kLocalShadowSlots = 16;
constexpr uint32_t kMapShadowLightSlots = kMapShadowSlots - kLocalShadowSlots;

// The **per-frame** atlas (§4.66), which is the other half of the same feature and the answer to
// everything the static one above cannot do: a light that moves, and a caster that moves.
//
// It is rebuilt from scratch every frame out of the frame's own draw list, so it needs no identity
// for a light and no test for whether one is static - which is the whole of §4.65's key table,
// stability gate and eviction, gone. An explosion's light rides a particle and gets a cube; a unit
// and a barrel are casters like anything else.
//
// **256-texel faces where the static atlas has 64**, because there are two orders of magnitude
// fewer of them: 42 slots against 682, so the blockiness §4.61 lists as its first regret is
// affordable to fix here. 16x16 tiles leaves 256 / 6 = 42 lights - against a measured frame peak
// of 11 with every effect in the game firing at once.
//
// **4096² and not 2048²** (§4.69). It started at 2048² with 128-texel faces, which play reported
// as low-res and jagged; the two halves of that are separate defects and this is the first - a
// 90-degree face across 128 texels is coarse enough to see the texels. Keeping all 42 slots at
// twice the face means four times the image, so 32 MB a slice against 8, and 64 MB for the ring
// against the sun's own 66. The other half was the single-tap compare, and is `local_shadow_taps`.
constexpr uint32_t kDynShadowFace = 256;
constexpr uint32_t kDynShadowAtlas = 4096;
constexpr uint32_t kDynShadowTilesPerRow = kDynShadowAtlas / kDynShadowFace;
constexpr uint32_t kDynShadowSlots = kDynShadowTilesPerRow * kDynShadowTilesPerRow / 6;

// **The atlas is RINGED, one image per frame in flight**, and that is the same defect the indirect
// batch had one object over. This is the first thing in this renderer to write an image every
// frame *and* sample it in the same frame: §4.61's atlas is written once per level and thereafter
// only read, which is exactly why it never needed this. With one image, frame N+1's bake declares
// `oldLayout = UNDEFINED` - it discards the contents - while frame N's world pass is still
// sampling them.
//
// Two images rather than one taller one, because the ring then costs **nothing in the shader**:
// each slice owns a bindless slot, and `GpuFrameData::dyn_shadow_texture` already carries whichever
// index the bake just wrote. 8 MB a slice, against the sun's 66 MB.
constexpr uint32_t kDynShadowRing = 2;

// How many cascades the sun's shadow map is split into, and therefore how many tiles the atlas
// carries. Four, in a 2x2 grid - the atlas is square either way, and a fifth would take it to a
// 3x2 that wastes a third of the image. `render.shadow_cascades` selects how many are LIVE, from
// 1 (which is §4.58's single map, at the same texel density) to this.
constexpr uint32_t kMaxShadowCascades = 4;

// Everything that is the same for every draw in a frame, in one place.
//
// **The push constant block ran out.** It reached exactly 128 bytes with the light grid (§4.56) -
// which is the minimum every Vulkan device guarantees, and AMD commonly reports exactly that - so
// the next thing to need space had to displace something. A frame-uniform value has no business
// being copied into a 128-byte block once per draw anyway; §4.56's own note said so.
//
// The three ADDRESSES are here for the same reason the knobs are: they are constant for a frame,
// and only the map-lighting path reads them. The four hot addresses stay in the push, where the
// vertex shader reaches them without a dependent load.
//
// Laid out with the 8-byte members FIRST, where no rule can disagree about their alignment, and
// 4-byte scalars after - the same reasoning that made GpuMaterial sixteen named uints rather than
// an array.
struct GpuFrameData {
  uint64_t map_lights;    // the level's own STDLIGHT rig for this frame (src/MapLights.h)
  uint64_t light_grid;    // the world-space grid, and its header
  uint64_t light_indices;
  // The LOD probe (§4.34) and the lighting-map knobs (src/VkLighting.h).
  float force_lod;
  float bump_scale;
  float bump_diffuse;
  float specular_scale;
  float gloss_min;
  float gloss_max;
  float specular_from_diffuse;
  float chrome_scale;
  float chrome_blur;
  float chrome_texgen;
  // Per-pixel lighting (§4.52) and runtime map lighting (§4.55).
  float per_pixel_lighting;
  float map_light_gain;
  float map_ambience;
  uint32_t map_light_count;
  uint32_t map_flags; // bit 0 on, bit 1 substitute everywhere, bit 2 the grid is usable
  // The sun's shadow map. `shadow_texture` is kNoTexture when there is none, which is the one
  // test the fragment shader makes - so a device without the pass, a level without a sun and the
  // knob being off all look the same from the shader and none of them is a branch of its own.
  uint32_t shadow_texture;
  uint32_t shadow_sampler;
  float shadow_strength;    // 0 no shadow, 1 fully dark - see SetShadowStrength for why 1
  float shadow_texel;       // 1 / the TILE's size, so a PCF tap is in a cascade's own uv
  float shadow_z_near;      // light-space z at the near plane, shared by every cascade
  float shadow_z_span;      // ... and the near-to-far distance, likewise shared
  uint32_t shadow_cascades; // how many of `cascades` below are live, 0 for none
  // The map lights' static shadow atlas (§4.61). `kNoTexture` is the one test, exactly as
  // `shadow_texture` above is for the sun's - "the knob is off", "the atlas could not be created"
  // and "this level has no map lights" are one state in the shader.
  uint32_t map_shadow_texture;
  uint32_t map_shadow_sampler;
  // How far along the surface normal a lookup is moved before it is projected, **in atlas texels
  // at that fragment's own distance from the light** (see SetMapShadowBias). The bias for this
  // atlas is a normal offset rather than a depth one because a 64-texel cube face is coarse - a
  // texel is `distance / 32` world units - so the depth error across one texel is dominated by
  // the surface's slope, which is exactly what an offset along the normal cancels.
  float map_shadow_offset;
  // The per-frame atlas (§4.66). `kNoTexture` is "off", "could not be created" and "no local light
  // in this frame" alike, which is the one test the shader makes - the same convention as the two
  // above. Its bias is separate because its face is 128 texels where the static one's is 64, so
  // one texel is half the world distance and the two knees do not have to coincide.
  uint32_t dyn_shadow_texture;
  uint32_t dyn_shadow_sampler;
  float dyn_shadow_offset;
  // The PCF radius for D3D's point and spot lights, in texels: 0 a single tap, 1 a 3x3, 2 a 5x5
  // (§4.69). Not named for either atlas because it applies to whichever one serves them - the
  // per-frame cube where a light has a slot in it, the static one where it does not.
  // **This was `pad1`**, so it costs no size change and `cascades` stays on its 16-byte boundary.
  uint32_t local_shadow_taps;
  // --- the PN-triangle amplification pass (§4.71) ---------------------------------------------
  //
  // **Eight scalars, and two of them are padding on purpose.** `cascades` below has to start on a
  // 16-byte boundary or its rows are not where any layout rule puts them, and 24 bytes of
  // addresses plus thirty 4-byte scalars already reached exactly 144 - so anything added here
  // comes in multiples of four. Six were wanted; eight is what keeps the array aligned.
  float tess_edge_pixels;  // the screen-space edge length a factor aims for
  float tess_max;          // the ceiling, clamped to maxTessellationGenerationLevel on the CPU
  float tess_min;          // a floor, so a uniform factor can be forced for an A/B
  // How much of the PN tangent term survives: 1 the full construction, 0 exactly linear (and so
  // exactly the untessellated surface, however high the factors go). The A/B for the shape.
  float pn_strength;
  // A normalised tangent term below this is snapped to **exactly zero**, which makes its corner
  // flat rather than nearly flat.
  //
  // The census measured why this is needed: on level02 only 6.4% of map triangles have all three
  // corners flat, so the construction's free hard-edge identity covers far less of this game than
  // it looks like it should, and a mean term of 0.094 domes a typical edge by ~3% of its length.
  // Snapping restores the identity for the near-flat majority.
  //
  // **It stays watertight**, which is the reason it is a threshold on this quantity and not on
  // anything else: the term depends only on `(Pi, Pj, Ni)`, so two triangles sharing an edge test
  // and snap it identically. A threshold on, say, the triangle's own flatness would not.
  float pn_flat_threshold;
  // The render target, so the hull shader can turn an NDC edge into pixels. Frame-uniform because
  // every draw this pass tessellates renders to the whole target - the one draw in the game that
  // sets a sub-rectangle is the upgrade screen (§4.47), which is not map geometry.
  float target_width;
  float target_height;
  // How far a PN control point may leave its chord, in world units (§4.74) - the ceiling that
  // stops a legitimate curve over a very long edge from reading as inflation.
  // **This was `pad_tess`**: the cap needed a slot and this block is fixed at eight scalars,
  // so it costs no size change and `cascades` stays on its 16-byte boundary.
  float pn_max_offset;
  // Bit 0: D3D's point and spot lights are in the sum (`render.local_lights`, on by default).
  // Bit 1: the map lights sample the atlas above (`render.map_shadows`).
  // Bit 2: D3D's point and spot lights sample it too (`render.local_shadows`, §4.65).
  //
  // The last two are separate bits rather than two texture fields because they are two features
  // sharing one image, and this was the pad word - which the `offsetof` asserts below check rather
  // than assume: 24 bytes of addresses plus thirty 4-byte scalars is 144, and both arrays that
  // follow have to start on a 16-byte boundary or their rows are not where any layout rule puts
  // them.
  //
  // **Its position is ABI, and the asserts below cannot see it.** §4.66 inserted the four
  // `dyn_shadow_*` words above this one here but *below* it in `world.slang`, so for four fields
  // the shader read the wrong word - `light_flags` came back as `dyn_shadow_texture`, which is
  // `kNoTexture` while the per-frame atlas is off, so all three bits read set and all three knobs
  // went inert (§4.67). A permutation preserves `sizeof`, so every assert here still passed.
  // Anything added to this struct goes in the same place in both files, in the same commit.
  uint32_t light_flags;
  //
  // Per cascade, in light space: `x`/`y` the box's snapped centre, `z` the reciprocal of its
  // half-extent, `w` its depth bias already converted out of texels (see SetShadowBias).
  float cascades[kMaxShadowCascades][4];
  // World -> **light space**, not to a cascade's clip space: the sun's orthonormal basis with no
  // scale and no translation, so the result is in world units and every cascade is then a centre
  // and a half-extent in it. That is what lets one transform serve four boxes, and it is why the
  // cascade selection is a pair of compares rather than four matrix multiplies.
  //
  // Row-vector, like every other matrix here. Last because it is 16 floats and putting it first
  // would push every scalar above past an offset worth checking.
  float sun_matrix[16];
  // --- screen-space ambient occlusion (§4.86) --------------------------------------------------
  //
  // **Appended after `sun_matrix`, and that placement is deliberate.** Every field above it has a
  // pinned offset - `cascades` at 176 and `sun_matrix` at 240 - and §4.67 is what happens when a
  // block is inserted at a different height in the two declarations: a permutation preserves
  // `sizeof`, so every assert here still passes while four fields read each other's words. The
  // end of the struct is the one place a field can be added without moving anything, so that is
  // where this went. It still has to be added in the same place in `world.slang`, in the same
  // commit, and `src/gen-shader-abi.py` is what now checks that rather than this comment.
  uint32_t ao_texture; // kNoTexture for "off", "not created" and "nothing drawn" alike
  uint32_t ao_flags;   // kAoMapOnly | kAoDebug
  float ao_direct;     // how much the occlusion scales the DIRECT sum; 0 is ambient only
  float pad_ao;
  // --- the PN split-corner table (§4.71) ---------------------------------------------------
  //
  // One bit per canonical vertex, saying the mesh has split this corner into vertices carrying
  // different normals - so its tangent term must be zeroed on both sides of the seam, or the two
  // patches build different boundary curves between the same two points and tear apart. Appended
  // after the AO block for the reason that block was appended after `sun_matrix`.
  uint64_t split_corners; // device address of the bitset in the frame scratch
  uint32_t split_base;    // the canonical-vertex index bit 0 stands for
  uint32_t split_count;   // how many bits are valid; 0 is the single "no table" test
  // --- HDR (see SetHdr below) ---------------------------------------------------------------
  //
  // kLinearInput | kUnclamped. Appended for the reason both blocks above were: every offsetof
  // below pins a field at or before `split_corners`, and the end is the only place a field can be
  // added without moving one of them.
  uint32_t colour_flags;
  // Three and not one: `cascades` is a `float4[]`, so std430 aligns this struct to 16 where the
  // scalar rule aligns it to 8, and only a multiple of 16 satisfies both. See the same note in
  // world.slang - `src/gen-shader-abi.py` is what enforces it.
  uint32_t pad_colour[3];
};
static_assert(sizeof(GpuFrameData) == 352, "the scratch stride is part of the shader ABI");
static_assert(offsetof(GpuFrameData, cascades) == 176, "std430 puts a float4 array on 16");
static_assert(offsetof(GpuFrameData, sun_matrix) == 240, "... and so does the matrix after it");
static_assert(offsetof(GpuFrameData, ao_texture) == 304, "the AO block appends, it does not insert");
static_assert(offsetof(GpuFrameData, split_corners) == 320, "... and so does this one");
static_assert(offsetof(GpuFrameData, colour_flags) == 336, "... and so does the HDR pair");

// GpuDrawRecord::lighting flags.
//
// The two colour paths are mutually exclusive and neither is the default: with both clear the
// vertex keeps its own colour, which is exactly what D3D produces with D3DRS_LIGHTING off.
constexpr uint32_t kLightSum = 0x01;     // run the real per-vertex equation
constexpr uint32_t kLitCollapse = 0x02;  // use `lit_colour` - the §4.20 material collapse
constexpr uint32_t kNormaliseNormals = 0x04; // D3DRS_NORMALIZENORMALS
constexpr uint32_t kSpecularEnable = 0x08;   // D3DRS_SPECULARENABLE
constexpr uint32_t kVertexColour = 0x10;     // the FVF supplies a diffuse, so COLOR1 exists
constexpr uint32_t kColourVertex = 0x20;     // D3DRS_COLORVERTEX
// The four D3DRS_*MATERIALSOURCE states, two bits each: 0 D3DMCS_MATERIAL, 1 D3DMCS_COLOR1,
// 2 D3DMCS_COLOR2. COLOR2 resolves to the material, because the canonical vertex drops the
// specular colour (§4.10) - `lit_draws_wanting_colour2` is what says whether that ever matters.
constexpr uint32_t kDiffuseSourceShift = 8;
constexpr uint32_t kAmbientSourceShift = 10;
constexpr uint32_t kSpecularSourceShift = 12;
constexpr uint32_t kEmissiveSourceShift = 14;

// Everything about one draw that is not a texture stage. 288 bytes, ~190 KB a frame at
// level01's 660-draw peak.
struct GpuDrawRecord {
  float mvp[16];               // world * view * projection, D3D row-vector, Y already flipped
  float world[16];             // world alone, for the world-space position lighting needs
  float normal_transform[12];  // 3 rows of 4: the inverse transpose of world's upper 3x3
  float material_ambient[4];
  float material_diffuse[4];
  float material_specular[4];  // rgb, and w = D3DMATERIAL8::Power
  float material_emissive[4];
  float global_ambient[4];     // D3DRS_AMBIENT
  float eye[4];                // xyz: the camera in world space, for the specular halfway vector
  // 3 rows of 4: world->view rotation, for the chrome pass's generated sphere-map coordinate.
  //
  // The *rotation* and not the whole view matrix, because that coordinate is a direction and a
  // translation would do nothing to it. A plain transpose of the view's upper 3x3 rather than an
  // inverse, which is only legal because the view matrix is rigid - the same property StoreEye
  // already leans on to recover the camera position from it.
  float view_rotation[12];
  uint32_t light_offset;       // into the frame's light array
  uint32_t light_count;
  uint32_t lighting;           // the flags above
  uint32_t lit_colour;         // packed D3DCOLOR, read only under kLitCollapse
};
static_assert(sizeof(GpuDrawRecord) == 336, "the scratch stride is part of the shader ABI");

// One of the game's draw calls, already reduced to what the shader needs.
//
// The matrices and the lighting inputs are NOT here - they are in this draw's GpuDrawRecord,
// which the capture layer writes straight into the frame's scratch. `record` is its index.
// Where a draw's vertices and indices live. A buffered draw pulls from the arenas; a
// user-pointer draw pulls from the frame's scratch, which is a different buffer entirely.
//
// The two streams carry this separately, because a buffered draw can have one of each: a
// buffer refilled after an earlier draw in the same frame already read it has its later
// versions parked in the scratch (§4.23), and the game refills vertices and indices
// independently.
enum class DrawSource : uint8_t { Arena, Scratch };

constexpr uint32_t kNoTexture = 0xffffffffu;

// One D3D8 texture stage, as the capture layer resolves it and as GpuMaterial carries it.
// `color` and `alpha` each pack one fixed-function operation - op in bits 0..7, its two D3DTA_*
// arguments in 8..15 and 16..23, and D3DTSS_TEXCOORDINDEX in 24..31 of the colour word. Packed
// because two words per stage is what the shader wants to switch on, and because it was once
// push constants, where 128 bytes is the whole budget.
struct DrawStage {
  uint32_t texture_index = kNoTexture; // bindless slot, kNoTexture for untextured
  uint32_t sampler_index = 0;
  uint32_t color = 0;
  uint32_t alpha = 0;
};

// D3DSHADEMODE, as GpuMaterial::shading carries it.
constexpr uint32_t kShadeFlat = 1;
constexpr uint32_t kShadeGouraud = 2;

// The other half of §2's design: everything the fragment shader needs about a draw's *surface*,
// as one entry of a per-frame array. `GpuDrawRecord` is per draw by construction - it holds a
// matrix - but a material is shared, and the game draws the same one many times a frame: 273
// draws on level02 intern to a few dozen materials, and interning is what makes it a table
// rather than a per-draw copy.
//
// It is a table for the reason §2 gives, which is not bandwidth: a second pass over the frame -
// a shadow map, an ID buffer, a wireframe - is a walk over the draw array with a different
// pipeline, and that only works if a draw is an *index* into shared state rather than a bundle
// of push constants only the recording loop knows how to rebuild.
//
// **Twelve named uint scalars, and the size is a multiple of 16.** Named scalars sit at 0, 4,
// 8 ... under std140, std430 and scalar rules alike; what those rules disagree about is an
// array's *stride*, which std140 rounds up to 16. 48 is already a multiple of 16, so the C++
// struct and the Slang one agree structurally rather than by betting on which rule Slang picked
// - the same reasoning GpuLight and GpuDrawRecord document, arrived at from the other side.
struct GpuMaterial {
  uint32_t stage0_texture = kNoTexture;
  uint32_t stage0_sampler = 0;
  uint32_t stage0_color = 0;
  uint32_t stage0_alpha = 0;
  uint32_t stage1_texture = kNoTexture;
  uint32_t stage1_sampler = 0;
  uint32_t stage1_color = 0;
  uint32_t stage1_alpha = 0;
  uint32_t stage_count = 0;
  // The alpha test: D3DCMPFUNC in bits 0..3 (0 = off) and D3DRS_ALPHAREF in bits 8..15. It is
  // here rather than on the draw because it is a property of the surface, and because two draws
  // that differ only in it would otherwise share a material and get one of the two tests.
  uint32_t flags = 0;
  // D3DRS_SHADEMODE, carried as the D3D value - 1 FLAT, 2 GOURAUD, 3 PHONG - for the reason
  // PipelineState carries its own states that way: the vocabulary stays the game's, so a draw is
  // comparable with `render.state`'s pipeline histogram with no decoder ring.
  //
  // It occupied what was a pad word when it was added, so it cost nothing. Only FLAT and GOURAUD
  // occur (§4.31); PHONG was never implemented by any D3D driver, and the shader treats anything
  // that is not 1 as GOURAUD.
  uint32_t shading = kShadeGouraud;
  // The material override's tint, as RGBA8 - R in bits 0..7 through A in 24..31, so 0xffffffff
  // is the identity and is what every material the game itself produces carries. Multiplied into
  // the fragment's final colour, after the texture stages, the alpha test and the specular add
  // (see SetMaterialOverride below).
  //
  // It occupied what was `pad0` when it was added, so it cost nothing either.
  uint32_t tint = 0xffffffffu;
  // The lighting map for this surface's **stage 0** texture, as a bindless slot, or kNoTexture -
  // which is what every material the game itself produces carries, and what makes the whole
  // feature a comparison in the fragment shader rather than a second code path. See
  // src/VkLighting.h for what the file is and where it comes from.
  //
  // It is keyed on stage 0 for the same reason `tint` is: that texture is the surface's identity,
  // where stage 1 is never the surface at all - measured over a whole level02 frame (notes
  // 4.51), it is the 256x256 A8 **fog-of-war** grid on 71 draws and `units\reflect.rim` on 90,
  // and nothing else. It samples through stage 0's own sampler, so a clamped surface's map is
  // clamped too - a wrap mismatch between a texture and its own companion would show as a seam
  // at exactly the edge.
  uint32_t lighting_texture = kNoTexture;
  // 1 when this material is Gunlok's chrome pass - stage 1 samples `units\reflect.rim`. See
  // IsChromeTexture in src/VkLighting.h for why that name is the whole test.
  //
  // On the material and not derived in the shader from `stage1_texture`, because the shader has
  // no way to ask what an image is called: a bindless slot is a number there, and the name lives
  // in the registry the capture layer owns. It is part of the intern key by construction, which
  // is right - two draws differing in it must not share an entry.
  //
  // It occupied what was `pad0` when it was added, so it cost nothing.
  uint32_t chrome = 0;
  // Reserved. Thirteen useful words are 56 bytes and an array's std140 stride rounds to 64, so
  // these are free, and spelling them out keeps the C++ and Slang structs the same size rather
  // than leaving one to guess at the tail.
  uint32_t pad1 = 0;
  uint32_t pad2 = 0;

  bool operator<(const GpuMaterial &other) const;
};
static_assert(sizeof(GpuMaterial) == 64, "the scratch stride is part of the shader ABI");

// The fixed-function state that selects a VkPipeline, carried as the D3D values themselves
// rather than as Vulkan enums: the translation belongs next to the pipeline it builds, and
// keeping the key in the game's own vocabulary makes a draw comparable with `render.state`'s
// pipeline histogram without a decoder ring.
//
// Five distinct values on level01 (§4.19), against the six §4.7 predicted - and the dominant
// one, 73% of draws, has both alpha test and alpha blending on. This is not a refinement: a
// single always-opaque pipeline draws three draws in four with the wrong coverage.
//
// The topology is part of the key rather than a reason to skip a draw. Four draws a frame are
// not triangle lists - three strips and one line list - and skipping them cost the HUD its
// green tint entirely (§4.20).
struct PipelineState {
  uint32_t topology = 4;    // D3DPRIMITIVETYPE: 4 is D3DPT_TRIANGLELIST
  uint32_t blend_enable = 0;
  uint32_t src_blend = 5;   // D3DBLEND_SRCALPHA
  uint32_t dest_blend = 6;  // D3DBLEND_INVSRCALPHA
  uint32_t depth_test = 1;  // D3DRS_ZENABLE
  uint32_t depth_write = 1;
  uint32_t depth_func = 4;  // D3DCMP_LESSEQUAL
  uint32_t cull_mode = 3;   // D3DCULL_CCW
  // D3DRS_COLORWRITEENABLE: RED 1, GREEN 2, BLUE 4, ALPHA 8. The HUD is drawn green-only
  // (§4.20), and with every channel written it comes out in the source art's own colours -
  // which looks like a plausible render and is not the game.
  uint32_t colour_write = 0xf;
  // The stencil test, which is here rather than in the shader for the same reason the depth
  // test is: it is a fixed-function unit, and the game uses it for classic shadow volumes -
  // two invisible passes counting into the buffer, then a 50%-black full-screen quad tested
  // against the count (§4.21). Without it that quad darkens the entire screen, which is why
  // the topologies were opt-in for two sections.
  //
  // Only the parts that are pipeline state in Vulkan are here. D3DRS_STENCILREF and the two
  // masks are dynamic state and live on the DrawItem, so a draw that differs only in its
  // reference value does not build a second pipeline.
  uint32_t stencil_enable = 0;
  uint32_t stencil_func = 8;   // D3DCMP_ALWAYS
  uint32_t stencil_fail = 1;   // D3DSTENCILOP_KEEP
  uint32_t stencil_zfail = 1;
  uint32_t stencil_pass = 1;
  // Which pass this pipeline belongs to, and therefore which colour format it declares: 0 is the
  // world pass's target, 1 the LDR one the 2D layers are drawn into after the tonemap (see
  // `Layer` below). **Set at record time, never by the capture layer** - whether the two passes
  // exist at all is `render.hdr`'s answer, and it can move between the scene being captured on the
  // game thread and the frame being recorded, so a bit stamped at capture would be a frame stale.
  // A pipeline whose format disagrees with its attachment is an invalid draw rather than a wrong
  // picture, so that frame matters. With HDR off nothing sets it and the pipeline set is exactly
  // what it was before this existed.
  uint32_t ldr_target = 0;
  // `depthClampEnable`, set for a pre-transformed draw and only for one. D3D **clamps** a
  // pre-transformed vertex's z into the viewport's MinZ..MaxZ rather than clipping the
  // primitive away - measured both ways round with `render.depth_probe` (notes §4.45) - and
  // with BuildMvp's compensation in place, "outside the slice" is exactly "outside NDC [0,1]",
  // which is the case this bit decides. It must stay off for the 3D path, where D3D really does
  // clip at the near and far planes.
  uint32_t depth_clamp = 0;
  // The PN-triangle amplification pass (§4.71). **Not set by the capture layer** - the recorder
  // has neither the draw-set predicates nor the material table, and which draws are the level
  // mesh is a renderer policy rather than something D3D was asked for. `RecordDraws` sets it on
  // its own copy of the key just before the lookup, so a state that differs only in this forks
  // one pipeline and nothing else about the frame moves.
  uint32_t tessellate = 0;

  bool operator<(const PipelineState &other) const;
};

struct DrawItem {
  uint32_t record = 0;     // this draw's GpuDrawRecord, in records into the frame's scratch
  // This draw's GpuMaterial, in materials into the frame's scratch. Assigned by SubmitDraw from
  // the stages and flags below, which stay on the item because they are what `render.draw_info`
  // prints - the index alone would only be readable against a table nothing else can see.
  uint32_t material = 0;
  uint32_t base_vertex;    // this draw's vertices, in canonical vertices into its source
  uint32_t first_index;    // in indices into its source; ignored when `indexed` is false
  uint32_t count;          // indices when indexed, vertices otherwise
  int32_t vertex_offset;   // D3D's BaseVertexIndex, added to every index
  // The stages and the alpha test. These three are what SubmitDraw interns into `material`, and
  // they stay on the item so `render.draw_info` can print a draw's surface without a decoder
  // ring for the table - the shader reads them only through GpuMaterial.
  uint32_t stage_count = 0; // stages up to the first D3DTOP_DISABLE; 0 means diffuse only
  DrawStage stages[2];
  PipelineState pipeline;
  // The alpha test, which is a shader `discard` rather than pipeline state: D3DCMPFUNC in bits
  // 0..3 (0 = the test is off) and D3DRS_ALPHAREF in bits 8..15.
  uint32_t flags = 0;
  // D3DRS_SHADEMODE. Not pipeline state here, although it is rasteriser state in D3D: the
  // shader carries the vertex colour twice, once interpolated and once flat, so selecting
  // between them costs a varying rather than a second pipeline per blend/depth/cull combination
  // (§4.31).
  uint32_t shade_mode = kShadeGouraud;
  // The dynamic half of the stencil state. Read only when `pipeline.stencil_enable` is set,
  // but written unconditionally, because a stale reference value is exactly the kind of thing
  // that would only show up on the one frame the game changes it.
  uint32_t stencil_ref = 0;
  uint32_t stencil_mask = 0xffffffffu;
  uint32_t stencil_write_mask = 0xffffffffu;
  // D3DVIEWPORT8's MinZ and MaxZ, which map a vertex's NDC depth into a slice of the depth
  // buffer. **Gunlok uses six of them and none is the default 0..1** (§4.32): the world sits in
  // 0.1..1.0 and the effect layers in thin slices around 0.02..0.06, so an overlay is in front
  // of the world by construction rather than by switching the depth test off. A backdrop pass
  // uses 1.0..1.0, which pins it to the far plane.
  //
  // Per draw and not per frame, because the game changes it between draws inside one scene -
  // which is the whole technique. Dynamic state, so it costs no pipeline.
  float min_depth = 0.0f;
  float max_depth = 1.0f;
  // ...and D3DVIEWPORT8's rectangle, per draw for the same reason. **Gunlok sets two of them**
  // (§4.47): the whole backbuffer for everything in a level, and 32,24 575x431 for the upgrade
  // screen. It is the rectangle rather than the depth slice that decides where a draw lands, so
  // ignoring it does not shift a layer by a hair - it stretches a whole screen over the window,
  // which is what play reported. A width of 0 means "no viewport recorded yet"; RecordDraws
  // falls back to the render target then, which is what every draw got before this existed.
  //
  // The rectangle is in the game's backbuffer pixels, and the offscreen target is the same size
  // (§4.38), so it needs no scaling on the way to Vulkan. It is BOTH the viewport and the
  // scissor there: D3D clips a pre-transformed vertex to it, measured with `render.viewport_probe`
  // (a 64x32 quad under a 60x40 rectangle rasterises 40x20), and a Vulkan viewport does not clip.
  int32_t viewport_x = 0;
  int32_t viewport_y = 0;
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  DrawSource vertex_source = DrawSource::Arena;
  DrawSource index_source = DrawSource::Arena;
  // **Which layer this draw belongs to, taken from the engine's own answer.** Gunlok has no
  // per-draw layer; what decides whether a 2D element lands in front of another is which camera
  // drew it, and `InitRenderCameras` gives each camera a slice of the depth range
  // (`rendering_notes.md` §4.4). Every one of those cameras is ORTHOGRAPHIC except `Camera_World`
  // and the sky camera, so `CurrentCameraIsPerspective` @ 0x007c1470 - which the engine itself
  // maintains, and which `src/HudFix.cpp` already reads - is exactly "is this draw part of the
  // world". No threshold is involved, and a depth-slice test would not do: there is an
  // orthographic camera at 0.06..0.30 that overlaps the world's 0.10..1.00.
  //
  // Measured on level02's settled frame: 7 draws of 268, in three runs interleaved with the world.
  // At the main menu and on the briefing screen it is every draw there is, which is the point -
  // those screens then leave the HDR pipeline entirely and come out as the shipped pixels.
  bool ui = false;
  bool indexed = true;     // DrawPrimitiveUP has no indices at all
  uint8_t index_stride = 2; // 2 or 4, for the index type to bind
  // The world-space box this draw's vertices occupy, for the shadow bakes to cull against.
  //
  // **`has_bounds == false` means "unknown", and every consumer must read it as "draw this"** -
  // never as an empty box. Three things produce it and none of them is an error: a
  // pre-transformed draw (whose vertices are in screen space, so a world box is meaningless), a
  // draw whose vertices live in the frame's scratch rather than the arena (nothing accumulates
  // boxes there), and an arena range whose blocks were only ever written in part. The bakes
  // count what they could not bound, which is what keeps a coverage regression visible rather
  // than merely slow.
  //
  // World space and not object space because the world matrix is only at hand in the capture
  // layer: it lives in this draw's GpuDrawRecord, which is in the frame's scratch - host-visible
  // and write-combined - and reading a matrix back out of that per caster per frame would cost
  // more than the cull saves.
  bool has_bounds = false;
  float bounds_min[3] = {0, 0, 0};
  float bounds_max[3] = {0, 0, 0};
};

struct DrawStats {
  bool ready = false;
  uint64_t items = 0;          // submitted this frame
  uint64_t max_items = 0;
  uint64_t drawn = 0;          // cumulative, actually recorded
  // Every Draw* call the capture layer received while the renderer was up, and every one that
  // became a DrawItem. **`seen - submitted - the skips below must be 0`**, and that is the
  // invariant §4.32 exists because nobody was checking.
  //
  // `DrawPrimitive` was never wired up: it was counted, forwarded to d3d8to9, and never turned
  // into a draw. Every "must be 0" counter here reads zero for it, because they all count
  // *reasons a draw was rejected* and this one was never offered - so a whole entry point went
  // missing in a way no existing number could show. Gunlok draws its additive glow sprites that
  // way, so fires rendered as bare scenery.
  //
  // The general lesson is worth more than the fix: a counter that says "of the work that reached
  // me, none went wrong" cannot see work that never reached it. This pair is the only reading
  // here that compares against what the GAME did rather than against what the renderer chose.
  uint64_t seen = 0;
  uint64_t submitted = 0;
  uint64_t index_binds = 0;    // how often the index source changed within a frame
  uint64_t skipped_user_ptr = 0;
  uint64_t skipped_unconvertible = 0; // a user-pointer FVF the converter does not handle
  uint64_t skipped_scratch_full = 0;
  uint64_t skipped_topology = 0; // not a triangle list
  uint64_t skipped_no_slot = 0;  // a buffer with no arena residency, so nothing to pull
  // ... split by which of the four reasons, because they have nothing in common: a stream
  // this layer never wrapped, a wrapper that never got an arena slot, and a 32-bit index
  // buffer are three different pieces of work.
  uint64_t skipped_foreign_stream = 0;
  uint64_t skipped_unslotted_vertices = 0;
  uint64_t skipped_unslotted_indices = 0;
  uint64_t skipped_index_stride = 0;
  uint64_t skipped_never_unlocked = 0; // ... of the unslotted ones, never unlocked either
  // Buffers given an arena slot from their own contents at draw time, because their only
  // Unlock happened before the renderer existed. `seed_refused_pool` is a buffer that could
  // not be read back safely; `seed_read_failures` is one whose read lock failed.
  uint64_t buffers_seeded = 0;
  uint64_t seed_refused_pool = 0;
  uint64_t seed_read_failures = 0;
  // Draws that ran the real per-vertex light sum, and how many of those had at least one light
  // switched on.
  //
  // The second number used to be `lit_draws_with_lights` and it was **blind by construction**:
  // its FVF test preceded its light test, so it returned before the lights were ever looked at
  // for any draw carrying a vertex colour - which is nearly all of them. It read 845 across a
  // whole level01 session and was frozen rather than small, which is indistinguishable from
  // "rare" without watching it move (§4.25). Both tests are gone; these count what they say.
  uint64_t lit_draws = 0;
  uint64_t lit_draws_with_lights = 0;
  // Draws naming D3DMCS_COLOR2 as a material source. The canonical vertex drops the specular
  // colour (§4.10), so the shader resolves those to the material instead - which is right for
  // level01, where nothing sets one, and is a silent approximation anywhere else.
  uint64_t lit_draws_wanting_colour2 = 0;
  // Draws with lighting on that arrived before the game's first SetMaterial. They keep their
  // vertex colour, because D3D8 documents no default material and every term of the equation
  // comes from one. Expected to be small and to stop growing once a level is up.
  uint64_t lit_draws_without_material = 0;
  // Enabled lights the frame's light array could not take, and draws whose GpuDrawRecord did
  // not fit. Both must be 0; the scratch peaks in `render.vulkan_report` are what to resize by.
  uint64_t dropped_lights = 0;
  uint64_t skipped_no_record = 0;
  uint64_t skipped_no_transform = 0;
  // Draws naming a texture-stage operation the shader does not implement, and draws using
  // more than the two stages it has. Both are drawn anyway - with the op treated as "take the
  // first argument" and the extra stages ignored - because a dropped draw is a hole in the
  // picture and a slightly wrong one is not. Neither is expected to be non-zero on level01;
  // both are how a level with richer materials would announce itself.
  uint64_t unsupported_stage_op = 0;
  uint64_t truncated_stages = 0;
  // Stages the game has a texture bound at that this layer could not turn into a bindless
  // image. The shader samples white there, which is not neutral for every op - ADDSIGNED with
  // a white texture brightens by 0.5 - so this is a "must be 0" counter, not a nicety.
  uint64_t stage_texture_unresolved = 0;
  // Draws with the stencil test switched on, and the ones that asked for it on a depth format
  // with no stencil aspect. The second is a "must be 0" counter and is what says the shadow
  // volumes are being counted rather than silently ignored - the state that made the shadow
  // quad darken the whole screen for two sections was invisible to every other number here.
  uint64_t stencil_draws = 0;
  uint64_t stencil_draws_without_buffer = 0;
  // Distinct GpuMaterials interned this frame, and the high-water mark. The frame's material
  // slice holds kMaxDrawsPerFrame of them, so `materials` can never exceed `items`; how far
  // *below* it runs is the measurement that says whether a table was worth building, and it is
  // also what the material-override surface addresses by. `dropped_materials` is a
  // draw the table could not take - unreachable by capacity, since the two limits agree, so a
  // non-zero reading means the scratch is unusable. Must be 0.
  uint64_t materials = 0;
  uint64_t max_materials = 0;
  uint64_t dropped_materials = 0;
  // Draws the game asked for D3DSHADE_FLAT on. Not an invariant either way - it is the size of
  // the thing §4.31 implemented, and on level02 all of it is the stencil shadow.
  uint64_t flat_shaded_draws = 0;
  // Draws a material override touched, and draws it removed. Neither is an invariant - both are
  // 0 until a mod registers something - but they are the only way to tell "the override is
  // registered and resolved" from "the frame actually draws with it". A key that matches an
  // asset the camera cannot see resolves, reports its image and paints nothing, which is
  // indistinguishable from a broken override without this (§4.44).
  uint64_t overridden_draws = 0;
  uint64_t hidden_draws = 0;
  // The game's own stencil shadow, dropped because the sun's shadow map has replaced it. Counted
  // into the seen == submitted + skips reconciliation for the reason hidden_draws is: a feature
  // that drops draws on purpose must not make that invariant read as broken.
  uint64_t stencil_shadow_draws_hidden = 0;
  // How often the viewport had to be reissued because a draw wanted a different depth slice.
  // Not an invariant - it is the size of the technique in §4.32, and a level where it reads 0
  // would be one where the engine never layers anything in front of the world.
  uint64_t viewport_sets = 0;
  uint64_t pipelines = 0;
  // How many times the world-space light grid has been rebuilt. Once per level with a light set,
  // so a number that climbs with the frame count means the rebuild test is broken.
  uint64_t light_grid_builds = 0;
  // How many draws cast into the sun's shadow map on the last frame that rendered one.
  uint64_t shadow_casters = 0;        // distinct pipeline states seen, i.e. VkPipelines created
  uint64_t pipeline_binds = 0;   // how often the bound pipeline changed within a frame
  // How many CONSECUTIVE runs the frame's draws fall into, where a run is a maximal stretch
  // sharing everything one `vkCmdDrawIndexedIndirect` would have to fix: the pipeline, the three
  // dynamic stencil values, the viewport and scissor, the index buffer and its type, and which
  // buffer the vertices come from.
  //
  // **This is the whole feasibility question for an indirect world pass**, and it is a
  // measurement rather than an argument. Runs may only be consecutive here: the list is recorded
  // in the order the game issued it because `RenderQueue_Flush` has already state-sorted the
  // opaque draws and put the back-to-front list last, so reordering to lengthen a run would
  // break blending. `runs` against `items` is the ceiling on what indirect submission could
  // remove - a frame whose runs average 1 would pay indirect's overhead for nothing.
  uint64_t batch_runs = 0;
  uint64_t batch_longest = 0;    // the longest single run, for the shape of the distribution
  // The draws those runs cover, THIS frame. Not `drawn`, which is cumulative - dividing a
  // cumulative total by a per-frame count is the kind of ratio that looks plausible forever.
  uint64_t batch_draws = 0;
  uint64_t pipeline_failures = 0; // must be 0: a state whose pipeline would not build
  uint64_t dropped_over_capacity = 0; // must be 0
};

// Creates the pipeline and the depth buffer. Needs the bindless set to exist, so it is called
// after StartResources.
bool StartDraw(uint32_t width, uint32_t height, uint32_t colour_format);
void ShutdownDraw();
bool DrawReady();

// The depth buffer follows the swapchain, so a resize has to rebuild it.
bool ResizeDraw(uint32_t width, uint32_t height);

// Appends one item to the frame's list. Cheap and does no Vulkan work.
void SubmitDraw(const DrawItem &item);

// The index the next SubmitDraw will occupy, which is what `render.draw_info` and
// `render.draw_hide` index by. The capture layer needs it to answer "is the draw I am about to
// build the one someone armed a diagnostic on".
uint32_t PendingDrawIndex();

// Records the whole list into `cmd`, inside a rendering pass the caller has begun, then clears
// it. The depth image view is the caller's to attach - see DepthImageView.
// Which layer to record. Under `render.hdr` the frame is two passes with the tonemap between them
// - `World` into the float target, `Ui` into the LDR one afterwards - because a tonemap curve
// applied to Gunlok's menus, briefing screens and HUD recolours content that was authored final
// (notes §4.92: before the split, ACES moved 99.61% of the main menu). `All` is the single-pass
// frame every build before that drew, and is what runs whenever HDR is off.
enum class Layer { All, World, Ui };

void RecordDraws(void *command_buffer, Layer layer = Layer::All);

// Whether this frame has any 2D draw at all. VkRenderer asks before running the UI pass, so a
// frame with nothing in that layer - which is most of an in-level frame's neighbours - costs no
// pass, no barrier and no depth clear.
bool HasUiDraws();

// Build the world-space light grid, if the level's light set has changed since it was last built.
//
// **Must be called outside a render pass**, which is why it is the renderer's to call and not
// RecordDraws': a compute dispatch inside vkCmdBeginRendering is invalid, and RecordDraws runs
// there. It sits with the uploads, before the world pass begins.
//
// Usually a no-op. The map's lights are static in world space, so the grid is rebuilt once per
// level rather than once per frame - see src/shaders/lightgrid.slang for why that is the shape
// this takes instead of a view-space cluster grid.
void BuildLightGrid(void *command_buffer);

// Render the sun's shadow map: a depth-only pass over the same draw list, from the sun.
//
// **Outside any render pass and AFTER the draw records exist**, which pins it to one place - the
// renderer calls it beside BuildLightGrid, before the world pass begins. It reads the same
// GpuDrawRecord array the world pass does, so it must run after the scene has been recorded and
// before the scratch rotates.
void RecordShadowPass(void *command_buffer);

// Bake a slice of the map lights' static shadow atlas: one cube per light, from the map's own
// geometry, in the current frame's draw list (§4.61).
//
// **Outside any render pass and beside RecordShadowPass**, for the same two reasons - it needs the
// GpuDrawRecord array the scene was recorded into, and it begins a rendering of its own.
//
// **A slice, not the whole thing.** 686 lights x 6 faces x ~150 map draws is over half a million
// draw calls; issued in one submit that is seconds of GPU work, and Windows resets a device that
// does not make progress for two. So it bakes `render.map_shadow_rate` lights a frame and picks up
// where it left off, which finishes inside a second of a level starting. An unbaked light's tile
// holds the clear value, which reads as "nothing occludes" - so the shadows arrive rather than the
// level starting black.
void BakeMapShadows(void *command_buffer);

// The map lights' static shadows. **On since §4.64, and play is what settled it.**
//
// §4.61 left it off because no measurement could say whether the picture with these shadows was
// the right one - the game never had them - and then the first report from actually playing was
// that the map lights "don't cast shadows". A feature nobody can see is not a fidelity question.
// Sampling the atlas costs 0.50 ms on level01, the level with the most map lights in the game,
// and nothing measurable on level02, so cost was never the objection either.
//
// **The bake is gated on this as well**, so off costs nothing rather than a bake at every level
// start. Turning it back on restarts the bake, and the shadows arrive over the next few frames -
// which means the A/B is instant only once `render.map_shadow_report` says finished.
void SetMapShadows(bool enabled);
bool MapShadows();

// The normal offset a lookup is moved by, in atlas texels at the fragment's own distance from the
// light. See GpuFrameData::map_shadow_offset for why the bias for this atlas is an offset along
// the normal rather than a depth one.
void SetMapShadowBias(float texels);
float MapShadowBias();

// Whether the bake submits one `vkCmdDrawIndexedIndirect` per cube face or one draw call per
// caster per face (§4.62). On wherever the device has `multiDrawIndirect`, which is what turns
// 804,924 draw calls into 4,092 commands on level01.
//
// **The two must produce the same atlas**, and this knob is the only thing that can say so: they
// differ in the shader entry point, so switching it rebuilds the pipeline and re-bakes. Setting
// it on a device without the feature does nothing and reads back false.
void SetMapShadowIndirect(bool enabled);
bool MapShadowIndirectEnabled();

// How many lights are baked per frame. **The default comes from the submission path** - 256 with
// indirect drawing and 4 without - because §4.62 changed what the knob is for: what it used to
// spread was 1.9 seconds of draw-call submission, and one command a face leaves a bake that is a
// few milliseconds of GPU work in total. Changing it re-bakes from the start.
void SetMapShadowRate(int lights);
int MapShadowRate();

// What the atlas holds, what it refused and how far the bake has got - which is not optional
// reading, because a level with no map lights, an atlas that could not be created and a bake that
// has not started all look identical from the screen.
std::string MapShadowReport();

// The sun's shadow map. On by default; off is the build before it, and a level with no sun set
// produces no matrix and therefore no shadow, which is the same state.
// Drop the game's OWN stencil shadow - the blob under each unit - when the sun's shadow map is
// drawing a real one. On by default, because otherwise a unit has both.
//
// The three passes are identified by `stencil_enable`, which is exact rather than a heuristic:
// §4.31 measured that every flat-shaded draw on level01 and level02 is one of them, and nothing
// else in either level touches stencil at all. A level that used stencil for something else
// would lose it, which is what `render.stencil_shadow` exists to check.
void SetStencilShadow(bool enabled);
bool StencilShadow();

void SetSunShadows(bool enabled);
bool SunShadows();

// The depth offset a lookup is compared with, **in shadow-map texels of whichever cascade the
// fragment landed in** (§4.59). Texels rather than depth units because that is the unit acne is
// actually measured in: the depth error a flat surface accumulates across one texel is the texel's
// world size times the surface's slope in light space, so a value in texels is the same value on
// every cascade, on every level and at every `shadow_extent`. The same knob in depth units is
// none of those things.
//
// The default is a sweep, not a guess: on level04 the self-shadowing collapses between 1 and 2.5
// texels - 76% of the frame shadowed at 1, 16.6% at 2.5 - and everything above it is pure
// peter-panning, at 0.04-0.3% of the frame per texel.
void SetShadowBias(float texels);
float ShadowBias();

// How dark a shadowed fragment goes, 0 to 1. **The one knob here that is not a fidelity
// question**, because the game has no ground truth for it - it never had a real shadow.
//
// 1 is the physically correct value rather than the maximum one: the shadow attenuates only the
// direct terms, so 1 is "no sunlight reaches here" while the ambient and the level's own bake
// still light the surface. The default is 0.7, and both bounds are measured (§4.59) - §4.58's
// 0.55 leaves level04's unit shadows reading as a smudge, and 1.0 takes level02's covered start
// to 36% of its authored brightness.
void SetShadowStrength(float strength);
float ShadowStrength();

// Half the width of the world-space box the OUTERMOST cascade covers, centred on the camera's
// orbit pivot. Every inner cascade is half the one outside it, so `extent` is the range at which
// shadows stop and `extent / 2^(cascades-1)` is the sharp near field.
void SetShadowExtent(float extent);
float ShadowExtent();

// How many cascades are live, 1..kMaxShadowCascades. **1 is §4.58's single map**, at the same
// texel density, which is what makes cascading A/B-able on one paused frame.
void SetShadowCascades(int count);
int ShadowCascades();

// **A diagnostic, and the one that prices shadows from D3D's own point and spot lights** (§4.65).
// On by default. Off drops every point and spot light from the light sum and keeps the
// directionals, so a paused A/B paints exactly the pixels those lights reach - and since a shadow
// only ever removes light, that set strictly contains anything shadowing them could change. It is
// the ceiling, and taking it before designing the feature is what says whether the feature is
// worth a millisecond.
//
// It is a shader-side skip inside `light_geometry`, so it reaches the fixed-function sum and the
// lighting maps' response alike, and `true` restores the frame bit-identically.
void SetLocalLights(bool enabled);
bool LocalLights();

// --- screen-space ambient occlusion (§4.86) -----------------------------------------------------
//
// Two recorded passes: a camera-space prepass writing world position and normal per pixel, and a
// full-screen resolve that walks one **fixed** Poisson disc over it. The kernel is deliberately not
// randomised, which is what removes the blur pass every other screen-space AO needs - see
// src/shaders/ao.slang for the technique and src/VkDraw.cpp for the resources.
//
// **Off by default**, and it is a fidelity call rather than a cost one: the game never had ambient
// occlusion, so nothing here can be measured as closer to D3D8. Off is bit-identical to the build
// before it existed.
void SetAmbientOcclusion(bool enabled);
bool AmbientOcclusion();

// The hemisphere's radius, **in world units** - what "near enough to occlude" means. Level02's mean
// map edge is 1.952 units and the sun's sharp cascade is 8.75 across, which is the scale to think
// in. Default 3, and it is a sweep rather than a guess - see AoRadiusValue in src/VkDraw.cpp.
void SetAoRadius(float radius);
float AoRadius();

// The disc's radius, **as a fraction of the frame's height**, and deliberately independent of the
// one above. Constant across the frame, which is the technique's whole performance argument - every
// pixel walks the same texel pattern - and a value derived once per frame from the target size is
// still one constant, so expressing it this way costs that nothing.
//
// Not a pixel count, because Gunlok's render extent is not a constant: 640x480 on the machine the
// notes' numbers come from and 3072x1728 on the one this was written on. Default 0.07, which is 34
// pixels at 480 lines and 121 at 1728.
//
// A constant screen radius is affordable here in a way it would not be generally: Gunlok's camera
// is a fixed-height orbit, so one frame's depth spread is narrow and a constant screen radius is
// very nearly a constant world radius.
void SetAoScreenRadius(float fraction);
float AoScreenRadius();

// How far along the normal a tap has to be before it counts, in world units. This is the
// self-occlusion knob: too low and a flat wall shades itself out of its own quantisation, too high
// and a shallow crease stops registering. Default 0.05.
void SetAoBias(float bias);
float AoBias();

// A scale on the occlusion before it leaves the resolve pass, so the artistic weight is baked into
// the target and the world shader stays a plain multiply. Default 1.
void SetAoStrength(float strength);
float AoStrength();

// How much the occlusion also scales **D3D's own** diffuse sum - the sun and the level's dynamic
// lights, not the map rig, which is occluded in full whatever this says.
//
// 0 by default, and that is the no-double-counting setting rather than a taste one: every one of
// those lights already has a shadow map answering "is this light blocked" exactly, per light
// (§4.58, §4.61, §4.65, §4.66). 1 darkens them too and is the stylised end. The specular is never
// occluded at any setting.
void SetAoDirect(float direct);
float AoDirect();

// How many of the 64-point disc to walk, 1..64, and it defaults to all of them.
//
// **Not a quality dial with a cheap end.** A fixed kernel cannot trade its artefact for noise
// the way a randomised one does, so an under-sampled disc produces visible copies of every
// occluder's silhouette rather than grain - which is what a blur would have hidden and what
// there is no blur here to hide. The cost is linear in this and in nothing else.
void SetAoTaps(int taps);
int AoTaps();

// Restrict the term to the map's own geometry, on by default - the same restriction runtime map
// lighting carries and for the same reason (§4.55): a prop or a unit is a separate `RBOBJECT` whose
// vertex colours were baked from its own file's lights, and that bake already contains occlusion.
// Applying this on top of it darkens the same crease twice.
void SetAoMapOnly(bool enabled);
bool AoMapOnly();

// Replace the shaded frame with the occlusion term itself, as grey. The only way to see what the
// pass produced rather than what it did to a picture, which is what the radius and the tap count
// have to be tuned against.
void SetAoDebug(bool enabled);
bool AoDebug();

// Records the two passes. Outside any render pass, like the shadow bakes beside it.
void RecordAoPass(void *command_buffer);

// **The windowed range cutoff on D3D's point and spot lights** (§4.70). On by default; off
// restores D3D8's own hard switch-off at Range.
//
// D3D8 cuts a light dead at Range while `1/(a0 + a1 d + a2 d^2)` is still well above zero there -
// k = 0.140 for level02's fires, scaling a diffuse of 4.0 - so per pixel the boundary is a STEP in
// the value. It was invisible in the original for §4.64's reason: D3D8 evaluated the sum per
// vertex, so the step landed inside a triangle and interpolation destroyed it.
//
// **Not verified against a reported disc.** The step is real and removing it is measured (it only
// ever darkens, worth up to 25/255 on ~1% of a fire-camera frame), but three level02 cameras all
// had their local lights bounded by geometry silhouettes rather than by the range sphere, so
// nothing here has yet reproduced a visible rim from THIS path the way §4.64 reproduced one from
// the map lights'. Treat that as open.
//
// This exists as a knob rather than a bare change because it is the only way to read the result.
// §4.30 measured that two settles of level02 drift by the objectives text's fade and the units'
// animation phase, which no whole-frame number can separate from a renderer difference - so the
// A/B has to happen inside one paused frame, and that needs a switch.
void SetLocalLightWindow(bool enabled);
bool LocalLightWindow();

// --- shadows from D3D's own point and spot lights (§4.65) --------------------------------------

// One D3D light reduced to **what decides its occlusion**, which is the key its atlas slot is
// held under. Position, range, type and the cone - and deliberately **not its colour**, because
// occlusion does not depend on colour and the game's `ADD BLINKING LIGHT` blinks by rewriting the
// diffuse at a fixed position (measured, `render.frame_lights` shows it as two alternating
// contents at one place). Keying on colour would churn a slot thirty times a second for a light
// that never moves, and would have read as a cache that simply does not work.
// **Every field is the float's BITS**, not the float, for the reason `d3d8::LightKey` is: the key
// is byte-compared, and a float type has representations that compare unequal while meaning the
// same thing (-0.0 against 0.0) and equal-meaning bits that are not equal values (any NaN). Bits
// make the comparison total and let `has_unique_object_representations_v` below stand as the
// no-padding proof, which is the thing that would actually go wrong silently.
struct LocalShadowKey {
  uint32_t position[3] = {0, 0, 0};
  uint32_t range = 0;
  uint32_t direction[3] = {0, 0, 0};
  uint32_t cos_phi = 0; // the spot's outer cone; 0 for a point light
  uint32_t type = 0;    // D3DLIGHTTYPE

  // Two lights are the same occluder iff they are the same numbers. No quantisation - a light the
  // game nudges every frame SHOULD read as a new key, because that is precisely what the stability
  // gate is watching for.
  bool operator<(const LocalShadowKey &other) const {
    return std::memcmp(this, &other, sizeof(LocalShadowKey)) < 0;
  }
};
static_assert(sizeof(LocalShadowKey) == 36, "no padding, or memcmp compares uninitialised bytes");
static_assert(std::has_unique_object_representations_v<LocalShadowKey>);

// The slot this light's cube occupies in the static atlas, or **-1**, which is the answer for a
// light that has not been still long enough to earn one, one the atlas had no room for, and one
// whose cube has not been baked yet.
//
// **A light has no identity across frames** - `SetLight` reuses indices freely and a `GpuLight` is
// deduplicated by enable mask *within* one frame - so the key above is the only identity there is.
// That works because the game's local lights are measurably static (13 distinct contents over
// 5,525 consecutive frames of level02, none re-authored), and it degrades correctly for the ones
// that are not: level02's own `.gcs` attaches a light to `lift_a`, which runs on a track, and a
// mod can attach one to anything. A moving light produces a new key every frame, never reaches the
// stability threshold, never claims a slot and never costs a bake - it is simply unshadowed, which
// is exactly the state every one of them is in today.
// `frame` is the capture layer's own frame counter, because there is none on this side and the
// stability gate counts frames rather than calls - a light enabled on forty draws of one frame has
// held still for one frame, not forty.
int32_t AcquireLocalShadowSlot(const LocalShadowKey &key, uint64_t frame);

// --- the per-frame atlas (§4.66) ---------------------------------------------------------------

// Register one of this frame's D3D lights and get the slot its cube will occupy, or -1 when the
// atlas is full or the feature is off. **No key, no cache and no stability gate**: the table is
// emptied every frame, so "the same light" only has to mean "the same light within this frame",
// which the key above answers exactly. That is what lets an explosion's light - which moves every
// frame and therefore has no cross-frame identity at all - cast like anything else.
//
// Called from the capture layer's StoreLight, at the same point AcquireLocalShadowSlot is, and
// **before** the bake: the slot has to be in the `GpuLight` the frame's draws already point at.
int32_t RegisterDynamicShadowLight(const LocalShadowKey &key, uint64_t frame);

// Bake every light registered this frame, from the frame's OWN caster list. Outside any render
// pass and beside `BakeMapShadows`, for the same reason: it walks the draw list the world pass is
// about to, and a caster is whatever that list holds - map geometry, a unit, or a barrel.
void BakeDynamicShadows(void *command_buffer);

// **A feature**, on by default. The per-frame atlas: shadows from every D3D point and spot light
// in the frame, cast by every mobile thing in it.
void SetDynamicShadows(bool enabled);
bool DynamicShadows();

// Its bias, in atlas texels at the fragment's own distance from the light. Separate from
// `map_shadow_bias` because the face is 256 texels here and 64 there, so one texel is a quarter of
// the world distance and the two knees need not coincide.
void SetDynamicShadowBias(float texels);
float DynamicShadowBias();

// The PCF kernel's radius for **D3D's point and spot lights**, in atlas texels: 0 a single tap,
// 1 a 3x3, 2 a 5x5, clamped at 3. `render.local_shadow_taps`, and it reaches whichever atlas
// serves them - the per-frame cube where a light has a slot, the static one where it does not.
//
// **The map lights keep their single tap**, which is not an oversight: a fragment on level02 is in
// range of a mean of 11.5 of them (§4.54) and the sum already filters, so a kernel there would be
// a hundred taps a fragment for no visible change. One or two D3D lights reach a fragment and
// nothing averages them, which is why the same body needs two answers (§4.69).
void SetLocalShadowTaps(int radius);
int LocalShadowTaps();

// What the per-frame atlas did last frame: lights registered, how many the 42 slots refused,
// casters submitted and how they bucketed. **Not optional reading** - a light with no slot and a
// light with nothing to occlude look identical on screen.
std::string DynamicShadowReport();

// A bisect for the caster set: take only arena-sourced draws. Run-time so the two can be compared
// inside one session. **It is not the test for "is a unit a caster"** - a unit draws from the arena
// as often as not (level02's fires: 150 casters, one bucket, all arena), so this separates the
// user-pointer draws and nothing else. `map_only` below is that test.
void SetDynamicShadowArenaOnly(bool enabled);
bool DynamicShadowArenaOnly();

// Narrow the caster set to `IsMapGeometry` - **§4.65's set exactly**, so the A/B against it prices
// the half of this feature the static atlas cannot do: the props and the units as casters. The two
// tests differ in what a caster *is* rather than in where its vertices live, which is why
// `arena_only` cannot answer the same question.
void SetDynamicShadowMapOnly(bool enabled);
bool DynamicShadowMapOnly();

// Bake the atlas but never advertise it to the world pass, so it is rasterised and not sampled.
// The bisect between "the bake hangs" and "sampling it hangs".
void SetDynamicShadowSample(bool enabled);
bool DynamicShadowSample();

// **A feature, on by default**: reject a caster the light cannot reach, and then one the cube face
// does not contain, instead of drawing every caster into every face.
//
// Off is the build before it, and the pair is what prices it - the atlas must come out
// **bit-identical** either way, because a cull that changes the picture is a cull that is wrong.
// That is the whole A/B: this is not a fidelity knob and there is nothing to weigh, only a cost.
//
// It is a knob at all because the *bounds* it tests can be wrong in a way nothing else here would
// show. A box that does not cover its geometry culls a caster that should have drawn, and the
// symptom is one shadow missing from one face of one light - which reads as a shadow bug rather
// than as a bounds bug. `off` is how that hypothesis gets tested in one command.
void SetDynamicShadowCull(bool enabled);
bool DynamicShadowCull();

// --- the sun pass's own two (§4.77) -------------------------------------------------------------
//
// **Features, both on by default**, and the same rule as `dynamic_shadow_cull`: the atlas must
// come out bit-identical whichever way either is set, so these are cost knobs with nothing to
// weigh.
//
// `sun_shadow_cull` rejects a caster the cascade's box does not contain. The four cascades differ
// only in x/y half-extent and share a deliberately generous depth range, which is what makes the
// plane test exact rather than approximate - there is no occluder outside the box along the light
// direction that still casts into it.
//
// `sun_shadow_indirect` submits the survivors as one `vkCmdDrawIndexedIndirect` per (cascade,
// bucket) instead of a `vkCmdPushConstants` + `vkCmdDrawIndexed` pair per caster per cascade. It
// reads back false on a device with no `multiDrawIndirect`, or if the pipeline could not be built,
// and the direct path then draws the same culled set.
void SetSunShadowCull(bool enabled);
bool SunShadowCull();
void SetSunShadowIndirect(bool enabled);
bool SunShadowIndirect();
// What the last pass offered, culled and submitted - including the per-cascade split, which is
// where the halving of the cascade boxes becomes visible rather than assumed.
std::string SunShadowReport();

// --- the bisect knobs (§4.66) -------------------------------------------------------------------
//
// Four independent caps on how much of the bake actually runs, so the hang can be walked down to a
// configuration that SURVIVES - which is the only route to a RenderDoc capture here, since the
// failing frame can never write one (§4.66). **0 means "no cap" on all three counts.** They are
// deliberately applied at the latest possible point, so the report, the range check and the batch
// upload all still describe the whole set and only the submission shrinks.
void SetDynamicShadowMaxLights(int lights);
int DynamicShadowMaxLights();
void SetDynamicShadowMaxFaces(int faces);
int DynamicShadowMaxFaces();
void SetDynamicShadowMaxCasters(int casters);
int DynamicShadowMaxCasters();

// Issue the bake as one `vkCmdDrawIndexed` per caster per face instead of one
// `vkCmdDrawIndexedIndirect` per bucket per face - the same fallback §4.61's map bake keeps, and
// the same atlas either way. **The bisect that splits the indirect machinery from the pass**: the
// direct path reads no indirect buffer, takes no device address for its parameters and needs no
// `SV_DrawIndex`, so if it survives where indirect hangs the fault is in the batch and not in the
// caster set, the projection or the attachment.
void SetDynamicShadowIndirect(bool enabled);
bool DynamicShadowIndirect();

// **A feature**, on by default. Whether D3D's point and spot lights sample that atlas. Independent
// of `render.map_shadows`, because they are two different light systems sharing one image - but
// the atlas is baked if *either* wants it.
void SetLocalShadows(bool enabled);
bool LocalShadows();

// What the local half of the atlas is doing: how many keys are live, how many hold a slot, how
// many were refused for moving and how many for want of room. **Not optional reading** - a light
// that moves and a light the atlas is full for both read as "no shadow" on screen, and so does a
// bake that has not run.
std::string LocalShadowReport();

// Discards the list without drawing it, for a frame that is not rendered.
void ClearDraws();

// Record only draws `first .. last` of the frame's list, inclusive; the default (0, UINT32_MAX)
// is all of them. This is the bisect for "which draw painted that pixel", which nothing else
// here could answer: `render.draws` counts what was skipped and `render.state` histograms what
// was configured, but neither attributes a pixel to a draw. Setting a one-draw range renders
// that draw alone against an empty frame, which is the reading that ends the search (§4.29).
//
// Run-time only, and only useful on a paused frame - the list is rebuilt every frame from
// whatever the game submitted, so an index means nothing across frames unless nothing moves.
void SetDrawRange(uint32_t first, uint32_t last);
void GetDrawRange(uint32_t &first, uint32_t &last);

// Hide draws `first .. last` inclusive and record every other one. The **useful** half of the
// pair, and the reason both exist: a prefix range truncates the depth and stencil buffers along
// with the draw list, so a draw that only becomes visible because the geometry in front of it
// was never drawn reads as the draw that painted the pixel. Hiding a window leaves the rest of
// the frame - and therefore both buffers - almost intact, so "the pixel went away" means that
// window contains what painted it. Bisect on the window, not on a prefix (§4.29).
void SetDrawHide(uint32_t first, uint32_t last);
void GetDrawHide(uint32_t &first, uint32_t &last);
// Everything the renderer knows about one draw of the last recorded frame, as text. Empty if
// the index is past the end.
std::string DescribeDraw(uint32_t index);

// **Does this game's geometry carry smooth normals at all?** The measurement that decides whether
// a PN-triangle tessellation stage can reach anything, taken before one is built.
//
// PN triangles curve a patch by exactly the tangent term `dot(Pj - Pi, Ni)`, so a corner whose
// normal is perpendicular to both of its edges contributes nothing: every control point collapses
// to the linear one and the patch **is** the flat triangle, bit for bit. That makes hard edges
// free rather than a heuristic - and it also means a mesh whose vertex normals are all face
// normals is one tessellation cannot change. This walks the last frame's triangles and reports
// how much of it is in each class.
//
// The reported deviation is `|dot(normalize(Pj - Pi), Ni)|`, the tangent term normalised by edge
// length. That is the quantity the construction actually uses, not a proxy for it: a corner
// reading 0 is exactly flat, and one reading d bulges its edge by about `d * length / 3`.
//
// Bucketed by `IsMapGeometry` - the level mesh against everything else - because the two are
// authored by different tools and the curvature is expected to live in the placed objects.
//
// A REPL diagnostic and nothing else: `ReadArena` submits and waits, twice per draw.
std::string DescribeNormalCensus();

// **Where two triangles meet, do their two PN patches agree?** The question that comes after the
// normal census, and the one that decides whether tessellation tears the level mesh open.
//
// The watertight property §4.71 rests on is conditional, and the condition is about the *data*:
// `b210` for edge (P1,P2) is a function of P1, P2 and N1 alone, so the two triangles sharing that
// edge build the same boundary curve **provided each presents the same position and the same
// normal at each end**. Where the mesh has split a corner into two vertices carrying different
// normals - what an exporter does at a material boundary or a smoothing-group break - the two
// curves run through the same two points by different routes, and the patches pull apart.
//
// So this keys every edge on the **bit patterns of its two endpoint positions**, which is what
// makes a split corner read as one edge rather than two, and for each edge used by exactly two
// triangles measures the widest separation of the two boundary curves in world units, at the
// live `pn_strength` / `pn_flat_threshold` / `pn_max_offset`. It also counts the other way a seam
// opens: an edge whose two sides disagree about whether they are tessellated at all, which
// `IsMapGeometry` and `WantsTessellation` can produce at a material boundary.
//
// A REPL diagnostic and nothing else, and a heavier one than the normal census: it holds a map
// keyed on every distinct edge in the frame.
std::string DescribeSeamCensus();

// Builds this frame's PN split-corner table and copies it into the frame scratch (§4.71).
//
// **Called before RecordUploads and before every pass**, and both halves of that are load-bearing
// - see the call site in VkRenderer.cpp. The short version: its address is into the scratch, which
// rotates, and its analysis is an arena readback, which `RecordUploads` can put out of reach for
// exactly the frame a level's geometry is uploaded and first drawn. Records no commands of its own.
//
// A no-op unless tessellation and `render.pn_seam_fix` are both on, and bounded to a handful of
// newly-seen draws a frame - the analysis is an arena readback, which submits and waits.
void PrepareTessellationTables();

// `render.pn_seam_fix` - zero the PN tangent term at a corner the mesh has split into vertices
// carrying different normals, so the two patches meeting there build the same linear boundary and
// the seam stays closed (§4.71). On by default; the knob exists because it is the A/B that says
// what the rule costs the amplification, and because turning it off reproduces the tear.
void SetSplitCornerFix(bool enabled);
bool SplitCornerFix();
// The table's state, for the report: how many corners are marked, how many draws have been
// analysed, how many are still queued, and whether the bitset outgrew its scratch slice.
void SplitCornerCounts(uint32_t &corners, uint32_t &analysed_draws, uint32_t &pending_draws,
                       bool &too_large);

// The converted vertices and indices one draw was actually given, which is the question no other
// instrument here answers: `render.draws` counts what was skipped, `render.state` histograms what
// was configured, `verify_buffers` proves the arena holds what D3D held, and `draw_range` shows
// what a draw painted - but when a draw paints the wrong *shape*, none of them says why.
//
// Snapshotted at SubmitDraw rather than read back afterwards, because the scratch rotates at the
// bottom of the frame and reading it later is a race against the next scene. Set the index,
// let a frame go by, then read.
//
// **User-pointer draws only.** Their vertices are in the host-visible scratch by construction;
// a buffered draw's are in a device-local arena that is deliberately never mapped, and a
// readback there is `verify_buffers`' job.
void WatchDrawVertices(uint32_t index);
uint32_t WatchedDrawVertices();
std::string DescribeWatchedVertices();

// The LOD probe: force every texture fetch to an explicit mip level, or -1 to sample normally.
//
// It exists because the residual against the original D3D8 **scales with minification** (§4.33) -
// 2.95 on an oblique ground decal against a 0.008 reference floor, and 0.39 on a flat wall - with
// the sampler mapping, the texture pixels and the half-pixel origin all separately verified
// correct. That narrows it to which level is sampled, or to the filtering inside a level, and
// those two need different fixes.
//
// Pinning both sides to level 0 is what tells them apart: the reference takes
// `GKPLUS_NO_MIPMAP=1`, which forces `D3DTSS_MIPFILTER` to `D3DTEXF_NONE` in the forwarded call,
// and this side takes `render.force_lod = 0`. If they converge, the difference is LOD selection.
// If they do not, nothing about mip levels is involved and it is the filtering or the
// coordinates (§4.34).
//
// Run-time only, and a diagnostic rather than a feature - the game has no such state.
void SetForceLod(float lod);
float ForceLod();

// D3DRS_SHADEMODE: honour it, or interpolate everything the way every build before §4.31 did.
//
// On by default, because ignoring a state the game sets is not a defensible default. Off is the
// A/B - toggle it on a paused frame and the difference image is exactly the pixels flat shading
// moves, at a 0.000 noise floor, which is the only comparison sharp enough to see something this
// small (§4.26, §4.28).
void SetShadeMode(bool enabled);
bool ShadeMode();

// Run D3D8's light sum per PIXEL rather than per vertex.
//
// **On by default, and this is the first thing here that is a deliberate departure from the
// original rather than a reproduction of it.** Every other knob in this header exists so a
// difference against real D3D8 can be attributed; this one exists to make the game look better,
// and switching it off is what restores the reproduction. That is why `false` has to be
// bit-identical to the build before it and is checked as such.
//
// The equation is unchanged - the same `light_sum` runs, over the same lights, with the same
// N.L > 0 gate on the specular sum (§4.46). What changes is where it is evaluated, and therefore
// what is interpolated: a lit COLOUR across the triangle, or the position and normal the colour
// is computed from. Gouraud shading cannot represent a highlight smaller than a triangle or a
// point light's falloff across one, which on Gunlok's geometry means a projectile - a handful of
// vertices - has no lighting detail at all.
//
// Three consequences worth knowing, all of them real differences rather than approximations:
//
//   - the interpolated normal is **renormalised per pixel**, where the vertex path normalises
//     only under D3DRS_NORMALIZENORMALS. Interpolating two unit normals yields a shorter vector
//     between them, so not doing it darkens the middle of every triangle;
//   - `D3DRS_LOCALVIEWER`'s eye vector is per fragment rather than per vertex;
//   - `D3DRS_SHADEMODE` FLAT now flat-shades the *vertex colour* the sum consumes rather than the
//     sum's result. Nothing in level01 or level02 can see that - every flat-shaded draw there is
//     one of the stencil shadow's passes and none is lit (§4.31).
//
// `GKPLUS_VK_PER_PIXEL_LIGHTING=0` is the launch-time form, for a session that wants the
// reproduction from the first frame.
void SetPerPixelLighting(bool enabled);
bool PerPixelLighting();

// --- multisample antialiasing -----------------------------------------------------------------
//
// The world pass rasterised at N samples per pixel and resolved on the way out. Off (1) by
// default, because it is a departure from D3D8 like the lighting and shadow work above it and a
// default run has to keep the renderer's residual claim true.
//
// **Settable at any time**, which is the whole point: an A/B on a paused frame is what makes an
// antialiasing change judgeable at all, and a launch-time-only form would mean two sessions and
// two camera positions. `SetMsaa` only records the wanted count - `ReconcileRenderTarget` notices
// the mismatch on the next frame and does the work, under the `vkDeviceWaitIdle` it already takes
// for a resize. That is why this is cheap to expose despite rebuilding three things:
//
//   - the depth image, which carries the count on its own `VkImageCreateInfo`;
//   - a colour target at the render extent and N samples, which the pass draws into instead of
//     the offscreen/swapchain image - that image becomes the pass's RESOLVE attachment and every
//     consumer downstream (the scale blit, the overlay pass, the present barrier) is unchanged;
//   - **the whole world pipeline cache.** `rasterizationSamples` is pipeline state and is not
//     dynamic, so every cached pipeline is wrong the instant the count moves. They are destroyed
//     and rebuilt on demand, which costs the first frame after a change and nothing after it.
//
// Accepts 1, 2, 4, 8, 16, 32 or 64 and **clamps to what the device supports** rather than failing
// - `DeviceCaps::sample_counts`, which is the colour/depth/stencil intersection. A request that is
// not a power of two, or is zero, rounds DOWN to one that is.
//
// Reads back **effective, not wanted**: `Msaa()` is what the frame is being drawn at, so a 4 asked
// for on a device that tops out at 2 reads 2, and a request whose target failed to allocate reads
// 1. Nothing here reports "asked for" - a knob that could not tell "off" from "unavailable" is
// exactly what the stock preset's comment warns about, and the wanted value is kept only so a
// device that grows the capability across a Reset can take it.
//
// **What it does and does not smooth.** Geometric edges, including the stencil shadow volumes -
// stencil is per sample, so the blob's border resolves smoothly for free. It does nothing for
// alpha-TESTED cutouts (the sprites and the foliage), which stay as hard as they were: fixing
// those needs alpha-to-coverage, which changes what every blended draw writes and so would have
// to be its own knob rather than a silent part of this one.
//
// `GKPLUS_VK_MSAA=4` sets the value the first frame comes up at. It is an initial value and not a
// mode - the knob is writable afterwards exactly as if it had never been set.
void SetMsaa(uint32_t samples);
uint32_t Msaa();

// The wanted count, for the stock preset's snapshot - see CurrentDepartures on why a preset must
// never snapshot through an effective-value getter. Not exposed to script.
uint32_t MsaaWanted();

// The count `ReconcileRenderTarget` should build its targets at: the wanted one clamped to
// `DeviceCaps::sample_counts`. Separate from `MsaaWanted` so the clamp happens once, on the way
// to the renderer, rather than at every place that compares two counts.
uint32_t MsaaTargetSamples();

// Adopt a sample count: stamp it on the depth image the next `ResizeDraw` builds, and destroy
// every cached world pipeline, since `rasterizationSamples` is baked into each. **The caller must
// already hold a `vkDeviceWaitIdle`** - see DestroyPipelineCache. `ReconcileRenderTarget` is the
// only caller, and it takes one for the resize it is doing anyway.
void ApplySampleCount(uint32_t samples);

// --- HDR: a linear-light pipeline with an SDR tonemap ------------------------------------------
//
// See "In progress: HDR" in vulkan_renderer_plan.md for the design and the four decisions behind
// it. In this header, what a caller has to know:
//
// `render.hdr` makes the world pass draw into `R16G16B16A16_SFLOAT` instead of the swapchain's
// 8-bit BGRA and replaces the scale blit with a full-screen tonemap pass. It is the biggest
// departure in this file - the first one whose goal is not reproduction - so it is off by default
// and it is in the stock preset's set.
//
// **Settable at any time**, and the machinery for that already exists: the colour format is baked
// into every world pipeline exactly the way `rasterizationSamples` is, so it invalidates the cache
// whole, and `ReconcileRenderTarget` is where both are noticed under the `vkDeviceWaitIdle` it
// already takes. The knob is a store; the work happens between frames.
//
// Three sub-knobs, and they exist so the feature can be bisected on one paused frame rather than
// argued about:
//
//   - `render.linear_input` sRGB-decodes every albedo the shader reads - textures and unpacked
//     D3DCOLOR vertex colours, and deliberately NOT light or material colours, which are
//     intensities rather than pictures (see kLinearInput in world.slang). With it off, `hdr` is
//     the extended-range design: the same numbers in a wider container, nothing but over-range
//     changed. On by default, since running the arithmetic on gamma-encoded values is the thing
//     being fixed.
//   - `render.tonemap` picks the operator, and **it applies to the world alone**: the 2D layers are
//     drawn after the tonemap (see `Layer` above, notes §4.92), so no operator reaches Gunlok's
//     menus, briefing screens, HUD or inventory. That was not always so - before the split ACES
//     recoloured 99.61% of the main menu, and `rolloff` being the identity below its knee was the
//     only thing holding those screens together. It is still the default, but now because it is
//     the conservative choice rather than because anything depends on it. `clamp` is the bisect
//     setting: with `linear_input` off it makes the pass a reproduction of the blit it replaced.
//   - `render.exposure`, `render.tonemap_knee` and `render.tonemap_white` are the operator's
//     parameters. Exposure multiplies before the operator, which is the only place it can go.
//
// `GKPLUS_VK_HDR=1` is the launch-time form.
void SetHdr(bool on);
bool Hdr();
void SetLinearInput(bool on);
bool LinearInput();
// 0 clamp, 1 rolloff, 2 reinhard, 3 aces, 4 filmic (Hable), 5 agx. Out of range is stored as asked
// and behaves as clamp, the same way `SetMsaa` stores a count the device cannot serve.
//
// `tonemap_white` is read by `reinhard` and `filmic`, `tonemap_knee` by `rolloff` alone, and
// **`agx` reads neither** - its range is fixed by its own log window, so `exposure` is its only
// control.
void SetTonemap(uint32_t op);
uint32_t Tonemap();
void SetExposure(float value);
float Exposure();
void SetTonemapKnee(float value);
float TonemapKnee();
void SetTonemapWhite(float value);
float TonemapWhite();

// The format `ReconcileRenderTarget` should build the offscreen and multisampled targets at, given
// the swapchain's own. Separate from `Hdr()` for the reason `MsaaTargetSamples` is separate from
// `MsaaWanted`: the choice is made once, on the way to the renderer, rather than at each of the
// three places that compare two formats.
uint32_t HdrTargetFormat(uint32_t swapchain_format);

// Adopt a colour format: stamp it on every world pipeline built from here on, and destroy the ones
// already cached, since the attachment format is baked into each. **The caller must already hold a
// `vkDeviceWaitIdle`**, exactly as for `ApplySampleCount`, and `ReconcileRenderTarget` is again the
// only caller.
void ApplyColourFormat(uint32_t format);

// Whether the tonemap pass is up. False on a device that could not build it, where the renderer
// falls back to the blit and therefore to SDR - the same "lose the feature, not the frame" rule
// every other optional pass in this file follows.
bool TonemapReady();

// Record the tonemap pass: a full-screen triangle reading `kTonemapSourceSlot` at
// `source_width x source_height` and writing `dest_view` at `dest_width x dest_height`. It begins
// and ends its own rendering; the caller owns the layout transitions on both images, because the
// caller is the only thing that knows what touched them last.
void RecordTonemap(void *command_buffer, uint64_t dest_view, uint32_t source_width,
                   uint32_t source_height, uint32_t dest_width, uint32_t dest_height);

// --- the stock-look preset (§4.87) ------------------------------------------------------------
//
// Every deliberate departure from D3D8, switched together. `render.stock = true` is the setup a
// fidelity comparison against `GKPLUS_RENDERER=d3d8` needs, in one write instead of eleven;
// `false`
// puts back what was there before.
//
// **A preset over the knobs, not a mode of its own.** Nothing in the draw path reads it - it
// writes the same setters a hand-typed A/B does, and each keeps its own behaviour on the way
// through, so `SetLocalShadows` still forgets its atlas keys and `SetLightingMaps` still drops its
// images. That is what makes it correct by construction rather than a second description of the
// pipeline that can drift from the first.
//
// The set is exactly what this header already documents as "off is the build before it existed":
// `per_pixel_lighting`, `map_lighting` and `lighting_maps` - §4.60's "three departures to switch
// off" - plus the four shadow systems the game never had (`sun_shadows`, `map_shadows`,
// `dynamic_shadows`, `local_shadows`), plus `ao`, `tessellation`, `msaa` and `hdr`. The last four
// are off by default already and are here anyway, so that a session which turned them on is not a
// session this lies about.
//
// `msaa` is the one member that is not a bool, and it is in the set for the plainest reason of
// all: the original rasterised one sample per pixel, so any other count is a departure and a
// residual measured under it is measuring this rather than the renderer.
//
// Two things deliberately outside it:
//
//   - **`stencil_shadow` needs no entry.** The game's own blob is dropped only while the sun's map
//     is actually drawing a real one (see SetStencilShadow), so `sun_shadows = false` restores it
//     by itself. Listing it would be a second place to keep that interaction true.
//   - **`hdr`'s sub-knobs need no entry either**, and for a different reason from
//     `stencil_shadow`'s: `linear_input`, `tonemap`, `exposure`, `tonemap_knee` and
//     `tonemap_white` describe how a float target is presented, and with `hdr` off there is no
//     float target for them to describe. Putting them in the set would restore five values that
//     were already inert, and would make an operator the user chose part of what "stock" means.
//   - **The fidelity knobs are untouched** - `half_pixel`, `rhw_depth_raw`, `viewport_rect`,
//     `shade_mode`, `local_lights`, `map_light_cull`. For every one of those ON *is* the
//     reproduction, so switching them would move the frame away from D3D8 rather than towards it.
//     `stock` is not "turn the renderer off"; it is "draw what the original drew".
//
// **It restores the session, not the build's defaults.** Switching to stock snapshots the eleven
// first, so switching back returns a `local_shadows` that was off before to off. The snapshot is
// taken only on a transition *into* stock, so writing `true` twice cannot overwrite it with the
// values it just wrote; with no snapshot to restore - a fresh session's first `false` - it applies
// the defaults, which is the shipped pipeline with `ao` and `tessellation` still off.
//
// Only the eleven switches move. Every parameter under them - `shadow_bias`, `map_light_gain`,
// `bump_scale`, the AO radius - is left exactly as it was, so a tuned value survives the round
// trip without being part of the snapshot at all.
//
// Reads back **derived**: true iff all eleven are currently configured off, so turning one back
// on by hand makes it read false rather than leaving a mode flag that disagrees with the frame. It is
// the *wanted* value of each that is compared, not the effective one - `ao` reads false until its
// pass exists and `tessellation` false on a device without the feature, and a preset that could
// not tell "off" from "unavailable" would restore the wrong frame on that device.
//
// `GKPLUS_VK_STOCK=1` is the launch-time form, applied once when the pipeline comes up.
void SetStock(bool enabled);
bool Stock();

// --- PN-triangle amplification (§4.71) --------------------------------------------------------
//
// Hardware tessellation over the level mesh, with the generated points placed on a cubic Bezier
// patch fitted to the triangle's three corner positions and corner normals (Vlachos et al.).
//
// **The reason this construction and not displacement**: its edge control point,
// `(2*P1 + P2 - dot(P2 - P1, N1) * N1) / 3`, collapses to the linear one exactly when the corner
// normal is the face normal - so a flat-shaded wall reproduces itself bit for bit at any factor,
// with no threshold and no per-material opt-in, while a smooth-normalled pipe curves. It is also
// watertight across a shared edge by construction, where a height-map displacement cracks at
// every material boundary. And it needs no new matrix and no change to the 48-byte vertex.
//
// **What the game actually has**, measured with `render.normal_census` before any of this was
// built, on level02's settled camera: the level mesh is 1,611 triangles of which only **6.4%**
// have all three corners flat, 27.7% of corners are genuinely curved, and the mean tangent term
// is 0.094 - which domes a typical edge by about 3% of its length. So the free hard-edge identity
// covers far less of this game than the construction's reputation suggests, and
// `pn_flat_threshold` is what restores it for the near-flat majority. The props are a different
// mesh entirely: strongly bimodal, 42% exactly flat and 53% curved.
enum class TessSet : uint32_t {
  Off = 0,
  // `IsMapGeometry` - the level mesh, which is what `SubmitAndFlushMapGeometry` submits.
  Map = 1,
  // `IsDynamicCaster` - opaque, depth-writing, indexed - which adds the props and the units. The
  // census says this is where more than half the curvature is, so it is not a debug-only setting.
  All = 2,
};

struct TessellationParams {
  // The screen-space edge length, in the render target's own pixels, that a factor aims for. A
  // patch edge covering twice this gets a factor of 2.
  float edge_pixels = 24.0f;
  // The ceiling and the floor on a factor. `max` is clamped to the device's
  // maxTessellationGenerationLevel; `min` above 1 forces uniform amplification, which is how the
  // shape can be judged without the factors varying underneath it.
  float max_factor = 8.0f;
  float min_factor = 1.0f;
  // How much of the PN tangent term survives: 1 the full construction, 0 exactly linear. At 0 the
  // surface is the untessellated one however high the factors go, which makes it the A/B that
  // separates "the amplification is wrong" from "the curvature is wrong".
  float pn_strength = 1.0f;
  // A normalised tangent term at or below this is snapped to exactly zero, making its corner
  // flat. **Watertight**, because the term is a function of `(Pi, Pj, Ni)` alone and the triangle
  // across the edge tests the identical number - which is why the threshold is on this quantity
  // and not on, say, the triangle's own flatness.
  float pn_flat_threshold = 0.02f;
  // **The ceiling the flat threshold's floor could not reach** (§4.74): how far, in world units, a
  // control point may sit off its chord. `pn_flat_threshold` is normalised by the edge length on
  // purpose, so it means the same thing at every scale - and that is exactly why it cannot bound
  // this. The bulge is `term * length / 3`, and Gunlok builds its round objects from very few,
  // very long segments, so an entirely legitimate 0.3 term on a 3-unit pipe segment moves the
  // surface half a unit and the pipe reads as inflated rather than rounded.
  //
  // In world units and not a fraction of the edge, because what it bounds is a distance on screen.
  // A large number disables it; the A/B against `pn_strength = 0` is what separates "the ceiling
  // is wrong" from "the curvature is wrong".
  float pn_max_offset = 0.08f;
  // The shadow passes' factor, uniform over every edge - which makes those passes watertight for
  // free, since a constant cannot disagree with itself across a shared edge. Lower than the
  // colour pass's ceiling on purpose: a shadow map does not need silhouette detail the way a
  // silhouette does, and this pass is where the cost is (§4.62's 4,092 indirect faces).
  float shadow_factor = 3.0f;
};

void SetTessellationEnabled(bool enabled);
bool TessellationEnabled();
void SetTessellationSet(TessSet set);
TessSet TessellationSet();
// Whether the shadow passes tessellate too. Separable from the colour pass because the bake is
// where the cost is (§4.62's 4,092 indirect faces), so a frame-time regression can be attributed
// to one half rather than to the feature.
void SetTessellationShadows(bool enabled);
bool TessellationShadows();
const TessellationParams &TessParams();
TessellationParams &MutableTessParams();
// How many draws the last frame tessellated, and how many patches that came to. The reading that
// says the set predicate is selecting what it was meant to - a count that jumps when
// `render.tess_set` changes to `all` and not otherwise.
void TessellationCounts(uint32_t &draws, uint32_t &patches);

// Runtime map lighting: replace the level's BAKED per-vertex colour with a per-pixel evaluation
// of the `.rif`'s own light rig (src/MapLights.h, model fitted in §4.54).
//
// **It substitutes into the slot the bake already occupies.** `D3DRS_DIFFUSEMATERIALSOURCE` is
// `D3DMCS_COLOR1` on every lit draw, so the vertex colour *is* the material diffuse inside D3D's
// own equation - not the final colour. Replacing that one term leaves the live light sum, both
// texture stages and the gamma-space multiply exactly as they were, which is why the level's
// brightness balance survives. Neutralising the bake to white and lighting on top instead is
// §4.25's bug: everything unfogged and far too bright.
//
// No gamma conversion anywhere, and that is not an oversight. The model was fitted directly
// against the stored bytes over 255, so what it produces is already in the encoding the stages
// consume - the fit absorbed the transfer function rather than leaving one to apply.
//
// **On by default since §4.60.** It was off on performance grounds rather than fidelity ones - it
// evaluated every light in the level per pixel, 686 of them on level01 - and §4.56's world-space
// grid fixed that without anyone measuring by how much. §4.60 did: with the grid it costs
// **1.83 ms on level01** and nothing measurable on level02, level04 or level05, against 30 ms
// without it. That was the whole of the case for off.
//
// It is still the level's rig, so it is still judged on level04 or level05 - level02 fits it at
// r 0.37 against 0.87-0.96 elsewhere (§4.54), and level02 is the level everything else here is
// measured on. A fidelity comparison against `GKPLUS_RENDERER=d3d8` now has three departures to
// switch off, not two.
void SetMapLighting(bool enabled);
bool MapLighting();

// The one free parameter of that model, and the default (1.35) is the mean of the fitted gains on
// the three levels where the model holds - 1.1 on level01, 1.5 on level04 and level05. It is a
// per-level quantity rather than a constant, so this is a lever and not a calibration.
//
// It is also the sharpest validation the feature has: on level04 the on-screen difference from
// the bake minimises at exactly what the offline fit put it at, from vertex data with no
// rendering involved (§4.55). **The three moved from 0.9 / 1.35 / 1.35 with §4.64's windowed
// tail**, and had to: a dimmer tail refits to a brighter gain.
// Substitute on every lit draw rather than only on the map's own geometry.
//
// Off by default, and the default is measured: a prop or a unit is a separate `RBOBJECT` whose
// `SHPVTINT` was baked from its OWN file's light rig, so substituting the level's there swaps one
// object's bake for another's. The fit in §4.54 only ever covered the map object. This exists so
// that claim stays checkable rather than as a feature.
// Bin the map's lights into a world-space grid and have each fragment read only its own cell,
// instead of looping every light in the level.
//
// **On by default, and off must be BIT-IDENTICAL rather than close.** A light's `range` is a hard
// cutoff in the fitted model, so a light whose sphere misses a cell contributes exactly zero to
// every fragment in it - the grid drops nothing that would have been added. That makes the A/B a
// correctness test rather than a quality trade, and it is the only test that can catch a cell
// silently missing a light, which otherwise looks like art.
void SetMapLightCull(bool enabled);
bool MapLightCull();

void SetMapLightingAll(bool enabled);
bool MapLightingAll();

void SetMapLightGain(float gain);
float MapLightGain();

// The D3D9 pixel-centre convention, as a half-pixel viewport offset.
//
// D3D8/9 put the centre of pixel (i, j) at screen coordinate (i, j); Vulkan (and D3D10+) put it
// at (i + 0.5, j + 0.5). Both map the same NDC onto the same rectangle, so nothing about the
// projection differs - what differs is where inside each pixel the rasteriser samples, which
// shifts every interpolated value by half a pixel. Measured rather than assumed: aligning a
// d3d9 shot against a Vulkan one of the same paused frame minimises at exactly (+0.5, +0.5)
// over the scene, and the residual is the edge fringe on every silhouette (notes §4.28).
//
// On by default; `render.half_pixel = false` restores the previous behaviour, which is the only
// way to A/B it on one paused frame at a 0.00 noise floor rather than across two launches.
void SetHalfPixel(bool enabled);
bool HalfPixel();
// The world pass's viewport origin: 0.5 with the offset on, 0 without. The overlay does not use
// it - ImGui's Vulkan backend sets its own viewport, and it is drawn for the human rather than
// to match d3d9.
float ViewportOrigin();

// A pre-transformed vertex's z is the depth value, clamped to the viewport's slice - it is not
// something to run the viewport's depth range over.
//
// D3D skips the viewport transform for a D3DFVF_XYZRHW vertex - that is what "pre-transformed"
// means - and clamps instead: `depth = clamp(z, MinZ, MaxZ)`, no scale and no bias. Measured
// with `render.depth_probe` against the real D3D8 runtime, discriminating in both directions
// and with z inside the slice and outside it (notes §4.45). Vulkan has no such bypass: minDepth
// and maxDepth are applied to every vertex the pipeline rasterises. With Gunlok's world slice of
// 0.1..1.0 that turned every screen-space z into `0.1 + 0.9 * z`, pushing each one away from the
// camera by `0.1 * (1 - z)` - an error that shrinks as the draw approaches the far plane, which
// is why the effect layers came and went with camera distance rather than being uniformly wrong.
//
// Two halves, and they only work together (the first cut had one and regressed the HUD):
// `BuildMvp` feeds Vulkan `(z - MinZ) / (MaxZ - MinZ)` so the viewport transform hands back `z`,
// and `PipelineState::depth_clamp` turns the resulting out-of-NDC case into D3D's clamp rather
// than a clip.
//
// On by default, and read at record time, so both halves move together. `render.rhw_depth_raw =
// false` restores the previous behaviour on the next frame - the game re-issues the same draws
// while paused, so this is still A/B-able on one paused frame rather than across two launches.
void SetRhwDepthRaw(bool enabled);
bool RhwDepthRaw();

// Honour D3DVIEWPORT8's RECTANGLE per draw, as the Vulkan viewport and scissor, rather than
// covering the whole render target with one (§4.47).
//
// Gunlok sets two rectangles. Everything in a level uses the whole backbuffer, where the two
// behaviours coincide - which is why this went unnoticed for forty-six sections and why every
// whole-frame number measured on level02 is unaffected. The **upgrade screen** sets
// `32,24 575x431`, and with the rectangle ignored its draws are stretched over the full 640x480
// and anchored at 0,0: the panel and the character grow by 640/575, the edges fall off the
// window, and - because the frame mixes rectangles, the HUD plates staying at `0,0 640x480` -
// the parts that did not move end up somewhere else relative to the parts that did. That is the
// "selection rectangles and text are shifted" a player sees.
//
// Two halves again, and again they only work together: the Vulkan viewport places the 3D
// projection inside the rectangle the way D3D's does, and `BuildMvp` **subtracts** the
// rectangle's origin from a pre-transformed vertex, because D3D does not add it there and Vulkan
// will. Both measured with `render.viewport_probe` against the real D3D8 runtime, which also
// settled the third part - D3D *clips* to the rectangle, so it is the scissor as well.
//
// On by default, and read at record time so both halves move together, exactly like
// `rhw_depth_raw`. `render.viewport_rect = false` is the pre-§4.47 behaviour.
void SetViewportRect(bool enabled);
bool ViewportRect();

// --- the material override --------------------------------------------------------------------
//
// What a mod says about every draw that samples one texture, and the first thing here that adds
// something the game never had rather than reproducing something it does.
//
// **Keyed by the `.rim` asset name, because that is the identity a mod can write down.** A
// bindless index is assigned at image creation and depends on load order; a wrapper pointer is
// not even stable within a session. The name is what a mod author can see in `render.textures`
// and put in a file, so it is what the table is addressed by - a case-insensitive *substring*
// of the path the engine acquired the texture under, which is the same rule `render.probe` takes
// (§4.35).
//
// **It is a rewrite of a material-table entry, not a per-draw interception**, which is what §2's
// design was for. `GpuMaterial` is interned per frame from the D3D state (§4.30), so an override
// applies where the material is built: 274 draws on level02 are 29 materials, and a texture
// swapped here is swapped for every draw that shares one. The per-draw cost is one array lookup
// by bindless index, and none at all while no override is registered.
//
// Three things one override can do, all resolved from the same key:
//   - `texture` names another loaded image to sample *instead*, at whichever stage the original
//     is bound to. The stage keeps its own sampler and its own colour/alpha ops: those are the
//     game's choices about how the surface is shaded, and only the picture is being replaced.
//   - `tint` multiplies the fragment's final colour, after the stages, the alpha test and the
//     specular add. Applying it *after* the alpha test is deliberate - a tint that could change
//     which fragments are discarded would move silhouettes, and cutting holes in geometry is not
//     what a colour is for.
//   - `hide` drops every draw whose stage-0 texture matches, before it reaches the frame's list.
//
// `tint` and `hide` key on the draw's **stage 0** texture, which is the surface's own identity;
// `texture` applies at any stage, so replacing the chrome layer works too. This is a Vulkan-renderer
// feature and does nothing under `GKPLUS_RENDERER=d3d8` or `d3d9` - those forward to a runtime
// that has never heard of it.
struct MaterialOverride {
  // A `.rim` substring naming the image to sample instead. Empty keeps the original.
  std::string texture;
  float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bool hide = false;
};

// Registers or replaces the override for `name`. Insertion order is preserved and the first
// matching key wins, so a later, broader key cannot silently take over from an earlier one.
void SetMaterialOverride(const std::string &name, const MaterialOverride &over);
// Removes one by its key, matched exactly (case-insensitively) rather than as a substring - the
// key is a string the caller chose, so removing it should not depend on what it matched.
bool RemoveMaterialOverride(const std::string &name);
void ClearMaterialOverrides();
// Every registered override, with the live images its key currently resolves to and what each
// resolves *to*. The readback matters more here than for a diagnostic: a substring key can match
// nothing, or match more than the author meant, and neither shows up as an error.
std::string DescribeMaterialOverrides();

uint64_t DepthImageView();
uint64_t DepthImage();
// Whether the chosen depth format carries a stencil aspect. The renderer needs it for the
// attachment's aspect mask, its layout and the stencil attachment itself; every one of those
// is wrong for a depth-only format and wrong in a way validation reports rather than draws.
bool DepthHasStencil();
uint32_t DepthFormatValue();

const DrawStats &Draws();
// The capture layer counts what it could not turn into a draw, so the skip reasons live with
// the rest of the draw statistics rather than in a second place.
DrawStats &MutableDrawStats();
std::string FormatDrawStats();

} // namespace vulkan
} // namespace gk
