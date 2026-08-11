#pragma once

// The capture layer's own internals, shared between the two translation units that make it up.
//
// `D3D8Capture.cpp` is the **recorder**: the wrappers, the shadow state, the setters that keep it,
// and the reduction of each draw to a `vulkan::DrawItem`. `D3D8CaptureReport.cpp` is the
// **evidence**: the histograms, the verifiers, the frame draw log and every `render.*` reading
// built on them. They were one 5,000-line file, and roughly a third of it was the second thing.
//
// The split is along a real line rather than a size one. Nothing in the report TU is on the path
// a frame takes - it only reads what the recorder wrote, and its counters are collected through
// the `Note*` / `Log*` entry points declared at the bottom of this file. Getting that backwards
// is the hazard the split exists to make visible: a diagnostic that mutates state the renderer
// then reads is not a diagnostic.
//
// This header is deliberately NOT `D3D8Capture.h`. That one is the public surface - what
// `entry.cpp`, `JsRender.cpp` and the Vulkan side may use. Nothing outside these two `.cpp`
// files should include this one.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d8to9.hpp>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "D3D8Capture.h"
#include "D3D8Device.gen.inc.h"
#include "Profiler.h"
#include "Render.h"
#include "VkDraw.h"
#include "VkResources.h"
#include "VertexFormat.h"

namespace gk {
namespace d3d8 {

// The live counters, and whether there is a device at all. Defined in D3D8Capture.cpp: the
// recorder owns them and the report TU only reads them.
extern CaptureStats TheStats;
extern bool HaveDevice;

// The bisect and instrument switches, all read once at construction from the environment. They
// are measuring instruments rather than features - each makes the game's OWN renderer draw the
// scene without one thing the Vulkan path is missing, which is what tells "we are missing X"
// apart from "we are wrong in some other way that looks like missing X". D3D8Capture.cpp holds
// the definitions and the reasoning for each.
extern bool WrapVertexBuffers;
extern bool WrapIndexBuffers;
extern bool ForceLightingOff;
extern bool ForceStage1Off;
extern bool ForceSpecularOff;
extern bool ForceNoMipmap;
extern bool ForceNoCull;
extern bool ForceNoZTest;
extern bool ForceNoAlphaTest;
extern bool ForceNoBlend;
extern bool SkipTopologies;
extern bool DrawStrips;
extern bool DrawLines;
extern bool SkipSeeding;
extern bool SkipLitColour;
extern bool SkipStateDefaults;
extern bool DrawLightSum;
extern bool SeedTextures;
extern bool ApplyTextureBlits;

// Counters the reports print and the recorder increments. Not on CaptureStats because they
// measure the SETTERS rather than the draws, and the public struct is the renderer's contract.
extern uint64_t LightSets;
extern uint64_t LightEnables;
extern uint64_t MaterialSets;

// Recorder helpers the reports also need. Defined in D3D8Capture.cpp.
//
// `NoteRewrite` is declared up here rather than beside the rest because `BufferWrapper` calls it
// from a member of a template, where a later declaration is not found.
void NoteRewrite(uint32_t flags, bool overlaps);
uint32_t ActiveStages();
uint32_t ElementCount(D3DPRIMITIVETYPE type, uint32_t primitives);

// The one device the game ever creates, borrowed rather than owned. Null before CreateDevice.
// Declared here, defined below - the wrappers all reach for it and it reads better beside the
// rest of the shared state than buried among them.
struct CaptureDevice;
extern CaptureDevice *TheCaptureDevice;

// Every `.rim` cache record the engine has minted, which is how a D3D texture gets a NAME - the
// only identity a mod can write down (§4.14). Swept per frame rather than resolved once,
// because the record is minted with its D3D pointer null and filled in much later.
extern std::set<AwTexture *> RimRecords;

// What the game has been observed to set, kept so the report can print values rather than
// counts. §4.31 is the case for values: a *count* of flat-shaded draws says 2% and stops, where
// the histogram says all three configurations using it are the stencil shadow - which is the
// fact that decided how to implement it.
extern std::set<uint64_t> ViewportDepthRanges;
extern std::set<uint64_t> ViewportRects;
extern uint32_t BackBufferWidth;
extern uint32_t BackBufferHeight;
extern uint32_t AutoDepthStencilFormat;
extern bool AutoDepthStencilEnabled;

// The camera in world space, from the view matrix. Declared here rather than left file-local
// because the report reads it back off the device for a watched draw, and two copies of a
// matrix inversion is exactly how the report and the shader would come to disagree.
void StoreEye(float *out, const D3DMATRIX &view);
extern ClearValues ClearState;
extern std::set<uint32_t> MaterialKeys;
extern std::set<uint32_t> PipelineKeys;
extern std::set<uint32_t> MaterialKeysThisFrame;
extern uint64_t UnresolvedForeign[2];
extern uint64_t UnresolvedNoImage[2];
extern std::map<uint64_t, uint64_t> UnresolvedFormats;
extern std::map<uint64_t, uint64_t> FirstVertexColours;
extern std::vector<std::string> OutOfRangeDraws;
extern std::vector<std::string> RefusedDraws;
extern std::map<uint64_t, uint64_t> UnslottedVertexBuffers;
extern std::map<uint64_t, uint64_t> RewriteLocks;

// Guards the diagnostic CONTAINERS that a hook on the executor-reachable path mutates -
// currently `RewriteLocks`, written by NoteRewrite from BufferWrapper::UploadLocked and read by
// the report TU's histogram walk.
//
// Deliberately NOT extended to the scalar counters in `TheStats`. Those are also raced, and are
// left that way on purpose: each is a monotonically increasing uint64_t whose high dword stays
// zero for a session, so a lost increment costs a count in a diagnostic and nothing reads one to
// decide anything. A container is different in kind - losing an insert is not the failure, the
// tree corrupting is. The four `live_*` fields are the exception among the scalars and are
// atomic, because they are the only ones DECREMENTED: a lost decrement accumulates for the whole
// session and skews the residency figure section 4.8 sized the arenas from.
inline std::mutex CaptureDiagLock;
using CaptureDiagGuard = std::lock_guard<std::mutex>;

// `render.ref_range` / `render.ref_hide`: the bisect windows for the runtime this layer
// FORWARDS to, which is what makes the ORIGINAL bisectable (§4.42).
extern uint32_t RefRangeFirst;
extern uint32_t RefRangeLast;
extern uint32_t RefHideFirst;
extern uint32_t RefHideLast;

// --- the shadow state -------------------------------------------------------------------
//
// A flat mirror of the D3D8 fixed-function state, updated by the intercepted setters. Flat
// arrays indexed by the D3D enum rather than a map: the whole thing is about 2 KB, lookup is
// a load, and the API bounds the index space (render states stop below 210, stage states
// below 32). A map would cost more than the memory it saved.
//
// This is the "recorder, not a translation layer" half of vulkan_renderer_notes.md section 1.
// Nothing here reproduces D3D semantics; it remembers what was set so that each draw can be
// reduced to the two keys the Vulkan renderer actually needs.

constexpr uint32_t kMaxRenderState = 256;
constexpr uint32_t kMaxStageState = 32;
constexpr uint32_t kStages = 8;
constexpr uint32_t kLights = 8;

struct ShadowState {
  uint32_t render_states[kMaxRenderState] = {};
  uint32_t stage_states[kStages][kMaxStageState] = {};
  IDirect3DBaseTexture8 *textures[kStages] = {};
  uint32_t fvf = 0;
  // The three transforms the recorder ever sees set - D3DTS_VIEW (2), D3DTS_PROJECTION (3) and
  // D3DTS_WORLD (256). Kept as raw D3D row-major, because that is the form the shader consumes
  // (see DrawItem). `have` is per-matrix rather than a single flag: a draw issued before the
  // projection is set has nothing to transform by and must be skipped, not drawn with identity.
  D3DMATRIX world = {};
  D3DMATRIX view = {};
  D3DMATRIX projection = {};
  bool have_world = false;
  bool have_view = false;
  bool have_projection = false;
  // The viewport, which the pre-transformed (XYZRHW) path needs to map pixels to clip space.
  // D3DVIEWPORT8's depth range. Recorded because the renderer hardcodes 0..1 and nothing said
  // whether the game agrees - and a viewport depth range is exactly how a 2000-era engine pins
  // an overlay pass to the near plane (§4.32).
  float viewport_min_z = 0.0f;
  float viewport_max_z = 1.0f;
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  // ...and its rectangle. Zero for every draw of the world, which is why this went unrecorded
  // for the whole of §4.1-§4.46 - but the upgrade screen sets 32,24 575x431, and a renderer that
  // ignores it stretches that screen over the whole window (§4.47).
  int32_t viewport_x = 0;
  int32_t viewport_y = 0;
  // The fixed-function lighting state, kept for the same reason as the transforms: the
  // renderer has to reproduce it, and the only way to find out what the game asks for is to
  // record what it sets. D3D8 has eight hardware light slots and the recorder has never seen
  // an index above 0, so eight is the API's bound rather than a guess.
  D3DLIGHT8 lights[kLights] = {};
  bool light_set[kLights] = {};
  bool light_enabled[kLights] = {};
  D3DMATERIAL8 material = {};
  bool have_material = false;
};

inline ShadowState State;


// Every texture-stage configuration the game draws with, and how often. This is the input to
// the multitexture shader: it says which fixed-function ops have to exist, rather than which
// ones D3D8 defines. The key is the configuration itself and not a hash, because unlike the
// material and pipeline counts - which only had to be *sized* - these values are going to be
// implemented one by one.
struct StageConfig {
  // The FVF is part of the key so the 2D draws can be told from the 3D ones: XYZRHW (0x004) is
  // the HUD and the text, and "what is the HUD doing differently" is otherwise unanswerable
  // from a histogram that mixes it with 100,000 world draws.
  uint32_t fvf = 0;
  uint32_t stages = 0;
  // colorop, colorarg1, colorarg2, alphaop, alphaarg1, alphaarg2, texcoordindex, and then the
  // five that pick the sampler: magfilter, minfilter, mipfilter, addressu, addressv. The
  // sampler states are in the key because a blur has no counter - the only way to find out that
  // the HUD was being minified through a mip chain the game had switched off was to see which
  // filter combination each group of draws actually ran with (§4.28).
  uint32_t stage[2][12] = {};
  uint32_t textured[2] = {}; // uint32_t and not bool: see the assertion below

