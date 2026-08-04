#pragma once

// The D3D8 capture layer - the seam the Vulkan renderer will eventually sit behind.
//
// vulkan_renderer_notes.md section 1 is the argument for putting it here rather than on the
// render queue, and it rests on a measurement in rendering_notes.md section 4.1: of the nine
// functions that call the four `Aw_Draw*` wrappers, only two are downstream of
// RenderQueue_Flush. Text, particles, the in-game menus, the shadow renderer and the
// world-effect overlays all draw immediately and are invisible to a queue hook. Two of the
// four wrappers are also user-pointer draws, so for those no IDirect3DVertexBuffer8 ever
// exists and a CreateVertexBuffer hook misses them too.
//
// What IS total is the device: exactly four call sites in the whole binary reach an
// IDirect3DDevice8::Draw* slot, all four inside those wrappers, and they are the only
// functions that also reference `direct3d_device` @ 0x007c121c.
//
// This is a RECORDER, not a translation layer. It never implements D3D8 semantics on
// anything - it keeps a flat struct of fixed-function state and, per draw, snapshots the
// parts that matter. Today every method still forwards to d3d8to9 and the game renders
// exactly as before; the point of this phase is to measure what the state set actually is.
//
// Wiring: `D3D8CaptureSystem` detours d3d8to9's own `Direct3DCreate8` rather than replacing
// it. Replacing it would mean re-implementing the d3dx9_43.dll loading it does (whose
// function pointers d3d8to9_device.cpp then uses), and dropping its translation unit would
// take those definitions with it.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct IDirect3DDevice8;
struct IDirect3DDevice9;

namespace gk {
namespace d3d8 {

// What Phase 0b is measuring. Everything here is main-thread (the renderer does no work on
// the executor thread), so none of it is synchronised.
struct CaptureStats {
  // Per-draw observations. The FVF set is what decides whether the "one canonical vertex
  // format" simplification in vulkan_renderer_notes.md section 2 is safe.
  std::map<uint32_t, uint64_t> fvf_counts;
  std::map<uint32_t, uint64_t> primitive_type_counts;
  uint64_t draws_buffered = 0; // DrawPrimitive + DrawIndexedPrimitive
  // DrawIndexedPrimitive calls whose MinIndex/NumVertices or StartIndex/PrimitiveCount reach
  // past the bound buffer. D3D8 tolerated it, D3D9 rejects the call outright, and d3d8to9
  // returns D3D_OK either way - so this is not our defect but the reference renderer's, and
  // it is the only counter here that is expected to be non-zero (§4.29).
  uint64_t draws_out_of_range = 0;
  // Draw calls the FORWARDED runtime refused - a failing HRESULT out of `inner_`. Nothing had
  // ever read one, so "the reference does not draw this" had no explanation that did not
  // require the reference's state to differ from ours. A refused call draws nothing while
  // every state, every vertex and every counter on this side reads perfectly correct, which is
  // exactly the shape of §4.29's HUD columns and §4.40's plate quad.
  uint64_t draws_refused = 0;
  uint64_t draws_user_ptr = 0; // the two *UP variants

  // Fixed-function state actually exercised. Keyed by D3D enum; the value set is bounded in
  // practice, which is the point - it sizes GpuMaterial.
  std::map<uint32_t, std::set<uint32_t>> render_states;
  std::map<uint32_t, std::set<uint32_t>> stage_states; // key = stage << 16 | type
  std::set<uint32_t> transform_states;
  uint32_t max_stage_used = 0;
  uint32_t max_light_index = 0;

  // Resources, for arena sizing and the bindless texture table capacity.
  //
  // The `_created` and `_bytes` figures are CUMULATIVE - every buffer ever made, including
  // the per-frame churn, which for a level session runs to tens of thousands of buffers and
  // hundreds of megabytes. They say nothing about what is resident.
  uint64_t textures_created = 0;
  uint64_t vertex_buffers_created = 0;
  uint64_t index_buffers_created = 0;
  uint64_t vertex_bytes = 0;
  uint64_t index_bytes = 0;
  std::map<uint32_t, uint64_t> texture_formats;

