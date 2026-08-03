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
// vulkan_renderer_notes.md section 2 is the design this is converging on. Half of it is here
// now: a draw's per-draw *data* lives in a GpuDrawRecord array and the push constants carry
// only its index (§4.26). The material half - GpuMaterial, keyed by asset name - is still per
// draw in the push constants, so the texture stages below are the part still to move.

#include <cstdint>
#include <string>

namespace gk {
namespace vulkan {

// --- the shader ABI ---------------------------------------------------------------------------
//
// Two arrays a frame, both in host-visible scratch that rotates with the vertex scratch, both
// reached by device address. They exist because the 128-byte push constant block was full at
// 120 (§4.19) and the light sum needs a per-draw matrix, five material colours and a variable
// number of lights - none of which fits.
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

// One D3D8 texture stage, as the shader consumes it. `color` and `alpha` each pack one
// fixed-function operation - op in bits 0..7, its two D3DTA_* arguments in 8..15 and 16..23,
// and D3DTSS_TEXCOORDINDEX in 24..31 of the colour word. Packed rather than spelled out
// because the whole thing is push constants, and 128 bytes is the guaranteed budget.
struct DrawStage {
  uint32_t texture_index = kNoTexture; // bindless slot, kNoTexture for untextured
  uint32_t sampler_index = 0;
  uint32_t color = 0;
  uint32_t alpha = 0;
};

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

  bool operator<(const PipelineState &other) const;
};

struct DrawItem {
  uint32_t record = 0;     // this draw's GpuDrawRecord, in records into the frame's scratch
  uint32_t base_vertex;    // this draw's vertices, in canonical vertices into its source
  uint32_t first_index;    // in indices into its source; ignored when `indexed` is false
  uint32_t count;          // indices when indexed, vertices otherwise
  int32_t vertex_offset;   // D3D's BaseVertexIndex, added to every index
  uint32_t stage_count = 0; // stages up to the first D3DTOP_DISABLE; 0 means diffuse only
  DrawStage stages[2];
  PipelineState pipeline;
  // The alpha test, which is a shader `discard` rather than pipeline state: D3DCMPFUNC in bits
  // 0..3 (0 = the test is off) and D3DRS_ALPHAREF in bits 8..15.
  uint32_t flags = 0;
  // The dynamic half of the stencil state. Read only when `pipeline.stencil_enable` is set,
  // but written unconditionally, because a stale reference value is exactly the kind of thing
  // that would only show up on the one frame the game changes it.
  uint32_t stencil_ref = 0;
  uint32_t stencil_mask = 0xffffffffu;
  uint32_t stencil_write_mask = 0xffffffffu;
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

// Records the whole list into `cmd`, inside a rendering pass the caller has begun, then clears
// it. The depth image view is the caller's to attach - see DepthImageView.
void RecordDraws(void *command_buffer);

// Discards the list without drawing it, for a frame that is not rendered.
void ClearDraws();

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