  bool operator<(const StageConfig &other) const {
    return std::memcmp(this, &other, sizeof(StageConfig)) < 0;
  }
};
static_assert(std::has_unique_object_representations_v<StageConfig>,
              "the ordering is a memcmp, so a padding byte would make equal keys compare "
              "unequal and the histogram would grow without bound");

inline std::map<StageConfig, uint64_t> StageConfigs;

// The same idea for the states that select a VkPipeline. `PipelineKey` already counts how many
// distinct ones there are - six on level01 (§4.7) - but a count cannot be implemented against;
// this says what the six actually are.
struct PipelineConfig {
  uint32_t fvf = 0;
  uint32_t state[15] = {};
  bool operator<(const PipelineConfig &other) const {
    return std::memcmp(this, &other, sizeof(PipelineConfig)) < 0;
  }
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

// SHADEMODE is here rather than with the states this renderer merely records, because the
// histogram is what says which draws are flat-shaded - and a per-draw count alone cannot say
// whether they are the world, the HUD or the four non-triangle-list draws (§4.31).
inline constexpr uint32_t kPipelineStates[15] = {
    D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,     D3DRS_ALPHAFUNC,   D3DRS_ALPHABLENDENABLE,
    D3DRS_SRCBLEND,        D3DRS_DESTBLEND,    D3DRS_ZENABLE,     D3DRS_ZWRITEENABLE,
    D3DRS_CULLMODE,        D3DRS_COLORWRITEENABLE,
    D3DRS_STENCILENABLE,   D3DRS_STENCILFUNC,  D3DRS_STENCILPASS, D3DRS_STENCILZFAIL,
    D3DRS_SHADEMODE};

inline std::map<PipelineConfig, uint64_t> PipelineConfigs;

// Every draw that is not a triangle list, described. There are four a frame, so this can afford
// to record what they are rather than how many: which primitive type, which vertex format,
// whether they came from a buffer or a pointer, how many primitives, and what is bound.
struct OddTopology {
  uint32_t type = 0;
  uint32_t fvf = 0;
  uint32_t user_pointer = 0;
  uint32_t primitives = 0;
  uint32_t stages = 0;
  uint32_t texture_index = 0;
  uint32_t blend = 0;
  uint32_t depth_test = 0;
  uint32_t stencil = 0;      // D3DRS_STENCILENABLE
  uint32_t stencil_func = 0; // ... and what it tests, if it is on
  uint32_t stencil_ref = 0;
  // The screen-space box the draw covers, rounded to whole pixels. Only meaningful for an
  // XYZRHW draw, where the vertices already ARE screen coordinates - which is every one of
  // these. It is what says whether a draw paints a corner or the whole frame.
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t first_colour = 0;
  // z and rhw of the first vertex, as bits so the key stays memcmp-comparable. For an XYZRHW
  // vertex D3D uses z as the depth value and rhw as 1/w; this renderer ignores rhw, and a z
  // outside [0,1] is clipped by D3D and by Vulkan alike - so both are worth seeing before
  // concluding anything about a draw that appears in one renderer and not the other.
  uint32_t first_z = 0;
  uint32_t first_rhw = 0;

  bool operator<(const OddTopology &other) const {
    return std::memcmp(this, &other, sizeof(OddTopology)) < 0;
  }
};
static_assert(std::has_unique_object_representations_v<OddTopology>);

inline std::map<OddTopology, uint64_t> OddTopologies;


// One line per draw of the frame just gone. See LogDraw, which fills it.
struct LoggedDraw {
  uint32_t index = 0;
  uint32_t type = 0;
  uint32_t primitives = 0;
  uint32_t fvf = 0;
  uint32_t blend = 0, src_blend = 0, dest_blend = 0;
  uint32_t z_test = 0, z_write = 0, cull = 0, alpha_test = 0;
  float min_z = 0.0f, max_z = 0.0f;
  // The viewport rectangle this draw was issued under. A column of its own because the frame
  // list is what a wrongly-placed draw is chased through, and until §4.47 it could not show the
  // one state that decides where a draw lands at all.
  int32_t viewport_x = 0, viewport_y = 0;
  uint32_t viewport_width = 0, viewport_height = 0;
  bool user_pointer = false;
  std::string texture;
};

inline std::vector<LoggedDraw> DrawLog;
inline std::vector<LoggedDraw> DrawLogLastFrame;

// What the fixed-function lighting pipeline would compute a colour FROM, for the draws that
// carry no vertex diffuse of their own: the material, and how many lights are switched on.
// Those draws are the HUD, and with lighting on D3D takes their colour from the material -
// which is why the panel is green in the game and was not here (§4.20).
struct LightingInputs {
  uint32_t fvf = 0;
  uint32_t lighting = 0;
  uint32_t enabled_lights = 0;
  uint32_t diffuse = 0;  // the material, packed ARGB
  uint32_t ambient = 0;
  uint32_t emissive = 0;

  bool operator<(const LightingInputs &other) const {
    return std::memcmp(this, &other, sizeof(LightingInputs)) < 0;
  }
};
static_assert(std::has_unique_object_representations_v<LightingInputs>);

inline std::map<LightingInputs, uint64_t> LightingByFvf;

// One D3D light, reduced to a memcmp-comparable key so "the same light" means "the same
// numbers". **That is the only identity a D3D light has**: `SetLight` takes an index the game
// reuses freely, `GpuLight`s are deduplicated by enable mask within a frame, and nothing carries
// across a frame boundary at all - so the census below counts *contents*, and whether a light is
// static in world space is read off how many distinct contents the session has ever seen against
// how many are live in one frame.
//
// Everything is stored as bits rather than as floats so the key is byte-comparable and a NaN
// cannot make two identical lights compare unequal. No quantisation: a light the game re-sets to
// a slightly different position every frame SHOULD read as a new key, because that is exactly the
// thing this measurement is asking about.
struct LightKey {
  uint32_t type = 0;
  uint32_t position[3] = {0, 0, 0};
  uint32_t direction[3] = {0, 0, 0};
  uint32_t diffuse[3] = {0, 0, 0};
  uint32_t range = 0;
  uint32_t attenuation[3] = {0, 0, 0};
  uint32_t theta = 0, phi = 0, falloff = 0;