  // --- Phase 2b: what is actually resident ---------------------------------------------
  //
  // The number that sizes the Vulkan vertex/index arenas, and the reason the buffer objects
  // are wrapped at all: creation is visible from CreateVertexBuffer, but only Release
  // reaching zero says when it stops being live.
  uint64_t live_vertex_buffers = 0;
  uint64_t live_index_buffers = 0;
  uint64_t live_vertex_bytes = 0;
  uint64_t live_index_bytes = 0;
  uint64_t peak_live_vertex_bytes = 0;
  uint64_t peak_live_index_bytes = 0;
  uint64_t peak_live_buffers = 0;

  // How much data is written through Lock/Unlock per frame. This sizes the staging ring in
  // section 3 of vulkan_renderer_notes.md - the arena says how much must live on the GPU,
  // this says how much has to get there each frame.
  uint64_t locked_bytes_this_frame = 0;
  uint64_t max_locked_bytes_per_frame = 0;
  uint64_t locks = 0;

  // Buffers handed to SetStreamSource/SetIndices that this layer never created, so it cannot
  // unwrap them. Must be 0. A non-zero value means something is producing buffers behind the
  // capture device's back, and the pointer is passed through untouched rather than
  // static_cast to a wrapper it is not - which would read a garbage `inner_` and fault deep
  // inside d3d9.dll, a long way from the cause.
  uint64_t foreign_buffers = 0;

  // Locks whose contents could not be staged into a Vulkan arena - the arena was full, or
  // the payload was larger than the whole staging ring. Counted rather than truncated: a
  // partial upload would look like valid geometry. See vulkan::ResourceStats for which.
  uint64_t failed_uploads = 0;

  // Vertex buffers whose FVF the converter cannot handle, so they never reach the
  // arena. Must be 0: the game uses six FVFs and all six are supported (section 4.1),
  // so anything here is a layout the enumeration missed.
  uint64_t unconvertible_buffers = 0;

  // --- Phase 2c-iii: how the pixels actually arrive -------------------------------------
  //
  // D3D8 offers two ways into a texture's bits: IDirect3DTexture8::LockRect, and
  // GetSurfaceLevel followed by IDirect3DSurface8::LockRect. Measured rather than assumed -
  // the same discipline that found ProcessVertices.
  uint64_t live_textures = 0;
  uint64_t texture_lock_rects = 0;
  uint64_t texture_surface_levels = 0; // GetSurfaceLevel: the second route in

  // --- Phase 2c-iv: settling which route the engine takes --------------------------------
  //
  // `texture_surface_levels` counted the calls but not what was done with the result, and it
  // came out at exactly 2 per LockRect - equally consistent with a harmless query and with a
  // write path this layer could not see. IDirect3DSurface8 is now wrapped, so the answer is
  // these counters rather than the ratio.
  //
  // `surface_texture_lock_rects` is THE number: a LockRect on a surface that came from
  // GetSurfaceLevel is a texture write IDirect3DTexture8::LockRect never saw. Zero means the
  // texture path is total and the bindless upload can be built on LockRect alone.
  uint64_t live_surfaces = 0;
  uint64_t surface_lock_rects = 0;         // all surfaces, backbuffer included
  uint64_t surface_texture_lock_rects = 0; // ... of those, ones owning a texture level

  // The other two ways pixels can reach a texture without any lock at all. Both must be 0
  // for the same reason, and neither is hypothetical - CopyRects is how a D3D8 engine
  // typically uploads from a system-memory surface, and a texture bound as a render target
  // is written by the GPU with no CPU-visible copy anywhere.
  uint64_t surface_copy_rects = 0;      // CopyRects whose destination is a texture level
  uint64_t texture_render_targets = 0;  // SetRenderTarget with a texture's own surface
  // UpdateTexture: the fifth route, and the one §4.12's ratio could not see because it touches
  // neither LockRect nor CopyRects. Found by the content check, not by a counter.
  uint64_t texture_updates = 0;

  // The measured route turned out to be neither of the two guesses: the engine locks a
  // SYSTEMMEM staging texture and blits it into the one it binds. So LockRect really does see
  // every pixel - just on the wrong texture object - and CopyRects is what says where they
  // land. `copy_rects_untracked` is the invariant that makes an upload built on that sound:
  // a CopyRects into a texture whose source is NOT a level of a texture this layer wrapped is
  // a destination whose new contents it cannot know. Must be 0.
  uint64_t copy_rects_untracked = 0;
  uint64_t copy_rects_partial = 0; // ... with a sub-rect rather than the whole surface

