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
#include <string>

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
  float position[4];    // xyz in world space
  float direction[4];   // xyz in world space, already normalised
  float diffuse[4];
  float specular[4];
  float ambient[4];
  float attenuation[4]; // att0, att1, att2, range
  float spot[4];        // cos(theta/2), cos(phi/2), falloff, type
};
static_assert(sizeof(GpuLight) == 112, "the scratch stride is part of the shader ABI");

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
  uint32_t light_offset;       // into the frame's light array
  uint32_t light_count;
  uint32_t lighting;           // the flags above
  uint32_t lit_colour;         // packed D3DCOLOR, read only under kLitCollapse
};
static_assert(sizeof(GpuDrawRecord) == 288, "the scratch stride is part of the shader ABI");

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
  // where stage 1 is the lightmap set the game shares across a level. It samples through stage
  // 0's own sampler, so a clamped surface's map is clamped too - a wrap mismatch between a
  // texture and its own companion would show as a seam at exactly the edge.
  uint32_t lighting_texture = kNoTexture;
  // Reserved. Twelve useful words are 52 bytes and an array's std140 stride rounds to 64, so
  // these are free, and spelling them out keeps the C++ and Slang structs the same size rather
  // than leaving one to guess at the tail.
  uint32_t pad0 = 0;
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
  // `depthClampEnable`, set for a pre-transformed draw and only for one. D3D **clamps** a
  // pre-transformed vertex's z into the viewport's MinZ..MaxZ rather than clipping the
  // primitive away - measured both ways round with `render.depth_probe` (notes §4.45) - and
  // with BuildMvp's compensation in place, "outside the slice" is exactly "outside NDC [0,1]",
  // which is the case this bit decides. It must stay off for the 3D path, where D3D really does
  // clip at the near and far planes.
  uint32_t depth_clamp = 0;

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
  bool indexed = true;     // DrawPrimitiveUP has no indices at all
  uint8_t index_stride = 2; // 2 or 4, for the index type to bind
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
  // How often the viewport had to be reissued because a draw wanted a different depth slice.
  // Not an invariant - it is the size of the technique in §4.32, and a level where it reads 0
  // would be one where the engine never layers anything in front of the world.
  uint64_t viewport_sets = 0;
  uint64_t pipelines = 0;        // distinct pipeline states seen, i.e. VkPipelines created
  uint64_t pipeline_binds = 0;   // how often the bound pipeline changed within a frame
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
void RecordDraws(void *command_buffer);

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
// `texture` applies at any stage, so replacing a lightmap works too. This is a Vulkan-renderer
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