  bool operator<(const LightKey &other) const {
    return std::memcmp(this, &other, sizeof(LightKey)) < 0;
  }
};
static_assert(std::has_unique_object_representations_v<LightKey>);

// What one distinct light did in one frame, and over the session.
struct LightCensusEntry {
  uint64_t draws = 0;         // draws this light was enabled on, this frame
  uint64_t frames = 0;        // frames it has been present in, over the session
  uint64_t first_frame = 0;   // ... and the first of them
  uint64_t last_frame = 0;
};

// The frame being built, the last complete one, and the session. `FormatFrameLights` reads the
// middle one for the same reason `render.frame_draws` does: the REPL runs at Present, so the
// frame in progress is one draw long by the time anything asks.
inline std::map<LightKey, LightCensusEntry> LightCensus;
inline std::map<LightKey, LightCensusEntry> LightCensusLastFrame;
inline std::map<LightKey, LightCensusEntry> LightCensusSession;
inline uint64_t LightCensusFramesWithLights = 0;
inline uint64_t LightCensusMaxPerFrame = 0;


// Ours is the only reference the game gets; `inner` is released when ours reaches zero.
// Kept deliberately simple: Gunlok creates one device and never queries for another
// interface off it, so there is no aggregation or tear-off to model.
template <typename Interface> struct Wrapper : Interface {
  explicit Wrapper(Interface *inner) : inner_(inner) {}

  // A COM interface has no virtual destructor, and declaring one here introduces a NEW
  // virtual rather than overriding a slot - so MSVC appends it after all of `Interface`'s
  // own slots and the game's vtable indices are unaffected. It is needed because Release
  // does `delete this` through a Wrapper<Interface> *, which is undefined without it.
  virtual ~Wrapper() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override {
    if (!ppvObj) {
      return E_POINTER;
    }
    if (riid == __uuidof(Interface) || riid == __uuidof(IUnknown)) {
      AddRef();
      *ppvObj = static_cast<Interface *>(this);
      return S_OK;
    }
    // Deliberately NOT forwarded to `inner_`. Forwarding would hand the caller the
    // unwrapped d3d8to9 object, and every draw made through it would be invisible here -
    // which is the one thing this layer exists to prevent.
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --refs_;
    if (remaining == 0) {
      inner_->Release();
      delete this;
    }
    return remaining;
  }

  Interface *inner_ = nullptr;

private:
  // Atomic because a COM refcount must be: the executor thread creates and releases vertex
  // buffers (see the note above LiveWrapperLock), so a plain `++`/`--` here is a read-modify-write
  // race whose lost decrement leaks the wrapper and whose lost increment frees it early - a
  // use-after-free on an object the renderer dereferences every draw.
  std::atomic<ULONG> refs_{1};
};

struct CaptureDevice;

// Whether the unlock of a read-only lock may skip the upload path.
// `SetSkipReadOnlyUnlocks` is the setter; see D3D8Capture.h for why it is run-time.
extern bool SkipReadOnlyUnlocksEnabled;
// Whether ProcessVertices is computed here instead of being forwarded to D3D9, and whether every
// call is additionally checked against D3D9's own answer. See D3D8Capture.h.
extern bool SoftwareProcessVerticesEnabled;
extern bool VerifyProcessVerticesArmed;

// --- buffer wrappers ---------------------------------------------------------------------
//
// Wrapped so that Release reaching zero is observable. `Wrapper<T>` already owns exactly one
// reference to `inner_` and destroys it at that point, which is the hook.
//
// A subtlety worth stating: the *device* also holds references, but on `inner_`, not on the
// wrapper - SetStreamSource is given `inner_`. So the game releasing its last reference
// destroys our wrapper while the device may still hold the inner buffer, and that is
// correct: what is being measured is what the game considers live.

template <typename Interface> struct BufferWrapper : Wrapper<Interface> {
  BufferWrapper(Interface *inner, uint32_t length, bool vertex, uint32_t fvf)
      : Wrapper<Interface>(inner), length_(length), vertex_(vertex), fvf_(fvf) {
    if (vertex && !vulkan::FvfSupported(fvf)) {
      unconvertible_ = true;
      ++TheStats.unconvertible_buffers;
    }
    if (vertex_) {
      ++TheStats.live_vertex_buffers;
      TheStats.live_vertex_bytes += length_;
      if (TheStats.live_vertex_bytes > TheStats.peak_live_vertex_bytes) {
        TheStats.peak_live_vertex_bytes = TheStats.live_vertex_bytes;
      }
    } else {
      ++TheStats.live_index_buffers;
      TheStats.live_index_bytes += length_;
      if (TheStats.live_index_bytes > TheStats.peak_live_index_bytes) {
        TheStats.peak_live_index_bytes = TheStats.live_index_bytes;
      }
    }
    const uint64_t total = TheStats.live_vertex_buffers + TheStats.live_index_buffers;
    if (total > TheStats.peak_live_buffers) {
      TheStats.peak_live_buffers = total;
    }
  }

  ~BufferWrapper() override {
    vulkan::FreeSlot(slot_);
    if (vertex_) {
      --TheStats.live_vertex_buffers;
      TheStats.live_vertex_bytes -= length_;
    } else {
      --TheStats.live_index_buffers;
      TheStats.live_index_bytes -= length_;
    }
  }

  // `caller` is the game address that called IDirect3DVertexBuffer8::Lock, taken by the wrapper
  // with `_ReturnAddress()`. One dword, kept because it is the only thing on this path that can
  // NAME a producer: a buffer is an anonymous 64 KB of bytes, the layout census knows only its
  // FVF, and the sampler cannot help - it walks the stack of whatever thread it interrupted, not
  // of the lock. `prof::Describe` turns it into a game function through the Ghidra symbol map
  // (`utils/symdump/`). Overwritten each lock rather than accumulated: the buffers worth asking
  // about are refilled from one site.
  void NoteLock(uint32_t offset, uint32_t size, BYTE **data, uint32_t flags, uint32_t caller) {
    ++TheStats.locks;
    lock_caller_ = caller;
    last_lock_flags_ = flags;
    // A zero SizeToLock means "the whole buffer" in D3D8.
    locked_offset_ = offset;
    locked_bytes_ = size == 0 ? length_ - offset : size;
    locked_ = data != nullptr ? *data : nullptr;
    locked_flags_ = flags;
    TheStats.locked_bytes_this_frame += locked_bytes_;
  }

  // Read on the way OUT of the lock, not on the way in: the game writes its geometry between
  // Lock and Unlock, so before Unlock is the only moment the data is both complete and still
  // mapped. Uploading here means the arenas see exactly what D3D9 sees.
  void UploadLocked() {
    // The one thing the Vulkan path adds that no render knob turns off, and it runs on BOTH
    // game threads (§4.72). It was the leading suspect for the frame-time divergence in §4.79
    // and was never measured; these three zones are what measure it.
    GK_ZONE("upload/UploadLocked", prof::Cat::Upload);
    ++unlocks_;
    // A READ-ONLY lock cannot have changed anything, so there is nothing to re-upload - and this
    // is not a micro-optimization, it was **84% of all per-frame vertex conversion** (§4.84).
    //
    // Gunlok uses `IDirect3DDevice8::ProcessVertices` to have the driver transform geometry into
    // screen space, and then locks the destination buffer `D3DLOCK_READONLY` to read the
    // transformed vertices back. From this layer that read is indistinguishable from a refill -
    // Lock, then Unlock - so the whole 64 KB was being converted to canonical vertices, staged and
    // copied to the GPU on the unlock of a read. On a settled level02 camera that was 126,600
    // vertices a frame of D3DFVF_XYZRHW, ~6 MB of writes, for two SYSTEMMEM buffers that **no
    // draw has ever named as its stream source**.
    //
    // The test is D3D8's own contract rather than anything inferred about those buffers ("the
    // application will not write to the buffer"), which is why it sits here and not behind a
    // heuristic about which buffers look unused: every consumer of a vertex buffer's contents in
    // the runtime and the driver already depends on it, and an app that broke it would corrupt
    // far more than this. `render.vertex_buffer_load` reports the flags, so a buffer arriving here
    // read-only is visible rather than merely assumed.
    //
    // Returning *before* the rewrite bookkeeping is deliberate. A read is not a rewrite: counting
    // one would inflate `draws_reading_rewritten_buffers`, and - much worse - could take the
    // `rewritten_after_draw` branch and park a whole-buffer *version* of it in the frame's
    // scratch, which converts the entire buffer rather than the locked range. That would make the
    // read-back cost more than the refill it is not.
    if ((locked_flags_ & D3DLOCK_READONLY) != 0) {
      ++TheStats.readonly_unlocks;
      if (locked_ != nullptr && vertex_) {
        const uint32_t stride = vulkan::FvfStride(fvf_);
        const uint32_t skipped = stride == 0 ? 0 : locked_bytes_ / stride;
        TheStats.readonly_unlock_vertices += skipped;
        skipped_vertices_ += skipped;
      }
      if (SkipReadOnlyUnlocksEnabled) {
        locked_ = nullptr;
        return;
      }
    }
    // Rewritten after this frame's draws already read it: the slot cannot hold both versions,
    // and the draw list is not recorded until Present, so those draws would read this one.
    //
    // The test is "has ANY draw this frame read this buffer", not "have there been draws since
    // the last rewrite". Those differ exactly when the game refills the buffer twice with no
    // draw in between, and the difference is not academic: `draws_this_frame_` is zeroed by the
    // first rewrite, so the second read 0, took this branch's `else`, and wrote the slot -
    // *over* the version an earlier draw of the same frame was still pointing at. Gunlok
    // refills one shared 64 KB dynamic buffer about five times a frame, so this happened every
    // frame, and it is what drew the fire's screen-space glow quad in place of the HUD panel
    // that draw was issued for (§4.42). Once a draw has read the slot, the slot belongs to it
    // for the rest of the frame.
    const bool rewritten_after_draw = drawn_frame_ == TheStats.frames;
    if (rewritten_after_draw) {
      ++TheStats.buffer_rewritten_after_draw;
      ++(vertex_ ? TheStats.vertex_buffer_rewritten_after_draw
                 : TheStats.index_buffer_rewritten_after_draw);
      TheStats.draws_reading_rewritten_buffers += draws_this_frame_;
      // NOOVERWRITE is the game promising this write lands outside anything already drawn
      // from, which makes the rewrite harmless; DISCARD is the opposite. Neither is trusted on
      // its own - the byte ranges say whether the new write actually overlaps the old one.
      const bool overlaps =
          locked_offset_ < prev_locked_end_ && prev_locked_offset_ < locked_offset_ + locked_bytes_;
      NoteRewrite(locked_flags_, overlaps);
      if (overlaps) {
        ++TheStats.overlapping_rewrites_after_draw;
      }
      draws_this_frame_ = 0;
    }
    prev_locked_offset_ = locked_offset_;
    prev_locked_end_ = locked_offset_ + locked_bytes_;
    if (locked_ == nullptr || locked_bytes_ == 0) {
      locked_ = nullptr;
      return;
    }
    // The slot is claimed on first use rather than in the constructor, because the Vulkan
    // arenas may not exist yet when the game creates its earliest buffers - the renderer
    // comes up on the first Present, which is after the menu has built its geometry.
    if (!slot_.valid && vulkan::ResourcesReady() && !unconvertible_) {
      slot_ = vulkan::AllocateSlot(SlotBytes(), vertex_);
    }
    // The refill case: the slot holds the version an earlier draw this frame already reads, so
    // this one is parked in the frame's scratch and later draws are pointed at it instead.
    // Only a whole-buffer rewrite can be versioned this way - a partial one would need the
    // bytes it did not touch, and this layer keeps no copy of them - so a partial refill falls
    // through to the old behaviour and is counted rather than silently half-built.
    if (rewritten_after_draw) {
      if (locked_offset_ == 0 && locked_bytes_ == length_ && UploadVersionToScratch()) {
        locked_ = nullptr;
        return;
      }
      ++TheStats.unversioned_rewrites;
    }
    if (slot_.valid) {
      if (vertex_) {
        UploadConvertedVertices();
      } else if (!vulkan::UploadIntoSlot(slot_, locked_offset_, locked_, locked_bytes_)) {
        ++TheStats.failed_uploads;
      }
    }
    locked_ = nullptr;
  }

  // Parks the version just written in the frame's scratch, so the arena slot keeps the version
  // this frame's earlier draws are recorded against. The whole buffer goes in, not the locked
  // range: a draw may index anywhere in it, and the offsets stay the buffer's own that way.
  //
  // The scratch is the right home rather than a second arena slot because this data has exactly
  // the lifetime the scratch is built for - one frame, written by the CPU, read once. It is the
  // same trade as a user-pointer draw, arrived at from the other direction.
  bool UploadVersionToScratch() {
    if (unconvertible_) {
      return false;
    }
    if (vertex_) {
      const uint32_t stride = vulkan::FvfStride(fvf_);
      const uint32_t count = stride == 0 ? 0 : length_ / stride;
      if (count == 0) {
        return false;
      }
      const vulkan::ScratchAlloc alloc = vulkan::AllocateScratchVertices(count);
      if (!alloc.valid ||
          !vulkan::ConvertVertices(fvf_, locked_, count,
                                   static_cast<vulkan::CanonicalVertex *>(alloc.mapped), 0,
                                   vulkan::ConvertSource::Version)) {
        return false;
      }
      version_offset_ = alloc.offset;
      // The box for the shadow bakes' cull, taken here because this is the only moment the
      // whole version is readable: `alloc.mapped` is write-combined from now on, and the arena
      // slot this buffer owns keeps the OLDER contents by construction. From `locked_`, the
      // game's own bytes, rather than from the converted copy - same reason.
      version_bounds_ =
          vulkan::PositionBounds(fvf_, locked_, count, version_min_, version_max_);
    } else {
      // A 32-bit index buffer is refused by EmitDraw anyway, so a version of one would never
      // be read; refusing here keeps it out of the scratch as well.
      if (index_stride_ != 2) {
        return false;
      }
      const vulkan::ScratchAlloc alloc =
          vulkan::AllocateScratchIndices(length_ / index_stride_, index_stride_);
      if (!alloc.valid) {
        return false;
      }
      std::memcpy(alloc.mapped, locked_, length_);
      version_offset_ = alloc.offset;
    }
    version_frame_ = TheStats.frames;
    ++TheStats.buffer_versions_in_scratch;
    return true;
  }

  // How much arena this buffer needs. Vertices are widened to the canonical 48-byte layout,
  // so the slot is sized in *vertices*, not in source bytes; indices are copied verbatim.
  uint32_t SlotBytes() const {
    if (!vertex_) {
      return length_;
    }
    const uint32_t stride = vulkan::FvfStride(fvf_);
    return stride == 0 ? 0 : (length_ / stride) * sizeof(vulkan::CanonicalVertex);
  }

  void UploadConvertedVertices() {
    const uint32_t stride = vulkan::FvfStride(fvf_);
    // A lock that does not start on a vertex boundary cannot be mapped onto canonical
    // vertices at all. Never observed; counted rather than guessed at.
    if (stride == 0 || locked_offset_ % stride != 0) {
      ++TheStats.failed_uploads;
      return;
    }
    const uint32_t count = locked_bytes_ / stride;
    if (count == 0) {
      return;
    }
    converted_vertices_ += count;
    // One scratch buffer PER THREAD, kept across calls so the allocation amortizes.
    //
    // It was `static` - one for the process - on the reasoning that "conversion is
    // main-thread". It is not, and this is the same wrong assumption vulkan_renderer_notes.md
    // section 4.72 removed from VkResources.h, three lines below where it was written: the
    // executor thread reaches here through CaptureVertexBuffer::Unlock -> UploadLocked, which
    // is literally the stack that was sampled off the hung build. Shared, the `resize` below
    // can free the block the other thread is mid-`ConvertVertices` into - a wild write of
    // sizeof(CanonicalVertex) per vertex through a dangling pointer, on the UCRT heap this DLL
    // owns rather than the game's pool. And with no reallocation at all the two conversions
    // simply interleave in one buffer, so each thread stages the other's vertices: wrong
    // geometry with every counter in TheStats reading clean.
    //
    // `ResourceLock` cannot cover this. It is taken inside vulkan::UploadIntoSlot, which is
    // reached at the END of this function - by then the pointer has already been prepared, and
    // it is passed in as an argument. The race is entirely upstream of the lock.
    thread_local std::vector<vulkan::CanonicalVertex> scratch;
    scratch.resize(count);
    {
      GK_ZONE("upload/ConvertVertices", prof::Cat::Upload);
      if (!vulkan::ConvertVertices(fvf_, locked_, count, scratch.data(), 0,
                                   vulkan::ConvertSource::Buffered)) {
        ++TheStats.failed_uploads;
        return;
      }
    }
    const uint32_t dst_offset =
        (locked_offset_ / stride) * sizeof(vulkan::CanonicalVertex);
    const uint32_t bytes =
        count * static_cast<uint32_t>(sizeof(vulkan::CanonicalVertex));
    // Split from the conversion above because they fail differently: this one can block on the
    // staging ring, that one is pure arithmetic over the game's bytes.
    GK_ZONE("upload/UploadIntoSlot", prof::Cat::Upload);
    if (!vulkan::UploadIntoSlot(slot_, dst_offset, scratch.data(), bytes)) {
      ++TheStats.failed_uploads;
    }
  }

  uint32_t length_ = 0;
  bool vertex_ = false;
  // The FVF CreateVertexBuffer was given. Zero for index buffers, and zero for a vertex
  // buffer the game declared without one - which would leave its layout unknowable here, so
  // `unconvertible_` drops it from the arena rather than uploading bytes of unknown shape.
  uint32_t fvf_ = 0;
  bool unconvertible_ = false;
  BYTE *locked_ = nullptr;
  uint32_t locked_offset_ = 0;
  uint32_t locked_bytes_ = 0;
  // The D3DLOCK_* flags of the lock now open. DISCARD (0x2000) and NOOVERWRITE (0x1000) are
  // the two that decide whether re-locking a drawn-from buffer is a hazard or a promise.
  uint32_t locked_flags_ = 0;
  // The byte range the previous lock of this frame covered, so an overlap can be told from an
  // append without trusting the flags alone.
  uint32_t prev_locked_offset_ = 0;
  uint32_t prev_locked_end_ = 0;
  // How many times the game has unlocked this buffer. A slot is claimed on the first one, so
  // zero here on a buffer that is being DRAWN says the contents arrived some way this layer
  // cannot see - which is a different problem from an arena that ran out.
  uint64_t unlocks_ = 0;
  // Which frame last drew from this buffer, and how many of that frame's draws did. Together
  // they are the "rewritten after being drawn" test in UploadLocked.
  //
  // `draws_this_frame_` counts draws since the LAST rewrite, not since the frame began - it is
  // zeroed each time a rewrite is versioned, so `draws_reading_rewritten_buffers` attributes
  // each rewrite to the draws it endangered. That makes it the wrong thing to gate the freeze
  // on, which is why `drawn_frame_` alone decides it now: two rewrites in a row with no draw
  // between them left the second reading 0 and writing the SLOT, on top of the version an
  // earlier draw of the same frame was still pointing at. See UploadLocked.
  uint64_t drawn_frame_ = UINT64_MAX;
  uint32_t draws_this_frame_ = 0;
  // The frame whose scratch holds a later version of this buffer, and where in it. A draw uses
  // the version only while `version_frame_` is the current frame - the scratch slice is recycled
  // after that, so it expires on its own and there is nothing to clear.
  uint64_t version_frame_ = UINT64_MAX;
  uint32_t version_offset_ = 0;
  // The object-space box of the version above, for the shadow bakes' cull. Scoped to the same
  // frame `version_frame_` is, since it describes the same bytes.
  bool version_bounds_ = false;
  float version_min_[3] = {0, 0, 0};
  float version_max_[3] = {0, 0, 0};
  // 2 or 4. Lives here rather than on CaptureIndexBuffer because UploadVersionToScratch needs
  // it, and is left at 2 for a vertex buffer, where nothing reads it.
  uint32_t index_stride_ = 2;
  // Set once a read-back of this buffer's own contents has been tried, successfully or not, so
  // a buffer that cannot be read costs one attempt rather than one per draw.
  bool seeded_ = false;

  // Vertices this buffer has had converted into its slot, and vertices whose unlock skipped that
  // because the lock was read-only. Per buffer rather than per layout, because the layout census
  // cannot say whether 28 calls a frame are one buffer refilled 28 times or 28 buffers refilled
  // once, nor whether anything reads the result - and those have different fixes. Read by
  // `FormatVertexBufferLoad`, which is where §4.84 was found.
  uint64_t converted_vertices_ = 0;
  uint64_t skipped_vertices_ = 0;
  // The game address that last locked this buffer, and the flags of that lock. See NoteLock.
  uint32_t lock_caller_ = 0;
  uint32_t last_lock_flags_ = 0;
  // Set once this buffer has been the destination of IDirect3DDevice8::ProcessVertices, whose
  // output the driver writes with no Lock this layer can see.
  bool process_vertices_dest_ = false;

  // Whether any draw has ever read this buffer. Distinguishes "converted and used" from
  // "converted and never looked at" in the two counter pairs above - a buffer the game refills
  // every frame and never draws from is work with no output at all, and nothing here could see
  // that before.
  bool drawn_ever_ = false;

  // Gives this buffer an arena slot from its OWN current contents, for a buffer whose only
  // Unlock happened before the renderer existed. Exactly the same shape, and the same
  // justification, as EnsureTextureImage: being drawn is the definition of needing to be
  // resident, and the buffer is the only thing that still knows what is in it.
  //
  // The read lock is safe for what it is used on: the buffers reaching this path are
  // D3DPOOL_MANAGED, which keeps a system-memory copy by definition. `pool_` is recorded so
  // that stays a checked fact rather than a hope - a DEFAULT-pool buffer is refused.
  void SeedFromContents() {
    if (seeded_ || slot_.valid || unconvertible_ || !vulkan::ResourcesReady()) {
      return;
    }
    seeded_ = true;
    if (pool_ != D3DPOOL_MANAGED && pool_ != D3DPOOL_SYSTEMMEM) {
      ++vulkan::MutableDrawStats().seed_refused_pool;
      return;
    }
    slot_ = vulkan::AllocateSlot(SlotBytes(), vertex_);
    if (!slot_.valid) {
      return;
    }
    BYTE *data = nullptr;
    if (FAILED(LockForRead(&data)) || data == nullptr) {
      ++vulkan::MutableDrawStats().seed_read_failures;
      return;
    }
    locked_ = data;
    locked_offset_ = 0;
    locked_bytes_ = length_;
    if (vertex_) {
      UploadConvertedVertices();
    } else if (!vulkan::UploadIntoSlot(slot_, 0, data, length_)) {
      ++TheStats.failed_uploads;
    }
    locked_ = nullptr;
    UnlockAfterRead();
    ++vulkan::MutableDrawStats().buffers_seeded;
  }

  // The two buffer interfaces have the same Lock signature but no common base that declares
  // it, so the leaf supplies these.
  virtual HRESULT LockForRead(BYTE **data) = 0;
  virtual void UnlockAfterRead() = 0;

  uint32_t pool_ = D3DPOOL_MANAGED;
  // The D3DUSAGE_* the buffer was created with. Reported beside the pool because the two together
  // are what say whether this layer can ever read a buffer's contents back: MANAGED and SYSTEMMEM
  // keep a system-memory copy by definition, DEFAULT + D3DUSAGE_DYNAMIC keeps nothing.
  uint32_t usage_ = 0;
  // This buffer's own region of the arena, held for its whole lifetime and released in the
  // destructor. See BufferSlot in VkResources.h for why it is per-buffer and not per-upload.
  vulkan::BufferSlot slot_;
};

// Declared before the wrappers so their constructors can register themselves; defined with
// Unwrap, which is where the reason they exist is written up.
//
// **Both game threads mutate these**, which is why they have a lock. The executor thread
// creates and destroys vertex buffers: a Ghidra reachability closure from ExecutorThreadProc
// @ 0x00509050 (452 functions) intersected with the 66 functions that reference
// `direct3d_device` @ 0x007c121c yields exactly one - `VertexBufferSet_Create` @ 0x005a2e40,
// the sole caller of the device's CreateVertexBuffer - and it is reached as
// ExecutorThreadProc -> SpawnProjectileActor -> ... -> Renderable_CtorFromShape ->
// MakeBoxCorners -> SharedVB_AddEntry -> SharedVB_Rebuild, which destroys the old
// VertexBufferSet before creating the new one. So a `std::set` insert and erase run on the
// executor while the main thread walks the same tree thousands of times a frame through
// Unwrap and EmitDraw. That is the exact failure of vulkan_renderer_notes.md section 4.72 -
// a red-black tree mutated under a walk gives a cycle and a non-terminating lookup, with no
// exception raised.
//
// Recursive for the same reason ResourceLock is: a guarded walk that ends up releasing a
// wrapper would re-enter through the destructor, and a plain mutex would self-deadlock there.
// The lock is held only across set operations - never across a call back into D3D or the game.
inline std::recursive_mutex LiveWrapperLock;
using LiveWrapperGuard = std::lock_guard<std::recursive_mutex>;
inline std::set<const void *> LiveVertexWrappers;
inline std::set<const void *> LiveIndexWrappers;

// The membership test every reader wants, with the lock taken for it. A bare `.count()` on
// either set from outside this pair of functions is a bug - see the note above.
inline bool IsLiveVertexWrapper(const void *p) {
  if (p == nullptr) {
    return false;
  }
  LiveWrapperGuard guard(LiveWrapperLock);
  return LiveVertexWrappers.count(p) != 0;
}

inline bool IsLiveIndexWrapper(const void *p) {
  if (p == nullptr) {
    return false;
  }
  LiveWrapperGuard guard(LiveWrapperLock);
  return LiveIndexWrappers.count(p) != 0;
}

// Clears `TheCaptureDevice`'s borrowed `stream0_` / `indices_` if either still names `wrapper`.
//
// A free function, and defined in D3D8Capture.cpp, because the wrapper destructors below run
// before CaptureDevice is a complete type here.
//
// The device stores the WRAPPER raw when the game calls SetStreamSource/SetIndices - borrowed,
// with the device's own reference held on `inner_` instead - and nothing used to clear it when
// the wrapper died. The executor destroys vertex buffers (see LiveWrapperLock above), so the
// device could be left naming a freed object that `EmitDraw` then casts and dereferences.
// `IsLiveVertexWrapper` usually catches it and the draw is skipped, but "usually" is doing real
// work there: the pointer can also be reused by a NEW wrapper at the same address, which passes
// the liveness test and draws the wrong geometry. Clearing at the source removes both.
void ForgetBoundBuffer(const void *wrapper);

struct CaptureVertexBuffer final : BufferWrapper<IDirect3DVertexBuffer8> {
  CaptureVertexBuffer(IDirect3DVertexBuffer8 *inner, uint32_t length, uint32_t fvf,
                      uint32_t pool, uint32_t usage)
      : BufferWrapper(inner, length, true, fvf) {
    pool_ = pool;
    usage_ = usage;
    LiveWrapperGuard guard(LiveWrapperLock);
    LiveVertexWrappers.insert(this);
  }
  ~CaptureVertexBuffer() override {
    ForgetBoundBuffer(this);
    LiveWrapperGuard guard(LiveWrapperLock);
    LiveVertexWrappers.erase(this);
  }