  // Read-only locks that failed, so those pixels never reached the image. Must be 0: the game
  // uses only MANAGED and SYSTEMMEM, and both are lockable by definition.
  uint64_t texture_read_failures = 0;

  // Textures given a Vulkan image and populated from their own contents. Should converge on
  // the MANAGED half of `texture_pools`: every texture the game binds gets one, whether or not
  // anything is ever blitted into it.
  uint64_t images_seeded = 0;
  // Textures resolved back to the `.rim` path the engine acquired them under. Not all of them
  // will be: a procedural or engine-internal texture has no cache record, and that is fine -
  // what matters is that everything a mod might want to name is named. Compare against
  // `images_seeded`.
  uint64_t textures_named = 0;
  // Diagnostics for when a texture does not resolve: how many cache records the hook has seen,
  // and how many of those have had a D3D texture stored into them. They separate "the hook is
  // missing acquisitions" from "the record's +0x00 is not the join".
  uint64_t rim_records = 0;
  uint64_t rim_records_bound = 0;

  // Textures by D3DPOOL, which is what separates the staging copies from the ones actually
  // bound for drawing - the bindless table needs only the latter.
  std::map<uint32_t, uint64_t> texture_pools;

  // GetDevice on a resource. Forwarding it would hand the game the unwrapped d3d8to9 device,
  // after which every call it made would be invisible here - the ProcessVertices failure
  // without the crash. Wrapped now; this counts whether the engine ever asks.
  uint64_t resource_get_devices = 0;

  // State blocks. These matter more than they look: a steady-state sample showed 11 render
  // states and 2 stage states across 873,200 draws, with a max texture stage of 0, which is
  // impossible for geometry carrying two UV sets. The material state is not absent - it is
  // compiled into a state block during the level load (AwMaterial::state_block, +0x30) and
  // replayed by ApplyStateBlock, which touches no Set* method.
  //
  // So the individual setters DO see every state, just at block-build time rather than at
  // draw time. `block_states` is what a block actually contains, which is the input to the
  // Phase 2 replay: a block built through the setters can be recorded and replayed into a
  // shadow state, but one made by CreateStateBlock/CaptureStateBlock snapshots device state
  // we never saw and cannot be reconstructed that way. `blocks_opaque` counts those.
  uint64_t blocks_recorded = 0; // built by BeginStateBlock .. EndStateBlock
  uint64_t blocks_opaque = 0;   // built by CreateStateBlock / re-armed by CaptureStateBlock
  uint64_t block_applies = 0;
  uint64_t block_states_total = 0; // summed size of every recorded block
  uint64_t max_block_states = 0;

  // --- Phase 2a: what the shadow state says each draw is made of ---------------------
  //
  // The recorder keeps a flat mirror of the fixed-function state and, per draw, reduces it to
  // two keys. These counts are the measurement that sizes the design in
  // vulkan_renderer_notes.md section 2:
  //
  //   material  - a hash of exactly the fields GpuMaterial will carry (the eight stages'
  //               textures and colour/alpha ops, stage count, alpha test). So "distinct
  //               materials" here predicts the real material table's size, rather than
  //               approximating it.
  //   pipeline  - the handful of states that select a VkPipeline: blend enable and factors,
  //               depth test/write, cull mode, alpha test. "Bucket by pipeline only" is only
  //               a good idea if this number stays small, so it is worth counting before
  //               committing to it.
  // How many draws are issued with fog and with fixed-function lighting on. Both are read
  // from the shadow state at draw time, so they say what the *renderer* has to reproduce
  // rather than what the game happened to set once - a state set and never drawn with costs
  // nothing to ignore.
  uint64_t draws_fogged = 0;
  uint64_t draws_lit = 0;

  uint64_t distinct_materials = 0;
  uint64_t distinct_pipelines = 0;
  uint64_t max_materials_per_frame = 0;
  uint64_t max_active_stages = 0; // stages with COLOROP != DISABLE at draw time

  // Blocks whose contents this layer could not witness, because they snapshot device state
  // rather than replaying calls. Phase 0b measured zero of these; if it ever stops being
  // zero, the shadow state is silently wrong after every Apply of one.
  uint64_t opaque_block_applies = 0;