  HRESULT LockForRead(BYTE **data) override {
    return inner_->Lock(0, 0, data, D3DLOCK_READONLY);
  }
  void UnlockAfterRead() override { inner_->Unlock(); }

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DVERTEXBUFFER8_METHODS(GK_DECL)
#undef GK_DECL
};

struct CaptureIndexBuffer final : BufferWrapper<IDirect3DIndexBuffer8> {
  // D3DFMT_INDEX16 is 101 and D3DFMT_INDEX32 is 102. The stride is kept rather than the format
  // because it is what the arena offset arithmetic needs, and because a 32-bit index buffer
  // cannot share the frame's single vkCmdBindIndexBuffer - a draw from one is skipped and
  // counted instead of being drawn with its indices misread as pairs of 16-bit ones.
  CaptureIndexBuffer(IDirect3DIndexBuffer8 *inner, uint32_t length, uint32_t format,
                     uint32_t pool)
      : BufferWrapper(inner, length, false, 0) {
    index_stride_ = format == 102 ? 4 : 2;
    pool_ = pool;
    LiveWrapperGuard guard(LiveWrapperLock);
    LiveIndexWrappers.insert(this);
  }
  ~CaptureIndexBuffer() override {
    ForgetBoundBuffer(this);
    LiveWrapperGuard guard(LiveWrapperLock);
    LiveIndexWrappers.erase(this);
  }

  HRESULT LockForRead(BYTE **data) override {
    return inner_->Lock(0, 0, data, D3DLOCK_READONLY);
  }
  void UnlockAfterRead() override { inner_->Unlock(); }

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DINDEXBUFFER8_METHODS(GK_DECL)
#undef GK_DECL
};

// --- textures and surfaces -----------------------------------------------------------------
//
// Textures are wrapped for one reason: LockRect is the only place a texture's pixels exist in
// a form this layer can read. The engine loads .rim files itself and hands the decoded bits to
// D3D, so there is no file to intercept and no D3D-side copy to read back.
//
// Whether that is *enough* is a measurement, not an assumption. D3D8 has three more ways into
// a texture's bits, none of which touches IDirect3DTexture8::LockRect:
//
//   GetSurfaceLevel then IDirect3DSurface8::LockRect
//   CopyRects into a texture's surface
//   SetRenderTarget onto a texture's surface, and let the GPU write it
//
// All three go through IDirect3DSurface8, which is why it is wrapped: with it, each route has
// its own counter and "are the pixels all visible here?" is a number rather than a hope. See
// vulkan_renderer_notes.md section 4.11.
struct CaptureTexture;

inline std::set<const void *> LiveTextureWrappers;
inline std::set<const void *> LiveSurfaceWrappers;

struct CaptureSurface final : Wrapper<IDirect3DSurface8> {
  // `owner` is the texture this surface is a level of, or null for a backbuffer, a render
  // target or a standalone image surface. It is what makes the counters answer the question
  // that matters: a lock on a texture level is a pixel write the texture path missed, a lock
  // on the backbuffer is not.
  CaptureSurface(IDirect3DSurface8 *inner, CaptureTexture *owner, uint32_t level);
  ~CaptureSurface() override;

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DSURFACE8_METHODS(GK_DECL)
#undef GK_DECL