  // Frame pacing, so "draws per frame" is a distribution rather than a total.
  uint64_t frames = 0;
  uint64_t draws_this_frame = 0;
  uint64_t max_draws_per_frame = 0;

  // A buffer rewritten AFTER a draw in the same frame already read it - the D3D "rename"
  // pattern, where one dynamic buffer is locked, filled, drawn, and locked again for the next
  // batch. The arena holds one slot per buffer and the draw list is not recorded until Present,
  // so every draw off that buffer ends up reading whatever the LAST lock left there. Non-zero
  // means some draws this session rendered the wrong contents. See §4.23.
  uint64_t buffer_rewritten_after_draw = 0;
  // Split by kind, because it decides the shape of the fix: a vertex-only pattern needs the
  // vertex source versioned and can leave the index arena alone.
  uint64_t vertex_buffer_rewritten_after_draw = 0;
  uint64_t index_buffer_rewritten_after_draw = 0;
  // The draws implicated: how many had already read a buffer that was rewritten afterwards.
  uint64_t draws_reading_rewritten_buffers = 0;
  // Of those rewrites, the ones whose new byte range overlaps the range the previous lock
  // wrote. This is the subset that is genuinely wrong: a D3DLOCK_NOOVERWRITE append writes
  // somewhere else and leaves the drawn-from bytes alone.
  uint64_t overlapping_rewrites_after_draw = 0;
  // Rewrites parked in the frame's scratch so the slot keeps the version the earlier draws
  // read. This is the fix working; it should track `buffer_rewritten_after_draw`.
  uint64_t buffer_versions_in_scratch = 0;
  // Rewrites that could NOT be versioned - a partial refill, whose untouched bytes this layer
  // does not keep, or a scratch that had no room. These still overwrite the slot, so the
  // earlier draws read the newer contents. Must be 0.
  uint64_t unversioned_rewrites = 0;
};

// The live counters. Valid from the moment the device is created; all zeroes before that.
const CaptureStats &Stats();

// A human-readable dump, for the REPL and the ImGui overlay.
std::string FormatStats();

// What the renderer has to reproduce, as the shadow state has it: the fog, lighting, blend and
// depth render states, the eight light slots, the material, and a histogram of every texture-
// stage and pipeline configuration actually drawn with.
//
// The histograms are the useful half, and they print VALUES rather than a count - six stage
// configurations and five pipeline states for a level01 session, which is small enough to
// implement one by one. `distinct_pipelines` had counted them since Phase 2a and thrown the
// values away, which is why "6 pipelines" sat in the notes for three sections without anyone
// being able to act on it.
//
// The single-state lines are a sample and are labelled as one: "now" means at Present, which is
// where the REPL runs, so it is whatever the last draw of the frame left behind. Every value
// each state has ever been set to is printed beside it for that reason.
std::string FormatShadowState();

// Discard everything counted so far. Useful for measuring one level load, or one menu
// screen, without the rest of the session in the sample.
void ResetStats();

// Which non-triangle-list topologies the Vulkan path draws, switchable at run time.
//
// Run time and not just an environment variable, because comparing two launches does not
// work: the scene is NOT reproducible between them - two Vulkan runs of identical code at the
// same settle differ by up to 8/255, which is larger than most of what is being measured.
// Toggling this with the game PAUSED gives two frames of the same scene that differ only by
// the feature, which is the only exact comparison available (§4.21). Both are ON by default
// since the stencil buffer landed (§4.27); GKPLUS_VK_TOPOLOGIES now selects a subset rather
// than opting in.
void SetTopologies(bool strips, bool lines);
void GetTopologies(bool &strips, bool &lines);

// Whether the real per-vertex light sum runs, switchable at run time for the same reason and
// with the same procedure. Off falls back to the §4.20 material collapse - the previous build's
// behaviour exactly - so the difference image between the two states IS the light sum (§4.26).
void SetLightSum(bool enabled);
bool GetLightSum();

// True once the game has created its device through us.
bool DeviceCreated();

// The size of the D3D backbuffer the game rasterises into, which is **not** the window's client
// area: Gunlok asks for 640x480 and the client is 628x468, so D3D's windowed Present stretches
// one onto the other (§4.37). A Vulkan viewport covering the swapchain instead scales every
// pre-transformed draw by 628/640 *during* rasterisation, which resamples the texture - so the
// renderer draws into an offscreen target this size and blits afterwards, the way the original
// does.
//
// False before CreateDevice, and false when the game left the size at 0 - which windowed D3D8
// reads as "match the client area", the case where there is nothing to correct for.
bool BackBufferExtent(uint32_t &width, uint32_t &height);

// What the game last cleared its whole backbuffer to.
//
// The Vulkan path clears its own attachments through a load op, so it has to use these rather
// than values of its own - and it used a hardcoded debug blue-grey for the colour until that was
// spotted on screen. The difference is not confined to the background: an alpha-blended or
// additive draw over an uncovered background **blends against it**, so a translucent beam against
// a black sky comes out lighter and hazier over a blue-grey one.
struct ClearValues {
  uint32_t colour = 0xff000000; // D3DCOLOR, ARGB
  float z = 1.0f;
  uint32_t stencil = 0;
  // False until the game has cleared the target at all. Black is the right assumption then: a
  // D3D8 backbuffer's contents after a Present are undefined, so nothing is being preserved.
  bool clears_target = false;
  uint64_t clears = 0;
  // Clears with a rectangle list, which a load op cannot express. Expected to be 0; a non-zero
  // reading means part of the frame is cleared separately and the load op is not the whole story.
  uint64_t partial_clears = 0;
};
const ClearValues &Clears();

// Reads every live texture's every mip level back off the GPU and compares it against the D3D
// texture it came from. Returns "<matched>/<checked> levels match" plus the first mismatch.
//
// The counters say the upload path ran; only this says it put the right bytes there. Stalls
// the GPU once per level, so it is a REPL diagnostic (`render.verify_textures()`).
std::string VerifyTextureImages();

// The same, for every live vertex and index buffer that owns an arena slot: reads the arena
// back and compares it against the buffer's own current contents. Returns
// "<matched>/<checked> buffers match" plus the first mismatch.
//
// A slot holding another buffer's bytes is invisible to every counter, because a draw
// addresses the arena by offset rather than by binding - it simply draws the wrong thing.
// `render.verify_buffers()`.
std::string VerifyBufferSlots();

// Reads the fixed-function state back off the device and diffs it against the shadow mirror.
// Returns "<matched>/<checked> states match the device" plus a line per mismatch.
//
// The mirror is the whole basis of the Vulkan renderer - every draw is described by what the
// shadow says was set - and nothing checked it against D3D until §4.39 found a state-block bug
// that let the two diverge silently for a whole scene. This is `VerifyTextureImages` and
// `VerifyBufferSlots` pointed at state instead of at bytes.
//
// Only the states the shadow tracks and the game has actually set are compared: walking the
// whole render-state space would ask D3D about indices that are not states and report its
// refusals as mismatches. The device compared against is whatever this layer forwards to -
// d3d8to9 under `GKPLUS_RENDERER=vulkan`, Windows' own D3D8 under `d3d8` - which is exactly the
// mirror's contract. A `GKPLUS_NO_*` switch makes the two differ deliberately, so the report
// says when one is set.
std::string VerifyShadowState();

// The same comparison taken **at the moment one draw is issued**, which is the form that can see
// a divergence that only exists mid-scene. Set the draw index, let a frame pass, then read the
// report - the same set-and-read-back shape as `render.draw_vertices`, and for the same reason.
//
// Needs `GKPLUS_RENDERER=vulkan`: the index is a position in the Vulkan draw list, which the
// other renderer modes do not build. -1 disarms.
void WatchDrawState(int64_t index);
int64_t WatchedDrawState();
std::string DescribeWatchedDrawState();

// What the same watched draw actually **pulled**: the indices and vertices the shader reads out
// of the arena, beside the ones D3D reads out of the game's own buffer for that draw.
//
// The one reading nothing else here gives. `VerifyBufferSlots` proves a slot holds what its
// buffer holds and `render.draw_info` prints the offsets a draw was given, but neither says the
// draw addressed the right place - and the arena is one buffer every slot shares, so addressing
// it wrongly yields *other geometry* rather than garbage. That looks like a draw in the wrong
// position and nothing like a bug, which is §4.16's lesson and the gap it left open.
std::string DescribeWatchedDrawGeometry();

// The D3D9 device behind whatever IDirect3DDevice8 the game is holding, unwrapping our
// capture device if it is one. src/GUI.cpp needs this: it reads the game's own
// `direct3d_device` global @ 0x007c121c, which now holds a CaptureDevice rather than a
// d3d8to9 Direct3DDevice8, so the static cast it used to do would be reading the wrong
// object. Returns null if the pointer is neither.
IDirect3DDevice9 *ResolveD3D9Device(IDirect3DDevice8 *device);

// True under `GKPLUS_RENDERER=d3d8`, where the capture layer forwards to Windows' own 32-bit
// D3D8 instead of to d3d8to9 - the reference this project spent thirty sections not having
// (§4.33). There is no D3D9 device behind it, so the ImGui DX9 overlay cannot start; that is a
// deliberate limitation of a mode whose only job is to be the ground truth in an A/B.
bool PassthroughToSystemD3D8();

// Arm the synthetic quad probe: one textured quad, pre-transformed to exact screen pixels, drawn
// last through the capture device's own methods so the reference and this renderer are handed
// the same geometry, texture and stage setup (§4.35). `name` is a case-insensitive substring of
// a live texture's `.rim` path, or empty to disarm; `scale` is screen pixels per texel, so 1.0
// is the no-minification case. Returns what it armed, as text.
// `offset` shifts the quad's top-left by that many pixels, which is what moves the sample points
// between texel corners and texel centres - at 0 a 1:1 quad samples corners, which is the worst
// case for bilinear and not where real geometry lands (§4.35).
// `alpha` renders the texture's alpha channel as greyscale instead of its colour, which is the
// only way to compare alpha at all - a screenshot has no alpha channel, and a probe with blending
// off never shows it.
std::string ArmProbeQuad(const std::string &name, double scale, bool mipmap, double offset,
                         bool alpha);

// `render.ref_range` / `render.ref_hide` - `vulkan::SetDrawRange` and `SetDrawHide` pointed at
// the runtime this layer FORWARDS to, so the reference can be bisected the way the Vulkan list
// already could. A draw outside the range, or inside the hide window, is simply not forwarded.
//
// This is the reading that was missing on the reference side, and its absence is why §4.39 and
// §4.40 could establish "this renderer paints a quad the original does not" and then have
// nowhere to go: every follow-up question - what does the original paint for THAT draw, is it
// painted and then covered, is it painted somewhere else - is a `draw_range` question, and
// `draw_range` only existed for us. Works in `d3d8` and `d3d9` mode, which is the point.
//
// The index is the draw's position in the frame in call order, which is what `PendingDrawIndex`
// counts while every draw reaches the renderer - so a draw located in `vulkan` mode carries its
// number over to `d3d8`. `seen == submitted` in `render.draws` is the check that they are the
// same list; nothing else guarantees it.
//
// Run-time only, and only meaningful on a paused frame: the list is rebuilt every frame.
// `render.frame_draws([first, last])` - one line per draw of the last complete frame, built from
// the shadow state and the call's arguments, so it exists in **every** renderer mode. It is what
// `ref_range` is aimed with: an index is a position in a list the game rebuilds every frame, and
// two runs of one camera do not have the same number of draws, so a number carried over from a
// `vulkan` session lands on a different draw. This is how to find the same draw again.
std::string FormatFrameDraws(uint32_t first, uint32_t last);

void SetRefRange(uint32_t first, uint32_t last);
void GetRefRange(uint32_t &first, uint32_t &last);
void SetRefHide(uint32_t first, uint32_t last);
void GetRefHide(uint32_t &first, uint32_t &last);

// Detours d3d8to9's Direct3DCreate8 in the constructor, undoes it in the destructor. Held by
// the `Subsystems` aggregate in entry.cpp, and it must be constructed before anything that
// wants to see the device - which in practice means before GUISystem.
struct D3D8CaptureSystem {
  D3D8CaptureSystem();
  ~D3D8CaptureSystem();

  D3D8CaptureSystem(const D3D8CaptureSystem &) = delete;
  D3D8CaptureSystem &operator=(const D3D8CaptureSystem &) = delete;
};

} // namespace d3d8
} // namespace gk