  CaptureTexture *owner_ = nullptr;
  uint32_t level_ = 0; // mip level within `owner_`; meaningless when it is null

  // Inner surface -> the wrapper that owns it. Two things make this a cache rather than a
  // bump-allocation per call. Identity: GetSurfaceLevel runs ~39,000 times per level load
  // and D3D8 callers do compare surface pointers, so handing out a fresh wrapper per call
  // would make the same surface look like a different one. And lifetime: the wrapper holds a
  // reference on `inner_` for as long as it is in this map, so an entry can never be stale -
  // d3d8to9 cannot destroy a surface we are still pointing at and reuse the address.
  static std::map<IDirect3DSurface8 *, CaptureSurface *> SurfaceWrappers;
};

struct CaptureTexture final : Wrapper<IDirect3DTexture8> {
  CaptureTexture(IDirect3DTexture8 *inner, uint32_t width, uint32_t height, uint32_t levels,
                 uint32_t format, uint32_t pool)
      : Wrapper(inner), width_(width), height_(height), levels_(levels), format_(format),
        pool_(pool) {
    // A `Levels` of 0 means "make the whole chain", so the count has to come from the object
    // rather than from the argument. Asked rather than derived from the dimensions, because
    // D3D is entitled to stop early.
    if (levels_ == 0) {
      levels_ = inner->GetLevelCount();
    }
    LiveTextureWrappers.insert(this);
    ++TheStats.live_textures;
  }

  ~CaptureTexture() override {
    vulkan::DestroyTextureImage(image_);
    LiveTextureWrappers.erase(this);
    --TheStats.live_textures;
  }

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DTEXTURE8_METHODS(GK_DECL)
#undef GK_DECL

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t levels_ = 0;
  uint32_t format_ = 0;
  uint32_t pool_ = 0;  // D3DPOOL: SYSTEMMEM is a staging copy, DEFAULT/MANAGED is drawn with
  uint32_t usage_ = 0; // D3DUSAGE, for AUTOGENMIPMAP (0x400) - see MirrorBlitDestination
  // Claimed the first time this texture is bound or blitted into, not at creation: a texture
  // may exist long before the renderer comes up, and a SYSTEMMEM staging copy never needs one
  // at all. `image_failed_` stops a texture whose format has no VkFormat from being retried on
  // every SetTexture, which would turn one unsupported texture into millions of counts.
  vulkan::TextureImage image_;
  bool image_failed_ = false;
  // Per-texture provenance, so a content mismatch can say which route wrote it. The global
  // counters cannot: they show 1:1 locks-to-blits across all textures, which is consistent
  // with one texture being written directly and another only ever blitted.
  uint64_t own_locks_ = 0; // the GAME locking this texture, not our read-backs
  uint64_t blits_in_ = 0;  // CopyRects with this texture as the destination
  uint32_t levels_blitted_ = 0; // bitmask: which mip levels a blit ever landed on
  // The `.rim` path this texture was acquired under, empty if it has no cache record. This is
  // the only stable, writable-down identity a texture has - the wrapper pointer differs every
  // run and means nothing to a mod.
  std::string rim_path_;
};

struct CaptureD3D8 final : Wrapper<IDirect3D8> {
  using Wrapper::Wrapper;

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3D8_METHODS(GK_DECL)
#undef GK_DECL
};

struct CaptureDevice final : Wrapper<IDirect3DDevice8> {
  CaptureDevice(IDirect3DDevice8 *inner, CaptureD3D8 *parent)
      : Wrapper(inner), parent_(parent) {}

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DDEVICE8_METHODS(GK_DECL)
#undef GK_DECL

  CaptureD3D8 *parent_ = nullptr;
  // The window CreateDevice was given. Captured here rather than read from the game's own
  // HWND global because this is the one place it is known for certain to be the window the
  // backbuffer belongs to.
  HWND window_ = nullptr;

  // The wrappers the game last bound, so the getters can return the same objects rather than
  // the inner buffers. Borrowed, not owned: the game holds its own reference for as long as
  // it cares, and the device's reference is on the inner object.
  IDirect3DVertexBuffer8 *stream0_ = nullptr;
  uint32_t stream0_stride_ = 0;
  IDirect3DIndexBuffer8 *indices_ = nullptr;
  uint32_t base_vertex_ = 0;

  // Everything the software ProcessVertices path needs, resolved once per call so the transform,
  // the verifier and the range checks all read the same values. See D3D8Capture.cpp, §4.85.
  struct ProcessVerticesJob {
    CaptureVertexBuffer *source;
    CaptureVertexBuffer *dest;
    uint32_t source_offset;
    uint32_t source_stride;
    uint32_t dest_offset;
    uint32_t bytes;      // of the destination, always VertexCount * 16
    // world x view x projection, in D3D's conventions, not Vulkan's. **Double**, and that is
    // measured - see BuildProcessVerticesMatrix.
    double mvp[16];
  };
  bool ResolveProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                              IDirect3DVertexBuffer8 *pDestBuffer, ProcessVerticesJob &job);
  bool RunSoftwareProcessVertices(const ProcessVerticesJob &job, UINT VertexCount, float *out);
  void VerifySoftwareProcessVertices(const ProcessVerticesJob &job, UINT VertexCount);

  // Not a COM method - a helper that turns one of the game's draws into a vulkan::DrawItem.
  // A member because it needs the bound stream, indices and base vertex, which are the
  // device's state and not the shadow's.
  // Does this DrawIndexedPrimitive address vertices or indices its bound buffers do not have?
  // D3D8's runtime tolerated an out-of-range MinIndex/NumVertices; D3D9's validates it and
  // fails the call - and d3d8to9 returns D3D_OK regardless, so a rejected draw is silent on
  // both sides of the wrapper. This renderer pulls by index and does not care, which is exactly
  // the shape of "drawn here, missing in the reference" (§4.29).
  void NoteIndexedRange(D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices,
                        UINT start_index, UINT primitive_count);
  // Both buffered draw entry points. `indexed` false is DrawPrimitive, which reads no index
  // buffer at all and counts its vertices from `start_vertex`.
  //
  // `min_index` and `num_vertices` are DrawIndexedPrimitive's own, and they are here for the
  // shadow bakes' culling: D3D makes the caller state the vertex range its indices reach into,
  // which is the only thing that turns an index range into a box. Ignored when `indexed` is
  // false, where the range is `start_vertex` for as many vertices as the topology implies.
  void EmitDraw(D3DPRIMITIVETYPE type, UINT start_index, UINT primitive_count, bool indexed,
                UINT start_vertex, UINT min_index = 0, UINT num_vertices = 0);
  // Fills `item`'s world-space box from whichever source can supply one, or leaves it unbounded.
  void StoreDrawBounds(vulkan::DrawItem &item, UINT min_index, UINT num_vertices,
                       uint32_t elements);
  // Transforms an object-space box by the current world matrix onto the item. Split out because
  // three call sites reach it from three different vertex sources.
  void StoreWorldBounds(vulkan::DrawItem &item, const float *lo, const float *hi);
  // The synthetic quad probe (§4.35). Issued through this device's OWN methods rather than
  // through `inner_`, which is the entire point: the states and the draw go down both paths at
  // once, so the reference and this renderer are handed the same geometry, the same texture and
  // the same stage setup with nothing of the scene left in the way.
  void DrawProbeQuad();
  void DrawDepthProbe();
  void DrawViewportProbe();

  void EmitDrawUP(D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertex_data,
                  UINT vertex_stride, const void *index_data, D3DFORMAT index_format,
                  UINT vertex_count);
};


inline CaptureTexture *ProbeTexture = nullptr;
inline float ProbeScale = 1.0f;
inline uint32_t ProbeMipFilter = 0; // D3DTEXF_NONE, so a 1:1 quad cannot silently walk the chain
// Where the quad's top-left sits, in pixels from (16, 16). It exists because **a 1:1 quad at
// integer coordinates samples texel BOUNDARIES, not texel centres** (§4.35).
//
// The arithmetic: a vertex at x0 with u=0 and one at x0+W with u=1 makes the coordinate under
// screen sample position `sx` equal to `(sx - x0) / W`, and texel j's centre is at `(j+0.5)/W`.
// The renderer samples at the pixel centre and then shifts the whole viewport by half a pixel to
// match D3D (§4.28), so `sx` comes out at an integer - which lands on `j/W`, the corner where
// four texels meet and bilinear weights them equally. An offset of +0.5 puts it back on the
// centre, where bilinear returns the texel exactly and a difference means something.
inline float ProbeOffset = 0.0f;
// Render the texture's ALPHA as greyscale instead of its colour, via D3DTA_ALPHAREPLICATE on the
// colour argument. The screenshot has no alpha channel, so this is the only way to compare the
// two renderers' idea of a texture's alpha at all - and alpha is the one input a probe with
// blending off still cannot see (§4.36).
inline bool ProbeAlpha = false;
inline RECT ProbeRect = {};

// The depth probe (§4.45). It answers one question the API documentation does not settle and no
// reading of Gunlok's own draws can: **does D3D run the viewport's MinZ/MaxZ over a
// pre-transformed vertex, or is its z already the depth value?** Vulkan has no choice - the
// viewport transform applies to every vertex - so if D3D skips it, every screen-space draw in
// the frame is at the wrong depth here, and the two readings differ by `MinZ * (1 - z)`, which
// is small enough to hide everywhere except where the game puts a layer just in front of a wall.
//
// It clears the depth buffer to `clear_z`, sets a viewport of `min_z..max_z`, and draws one
// opaque XYZRHW quad at `quad_z` with ZFUNC LESS and no depth write. The two answers are a quad
// that appears and a quad that does not, so the reading needs no precision at all:
//
//     clear 0.5, viewport 0.0..0.5, quad z 0.8
//       viewport applied -> depth 0.40, passes LESS against 0.5 -> the quad is drawn
//       viewport skipped -> depth 0.80, fails                   -> the quad is absent
//
// Armed with `render.depth_probe(...)` and drawn from Present alongside the texture probe, so it
// goes down both paths at once and d3d8, d3d9 and vulkan all answer the same question.
inline bool DepthProbeArmed = false;
inline float DepthProbeQuadZ = 0.8f;
inline float DepthProbeClearZ = 0.5f;
inline float DepthProbeMinZ = 0.0f;
inline float DepthProbeMaxZ = 0.5f;

// The viewport-rectangle probe (§4.47), the depth probe's sibling and for the same reason: the
// API documentation does not settle **whether D3D adds the viewport's X/Y to a pre-transformed
// vertex**, and no reading of Gunlok's own draws can, because every viewport it set for the whole
// life of this renderer was at 0,0 - where the two answers coincide.
//
// The upgrade screen is the first thing that sets a sub-rectangle (32,24 575x431), so the answer
// now decides where every 2D draw on it lands. It draws one opaque magenta XYZRHW quad at fixed
// screen pixels under a viewport of `x,y w*h`, and the reading is where the quad appears:
//
//     viewport 100,60 200x150, quad at (120,80)-(184,112)
//       X/Y added   -> the quad is at (220,140)-(284,172)
//       X/Y ignored -> the quad is at (120, 80)-(184,112)
//
// Both land inside the rectangle, so viewport *clipping* cannot turn one answer into the other -
// which is the trap the depth probe's "keep the quad inside the slice" rule warns about.
inline bool ViewportProbeArmed = false;
inline int32_t ViewportProbeX = 100;
inline int32_t ViewportProbeY = 60;
inline uint32_t ViewportProbeWidth = 200;
inline uint32_t ViewportProbeHeight = 150;


inline int64_t VerifyDrawIndex = -1;
inline std::string VerifyDrawReport;
// The lighting inputs read **off the device** at the moment the watched draw was issued: the
// material, the enabled lights, and the states that decide where each material colour comes
// from. Separate from `VerifyDrawReport`, which only says whether the mirror *agrees* with the
// device - a mirror can agree perfectly and the shader still be handed the wrong equation, and
// until §4.46 there was no way to read the numbers a draw was actually lit with. `render.state`
// prints the same things for the *last* draw of the frame, which for a level02 frame is the
// text, not the geometry anyone is looking at.
inline std::string VerifyDrawLighting;
inline bool VerifyDrawValid = false;

// What the watched draw was made of, snapshotted when it is issued. The buffers have to be
// remembered here rather than looked up later: `stream0_` and `indices_` are whatever is bound
// *now*, and by the time anyone reads a report the frame has moved on.
//
// The readback itself is NOT done here. It submits and waits on the GPU, and doing that in the
// middle of the scene the draw belongs to would stall the frame being measured.
// How many vertices from the draw's own base the at-draw arena read covers. Twelve is what the
// report prints, and the read stalls the frame, so there is no reason to take more.
constexpr uint32_t kWatchedVertices = 12;

struct WatchedGeometry {
  bool valid = false;
  vulkan::DrawItem item;
  CaptureVertexBuffer *vertices = nullptr;
  CaptureIndexBuffer *indices = nullptr;
  uint32_t vertex_bias = 0; // D3D's BaseVertexIndex (or StartVertex), which base_vertex folds in
  // The bound texture at each stage, by the name the game knows it under, beside the name of the
  // bindless image the draw actually samples. The state verifier proves the same texture OBJECT
  // is bound as the device has; it says nothing about whether our image index names that
  // object's pixels, which is a second mapping and a second chance to be wrong.
  std::string bound_name[2];
  std::string image_name[2];
  uint32_t image_index[2] = {0xffffffffu, 0xffffffffu};
  // The game's vertex buffer read AT THE MOMENT THE DRAW IS ISSUED, converted. The report's
  // other D3D column is read back later, which is a different question and quietly a weaker
  // one: a dynamic buffer refilled after the draw holds the newer version by then, so the
  // arena and the late read can agree perfectly while the runtime drew from bytes neither of
  // them ever saw. That is the one way "same state, same vertices, different picture" can be
  // true, and nothing here could see it.
  std::vector<vulkan::CanonicalVertex> at_draw;
  bool at_draw_read = false;
  // The ARENA, read at the same instant. Both deferred columns answer "what does this hold
  // now", and now is a frame or more after the draw - long enough for the game to refill the
  // buffer, and long enough for the wrapper to have been destroyed and its slot handed to
  // another buffer (8,539 vertex buffers created against 333 live). So neither can tell "the
  // draw pulled the wrong bytes" from "the bytes moved on afterwards", which is the whole
  // question. This one can.
  std::vector<vulkan::CanonicalVertex> arena_at_draw;
  bool arena_at_draw_read = false;
  // The buffer's own bookkeeping at that instant, which is what decides whether a draw reads
  // the slot or a scratch version (§4.23).
  std::string book;
};
inline WatchedGeometry VerifyDrawGeometry;

// The inner object a wrapper holds, or the argument unchanged when it is not one of ours. The
// membership check against the Live*Wrappers sets is what makes the downcast sound.
IDirect3DBaseTexture8 *UnwrapTexture(IDirect3DBaseTexture8 *texture);
IDirect3DVertexBuffer8 *Unwrap(IDirect3DVertexBuffer8 *buffer);
IDirect3DIndexBuffer8 *Unwrap(IDirect3DIndexBuffer8 *buffer);

// One mip level's pixels, from whichever of the two copies has them (§4.12).
bool ReadTextureLevel(CaptureTexture &texture, uint32_t level, D3DLOCKED_RECT &locked);

// --- the seam between the recorder and the evidence ---------------------------------------
//
// Everything below is defined in D3D8CaptureReport.cpp. The recorder calls the `Note*`/`Log*`
// half from the draw path and never reads any of it back; the `render.*` readings in
// D3D8Capture.h are the other direction.

// The `.rim` path a D3D texture was acquired under, or null. A linear scan on purpose: it runs
// once per texture at image creation, so an index would cost more upkeep than it saves.
void ResolveTextureNames();
const std::string *TextureAssetName(IDirect3DBaseTexture8 *texture);

// A D3D enum's name, or null. Used only by the reports.
const char *RenderStateName(uint32_t state);
const char *StageStateName(uint32_t type);

// Reads the fixed-function state back off the device and diffs it against the shadow mirror.
// Every counter in this layer is computed FROM the mirror, so this is the only reading that can
// say the mirror itself is right (§4.40).
std::string CompareShadowToDevice(CaptureDevice *capture);

// Collectors, called from the recorder's draw path. Each is a measurement that costs a little
// per draw and has paid for itself at least once.
void NoteOddTopology(D3DPRIMITIVETYPE type, bool user_pointer, uint32_t primitives,
                     const void *vertex_data, uint32_t vertex_stride, uint32_t vertex_count);
void LogDraw(D3DPRIMITIVETYPE type, bool user_pointer, uint32_t primitives);
// One draw's enabled lights, from ResolveLightRun - which is where they are already in hand, and
// which runs for every lit draw whether or not the run itself was a cache hit. `lights` is the
// run's contents; the collector expands them into the per-frame census above.
void NoteLightRun(const vulkan::GpuLight *lights, uint32_t count, uint64_t frame);
// Rotate the census at Present, exactly as the draw log rotates.
void RotateLightCensus(uint64_t frame);
void NoteDrawResult(HRESULT hr, const char *which, D3DPRIMITIVETYPE type,
                    uint32_t primitive_count);
// Snapshots the device state, the game's own vertices and the arena AT THE MOMENT one draw is
// issued. The at-draw reads are the point: a deferred readback proves consistency, not
// correctness, which is how §4.42's defect survived three sections of instruments (§4.42).
void MaybeVerifyStateForDraw(CaptureDevice *capture, const vulkan::DrawItem &item,
                             uint32_t vertex_bias);

} // namespace d3d8
} // namespace gk
