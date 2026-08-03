#include "D3D8Capture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <d3d8to9.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "Core.h"
#include "D3D8Device.gen.inc.h"
#include "DetourUtils.h"
#include "Render.h"
#include "VkDraw.h"
#include "VkRenderer.h"
#include "VertexFormat.h"
#include "VkResources.h"

// d3d8to9's own entry point. Declared rather than included because d3d8to9.hpp does not
// export it; taking its address here is what D3D8CaptureSystem detours. See the header for
// why this is a detour and not a replacement.
extern "C" IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion);

namespace gk {
namespace d3d8 {
namespace {

CaptureStats TheStats;
bool HaveDevice = false;

// GKPLUS_WRAP_BUFFERS = none | vb | ib | both (default both). Wrapping the buffer objects is
// the only way to see them destroyed, but it also changes the pointers the game holds - so
// when something breaks, this is what says which half did it, without a rebuild per attempt.
bool WrapVertexBuffers = true;
bool WrapIndexBuffers = true;

// GKPLUS_NO_LIGHTING=1 / GKPLUS_NO_STAGE1=1, read once at construction. Both are measuring
// instruments rather than features: each makes the game's OWN renderer draw the scene without
// one thing the Vulkan path is missing, which is what tells "we are missing X" apart from "we
// are wrong in some other way that happens to look like missing X". See SetRenderState and
// SetTextureStageState.
bool ForceLightingOff = false;
bool ForceStage1Off = false;
bool ForceSpecularOff = false;
// GKPLUS_NO_MIPMAP=1, the same instrument pointed at mip selection: force D3DTSS_MIPFILTER to
// D3DTEXF_NONE in the forwarded call, so the reference renderer samples level 0 whatever the
// footprint. It exists because a heavily minified draw is where two implementations' LOD
// choices diverge most, and a quad three pixels wide sampling a 1024-texel texture is as
// minified as this game gets (§4.29).
bool ForceNoMipmap = false;
// GKPLUS_NO_CULL=1 / GKPLUS_NO_ZTEST=1 - the same instrument pointed at the two states that make
// a draw vanish outright rather than come out wrong.
bool ForceNoCull = false;
bool ForceNoZTest = false;

// GKPLUS_VK_SKIP: which of this renderer's own features to switch OFF, as letters -
// 't' topologies past triangle lists, 's' seeding a buffer from its own contents, 'l' the
// material colour for unlit-vertex draws, 'd' the API's initial state defaults. The same
// bisect knob as GKPLUS_WRAP_BUFFERS and for the same reason: four changes landed together
// and something in them made the picture worse, which no counter can tell you.
//
// The topologies were OFF by default for two sections, and the reason was never the topologies:
// drawing the strips cost 2.3/255 because one of them is the shadow-volume darkening quad, and
// with no stencil buffer to mask it, it covered the frame (§4.20, §4.21). Now that the depth
// format carries a stencil aspect and the pipelines test against it, they are ON, and
// GKPLUS_VK_TOPOLOGIES=none (or GKPLUS_VK_SKIP=t) is the way back for a bisect.
bool SkipTopologies = false;
// GKPLUS_VK_TOPOLOGIES also takes `strip` and `line`, so the two can be told apart - there are
// only three strip draws and one line-list draw in a frame, and they need not be wrong for the
// same reason.
bool DrawStrips = true;
bool DrawLines = true;
bool SkipSeeding = false;
bool SkipLitColour = false;
bool SkipStateDefaults = false;
// The real per-vertex light sum (§4.26), on by default and switchable at run time through
// `render.lighting`. Off falls back to the §4.20 material collapse, which is the previous
// build's behaviour exactly - so the two can be compared on ONE paused frame instead of across
// two launches, where the noise floor is 8.06/255 and buries the effect (§4.21).
bool DrawLightSum = true;

void ReadWrapMode() {
  char value[16] = {};
  const DWORD len =
      ::GetEnvironmentVariableA("GKPLUS_WRAP_BUFFERS", value, sizeof(value));
  const std::string mode(value, len);
  if (mode.empty() || mode == "both") {
    return;
  }
  WrapVertexBuffers = mode == "vb";
  WrapIndexBuffers = mode == "ib";
  DebugWrite("gkplus: buffer wrapping mode '" + mode + "'\n");
}

// GKPLUS_TEXTURE_UPLOAD = both | seed | blits (default both). The same bisect knob as
// GKPLUS_WRAP_BUFFERS, and for the same reason: a texture whose contents are wrong says
// nothing about which of the two halves put them there. `render.verify_textures()` with one
// half switched off is what tells them apart.
bool SeedTextures = true;
bool ApplyTextureBlits = true;

void ReadTextureUploadMode() {
  char value[16] = {};
  const DWORD len =
      ::GetEnvironmentVariableA("GKPLUS_TEXTURE_UPLOAD", value, sizeof(value));
  const std::string mode(value, len);
  if (mode.empty() || mode == "both") {
    return;
  }
  SeedTextures = mode == "seed";
  ApplyTextureBlits = mode == "blits";
  DebugWrite("gkplus: texture upload mode '" + mode + "'\n");
}

// --- texture provenance ------------------------------------------------------------------
//
// The capture layer sees pixels with no idea what they are: a D3D texture carries no name, and
// the engine decodes its `.rim` files itself. But a mod cannot say "replace the water texture"
// or "give this material a normal map" by pointing at a wrapper pointer that differs every run
// - it needs the asset name. So the name has to be recovered while the link still exists.
//
// It exists in exactly one place. `AcquireRimTexture` @ 0x005a15b0 is the engine's whole
// texture-acquire path (31 call sites, from the console to the shape loader), and the 0x34-byte
// record it mints IS `AwTexture` (src/Render.h): the strdup'd path at +0x2c, and at +0x00 the
// `IDirect3DBaseTexture8 *` the loader stores there once the texture exists - which, with this
// layer installed, is our own CaptureTexture wrapper. So the join is a pointer compare.
//
// Hooked rather than called, and hooked HERE rather than in src/Render.cpp, which is
// deliberately pure struct + native API with no `*System` of its own. `D3D8CaptureSystem`
// already owns a detour and already owns the textures this names.
//
// `__thiscall` with two stack arguments, confirmed by `RET 0x8` at 0x005a1711 - the cache is in
// ECX.
//
// **The record is minted with its D3D pointer null and filled in much later**, and that timing
// is the whole reason this is a per-frame sweep rather than a lookup. Resolving once, when a
// texture is first bound, named 5 of 53 - not because the join was wrong but because the record
// was not populated yet at that moment. Sweeping the records instead converges within a frame
// of the loader storing the pointer, and costs one pass over ~130 records.
//
// `RimJoinHistogram()` is what established that, and it stays: it reports which offset of a
// record actually holds a live wrapper, so "the field moved" and "we looked too early" can
// never again be confused for one another.

// Every record the engine has minted. A cache *hit* returns an existing record, so this is a
// set rather than a list - the 31 call sites hit far more often than they miss. Records are
// never freed: the cache is an MRU list over a hash that only grows, and a level teardown
// leaves them for the next level to hit.
std::set<AwTexture *> RimRecords;

// `__thiscall` with stack arguments is spelled as a member function of a stand-in class, which
// is the same shape `PickupActor::Associate` uses in ScriptQueue.cpp and for the same reason: a
// `__fastcall` hook would take `path` in EDX, where the original expects the stack.
struct RimCacheAbi {
  AwTexture *HookedAcquire(const char *path, unsigned flags);
};

AwTexture *(RimCacheAbi::*AcquireRimTexture)(const char *, unsigned) = nullptr;

AwTexture *RimCacheAbi::HookedAcquire(const char *path, unsigned flags) {
  AwTexture *const record = (this->*AcquireRimTexture)(path, flags);
  if (record != nullptr) {
    RimRecords.insert(record);
  }
  return record;
}

// The `.rim` path a D3D texture was acquired under, or null.
//
// A linear scan, and it stays one on purpose: it runs once per texture at image creation - 59
// times on level01 against ~130 records - so an index would cost more upkeep than it saves, and
// it would have to be invalidated every time a record's D3D pointer was filled in behind us.
void ResolveTextureNames();

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

ShadowState State;

// D3D8's documented initial state, for the states the renderer reads. The mirror is zeroed
// otherwise, and zero is a *legal value* for most of these rather than an obviously-missing
// one - which makes the difference invisible until it is not. D3DRS_COLORWRITEENABLE is the
// sharp case: its default is all four channels and its zero means write none of them, so a
// draw issued before the game ever sets it would render nothing at all.
//
// Only the states something here consumes are listed. Adding one to a pipeline key or to the
// shader is a reason to check whether its default is 0, not merely to read it.
void InitialiseShadowState() {
  if (SkipStateDefaults) {
    return;
  }
  uint32_t *rs = State.render_states;
  rs[D3DRS_ZENABLE] = D3DZB_TRUE;
  rs[D3DRS_ZWRITEENABLE] = TRUE;
  rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  rs[D3DRS_ALPHABLENDENABLE] = FALSE;
  rs[D3DRS_ALPHATESTENABLE] = FALSE;
  rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_ALPHAREF] = 0;
  rs[D3DRS_COLORWRITEENABLE] = 0xf;
  rs[D3DRS_LIGHTING] = TRUE;
  rs[D3DRS_SPECULARENABLE] = FALSE;
  rs[D3DRS_TEXTUREFACTOR] = 0xffffffff;
  // The light sum's own states. Three of these have a non-zero default and each one changes
  // the picture: D3DRS_COLORVERTEX off makes every material source read the material, and the
  // two COLOR sources decide whether a vertex diffuse is the material diffuse (which it is,
  // and which is the whole of §4.25) or is ignored. D3DRS_AMBIENT and NORMALIZENORMALS do
  // default to 0/FALSE and are listed anyway, because "the default happens to be zero" is
  // exactly the thing that stops being true when a state is added to this list.
  rs[D3DRS_COLORVERTEX] = TRUE;
  rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_AMBIENT] = 0;
  rs[D3DRS_NORMALIZENORMALS] = FALSE;
  // The stencil states, and the two masks are the reason this block is not optional for them.
  // The game switches D3DRS_STENCILENABLE, the func, the ref and the three ops for its shadow
  // volumes and never touches D3DRS_STENCILMASK or D3DRS_STENCILWRITEMASK, whose default is
  // all-ones. Left at zero, the shadow passes would write no bits at all - and the test would
  // still pass, since 0 <= 0 - so the mask would come out empty and the darkening quad would
  // cover the frame, which is the exact symptom the stencil buffer was added to fix (§4.21).
  rs[D3DRS_STENCILENABLE] = FALSE;
  rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILREF] = 0;
  rs[D3DRS_STENCILMASK] = 0xffffffff;
  rs[D3DRS_STENCILWRITEMASK] = 0xffffffff;

  for (uint32_t stage = 0; stage < kStages; ++stage) {
    uint32_t *ss = State.stage_states[stage];
    // Stage 0 modulates the texture with the vertex colour and every other stage is off; the
    // arguments default the same way at every stage whether or not the stage is enabled.
    ss[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    ss[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    ss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    ss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
    ss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    ss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    ss[D3DTSS_TEXCOORDINDEX] = stage;
    // The sampler half, and none of these five defaults is zero-shaped. Left at zero the
    // renderer built a LINEAR/LINEAR sampler with mipmapping on for every stage the game never
    // configured, where D3D8 defaults to POINT/POINT with mipmapping OFF - which is a blur, and
    // a blur is invisible against a counter. It cost the HUD 10% of its contrast (§4.28).
    ss[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
    ss[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    ss[D3DTSS_MAGFILTER] = D3DTEXF_POINT;
    ss[D3DTSS_MINFILTER] = D3DTEXF_POINT;
    ss[D3DTSS_MIPFILTER] = D3DTEXF_NONE;
  }
}

// How often the game touches the lighting state at all. Zero for all three would say the
// fixed-function lighting pipeline is unused and every colour is baked into the vertices.
uint64_t LightSets = 0;
uint64_t LightEnables = 0;
uint64_t MaterialSets = 0;

// Bumped whenever a light slot's contents change, so the per-frame cache that turns an enable
// mask into a run of GpuLights knows when the mask is no longer enough to identify the run.
uint64_t LightGeneration = 0;

uint64_t StatesInCurrentBlock = 0;
bool RecordingBlock = false;

void NoteBlockState() {
  if (RecordingBlock) {
    ++StatesInCurrentBlock;
  }
}

// One recorded state change. Only the four setters that can appear inside a material's state
// block are modelled - Phase 0b showed AwMaterial_Compile issues render states and stage
// states, and ApplyStateBlock is what replays them. A transform or a light inside a block
// would be missed; `opaque_block_applies` is the counter that would notice the equivalent
// problem for CreateStateBlock, and this list is deliberately narrow rather than speculative.
enum class OpKind : uint8_t { RenderState, StageState, Texture, VertexShader };

struct StateOp {
  OpKind kind;
  uint32_t a = 0;     // render state, or stage
  uint32_t b = 0;     // stage state type
  uint32_t value = 0;
  IDirect3DBaseTexture8 *texture = nullptr;
};

struct StateBlock {
  std::vector<StateOp> ops;
  bool opaque = false; // built by CreateStateBlock/CaptureStateBlock - contents unwitnessed
};

std::map<uint32_t, StateBlock> Blocks;
std::vector<StateOp> RecordingOps;

// Being bound is the definition of "this texture will be sampled", so it is what gives a
// texture its image - including one that is never blitted into.
void EnsureTextureImageForBinding(IDirect3DBaseTexture8 *texture);

void ApplyOp(const StateOp &op) {
  switch (op.kind) {
  case OpKind::RenderState:
    if (op.a < kMaxRenderState) {
      State.render_states[op.a] = op.value;
    }
    break;
  case OpKind::StageState:
    if (op.a < kStages && op.b < kMaxStageState) {
      State.stage_states[op.a][op.b] = op.value;
    }
    break;
  case OpKind::Texture:
    if (op.a < kStages) {
      State.textures[op.a] = op.texture;
      // Here rather than in SetTexture, because a texture can be bound WITHOUT SetTexture
      // being called: ApplyStateBlock replays through this function and touches no Set*
      // method at all. That is how level01's lightmaps arrived - 83,176 draws a session
      // binding a DXT1 texture at stage 1 that had no image, sampling white, and brightening
      // the scene through ADDSIGNED instead of darkening it (§4.19).
      EnsureTextureImageForBinding(op.texture);
    }
    break;
  case OpKind::VertexShader:
    State.fvf = op.value;
    break;
  }
}

// Every intercepted setter funnels through here: it updates the shadow state, and while a
// block is being recorded it also appends to that block. Both, not either - the state a
// block sets is genuinely set on the device at record time too.
void Record(const StateOp &op) {
  ApplyOp(op);
  if (RecordingBlock) {
    RecordingOps.push_back(op);
  }
}

// --- reducing a draw to two keys ---------------------------------------------------------
//
// FNV-1a over the bytes that matter. Interning the hashes rather than the states themselves
// keeps this to a set of integers; a collision would undercount by one, which is acceptable
// for a measurement and is why these numbers size the design rather than drive it.
uint32_t HashBytes(const void *data, size_t size, uint32_t seed = 2166136261u) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    seed = (seed ^ bytes[i]) * 16777619u;
  }
  return seed;
}

// D3DTSS_COLOROP is 1 and D3DTOP_DISABLE is 1: a stage is live until the first disabled one.
uint32_t ActiveStages() {
  for (uint32_t i = 0; i < kStages; ++i) {
    if (State.stage_states[i][D3DTSS_COLOROP] == D3DTOP_DISABLE) {
      return i;
    }
  }
  return kStages;
}

// The sampler each stage is currently configured for, resolved from the shadow state. D3D8
// carries sampler settings as texture-stage state rather than as a separate object, so the
// combination has to be collapsed into one - which `vulkan::AcquireSampler` does, dedupED by
// value. The recorder measured 2-3 distinct values per field, so the table stays tiny.
uint32_t StageSampler(uint32_t stage) {
  return vulkan::AcquireSampler(State.stage_states[stage][D3DTSS_MAGFILTER],
                                State.stage_states[stage][D3DTSS_MINFILTER],
                                State.stage_states[stage][D3DTSS_MIPFILTER],
                                State.stage_states[stage][D3DTSS_ADDRESSU],
                                State.stage_states[stage][D3DTSS_ADDRESSV]);
}

uint32_t FloatBits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// Every distinct (MinZ, MaxZ) the game has ever set, as a pair of float bit patterns. A set
// rather than a counter: the question is "does it ever leave 0..1", and one occurrence answers
// it.
std::set<uint64_t> ViewportDepthRanges;

std::set<uint32_t> MaterialKeys;
std::set<uint32_t> PipelineKeys;
std::set<uint32_t> MaterialKeysThisFrame;

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

std::map<StageConfig, uint64_t> StageConfigs;

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
const uint32_t kPipelineStates[15] = {
    D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,     D3DRS_ALPHAFUNC,   D3DRS_ALPHABLENDENABLE,
    D3DRS_SRCBLEND,        D3DRS_DESTBLEND,    D3DRS_ZENABLE,     D3DRS_ZWRITEENABLE,
    D3DRS_CULLMODE,        D3DRS_COLORWRITEENABLE,
    D3DRS_STENCILENABLE,   D3DRS_STENCILFUNC,  D3DRS_STENCILPASS, D3DRS_STENCILZFAIL,
    D3DRS_SHADEMODE};

std::map<PipelineConfig, uint64_t> PipelineConfigs;

// Why a bound texture did not reach the shader, split by stage - "the wrapper is not ours" and
// "the wrapper has no image" have completely different causes, and the second one names the
// D3DFORMAT and D3DPOOL that produced it.
uint64_t UnresolvedForeign[2] = {};
uint64_t UnresolvedNoImage[2] = {};
std::map<uint64_t, uint64_t> UnresolvedFormats; // pool << 32 | format

// The first converted vertex's colour, per FVF, for the user-pointer draws. Reading the
// vertices this layer is about to hand the shader is the only way to tell "the game is not
// sending us the colour" apart from "we are not applying the colour it sends".
std::map<uint64_t, uint64_t> FirstVertexColours; // fvf << 32 | D3DCOLOR

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

std::map<OddTopology, uint64_t> OddTopologies;

// The first few out-of-range DrawIndexedPrimitive calls, spelled out. See NoteIndexedRange.
std::vector<std::string> OutOfRangeDraws;

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

std::map<LightingInputs, uint64_t> LightingByFvf;

uint32_t PackColour(const D3DCOLORVALUE &c) {
  auto channel = [](float v) {
    const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
  };
  return (channel(c.a) << 24) | (channel(c.r) << 16) | (channel(c.g) << 8) | channel(c.b);
}

// Vertex buffers that are drawn from but hold no arena slot, keyed by fvf << 32 | length. The
// FVF is the interesting half: a buffer created with none has no stride this layer can derive,
// so `SlotBytes` is 0 and the allocation quietly does nothing.
std::map<uint64_t, uint64_t> UnslottedVertexBuffers;


// Re-locks of a buffer this frame's draws had already read, keyed by
// `D3DLOCK_* flags << 1 | the new range overlapping the old one`. The flags say what the game
// intends and the overlap says what it did; a NOOVERWRITE append is harmless and a DISCARD
// over the same bytes is not, so the pair is what distinguishes them. See §4.23.
std::map<uint64_t, uint64_t> RewriteLocks;

void NoteRewrite(uint32_t flags, bool overlaps) {
  ++RewriteLocks[(uint64_t(flags) << 1) | (overlaps ? 1u : 0u)];
}

// The `.rim` path a bound texture was acquired under, or null for one this layer does not own
// or that has no cache record behind it. Defined once CaptureTexture exists; declared here
// because MaterialKey is the only caller and lives above it.
const std::string *TextureAssetName(IDirect3DBaseTexture8 *texture);

// Exactly the fields GpuMaterial is specified to carry, so the count of distinct values here
// predicts that table's size instead of merely correlating with it.
uint32_t MaterialKey(uint32_t stages) {
  uint32_t hash = HashBytes(&stages, sizeof(stages));
  for (uint32_t i = 0; i < stages; ++i) {
    const uint32_t ops[6] = {
        State.stage_states[i][D3DTSS_COLOROP],   State.stage_states[i][D3DTSS_COLORARG1],
        State.stage_states[i][D3DTSS_COLORARG2], State.stage_states[i][D3DTSS_ALPHAOP],
        State.stage_states[i][D3DTSS_ALPHAARG1], State.stage_states[i][D3DTSS_ALPHAARG2]};
    hash = HashBytes(ops, sizeof(ops), hash);
    // The texture's **asset name**, not its wrapper pointer. Both distinguish the same draws
    // within one run, and only one of them means anything outside it: the pointer is an address
    // the allocator happened to return, so the same material hashes differently on every launch
    // and identically to a *different* material that reused a freed wrapper. The `.rim` path is
    // the identity a mod has to be able to write down (§4.14), which is what this number has to
    // be about if it is to predict the size of a table addressed that way.
    //
    // A texture with no cache record behind it - procedural, or engine-internal - has no name,
    // and falls back to the pointer rather than collapsing every such texture into one
    // material. That is the pre-existing behaviour, kept for exactly the draws it was right for.
    IDirect3DBaseTexture8 *texture = State.textures[i];
    const std::string *name = TextureAssetName(texture);
    if (name != nullptr && !name->empty()) {
      hash = HashBytes(name->data(), static_cast<uint32_t>(name->size()), hash);
    } else {
      hash = HashBytes(&texture, sizeof(texture), hash);
    }
  }
  const uint32_t alpha[2] = {State.render_states[D3DRS_ALPHATESTENABLE],
                             State.render_states[D3DRS_ALPHAREF]};
  return HashBytes(alpha, sizeof(alpha), hash);
}

// The states that select a VkPipeline. Small on purpose: section 2's "bucket by pipeline
// only" is only sound if this count stays in the handful, which is what gets measured.
uint32_t PipelineKey() {
  const uint32_t states[7] = {State.render_states[D3DRS_ALPHABLENDENABLE],
                              State.render_states[D3DRS_SRCBLEND],
                              State.render_states[D3DRS_DESTBLEND],
                              State.render_states[D3DRS_ZENABLE],
                              State.render_states[D3DRS_ZWRITEENABLE],
                              State.render_states[D3DRS_CULLMODE],
                              State.render_states[D3DRS_ALPHATESTENABLE]};
  return HashBytes(states, sizeof(states));
}

void SnapshotDraw() {
  const uint32_t stages = ActiveStages();
  if (stages > TheStats.max_active_stages) {
    TheStats.max_active_stages = stages;
  }
  // Interning the samplers at draw time is what discovers the combinations the game actually
  // uses - the same discipline the FVF enumeration followed, rather than creating a sampler
  // per conceivable D3D state up front.
  if (vulkan::ResourcesReady()) {
    for (uint32_t i = 0; i < stages; ++i) {
      StageSampler(i);
    }
  }
  StageConfig config;
  config.fvf = State.fvf;
  config.stages = stages;
  for (uint32_t i = 0; i < 2; ++i) {
    static const uint32_t kFields[12] = {
        D3DTSS_COLOROP,   D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP,
        D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX,
        D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER, D3DTSS_ADDRESSU,
        D3DTSS_ADDRESSV};
    for (uint32_t f = 0; f < 12; ++f) {
      config.stage[i][f] = State.stage_states[i][kFields[f]];
    }
    config.textured[i] = State.textures[i] != nullptr ? 1u : 0u;
  }
  ++StageConfigs[config];

  // Only for the layouts with no D3DFVF_DIFFUSE (0x040): those are the ones whose colour the
  // fixed function has to invent, and the only ones where the material can be seen.
  if ((State.fvf & 0x040u) == 0) {
    LightingInputs inputs;
    inputs.fvf = State.fvf;
    inputs.lighting = State.render_states[D3DRS_LIGHTING];
    for (uint32_t i = 0; i < kLights; ++i) {
      inputs.enabled_lights += State.light_enabled[i] ? 1u : 0u;
    }
    inputs.diffuse = PackColour(State.material.Diffuse);
    inputs.ambient = PackColour(State.material.Ambient);
    inputs.emissive = PackColour(State.material.Emissive);
    ++LightingByFvf[inputs];
  }

  PipelineConfig pipeline;
  pipeline.fvf = State.fvf;
  for (uint32_t i = 0; i < 15; ++i) {
    pipeline.state[i] = State.render_states[kPipelineStates[i]];
  }
  ++PipelineConfigs[pipeline];

  if (State.render_states[D3DRS_FOGENABLE] != 0) {
    ++TheStats.draws_fogged;
  }
  if (State.render_states[D3DRS_LIGHTING] != 0) {
    ++TheStats.draws_lit;
  }
  MaterialKeys.insert(MaterialKey(stages));
  MaterialKeysThisFrame.insert(MaterialKey(stages));
  PipelineKeys.insert(PipelineKey());
  TheStats.distinct_materials = MaterialKeys.size();
  TheStats.distinct_pipelines = PipelineKeys.size();
}



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
  ULONG refs_ = 1;
};

struct CaptureDevice;

// Every resource has a `GetDevice`, and forwarding it hands the game the unwrapped d3d8to9
// device - after which every call it made would be invisible here. That is the
// ProcessVertices failure without the crash, and it is why the generator's wrapped-parameter
// check now covers IDirect3DDevice8 too.
//
// Defined once, below CaptureDevice, and shared by all four resource wrappers. False means
// there is no capture device to hand back and the caller should forward.
bool TryGetCaptureDevice(IDirect3DDevice8 **ppDevice);

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

  void NoteLock(uint32_t offset, uint32_t size, BYTE **data, uint32_t flags) {
    ++TheStats.locks;
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
    ++unlocks_;
    // Rewritten after this frame's draws already read it: the slot cannot hold both versions,
    // and the draw list is not recorded until Present, so those draws would read this one.
    const bool rewritten_after_draw = drawn_frame_ == TheStats.frames && draws_this_frame_ != 0;
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
                                   static_cast<vulkan::CanonicalVertex *>(alloc.mapped))) {
        return false;
      }
      version_offset_ = alloc.offset;
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
    // One scratch buffer for the process: conversion is main-thread and immediately
    // consumed by the staging copy, so there is nothing to keep per buffer.
    static std::vector<vulkan::CanonicalVertex> scratch;
    scratch.resize(count);
    if (!vulkan::ConvertVertices(fvf_, locked_, count, scratch.data())) {
      ++TheStats.failed_uploads;
      return;
    }
    const uint32_t dst_offset =
        (locked_offset_ / stride) * sizeof(vulkan::CanonicalVertex);
    const uint32_t bytes =
        count * static_cast<uint32_t>(sizeof(vulkan::CanonicalVertex));
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
  uint64_t drawn_frame_ = UINT64_MAX;
  uint32_t draws_this_frame_ = 0;
  // The frame whose scratch holds a later version of this buffer, and where in it. A draw uses
  // the version only while `version_frame_` is the current frame - the scratch slice is recycled
  // after that, so it expires on its own and there is nothing to clear.
  uint64_t version_frame_ = UINT64_MAX;
  uint32_t version_offset_ = 0;
  // 2 or 4. Lives here rather than on CaptureIndexBuffer because UploadVersionToScratch needs
  // it, and is left at 2 for a vertex buffer, where nothing reads it.
  uint32_t index_stride_ = 2;
  // Set once a read-back of this buffer's own contents has been tried, successfully or not, so
  // a buffer that cannot be read costs one attempt rather than one per draw.
  bool seeded_ = false;

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
  // This buffer's own region of the arena, held for its whole lifetime and released in the
  // destructor. See BufferSlot in VkResources.h for why it is per-buffer and not per-upload.
  vulkan::BufferSlot slot_;
};

// Declared before the wrappers so their constructors can register themselves; defined with
// Unwrap, which is where the reason they exist is written up.
std::set<const void *> LiveVertexWrappers;
std::set<const void *> LiveIndexWrappers;

struct CaptureVertexBuffer final : BufferWrapper<IDirect3DVertexBuffer8> {
  CaptureVertexBuffer(IDirect3DVertexBuffer8 *inner, uint32_t length, uint32_t fvf,
                      uint32_t pool)
      : BufferWrapper(inner, length, true, fvf) {
    pool_ = pool;
    LiveVertexWrappers.insert(this);
  }
  ~CaptureVertexBuffer() override { LiveVertexWrappers.erase(this); }

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
    LiveIndexWrappers.insert(this);
  }
  ~CaptureIndexBuffer() override { LiveIndexWrappers.erase(this); }

  HRESULT LockForRead(BYTE **data) override {
    return inner_->Lock(0, 0, data, D3DLOCK_READONLY);
  }
  void UnlockAfterRead() override { inner_->Unlock(); }

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DINDEXBUFFER8_METHODS(GK_DECL)
#undef GK_DECL
};

#define GK_FORWARD_VB(ret, name, params, args)                                            \
  ret STDMETHODCALLTYPE CaptureVertexBuffer::name params { return inner_->name args; }
GK_IDIRECT3DVERTEXBUFFER8_FORWARDED(GK_FORWARD_VB)
#undef GK_FORWARD_VB

#define GK_FORWARD_IB(ret, name, params, args)                                            \
  ret STDMETHODCALLTYPE CaptureIndexBuffer::name params { return inner_->name args; }
GK_IDIRECT3DINDEXBUFFER8_FORWARDED(GK_FORWARD_IB)
#undef GK_FORWARD_IB

HRESULT STDMETHODCALLTYPE CaptureVertexBuffer::GetDevice(IDirect3DDevice8 **ppDevice) {
  if (ppDevice != nullptr && TryGetCaptureDevice(ppDevice)) {
    return D3D_OK;
  }
  return inner_->GetDevice(ppDevice);
}

HRESULT STDMETHODCALLTYPE CaptureIndexBuffer::GetDevice(IDirect3DDevice8 **ppDevice) {
  if (ppDevice != nullptr && TryGetCaptureDevice(ppDevice)) {
    return D3D_OK;
  }
  return inner_->GetDevice(ppDevice);
}

HRESULT STDMETHODCALLTYPE CaptureVertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock,
                                                     BYTE **ppbData, DWORD Flags) {
  const HRESULT hr = inner_->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
  if (SUCCEEDED(hr)) {
    NoteLock(OffsetToLock, SizeToLock, ppbData, Flags);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureVertexBuffer::Unlock() {
  UploadLocked();
  return inner_->Unlock();
}

HRESULT STDMETHODCALLTYPE CaptureIndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock,
                                                    BYTE **ppbData, DWORD Flags) {
  const HRESULT hr = inner_->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
  if (SUCCEEDED(hr)) {
    NoteLock(OffsetToLock, SizeToLock, ppbData, Flags);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureIndexBuffer::Unlock() {
  UploadLocked();
  return inner_->Unlock();
}

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

std::set<const void *> LiveTextureWrappers;
std::set<const void *> LiveSurfaceWrappers;

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

std::map<IDirect3DSurface8 *, CaptureSurface *> CaptureSurface::SurfaceWrappers;

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

const std::string *TextureAssetName(IDirect3DBaseTexture8 *texture) {
  if (texture == nullptr || LiveTextureWrappers.count(texture) == 0) {
    return nullptr;
  }
  return &static_cast<CaptureTexture *>(texture)->rim_path_;
}

// The reference on `owner_` is not bookkeeping for its own sake: the game releases its
// texture wrappers independently of the surfaces it took off them, so without it a surface
// outliving its texture would read a destroyed object every time it checked whether it was a
// texture level. D3D8's own surfaces hold a reference on their container for the same reason.
CaptureSurface::CaptureSurface(IDirect3DSurface8 *inner, CaptureTexture *owner,
                               uint32_t level)
    : Wrapper(inner), owner_(owner), level_(level) {
  if (owner_ != nullptr) {
    owner_->AddRef();
  }
  LiveSurfaceWrappers.insert(this);
  ++TheStats.live_surfaces;
}

CaptureSurface::~CaptureSurface() {
  LiveSurfaceWrappers.erase(this);
  SurfaceWrappers.erase(inner_);
  --TheStats.live_surfaces;
  if (owner_ != nullptr) {
    owner_->Release();
  }
}

// Takes the reference `inner` arrived with. On a cache hit that reference is surplus - the
// wrapper already holds one - so it is released here rather than leaked.
IDirect3DSurface8 *WrapSurface(IDirect3DSurface8 *inner, CaptureTexture *owner,
                               uint32_t level = 0) {
  if (inner == nullptr) {
    return nullptr;
  }
  const auto found = CaptureSurface::SurfaceWrappers.find(inner);
  if (found != CaptureSurface::SurfaceWrappers.end()) {
    found->second->AddRef();
    inner->Release();
    return found->second;
  }
  auto *const wrapper = new CaptureSurface(inner, owner, level);
  CaptureSurface::SurfaceWrappers[inner] = wrapper;
  return wrapper;
}

// The wrapper behind a surface pointer, or null if it is not one of ours.
CaptureSurface *AsCaptureSurface(IDirect3DSurface8 *surface) {
  if (surface == nullptr || LiveSurfaceWrappers.count(surface) == 0) {
    return nullptr;
  }
  return static_cast<CaptureSurface *>(surface);
}

// The membership check is the same soundness argument as Unwrap() below: a static_cast to our
// type is undefined unless the pointer really is one of ours, and a COM interface carries
// nothing that says so.
IDirect3DSurface8 *UnwrapSurface(IDirect3DSurface8 *surface) {
  if (surface == nullptr) {
    return nullptr;
  }
  if (LiveSurfaceWrappers.count(surface) == 0) {
    ++TheStats.foreign_buffers;
    return surface;
  }
  return static_cast<CaptureSurface *>(surface)->inner_;
}

// True if this surface is a level of a texture, i.e. writing to it writes texture pixels.
bool IsTextureSurface(IDirect3DSurface8 *surface) {
  CaptureSurface *const wrapper = AsCaptureSurface(surface);
  return wrapper != nullptr && wrapper->owner_ != nullptr;
}

#define GK_FORWARD_TEX(ret, name, params, args)                                           \
  ret STDMETHODCALLTYPE CaptureTexture::name params { return inner_->name args; }
GK_IDIRECT3DTEXTURE8_FORWARDED(GK_FORWARD_TEX)
#undef GK_FORWARD_TEX

HRESULT STDMETHODCALLTYPE CaptureTexture::GetDevice(IDirect3DDevice8 **ppDevice) {
  if (ppDevice != nullptr && TryGetCaptureDevice(ppDevice)) {
    return D3D_OK;
  }
  return inner_->GetDevice(ppDevice);
}

HRESULT STDMETHODCALLTYPE CaptureTexture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect,
                                                    const RECT *pRect, DWORD Flags) {
  ++TheStats.texture_lock_rects;
  ++own_locks_;
  return inner_->LockRect(Level, pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE CaptureTexture::UnlockRect(UINT Level) {
  return inner_->UnlockRect(Level);
}

// The second route into a texture's bits. Wrapped rather than merely counted now: the count
// alone could not say whether the surface was being written or only queried.
HRESULT STDMETHODCALLTYPE CaptureTexture::GetSurfaceLevel(
    UINT Level, IDirect3DSurface8 **ppSurfaceLevel) {
  ++TheStats.texture_surface_levels;
  if (ppSurfaceLevel == nullptr) {
    return inner_->GetSurfaceLevel(Level, ppSurfaceLevel);
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr = inner_->GetSurfaceLevel(Level, &surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppSurfaceLevel = WrapSurface(surface, this, Level);
  return hr;
}

#define GK_FORWARD_SURFACE(ret, name, params, args)                                       \
  ret STDMETHODCALLTYPE CaptureSurface::name params { return inner_->name args; }
GK_IDIRECT3DSURFACE8_FORWARDED(GK_FORWARD_SURFACE)
#undef GK_FORWARD_SURFACE

// The measurement Phase 2c-iv exists to make. A lock on a texture level is a pixel write that
// IDirect3DTexture8::LockRect never saw; a lock on the backbuffer is not, so the two are
// counted apart.
HRESULT STDMETHODCALLTYPE CaptureSurface::LockRect(D3DLOCKED_RECT *pLockedRect,
                                                   const RECT *pRect, DWORD Flags) {
  ++TheStats.surface_lock_rects;
  if (owner_ != nullptr) {
    ++TheStats.surface_texture_lock_rects;
  }
  return inner_->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE CaptureSurface::UnlockRect() { return inner_->UnlockRect(); }

HRESULT STDMETHODCALLTYPE CaptureSurface::GetDevice(IDirect3DDevice8 **ppDevice) {
  if (ppDevice != nullptr && TryGetCaptureDevice(ppDevice)) {
    return D3D_OK;
  }
  return inner_->GetDevice(ppDevice);
}

// Returns the containing texture, through a `void **` - so no parameter names a wrapped
// interface and the generator's check cannot see this one. Handing back `inner_`'s container
// would leak d3d8to9's own texture into the game, and every draw made with it would be
// invisible here.
HRESULT STDMETHODCALLTYPE CaptureSurface::GetContainer(REFIID riid, void **ppContainer) {
  if (ppContainer == nullptr) {
    return E_POINTER;
  }
  if (owner_ != nullptr && (riid == __uuidof(IDirect3DTexture8) ||
                            riid == __uuidof(IDirect3DBaseTexture8) ||
                            riid == __uuidof(IDirect3DResource8))) {
    owner_->AddRef();
    *ppContainer = static_cast<IDirect3DTexture8 *>(owner_);
    return D3D_OK;
  }
  if (riid == __uuidof(IDirect3DDevice8)) {
    return GetDevice(reinterpret_cast<IDirect3DDevice8 **>(ppContainer));
  }
  // A surface with no wrapped container - a backbuffer, a render target, an image surface -
  // asked for something else. Forwarded, because there is nothing of ours to hand back.
  return inner_->GetContainer(riid, ppContainer);
}

// --- the texture upload --------------------------------------------------------------------
//
// `CopyRects` is where a texture's pixels arrive: notes §4.12 measured that the bits the game
// locks belong to a SYSTEMMEM staging texture which is never bound for drawing, and the blit is
// the only place those bits and the texture that will sample them are both known.
//
// **The mirror reads the DESTINATION after the blit, not the source rectangles before it**, and
// that is a measurement rather than a preference. Replaying the source was the first design and
// it left exactly one texture per session wrong - a 512x512 DXT1 whose two blits were both
// whole-level copies from a 512x512 source, so no rectangle arithmetic was even in play. The
// content check (`render.verify_textures()`) is what found it and what settled it: reading the
// destination afterwards is 17/17, reading the source beforehand is 16/17, on the same frame of
// the same scene.
//
// The mechanism was not pinned down, and the honest reason is that the destination read makes it
// moot: it reads the *result* of the copy rather than predicting it, so it is right whatever
// d3d8to9 does in between - including any perturbation from our own read-only lock of the
// source, which is the leading suspect. Two things were ruled out first, and both cost a run:
// d3d8to9's copy IS byte-exact (16 whole-level blits sampled, 0 differing), and the staging
// ring's wrap race is real but not this (fixing it changed nothing here).
//
// The cost is a whole level per blit instead of a rectangle. §4.12 measured 94% of blits as
// sub-rect, so this is the expensive choice - and it is affordable, which is the only reason it
// is acceptable: see the figure in the notes.
// Reads one whole mip level out of a texture. Both the seed and the blit path go through here,
// because "lock a level, hand its rows over" is the same operation either way.
//
// Straight to `inner_`, deliberately: this is our read, not the game's, and routing it through
// the wrapper would inflate the very `texture_lock_rects` counter §4.12 rests on. Read-only,
// so it neither dirties the resource nor forces a GPU sync - and the only two pools the game
// uses, MANAGED and SYSTEMMEM, are both lockable by definition.
bool ReadTextureLevel(CaptureTexture &texture, uint32_t level, D3DLOCKED_RECT &locked) {
  if (FAILED(texture.inner_->LockRect(level, &locked, nullptr, D3DLOCK_READONLY)) ||
      locked.pBits == nullptr) {
    ++TheStats.texture_read_failures;
    return false;
  }
  return true;
}

// Creates this texture's image if it has none, and seeds every mip level from the texture's
// own current contents.
//
// The seed is what makes the upload total rather than merely incremental, and both holes it
// fills were measured rather than reasoned about. A texture written before the renderer came
// up gets no blit we can see — the renderer starts on the first `Present`, long after the menu
// has loaded its art — and a texture that is bound and drawn but never blitted into gets none
// either: level01 creates 65 MANAGED textures and only 48 ever receive one.
//
// It works only because every texture the game binds is MANAGED (§4.12's pool histogram),
// which is lockable by definition. A DEFAULT-pool texture would not be, and there are none.
// Uploads every mip level of a texture from its own current contents.
void ReseedTextureImage(CaptureTexture &texture) {
  if (!texture.image_.valid) {
    return;
  }
  for (uint32_t level = 0; level < texture.levels_; ++level) {
    D3DLOCKED_RECT locked = {};
    if (!ReadTextureLevel(texture, level, locked)) {
      continue;
    }
    vulkan::UploadIntoTextureImage(texture.image_, level, 0, 0,
                                   (std::max)(1u, texture.width_ >> level),
                                   (std::max)(1u, texture.height_ >> level), locked.pBits,
                                   static_cast<uint32_t>(locked.Pitch));
    texture.inner_->UnlockRect(level);
  }
}

bool EnsureTextureImage(CaptureTexture &texture) {
  if (texture.image_.valid) {
    return true;
  }
  if (texture.image_failed_ || !vulkan::ResourcesReady()) {
    return false;
  }
  if (!vulkan::CreateTextureImage(texture.image_, texture.width_, texture.height_,
                                  texture.levels_, texture.format_)) {
    texture.image_failed_ = true; // unsupported format, already counted; do not retry
    return false;
  }
  // The sweep may already have found this texture's name before its image existed.
  if (!texture.rim_path_.empty()) {
    vulkan::NameTextureImage(texture.image_, texture.rim_path_);
  }
  if (SeedTextures) {
    ReseedTextureImage(texture);
    ++TheStats.images_seeded;
  }
  return true;
}

void EnsureTextureImageForBinding(IDirect3DBaseTexture8 *texture) {
  if (texture != nullptr && LiveTextureWrappers.count(texture) != 0) {
    EnsureTextureImage(*static_cast<CaptureTexture *>(texture));
  }
}

// Walks the cache records and names any texture that has acquired one since the last pass.
// Driven from Present, because a record is populated at an unpredictable point after the
// texture is created and there is no event for it.
void ResolveTextureNames() {
  TheStats.rim_records = RimRecords.size();
  TheStats.rim_records_bound = 0;
  for (AwTexture *const record : RimRecords) {
    if (record == nullptr || record->d3d_texture == nullptr) {
      continue;
    }
    ++TheStats.rim_records_bound;
    if (record->path == nullptr ||
        LiveTextureWrappers.count(record->d3d_texture) == 0) {
      continue;
    }
    auto &texture = *static_cast<CaptureTexture *>(record->d3d_texture);
    if (!texture.rim_path_.empty()) {
      continue;
    }
    texture.rim_path_ = record->path;
    ++TheStats.textures_named;
    // The image may not exist yet; NameTextureImage no-ops then, and EnsureTextureImage
    // picks the name up when it does.
    vulkan::NameTextureImage(texture.image_, texture.rim_path_);
  }
}

void MirrorBlitDestination(CaptureSurface &destination) {
  CaptureTexture &dst_texture = *destination.owner_;
  if (!EnsureTextureImage(dst_texture)) {
    return;
  }
  ++dst_texture.blits_in_;
  if (destination.level_ < 32) {
    dst_texture.levels_blitted_ |= 1u << destination.level_;
  }
  // The WHOLE chain, not just the level this blit named. A texture with both its levels
  // blitted and both mirrored still came back with level 1 wrong, so something writes mip
  // levels outside the blit that names them; re-reading every level converges regardless of
  // which moved, and took level01 from 126/150 mip levels byte-identical to 148/150.
  //
  // Deferring this read to the next bind instead was tried and is much worse - 85/150 -
  // because a texture blitted after its last bind is then never re-read at all. Doing both
  // costs a second whole-chain read per blit and measured the same as this alone, so it is
  // not here.
  ReseedTextureImage(dst_texture);
}

// SetTexture takes an IDirect3DBaseTexture8, so our wrapper arrives as a base pointer. The
// membership check is what makes the downcast sound - see Unwrap for buffers.
IDirect3DBaseTexture8 *UnwrapTexture(IDirect3DBaseTexture8 *texture) {
  if (texture == nullptr) {
    return nullptr;
  }
  if (LiveTextureWrappers.count(texture) == 0) {
    ++TheStats.foreign_buffers;
    return texture;
  }
  return static_cast<CaptureTexture *>(texture)->inner_;
}

// The game hands our wrappers back to SetStreamSource/SetIndices, so those have to unwrap
// before reaching the real device.
//
// The membership check is not defensive padding - it is the only sound way to do this. A
// `static_cast` to our wrapper type is undefined unless the pointer really is one of ours,
// and a COM interface carries nothing that says so. A buffer that never went through our
// CreateVertexBuffer therefore reads `inner_` from whatever happens to sit at that offset
// and hands a garbage pointer to D3D9 - which presents as an access violation *inside
// d3d9.dll*, nowhere near here. `foreign_buffers` counts any such pointer instead, and it
// is passed through untouched.
IDirect3DVertexBuffer8 *Unwrap(IDirect3DVertexBuffer8 *buffer) {
  if (buffer == nullptr) {
    return nullptr;
  }
  if (LiveVertexWrappers.count(buffer) == 0) {
    ++TheStats.foreign_buffers;
    return buffer;
  }
  return static_cast<CaptureVertexBuffer *>(buffer)->inner_;
}

IDirect3DIndexBuffer8 *Unwrap(IDirect3DIndexBuffer8 *buffer) {
  if (buffer == nullptr) {
    return nullptr;
  }
  if (LiveIndexWrappers.count(buffer) == 0) {
    ++TheStats.foreign_buffers;
    return buffer;
  }
  return static_cast<CaptureIndexBuffer *>(buffer)->inner_;
}

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
  void EmitDraw(D3DPRIMITIVETYPE type, UINT start_index, UINT primitive_count, bool indexed,
                UINT start_vertex);
  // The synthetic quad probe (§4.35). Issued through this device's OWN methods rather than
  // through `inner_`, which is the entire point: the states and the draw go down both paths at
  // once, so the reference and this renderer are handed the same geometry, the same texture and
  // the same stage setup with nothing of the scene left in the way.
  void DrawProbeQuad();

  void EmitDrawUP(D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertex_data,
                  UINT vertex_stride, const void *index_data, D3DFORMAT index_format,
                  UINT vertex_count);
};

// The one device the game ever creates. Held so a resource's GetDevice can hand back the
// wrapper rather than d3d8to9's object; borrowed, not owned, and never AddRef'd on our own
// account, because a self-reference would keep the device alive for its own lifetime.
CaptureDevice *TheCaptureDevice = nullptr;

bool TryGetCaptureDevice(IDirect3DDevice8 **ppDevice) {
  if (TheCaptureDevice == nullptr) {
    return false;
  }
  ++TheStats.resource_get_devices;
  TheCaptureDevice->AddRef();
  *ppDevice = TheCaptureDevice;
  return true;
}

// Every method we are not interested in yet. A method moved into the generator's
// INTERCEPTED list drops out of these, so forgetting to write its body is a link error
// rather than a method that silently keeps forwarding.
#define GK_FORWARD_D3D8(ret, name, params, args)                                          \
  ret STDMETHODCALLTYPE CaptureD3D8::name params { return inner_->name args; }
GK_IDIRECT3D8_FORWARDED(GK_FORWARD_D3D8)
#undef GK_FORWARD_D3D8

#define GK_FORWARD_DEVICE(ret, name, params, args)                                        \
  ret STDMETHODCALLTYPE CaptureDevice::name params { return inner_->name args; }
GK_IDIRECT3DDEVICE8_FORWARDED(GK_FORWARD_DEVICE)
#undef GK_FORWARD_DEVICE

// ---------------------------------------------------------------------------------------
// IDirect3D8: the one interception, and the whole reason this interface is wrapped.

HRESULT STDMETHODCALLTYPE CaptureD3D8::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS8 *pPresentationParameters,
    IDirect3DDevice8 **ppReturnedDeviceInterface) {
  IDirect3DDevice8 *device = nullptr;
  const HRESULT hr =
      inner_->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                           pPresentationParameters, &device);
  if (FAILED(hr) || !device) {
    return hr;
  }

  HaveDevice = true;
  // The shadow state mirrors a device, so it starts when one does - and it must start at the
  // API's defaults rather than at zero. See InitialiseShadowState.
  InitialiseShadowState();
  DebugWrite("gkplus: d3d8 capture device created\n");
  auto *const captured = new CaptureDevice(device, this);
  // hDeviceWindow wins where it is set: hFocusWindow is allowed to be null for a windowed
  // device, and D3D uses hDeviceWindow as the presentation target.
  captured->window_ = pPresentationParameters != nullptr &&
                              pPresentationParameters->hDeviceWindow != nullptr
                          ? pPresentationParameters->hDeviceWindow
                          : hFocusWindow;
  TheCaptureDevice = captured;
  *ppReturnedDeviceInterface = captured;
  return hr;
}

// ---------------------------------------------------------------------------------------
// IDirect3DDevice8. Phase 0b: record, then forward unchanged. Nothing here changes what the
// game renders - see vulkan_renderer_notes.md section 4.

HRESULT STDMETHODCALLTYPE CaptureDevice::GetDirect3D(IDirect3D8 **ppD3D8) {
  if (!ppD3D8) {
    return E_POINTER;
  }
  parent_->AddRef();
  *ppD3D8 = parent_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture8 **ppTexture) {
  ++TheStats.textures_created;
  ++TheStats.texture_formats[static_cast<uint32_t>(Format)];
  // The pool is what separates the SYSTEMMEM staging copies from the textures actually bound
  // for drawing. Only the latter need a VkImage.
  ++TheStats.texture_pools[static_cast<uint32_t>(Pool)];
  IDirect3DTexture8 *texture = nullptr;
  const HRESULT hr =
      inner_->CreateTexture(Width, Height, Levels, Usage, Format, Pool, &texture);
  if (FAILED(hr) || texture == nullptr) {
    return hr;
  }
  auto *const wrapper = new CaptureTexture(texture, Width, Height, Levels,
                                           static_cast<uint32_t>(Format),
                                           static_cast<uint32_t>(Pool));
  wrapper->usage_ = static_cast<uint32_t>(Usage);
  *ppTexture = wrapper;
  return hr;
}

// --- the ten device methods that carry a surface -----------------------------------------
//
// Every one of these was named by the generator's check_wrapped_params(), not by reading the
// header. `SetCursorProperties` in particular looks nothing like a resource call, which is
// precisely the shape that let ProcessVertices through twice.
//
// The out-parameter ones wrap; the in-parameter ones unwrap. Two also count, because they are
// the pixel routes that involve no lock at all: CopyRects into a texture level, and a texture
// level bound as a render target.

HRESULT STDMETHODCALLTYPE CaptureDevice::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    BOOL Lockable, IDirect3DSurface8 **ppSurface) {
  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr = inner_->CreateRenderTarget(Width, Height, Format, MultiSample, Lockable,
                                                &surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppSurface = WrapSurface(surface, nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    IDirect3DSurface8 **ppSurface) {
  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr =
      inner_->CreateDepthStencilSurface(Width, Height, Format, MultiSample, &surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppSurface = WrapSurface(surface, nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::CreateImageSurface(UINT Width, UINT Height,
                                                             D3DFORMAT Format,
                                                             IDirect3DSurface8 **ppSurface) {
  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr = inner_->CreateImageSurface(Width, Height, Format, &surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppSurface = WrapSurface(surface, nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::GetBackBuffer(UINT iBackBuffer,
                                                        D3DBACKBUFFER_TYPE Type,
                                                        IDirect3DSurface8 **ppBackBuffer) {
  if (ppBackBuffer == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr = inner_->GetBackBuffer(iBackBuffer, Type, &surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppBackBuffer = WrapSurface(surface, nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE
CaptureDevice::GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) {
  if (ppRenderTarget == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr = inner_->GetRenderTarget(&surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppRenderTarget = WrapSurface(surface, nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE
CaptureDevice::GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) {
  if (ppZStencilSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  IDirect3DSurface8 *surface = nullptr;
  const HRESULT hr = inner_->GetDepthStencilSurface(&surface);
  if (FAILED(hr)) {
    return hr;
  }
  *ppZStencilSurface = WrapSurface(surface, nullptr);
  return hr;
}

// The route the engine actually uses, measured: it locks a SYSTEMMEM staging texture and
// blits it into the one it binds. So IDirect3DTexture8::LockRect does see every pixel - on
// the staging texture - and this call is what says which texture they end up in.
//
// `copy_rects_untracked` is therefore the invariant an upload built on LockRect rests on: a
// destination whose source is not a level of a texture this layer wrapped is a texture whose
// new contents are unknown here.
HRESULT STDMETHODCALLTYPE CaptureDevice::CopyRects(
    IDirect3DSurface8 *pSourceSurface, const RECT *pSourceRectsArray, UINT cRects,
    IDirect3DSurface8 *pDestinationSurface, const POINT *pDestPointsArray) {
  CaptureSurface *const source = AsCaptureSurface(pSourceSurface);
  CaptureSurface *const destination = AsCaptureSurface(pDestinationSurface);
  if (destination != nullptr && destination->owner_ != nullptr) {
    ++TheStats.surface_copy_rects;
    if (source == nullptr || source->owner_ == nullptr) {
      ++TheStats.copy_rects_untracked;
    }
    if (pSourceRectsArray != nullptr || cRects != 0) {
      ++TheStats.copy_rects_partial;
    }
  }
  const HRESULT hr =
      inner_->CopyRects(UnwrapSurface(pSourceSurface), pSourceRectsArray, cRects,
                        UnwrapSurface(pDestinationSurface), pDestPointsArray);
  if (SUCCEEDED(hr) && destination != nullptr && destination->owner_ != nullptr &&
      ApplyTextureBlits) {
    MirrorBlitDestination(*destination);
  }
  return hr;
}

// The fourth route, and the only one with no CPU-visible copy anywhere: bind a texture level
// as the render target and let the GPU write it. Render-to-texture would have to be
// reproduced in Vulkan rather than uploaded, so this is counted separately from the rest.
HRESULT STDMETHODCALLTYPE CaptureDevice::SetRenderTarget(IDirect3DSurface8 *pRenderTarget,
                                                          IDirect3DSurface8 *pNewZStencil) {
  if (IsTextureSurface(pRenderTarget)) {
    ++TheStats.texture_render_targets;
  }
  return inner_->SetRenderTarget(UnwrapSurface(pRenderTarget),
                                 UnwrapSurface(pNewZStencil));
}

HRESULT STDMETHODCALLTYPE CaptureDevice::GetFrontBuffer(IDirect3DSurface8 *pDestSurface) {
  return inner_->GetFrontBuffer(UnwrapSurface(pDestSurface));
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetCursorProperties(
    UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) {
  return inner_->SetCursorProperties(XHotSpot, YHotSpot, UnwrapSurface(pCursorBitmap));
}

HRESULT STDMETHODCALLTYPE CaptureDevice::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer8 **ppVertexBuffer) {
  ++TheStats.vertex_buffers_created;
  TheStats.vertex_bytes += Length;
  IDirect3DVertexBuffer8 *buffer = nullptr;
  const HRESULT hr = inner_->CreateVertexBuffer(Length, Usage, FVF, Pool, &buffer);
  if (FAILED(hr) || buffer == nullptr) {
    return hr;
  }
  *ppVertexBuffer =
      WrapVertexBuffers ? new CaptureVertexBuffer(buffer, Length, FVF, Pool) : buffer;
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer8 **ppIndexBuffer) {
  ++TheStats.index_buffers_created;
  TheStats.index_bytes += Length;
  IDirect3DIndexBuffer8 *buffer = nullptr;
  const HRESULT hr = inner_->CreateIndexBuffer(Length, Usage, Format, Pool, &buffer);
  if (FAILED(hr) || buffer == nullptr) {
    return hr;
  }
  *ppIndexBuffer =
      WrapIndexBuffers ? new CaptureIndexBuffer(buffer, Length, static_cast<uint32_t>(Format),
                                                static_cast<uint32_t>(Pool))
                       : buffer;
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::BeginScene() { return inner_->BeginScene(); }

HRESULT STDMETHODCALLTYPE CaptureDevice::EndScene() { return inner_->EndScene(); }

HRESULT STDMETHODCALLTYPE CaptureDevice::Present(const RECT *pSourceRect,
                                                 const RECT *pDestRect,
                                                 HWND hDestWindowOverride,
                                                 const RGNDATA *pDirtyRegion) {
  // The frame boundary that actually holds. The game's own PresentScene @ 0x00574d30 is
  // gated on window focus (rendering_notes.md section 4), so it is not a reliable tick -
  // but it is the only thing that reaches here, so this counter measures rendered frames
  // rather than elapsed ones, which is what a draws-per-frame distribution wants.
  ++TheStats.frames;
  // Once a frame, because a cache record's D3D pointer is stored at an unpredictable point
  // after the texture is created and there is no event to hang this on. One pass over ~130
  // records; the work stops mattering as soon as everything is named.
  ResolveTextureNames();
  if (TheStats.draws_this_frame > TheStats.max_draws_per_frame) {
    TheStats.max_draws_per_frame = TheStats.draws_this_frame;
  }
  TheStats.draws_this_frame = 0;
  if (TheStats.locked_bytes_this_frame > TheStats.max_locked_bytes_per_frame) {
    TheStats.max_locked_bytes_per_frame = TheStats.locked_bytes_this_frame;
  }
  TheStats.locked_bytes_this_frame = 0;
  if (MaterialKeysThisFrame.size() > TheStats.max_materials_per_frame) {
    TheStats.max_materials_per_frame = MaterialKeysThisFrame.size();
  }
  MaterialKeysThisFrame.clear();

  // GKPLUS_RENDERER=vulkan: Vulkan owns the window from here. The game's D3D9 device and
  // every resource it created stay exactly as they are - they simply stop reaching the
  // screen - so this is a branch rather than a null device. See src/VkRenderer.h.
  //
  // The window comes from the device rather than from the game's global, because this is
  // the one place it is known for certain: it is what CreateDevice was given.
  // Before the renderer draws the frame, so the probe is in the list Vulkan records and in the
  // scene d3d8/d3d9 present. It is the last draw either way, which is what makes it unoccluded
  // without needing the depth buffer disabled for anything but itself.
  DrawProbeQuad();

  if (vulkan::RendererRequested()) {
    if (!vulkan::RendererReady()) {
      vulkan::StartRenderer(window_);
    }
    if (vulkan::RendererReady()) {
      vulkan::DrawFrame();
      return D3D_OK;
    }
    // Bring-up failed. Fall through to D3D so the game stays playable rather than showing
    // nothing; VkRenderer has already recorded why.
  }

  return inner_->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT STDMETHODCALLTYPE
CaptureDevice::Reset(D3DPRESENT_PARAMETERS8 *pPresentationParameters) {
  // A Reset is how the game changes resolution, so the swapchain is stale afterwards even
  // though no WM_SIZE need have reached us. Told explicitly rather than waiting for the
  // driver to report VK_ERROR_OUT_OF_DATE_KHR, which it is not obliged to do promptly.
  vulkan::NotifyResize();
  return inner_->Reset(pPresentationParameters);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::Clear(DWORD Count, const D3DRECT *pRects,
                                               DWORD Flags, D3DCOLOR Color, float Z,
                                               DWORD Stencil) {
  return inner_->Clear(Count, pRects, Flags, Color, Z, Stencil);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetTransform(D3DTRANSFORMSTATETYPE State,
                                                      const D3DMATRIX *pMatrix) {
  NoteBlockState();
  TheStats.transform_states.insert(static_cast<uint32_t>(State));
  if (pMatrix != nullptr) {
    switch (static_cast<uint32_t>(State)) {
    // Qualified because the parameter is also called `State` and shadows the shadow state.
    case D3DTS_VIEW:
      gk::d3d8::State.view = *pMatrix;
      gk::d3d8::State.have_view = true;
      break;
    case D3DTS_PROJECTION:
      gk::d3d8::State.projection = *pMatrix;
      gk::d3d8::State.have_projection = true;
      break;
    case D3DTS_WORLD:
      gk::d3d8::State.world = *pMatrix;
      gk::d3d8::State.have_world = true;
      break;
    default:
      break;
    }
  }
  return inner_->SetTransform(State, pMatrix);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetViewport(const D3DVIEWPORT8 *pViewport) {
  NoteBlockState();
  if (pViewport != nullptr) {
    State.viewport_width = pViewport->Width;
    State.viewport_height = pViewport->Height;
    State.viewport_min_z = pViewport->MinZ;
    State.viewport_max_z = pViewport->MaxZ;
    ViewportDepthRanges.insert((uint64_t(FloatBits(pViewport->MinZ)) << 32) |
                               FloatBits(pViewport->MaxZ));
  }
  return inner_->SetViewport(pViewport);
}

// The three lighting setters are recorded straight into the shadow state rather than through
// `Record`, i.e. they are deliberately NOT replayable from a state block. That is the same
// narrowness the OpKind list already has, and it holds for the same measured reason: every
// block is built by AwMaterial_Compile, which issues render states and stage states only. If
// a light ever appeared inside one, the shadow state would go stale after an Apply with
// nothing to notice - so `render.state` prints the counts, which is what would show it.
HRESULT STDMETHODCALLTYPE CaptureDevice::SetMaterial(const D3DMATERIAL8 *pMaterial) {
  NoteBlockState();
  if (pMaterial != nullptr) {
    State.material = *pMaterial;
    State.have_material = true;
    ++MaterialSets;
  }
  return inner_->SetMaterial(pMaterial);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetLight(DWORD Index, const D3DLIGHT8 *pLight) {
  NoteBlockState();
  if (Index > TheStats.max_light_index) {
    TheStats.max_light_index = Index;
  }
  if (Index < kLights && pLight != nullptr) {
    State.lights[Index] = *pLight;
    State.light_set[Index] = true;
    ++LightGeneration;
  }
  ++LightSets;
  return inner_->SetLight(Index, pLight);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::LightEnable(DWORD Index, BOOL Enable) {
  NoteBlockState();
  if (Index > TheStats.max_light_index) {
    TheStats.max_light_index = Index;
  }
  if (Index < kLights) {
    State.light_enabled[Index] = Enable != FALSE;
  }
  ++LightEnables;
  return inner_->LightEnable(Index, Enable);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetRenderState(D3DRENDERSTATETYPE State,
                                                        DWORD Value) {
  NoteBlockState();
  TheStats.render_states[static_cast<uint32_t>(State)].insert(
      static_cast<uint32_t>(Value));
  Record({OpKind::RenderState, static_cast<uint32_t>(State), 0,
          static_cast<uint32_t>(Value), nullptr});
  // GKPLUS_NO_LIGHTING=1: hold D3DRS_LIGHTING off in the forwarded call only. This is a
  // measuring instrument, not a feature - it makes the game's OWN renderer draw the scene
  // without the one thing the Vulkan path is missing, which is the only way to tell "our
  // renderer is missing lighting" apart from "our renderer is wrong in some other way that
  // happens to look flat". The shadow state still records the true value, so `render.state`
  // does not start lying while it is set.
  if (ForceLightingOff && State == D3DRS_LIGHTING) {
    Value = FALSE;
  }
  if (ForceSpecularOff && State == D3DRS_SPECULARENABLE) {
    Value = FALSE;
  }
  // The two reasons a draw disappears rather than comes out the wrong colour. Pointed at the
  // reference renderer they answer "is d3d8to9 culling this, or depth-rejecting it, or neither"
  // - which is one launch each against a session of reading its source (§4.29).
  if (ForceNoCull && State == D3DRS_CULLMODE) {
    Value = D3DCULL_NONE;
  }
  if (ForceNoZTest && State == D3DRS_ZENABLE) {
    Value = D3DZB_FALSE;
  }
  return inner_->SetRenderState(State, Value);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetTexture(DWORD Stage,
                                                     IDirect3DBaseTexture8 *pTexture) {
  NoteBlockState();
  if (Stage > TheStats.max_stage_used) {
    TheStats.max_stage_used = Stage;
  }
  // The shadow state keeps the WRAPPER, because the material key hashes pointer identity and
  // the wrapper is what the game consistently hands us. Only the device sees the inner one.
  // Giving the texture its image happens in ApplyOp, which this reaches through Record - see
  // there for why it cannot live here.
  Record({OpKind::Texture, Stage, 0, 0, pTexture});
  return inner_->SetTexture(Stage, UnwrapTexture(pTexture));
}

HRESULT STDMETHODCALLTYPE CaptureDevice::GetTexture(DWORD Stage,
                                                     IDirect3DBaseTexture8 **ppTexture) {
  if (ppTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  if (Stage >= kStages) {
    return inner_->GetTexture(Stage, ppTexture);
  }
  IDirect3DBaseTexture8 *texture = State.textures[Stage];
  if (texture != nullptr) {
    texture->AddRef();
  }
  *ppTexture = texture;
  return D3D_OK;
}

// The fifth way into a texture's pixels, and the last one D3D8 has: copy every dirty level of
// a SYSTEMMEM texture into a MANAGED or DEFAULT one, with no lock and no CopyRects anywhere.
// It went uninstrumented through §4.12 because the LockRect-to-CopyRects ratio was exactly 1:1
// and looked like a closed account - but a ratio says nothing about a route that touches
// neither counter. What found it was the content check: one texture whose image was stale
// although it had 0 game locks and 2 blits, and which a re-upload fixed.
HRESULT STDMETHODCALLTYPE CaptureDevice::UpdateTexture(
    IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture) {
  const HRESULT hr = inner_->UpdateTexture(UnwrapTexture(pSourceTexture),
                                           UnwrapTexture(pDestinationTexture));
  if (SUCCEEDED(hr) && pDestinationTexture != nullptr &&
      LiveTextureWrappers.count(pDestinationTexture) != 0) {
    ++TheStats.texture_updates;
    // Re-read the destination rather than replay the source: UpdateTexture's own rule is
    // "whatever D3D considers dirty", which this layer has no way to know. Reading the result
    // is both simpler and exactly right, and it is affordable because the call is rare.
    ReseedTextureImage(*static_cast<CaptureTexture *>(pDestinationTexture));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  NoteBlockState();
  if (Stage > TheStats.max_stage_used) {
    TheStats.max_stage_used = Stage;
  }
  TheStats.stage_states[(Stage << 16) | static_cast<uint32_t>(Type)].insert(
      static_cast<uint32_t>(Value));
  Record({OpKind::StageState, Stage, static_cast<uint32_t>(Type),
          static_cast<uint32_t>(Value), nullptr});
  // GKPLUS_NO_STAGE1=1: disable every stage past the first in the forwarded call. A state
  // block records what is forwarded, so a block built while this is set carries the disable
  // too and ApplyStateBlock cannot put the stage back.
  if (ForceStage1Off && Stage >= 1 && Type == D3DTSS_COLOROP) {
    Value = D3DTOP_DISABLE;
  }
  if (ForceNoMipmap && Type == D3DTSS_MIPFILTER) {
    Value = D3DTEXF_NONE;
  }
  return inner_->SetTextureStageState(Stage, Type, Value);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) {
  // The wrapper is remembered so the getter can hand back the same object the game gave us,
  // and unwrapped so the real device binds the buffer it actually owns.
  if (StreamNumber == 0) {
    stream0_ = pStreamData;
    stream0_stride_ = Stride;
  }
  return inner_->SetStreamSource(StreamNumber, Unwrap(pStreamData), Stride);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) {
  if (ppStreamData == nullptr || pStride == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  if (StreamNumber != 0) {
    return inner_->GetStreamSource(StreamNumber, ppStreamData, pStride);
  }
  // Returning `inner_` would hand the game a pointer this layer does not own, and its
  // Release would be unbalanced against the wrapper's.
  if (stream0_ != nullptr) {
    stream0_->AddRef();
  }
  *ppStreamData = stream0_;
  *pStride = stream0_stride_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetIndices(IDirect3DIndexBuffer8 *pIndexData,
                                                     UINT BaseVertexIndex) {
  indices_ = pIndexData;
  base_vertex_ = BaseVertexIndex;
  return inner_->SetIndices(Unwrap(pIndexData), BaseVertexIndex);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::GetIndices(IDirect3DIndexBuffer8 **ppIndexData,
                                                     UINT *pBaseVertexIndex) {
  if (ppIndexData == nullptr || pBaseVertexIndex == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  if (indices_ != nullptr) {
    indices_->AddRef();
  }
  *ppIndexData = indices_;
  *pBaseVertexIndex = base_vertex_;
  return D3D_OK;
}

// THE crash of Phase 2b. It takes a destination vertex buffer among five parameters and
// looks nothing like a resource call, so it sat in the forwarded list handing our wrapper
// to d3d8to9 - which static_casts it to its own Direct3DVertexBuffer8 and reads a proxy
// pointer out of the middle of our object. The result was an access violation in
// d3d9!CD3DHal::ProcessVertices with a garbage pointer, three frames below anything of ours.
HRESULT STDMETHODCALLTYPE CaptureDevice::ProcessVertices(
    UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
    IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) {
  return inner_->ProcessVertices(SrcStartIndex, DestIndex, VertexCount,
                                 Unwrap(pDestBuffer), Flags);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetVertexShader(DWORD Handle) {
  NoteBlockState();
  // AWAPI passes an FVF code here, never a compiled shader handle - all four Aw_Draw*
  // wrappers do `SetVertexShader(fvf)` and then compute the stride from the same value.
  // A real shader handle would have its low bit clear and be small; an FVF has
  // D3DFVF_POSITION_MASK bits set. Recorded raw so the distinction stays visible.
  ++TheStats.fvf_counts[static_cast<uint32_t>(Handle)];
  Record({OpKind::VertexShader, 0, 0, static_cast<uint32_t>(Handle), nullptr});
  return inner_->SetVertexShader(Handle);
}

// --- state blocks -----------------------------------------------------------------------
//
// A block built with BeginStateBlock .. EndStateBlock issues its state through the ordinary
// setters, so the histograms above already see its contents - just at build time (during the
// level load) rather than at draw time. That is the whole explanation for a steady-state
// sample reporting 11 render states and a max texture stage of 0.
//
// Counting how many states each block carries is what says whether Phase 2's shadow state
// can be reconstructed by replaying blocks, or whether it needs the device's own state.
HRESULT STDMETHODCALLTYPE CaptureDevice::BeginStateBlock() {
  RecordingBlock = true;
  StatesInCurrentBlock = 0;
  return inner_->BeginStateBlock();
}

HRESULT STDMETHODCALLTYPE CaptureDevice::EndStateBlock(DWORD *pToken) {
  RecordingBlock = false;
  const HRESULT hr = inner_->EndStateBlock(pToken);
  if (SUCCEEDED(hr) && pToken != nullptr) {
    Blocks[*pToken].ops = std::move(RecordingOps);
    Blocks[*pToken].opaque = false;
  }
  RecordingOps.clear();
  ++TheStats.blocks_recorded;
  TheStats.block_states_total += StatesInCurrentBlock;
  if (StatesInCurrentBlock > TheStats.max_block_states) {
    TheStats.max_block_states = StatesInCurrentBlock;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::ApplyStateBlock(DWORD Token) {
  ++TheStats.block_applies;
  // Replaying into the shadow state is the whole reason Phase 0b's "0 opaque blocks" result
  // mattered: the state a material applies is otherwise invisible here, because
  // ApplyStateBlock touches no Set* method.
  const auto found = Blocks.find(Token);
  if (found == Blocks.end() || found->second.opaque) {
    ++TheStats.opaque_block_applies;
  } else {
    for (const StateOp &op : found->second.ops) {
      ApplyOp(op);
    }
  }
  return inner_->ApplyStateBlock(Token);
}

// Both of these snapshot whatever the device state currently is, rather than replaying calls
// we witnessed - so their contents are opaque to this layer and to any replay built on it.
HRESULT STDMETHODCALLTYPE CaptureDevice::CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                          DWORD *pToken) {
  ++TheStats.blocks_opaque;
  const HRESULT hr = inner_->CreateStateBlock(Type, pToken);
  if (SUCCEEDED(hr) && pToken != nullptr) {
    Blocks[*pToken].ops.clear();
    Blocks[*pToken].opaque = true;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CaptureDevice::CaptureStateBlock(DWORD Token) {
  ++TheStats.blocks_opaque;
  Blocks[Token].ops.clear();
  Blocks[Token].opaque = true; // re-armed from device state we never saw
  return inner_->CaptureStateBlock(Token);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DeleteStateBlock(DWORD Token) {
  Blocks.erase(Token);
  return inner_->DeleteStateBlock(Token);
}

// --- turning one of the game's draws into a DrawItem ---------------------------------------
//
// Row-major, row-vector, exactly as D3D composes them, because that is what the shader consumes
// (see DrawItem). `a * b` here means "apply a, then b", which is D3D's reading order and the
// opposite of the column-vector one.
void MultiplyMatrix(const D3DMATRIX &a, const D3DMATRIX &b, float *out) {
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[row][k] * b.m[k][col];
      }
      out[row * 4 + col] = sum;
    }
  }
}

// The transform for this draw, or false if there is nothing sensible to use.
//
// Two families, decided by the FVF rather than guessed at: an ordinary vertex goes through
// world*view*projection, and a pre-transformed one (D3DFVF_XYZRHW, 0x4) is already in screen
// pixels and needs only a pixels-to-clip map. Handling both here rather than in the shader is
// what keeps the vertex path branch-free.
bool BuildMvp(uint32_t fvf, float *out) {
  constexpr uint32_t kXyzRhw = 0x004;
  if ((fvf & kXyzRhw) == kXyzRhw) {
    const float width = static_cast<float>(State.viewport_width);
    const float height = static_cast<float>(State.viewport_height);
    if (width <= 0.0f || height <= 0.0f) {
      return false;
    }
    // Pixels to clip. No Y flip: D3D screen space and Vulkan clip space both have Y growing
    // downwards, so this one is the same in both - it is the 3D path that needs the flip.
    const float m[16] = {
        2.0f / width, 0.0f,           0.0f, 0.0f,
        0.0f,         2.0f / height,  0.0f, 0.0f,
        0.0f,         0.0f,           1.0f, 0.0f,
        -1.0f,        -1.0f,          0.0f, 1.0f,
    };
    std::memcpy(out, m, sizeof(m));
    return true;
  }
  if (!State.have_view || !State.have_projection) {
    return false;
  }
  // An unset world transform is identity rather than a reason to skip: the game leaves it
  // alone for geometry already in world space, which is most of a level.
  D3DMATRIX identity = {};
  identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
  const D3DMATRIX &world = State.have_world ? State.world : identity;

  float world_view[16] = {};
  MultiplyMatrix(world, State.view, world_view);
  D3DMATRIX wv = {};
  std::memcpy(&wv, world_view, sizeof(world_view));
  MultiplyMatrix(wv, State.projection, out);

  // Vulkan's clip space has Y down where D3D's has it up. With a row-vector matrix the y
  // output is column 1, so negating that column is the whole flip.
  out[1] = -out[1];
  out[5] = -out[5];
  out[9] = -out[9];
  out[13] = -out[13];
  return true;
}

// The texture stages this draw is configured with, as the shader consumes them.
//
// §4.19 is why this exists at all: drawing stage 0 alone left level01 flat and bright, because
// the darkness of a Gunlok cavern is the SECOND stage - a lightmap blended over the diffuse
// texture with BLENDTEXTUREALPHA or ADDSIGNED. Neither fog nor D3D lighting was involved, and
// both were ruled out by making the game's own renderer draw without them.
//
// The stage count comes from D3DTSS_COLOROP, not from what is bound: a texture left over at
// stage 0 with the stage disabled must not be sampled, which is 2,160 draws a frame on
// level01.
uint32_t PackStageOp(uint32_t op, uint32_t arg1, uint32_t arg2, uint32_t texcoord) {
  return (op & 0xffu) | ((arg1 & 0xffu) << 8) | ((arg2 & 0xffu) << 16) |
         ((texcoord & 0xffu) << 24);
}

// Every D3DTEXTUREOP world.slang implements. An op outside this set still draws - the shader
// falls back to its first argument - but the draw is counted, so "the picture is subtly wrong"
// has a number attached rather than being something to notice by eye.
bool StageOpImplemented(uint32_t op) {
  switch (op) {
  case D3DTOP_SELECTARG1:
  case D3DTOP_SELECTARG2:
  case D3DTOP_MODULATE:
  case D3DTOP_MODULATE2X:
  case D3DTOP_MODULATE4X:
  case D3DTOP_ADD:
  case D3DTOP_ADDSIGNED:
  case D3DTOP_ADDSIGNED2X:
  case D3DTOP_SUBTRACT:
  case D3DTOP_BLENDDIFFUSEALPHA:
  case D3DTOP_BLENDTEXTUREALPHA:
  case D3DTOP_BLENDCURRENTALPHA:
    return true;
  default:
    return false;
  }
}

// The pipeline state and the alpha test, straight out of the shadow state. Both come from the
// same place and are filled together because the split between them is an implementation
// detail of the renderer - D3D has one set of render states, and which of them end up in a
// VkPipeline rather than in the shader is our decision, not the game's.
// How many indices - or, for a non-indexed draw, vertices - `primitives` of this topology
// need. Returns 0 for a primitive type the renderer does not draw, which is how a caller
// refuses rather than drawing a wrong-length draw.
bool TopologyEnabled(D3DPRIMITIVETYPE type) {
  switch (type) {
  case D3DPT_TRIANGLELIST:  return true;
  case D3DPT_TRIANGLESTRIP: return DrawStrips;
  case D3DPT_LINELIST:      return DrawLines;
  default:                  return false;
  }
}

uint32_t ElementCount(D3DPRIMITIVETYPE type, uint32_t primitives) {
  if (primitives == 0) {
    return 0;
  }
  switch (type) {
  case D3DPT_POINTLIST:     return primitives;
  case D3DPT_LINELIST:      return primitives * 2;
  case D3DPT_LINESTRIP:     return primitives + 1;
  case D3DPT_TRIANGLELIST:  return primitives * 3;
  case D3DPT_TRIANGLESTRIP: return primitives + 2;
  case D3DPT_TRIANGLEFAN:   return primitives + 2;
  default:                  return 0;
  }
}

void ResolvePipeline(vulkan::DrawItem &item, D3DPRIMITIVETYPE type) {
  item.pipeline.topology = static_cast<uint32_t>(type);
  item.pipeline.blend_enable = State.render_states[D3DRS_ALPHABLENDENABLE];
  item.pipeline.src_blend = State.render_states[D3DRS_SRCBLEND];
  item.pipeline.dest_blend = State.render_states[D3DRS_DESTBLEND];
  item.pipeline.depth_test = State.render_states[D3DRS_ZENABLE];
  item.pipeline.depth_write = State.render_states[D3DRS_ZWRITEENABLE];
  item.pipeline.depth_func = State.render_states[D3DRS_ZFUNC];
  item.pipeline.cull_mode = State.render_states[D3DRS_CULLMODE];
  item.pipeline.colour_write = State.render_states[D3DRS_COLORWRITEENABLE];
  item.pipeline.stencil_enable = State.render_states[D3DRS_STENCILENABLE];
  item.pipeline.stencil_func = State.render_states[D3DRS_STENCILFUNC];
  item.pipeline.stencil_fail = State.render_states[D3DRS_STENCILFAIL];
  item.pipeline.stencil_zfail = State.render_states[D3DRS_STENCILZFAIL];
  item.pipeline.stencil_pass = State.render_states[D3DRS_STENCILPASS];
  // Dynamic state on the Vulkan side, so it rides on the item rather than in the pipeline key.
  item.stencil_ref = State.render_states[D3DRS_STENCILREF];
  item.stencil_mask = State.render_states[D3DRS_STENCILMASK];
  item.stencil_write_mask = State.render_states[D3DRS_STENCILWRITEMASK];

  if (State.render_states[D3DRS_ALPHATESTENABLE] != 0) {
    item.flags = (State.render_states[D3DRS_ALPHAFUNC] & 0xfu) |
                 ((State.render_states[D3DRS_ALPHAREF] & 0xffu) << 8);
  }
  // Not part of the pipeline key - the shader selects between an interpolated and a flat copy of
  // the vertex colour, so this rides on the material instead (§4.31).
  item.shade_mode = State.render_states[D3DRS_SHADEMODE];
  // The viewport's depth slice, which the engine changes between draws to layer its effects in
  // front of the world (§4.32). Dynamic state on the Vulkan side, like the stencil reference.
  item.min_depth = State.viewport_min_z;
  item.max_depth = State.viewport_max_z;
}

// What fixed-function lighting turns this draw's vertex colour into, for the ONE case the
// renderer used to handle: lighting on, a vertex with no colour of its own, and no light
// enabled. D3D's equation
//
//     colour = emissive + ambient_material * global_ambient + SUM over lights(...)
//
// collapses there to the material's EMISSIVE, a single colour per draw, with the alpha coming
// from the diffuse source. It is what makes Gunlok's HUD panels green: they carry no vertex
// colour at all (FVF 0x112 and 0x212), so the material is the only thing that colours them, and
// rendering the vertex default of white instead showed the source art in its own colours - a
// perfectly plausible picture, and not the game's (§4.20).
//
// Kept, although the real sum below subsumes it, because it is the *previous build's* answer:
// `render.lighting = false` selects it, and that is what makes the light sum comparable on one
// paused frame rather than across two launches (§4.21's noise floor is 8.06/255).
uint32_t LitVertexColour() {
  const D3DMATERIAL8 &m = State.material;
  const uint32_t ambient = State.render_states[D3DRS_AMBIENT];
  auto channel = [](float value) {
    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
  };
  const float global_r = static_cast<float>((ambient >> 16) & 0xff) / 255.0f;
  const float global_g = static_cast<float>((ambient >> 8) & 0xff) / 255.0f;
  const float global_b = static_cast<float>(ambient & 0xff) / 255.0f;
  // The alpha of a lit vertex comes from the diffuse source. D3DMCS_COLOR1 would make that the
  // vertex's own, but this only runs where the vertex has none, so D3D falls back to the
  // material - and the HUD's alpha is genuinely in there: 0x80, 0x99, 0xcc.
  const uint32_t alpha = channel(m.Diffuse.a);
  return (alpha << 24) | (channel(m.Emissive.r + m.Ambient.r * global_r) << 16) |
         (channel(m.Emissive.g + m.Ambient.g * global_g) << 8) |
         channel(m.Emissive.b + m.Ambient.b * global_b);
}

// --- the light sum's inputs ------------------------------------------------------------------

void StoreColour(float *out, const D3DCOLORVALUE &colour) {
  out[0] = colour.r;
  out[1] = colour.g;
  out[2] = colour.b;
  out[3] = colour.a;
}

// A D3DCOLOR as four floats, for D3DRS_AMBIENT - which is a packed colour where every other
// lighting input is a float quad.
void StorePackedColour(float *out, uint32_t packed) {
  out[0] = static_cast<float>((packed >> 16) & 0xff) / 255.0f;
  out[1] = static_cast<float>((packed >> 8) & 0xff) / 255.0f;
  out[2] = static_cast<float>(packed & 0xff) / 255.0f;
  out[3] = static_cast<float>((packed >> 24) & 0xff) / 255.0f;
}

// The inverse transpose of `world`'s upper 3x3, written as three rows of four so it keeps the
// float4 stride the whole record uses. That is the transform a normal takes: the 3x3 alone is
// only right when the world matrix has no scale, and a unit's does.
//
// A singular 3x3 falls back to the matrix itself rather than producing infinities - it cannot
// happen for a transform the game draws with, and a NaN normal would take the whole draw's
// colour with it rather than failing where it was made.
void StoreNormalTransform(float *out, const D3DMATRIX &world) {
  const float(&m)[4][4] = world.m;
  const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
  const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
  const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const float determinant = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  if (determinant > -1e-12f && determinant < 1e-12f) {
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        out[row * 4 + col] = m[row][col];
      }
      out[row * 4 + 3] = 0.0f;
    }
    return;
  }
  // The cofactor matrix over the determinant IS the inverse transpose: cofactor[r][c] belongs
  // at [r][c] of adjugate-transpose, so no further transposition is needed here.
  const float inverse = 1.0f / determinant;
  const float cofactor[3][3] = {
      {c00, c01, c02},
      {m[0][2] * m[2][1] - m[0][1] * m[2][2], m[0][0] * m[2][2] - m[0][2] * m[2][0],
       m[0][1] * m[2][0] - m[0][0] * m[2][1]},
      {m[0][1] * m[1][2] - m[0][2] * m[1][1], m[0][2] * m[1][0] - m[0][0] * m[1][2],
       m[0][0] * m[1][1] - m[0][1] * m[1][0]},
  };
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      out[row * 4 + col] = cofactor[row][col] * inverse;
    }
    out[row * 4 + 3] = 0.0f;
  }
}

// The camera in world space, for the specular halfway vector. The view matrix is world->view
// and rigid, so its rotation inverts by transposition and the eye is the negated translation
// carried back through it.
void StoreEye(float *out, const D3DMATRIX &view) {
  const float(&m)[4][4] = view.m;
  out[0] = -(m[3][0] * m[0][0] + m[3][1] * m[0][1] + m[3][2] * m[0][2]);
  out[1] = -(m[3][0] * m[1][0] + m[3][1] * m[1][1] + m[3][2] * m[1][2]);
  out[2] = -(m[3][0] * m[2][0] + m[3][1] * m[2][1] + m[3][2] * m[2][2]);
  out[3] = 1.0f;
}

// The frame's light array, deduplicated by enable mask.
//
// Worth doing rather than appending per draw: `LightEnable` is called 118 million times a
// session (§4.25), lights are switched on and off around individual draws, and the same handful
// of masks recur - so this turns "up to eight records per draw" into a handful per frame. The
// generation is what keeps it honest when a light's *contents* change under an unchanged mask.
struct LightRun {
  uint32_t offset = 0;
  uint32_t count = 0;
};

std::map<uint32_t, LightRun> LightRunByMask;
uint64_t LightRunFrame = UINT64_MAX;
uint64_t LightRunGeneration = UINT64_MAX;

void StoreLight(vulkan::GpuLight &out, const D3DLIGHT8 &light) {
  out.position[0] = light.Position.x;
  out.position[1] = light.Position.y;
  out.position[2] = light.Position.z;
  out.position[3] = 1.0f;
  // Normalised here rather than in the shader: it is per draw rather than per vertex, and D3D
  // does not require the game to hand it a unit vector.
  const float length = std::sqrt(light.Direction.x * light.Direction.x +
                                 light.Direction.y * light.Direction.y +
                                 light.Direction.z * light.Direction.z);
  const float scale = length > 1e-8f ? 1.0f / length : 0.0f;
  out.direction[0] = light.Direction.x * scale;
  out.direction[1] = light.Direction.y * scale;
  out.direction[2] = light.Direction.z * scale;
  out.direction[3] = 0.0f;
  StoreColour(out.diffuse, light.Diffuse);
  StoreColour(out.specular, light.Specular);
  StoreColour(out.ambient, light.Ambient);
  out.attenuation[0] = light.Attenuation0;
  out.attenuation[1] = light.Attenuation1;
  out.attenuation[2] = light.Attenuation2;
  // A directional light has no range, and D3D ignores the field for one. Left as the light's
  // own value anyway, because the shader never reads it on that path.
  out.attenuation[3] = light.Range;
  out.spot[0] = std::cos(light.Theta * 0.5f);
  out.spot[1] = std::cos(light.Phi * 0.5f);
  out.spot[2] = light.Falloff;
  out.spot[3] = static_cast<float>(light.Type);
}

// Writes this draw's enabled lights into the frame's array and returns where they landed.
// `count` comes back 0 when nothing is enabled, which is a legitimate and common state - the
// draw is then lit by the emissive and global ambient terms alone.
void ResolveLightRun(uint64_t frame, uint32_t &offset, uint32_t &count) {
  offset = 0;
  count = 0;
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kLights; ++i) {
    if (State.light_enabled[i] && State.light_set[i]) {
      mask |= 1u << i;
    }
  }
  if (mask == 0) {
    return;
  }
  // The cache lives inside one frame and one light generation, because the scratch it indexes
  // into is emptied every frame and its contents are only right for the lights as they stood.
  if (frame != LightRunFrame || LightGeneration != LightRunGeneration) {
    LightRunByMask.clear();
    LightRunFrame = frame;
    LightRunGeneration = LightGeneration;
  }
  const auto found = LightRunByMask.find(mask);
  if (found != LightRunByMask.end()) {
    offset = found->second.offset;
    count = found->second.count;
    return;
  }

  uint32_t enabled = 0;
  for (uint32_t i = 0; i < kLights; ++i) {
    enabled += (mask & (1u << i)) != 0 ? 1u : 0u;
  }
  const vulkan::ScratchAlloc alloc = vulkan::AllocateScratchLights(enabled);
  if (!alloc.valid) {
    vulkan::MutableDrawStats().dropped_lights += enabled;
    return;
  }
  auto *lights = static_cast<vulkan::GpuLight *>(alloc.mapped);
  uint32_t written = 0;
  for (uint32_t i = 0; i < kLights; ++i) {
    if ((mask & (1u << i)) != 0) {
      StoreLight(lights[written++], State.lights[i]);
    }
  }
  offset = alloc.offset;
  count = written;
  LightRunByMask[mask] = LightRun{offset, written};
}

// Builds this draw's GpuDrawRecord and points the item at it. False means the frame's record
// scratch is full, which is a reason to skip the draw rather than to draw it against whatever
// record happens to be at index 0.
//
// This is where the fixed-function lighting state is SNAPSHOT, and per draw is the only place
// it can be: `render.state` samples between frames and sees every light off, which is what made
// the original A/B read as null and cost §4.19 its second headline claim. The state a per-draw
// quantity has between frames is not the state it has at any draw.
bool BuildDrawRecord(vulkan::DrawItem &item, const float *mvp, uint64_t frame) {
  const vulkan::ScratchAlloc alloc = vulkan::AllocateScratchDraws(1);
  if (!alloc.valid) {
    return false;
  }
  auto &record = *static_cast<vulkan::GpuDrawRecord *>(alloc.mapped);
  record = vulkan::GpuDrawRecord{};
  item.record = alloc.offset;
  std::memcpy(record.mvp, mvp, sizeof(record.mvp));

  // A pre-transformed vertex is never lit: it is already in screen space, and D3D takes its
  // colour straight from the vertex. Everything below would be meaningless for it.
  constexpr uint32_t kXyzRhw = 0x004;
  const bool pre_transformed = (State.fvf & kXyzRhw) == kXyzRhw;
  if (pre_transformed || State.render_states[D3DRS_LIGHTING] == 0) {
    return true;
  }

  // D3D8 documents no default material, and every colour in the equation comes from one - so a
  // draw issued before the game's first SetMaterial would light to black rather than to
  // anything defensible. Those keep the vertex colour, which is what the renderer did for every
  // draw until now and therefore cannot be a regression. The counter says whether it ever
  // happens after the first material, which would mean this guard is hiding something.
  if (!State.have_material) {
    ++vulkan::MutableDrawStats().lit_draws_without_material;
    return true;
  }

  if (!DrawLightSum) {
    // The previous build's behaviour, for the run-time A/B: the material collapse, and only
    // where the vertex has no colour of its own.
    if (!SkipLitColour && (State.fvf & D3DFVF_DIFFUSE) == 0) {
      bool any_light = false;
      for (uint32_t i = 0; i < kLights; ++i) {
        any_light = any_light || State.light_enabled[i];
      }
      if (!any_light) {
        record.lighting |= vulkan::kLitCollapse;
        record.lit_colour = LitVertexColour();
      }
    }
    return true;
  }

  const D3DMATRIX identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const D3DMATRIX &world = State.have_world ? State.world : identity;
  std::memcpy(record.world, &world, sizeof(record.world));
  StoreNormalTransform(record.normal_transform, world);
  StoreEye(record.eye, State.have_view ? State.view : identity);

  StoreColour(record.material_ambient, State.material.Ambient);
  StoreColour(record.material_diffuse, State.material.Diffuse);
  StoreColour(record.material_specular, State.material.Specular);
  record.material_specular[3] = State.material.Power;
  StoreColour(record.material_emissive, State.material.Emissive);
  StorePackedColour(record.global_ambient, State.render_states[D3DRS_AMBIENT]);

  uint32_t lighting = vulkan::kLightSum;
  if (State.render_states[D3DRS_NORMALIZENORMALS] != 0) {
    lighting |= vulkan::kNormaliseNormals;
  }
  if (State.render_states[D3DRS_SPECULARENABLE] != 0) {
    lighting |= vulkan::kSpecularEnable;
  }
  if (State.render_states[D3DRS_COLORVERTEX] != 0) {
    lighting |= vulkan::kColourVertex;
  }
  if ((State.fvf & D3DFVF_DIFFUSE) != 0) {
    lighting |= vulkan::kVertexColour;
  }
  const uint32_t sources[4] = {
      State.render_states[D3DRS_DIFFUSEMATERIALSOURCE] & 3u,
      State.render_states[D3DRS_AMBIENTMATERIALSOURCE] & 3u,
      State.render_states[D3DRS_SPECULARMATERIALSOURCE] & 3u,
      State.render_states[D3DRS_EMISSIVEMATERIALSOURCE] & 3u,
  };
  lighting |= sources[0] << vulkan::kDiffuseSourceShift;
  lighting |= sources[1] << vulkan::kAmbientSourceShift;
  lighting |= sources[2] << vulkan::kSpecularSourceShift;
  lighting |= sources[3] << vulkan::kEmissiveSourceShift;
  // D3DMCS_COLOR2 asks for the specular vertex colour, which the canonical vertex drops - but
  // only a draw whose FVF actually declares one loses anything, because D3D itself falls back
  // to the material where the vertex has no such colour. Gunlok sets SPECULARMATERIALSOURCE to
  // COLOR2 (the API default) on every draw, so testing the state alone counted all 1.35M of
  // them and said nothing; the FVF test is what makes this a defect counter.
  //
  // It reads 0, and that is a *structural* result rather than luck: the only FVF the game emits
  // with D3DFVF_SPECULAR is 0x1c4, which is D3DFVF_XYZRHW - pre-transformed, and therefore
  // never lit at all. The fallback is exact for every draw that is actually lit.
  if ((State.fvf & D3DFVF_SPECULAR) != 0) {
    for (const uint32_t source : sources) {
      if (source == D3DMCS_COLOR2) {
        ++vulkan::MutableDrawStats().lit_draws_wanting_colour2;
        break;
      }
    }
  }
  record.lighting = lighting;

  ResolveLightRun(frame, record.light_offset, record.light_count);
  ++vulkan::MutableDrawStats().lit_draws;
  if (record.light_count != 0) {
    ++vulkan::MutableDrawStats().lit_draws_with_lights;
  }
  return true;
}

void ResolveStages(vulkan::DrawItem &item) {
  const uint32_t active = ActiveStages();
  item.stage_count = (std::min)(active, 2u);
  if (active > 2) {
    ++vulkan::MutableDrawStats().truncated_stages;
  }
  for (uint32_t i = 0; i < item.stage_count; ++i) {
    const uint32_t *s = State.stage_states[i];
    vulkan::DrawStage &stage = item.stages[i];
    stage.color = PackStageOp(s[D3DTSS_COLOROP], s[D3DTSS_COLORARG1], s[D3DTSS_COLORARG2],
                              s[D3DTSS_TEXCOORDINDEX]);
    stage.alpha = PackStageOp(s[D3DTSS_ALPHAOP], s[D3DTSS_ALPHAARG1], s[D3DTSS_ALPHAARG2], 0);
    if (!StageOpImplemented(s[D3DTSS_COLOROP]) || !StageOpImplemented(s[D3DTSS_ALPHAOP])) {
      ++vulkan::MutableDrawStats().unsupported_stage_op;
    }

    IDirect3DBaseTexture8 *const bound = State.textures[i];
    if (bound == nullptr) {
      continue;
    }
    if (LiveTextureWrappers.count(bound) == 0) {
      ++vulkan::MutableDrawStats().stage_texture_unresolved;
      ++UnresolvedForeign[i];
      continue;
    }
    auto &texture = *static_cast<CaptureTexture *>(bound);
    if (texture.image_.valid) {
      stage.texture_index = texture.image_.index;
      stage.sampler_index = StageSampler(i);
    } else {
      ++vulkan::MutableDrawStats().stage_texture_unresolved;
      ++UnresolvedNoImage[i];
      ++UnresolvedFormats[(uint64_t(texture.pool_) << 32) | texture.format_];
    }
  }
}

// --- the synthetic quad probe (§4.35) ---------------------------------------------------------
//
// One textured quad, pre-transformed to exact screen pixels, drawn last, through this device's
// own methods so that the reference and this renderer get the same thing. Everything that could
// explain a difference in a *scene* is removed rather than controlled for: no lighting, no second
// stage, no alpha test, no blending, no depth, and `SELECTARG1(TEXTURE)` so the vertex colour
// cannot contribute either. What is left is the texture, the coordinates and the filter.
//
// `scale` is how many screen pixels a texel gets: 1.0 draws the texture at its own size, which is
// the case with no minification at all and therefore no LOD to choose. That is the point of the
// sweep - §4.34 ruled out mip *selection*, so the question is whether a 1:1 quad still differs.
namespace {

CaptureTexture *ProbeTexture = nullptr;
float ProbeScale = 1.0f;
uint32_t ProbeMipFilter = 0; // D3DTEXF_NONE, so a 1:1 quad cannot silently walk the chain
// Where the quad's top-left sits, in pixels from (16, 16). It exists because **a 1:1 quad at
// integer coordinates samples texel BOUNDARIES, not texel centres** (§4.35).
//
// The arithmetic: a vertex at x0 with u=0 and one at x0+W with u=1 makes the coordinate under
// screen sample position `sx` equal to `(sx - x0) / W`, and texel j's centre is at `(j+0.5)/W`.
// The renderer samples at the pixel centre and then shifts the whole viewport by half a pixel to
// match D3D (§4.28), so `sx` comes out at an integer - which lands on `j/W`, the corner where
// four texels meet and bilinear weights them equally. An offset of +0.5 puts it back on the
// centre, where bilinear returns the texel exactly and a difference means something.
float ProbeOffset = 0.0f;
// Render the texture's ALPHA as greyscale instead of its colour, via D3DTA_ALPHAREPLICATE on the
// colour argument. The screenshot has no alpha channel, so this is the only way to compare the
// two renderers' idea of a texture's alpha at all - and alpha is the one input a probe with
// blending off still cannot see (§4.36).
bool ProbeAlpha = false;
RECT ProbeRect = {};

} // namespace

void CaptureDevice::DrawProbeQuad() {
  if (ProbeTexture == nullptr || LiveTextureWrappers.count(ProbeTexture) == 0) {
    return;
  }
  const float width = static_cast<float>(ProbeTexture->width_) * ProbeScale;
  const float height = static_cast<float>(ProbeTexture->height_) * ProbeScale;
  // Top-left at a whole pixel plus `ProbeOffset`, which is what moves the sample points between
  // texel corners and texel centres - see the note on ProbeOffset.
  const float x0 = 16.0f + ProbeOffset, y0 = 16.0f + ProbeOffset;
  const float x1 = x0 + width, y1 = y0 + height;
  ProbeRect = {static_cast<LONG>(x0), static_cast<LONG>(y0), static_cast<LONG>(x1),
               static_cast<LONG>(y1)};

  BeginScene();
  SetRenderState(D3DRS_LIGHTING, FALSE);
  SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  SetRenderState(D3DRS_ZENABLE, FALSE);
  SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetRenderState(D3DRS_STENCILENABLE, FALSE);
  SetRenderState(D3DRS_FOGENABLE, FALSE);
  SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  SetTexture(0, ProbeTexture);
  SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_COLORARG1,
                       ProbeAlpha ? (D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE) : D3DTA_TEXTURE);
  SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  // The five sampler states are plain #defines in d3d8to9's header rather than enumerators,
  // because D3D9 moved them off the texture stage - hence the cast. They are the same states
  // §4.28 found the game sets only from inside state blocks.
  const auto stage_state = [](int value) {
    return static_cast<D3DTEXTURESTAGESTATETYPE>(value);
  };
  SetTextureStageState(0, stage_state(D3DTSS_MAGFILTER), D3DTEXF_LINEAR);
  SetTextureStageState(0, stage_state(D3DTSS_MINFILTER), D3DTEXF_LINEAR);
  SetTextureStageState(0, stage_state(D3DTSS_MIPFILTER), ProbeMipFilter);
  SetTextureStageState(0, stage_state(D3DTSS_ADDRESSU), D3DTADDRESS_CLAMP);
  SetTextureStageState(0, stage_state(D3DTSS_ADDRESSV), D3DTADDRESS_CLAMP);
  SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  // D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1 - the layout the game itself uses for its 2D
  // draws, so the converter is on a path it already takes rather than on one only the probe
  // exercises.
  struct ProbeVertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
    float u, v;
  };
  static_assert(sizeof(ProbeVertex) == 32);
  SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
  const ProbeVertex quad[4] = {
      {x0, y0, 0.5f, 1.0f, 0xffffffff, 0, 0.0f, 0.0f},
      {x1, y0, 0.5f, 1.0f, 0xffffffff, 0, 1.0f, 0.0f},
      {x0, y1, 0.5f, 1.0f, 0xffffffff, 0, 0.0f, 1.0f},
      {x1, y1, 0.5f, 1.0f, 0xffffffff, 0, 1.0f, 1.0f},
  };
  DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ProbeVertex));
  EndScene();
}

void CaptureDevice::NoteIndexedRange(D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices,
                                     UINT start_index, UINT primitive_count) {
  if (stream0_ == nullptr || indices_ == nullptr || stream0_stride_ == 0 ||
      LiveVertexWrappers.count(stream0_) == 0 || LiveIndexWrappers.count(indices_) == 0) {
    return;
  }
  const auto &vertex_buffer = *static_cast<CaptureVertexBuffer *>(stream0_);
  const auto &index_buffer = *static_cast<CaptureIndexBuffer *>(indices_);
  const uint64_t vertices_available = vertex_buffer.length_ / stream0_stride_;
  const uint64_t indices_available =
      index_buffer.index_stride_ == 0 ? 0 : index_buffer.length_ / index_buffer.index_stride_;
  const uint64_t vertices_wanted = static_cast<uint64_t>(min_index) + num_vertices;
  const uint64_t indices_wanted =
      static_cast<uint64_t>(start_index) + ElementCount(type, primitive_count);
  const bool vertices_over = vertices_wanted > vertices_available;
  const bool indices_over = indices_wanted > indices_available;
  if (!vertices_over && !indices_over) {
    return;
  }
  ++TheStats.draws_out_of_range;
  // The first few in full, because a count says it happens and only the numbers say what the
  // game asked for - and whether the overrun is one vertex or a whole buffer.
  if (OutOfRangeDraws.size() < 8) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "    draw %llu: vertices %llu+%u of %llu%s, indices %u+%u of %llu%s",
                  (unsigned long long)TheStats.draws_buffered, (unsigned long long)min_index,
                  num_vertices, (unsigned long long)vertices_available,
                  vertices_over ? "  <== over" : "", start_index,
                  ElementCount(type, primitive_count), (unsigned long long)indices_available,
                  indices_over ? "  <== over" : "");
    OutOfRangeDraws.emplace_back(line);
  }
}

// The buffered draws: `DrawIndexedPrimitive` and `DrawPrimitive`.
//
// **`DrawPrimitive` was not wired up at all until §4.32**, which is why it is worth saying what
// that looked like: the draw was counted by `CountDraw`, forwarded to d3d8to9, and never turned
// into a DrawItem - so it was missing from the Vulkan frame with **every skip counter reading
// zero**, because nothing had been skipped. Nothing was attempted. Gunlok draws its additive
// glow sprites this way, so a fire came out as bare scenery.
//
// The lesson is in the counters, not the omission: every "must be 0" reading here means "of the
// draws that reached this function", and a whole entry point that never reaches it is invisible
// to all of them. `draws_buffered` counts what the game issued and `DrawStats::items` counts
// what was submitted; the two disagreeing is the only signal that would have shown this, and
// nothing compared them.
void CaptureDevice::EmitDraw(D3DPRIMITIVETYPE type, UINT start_index, UINT primitive_count,
                             bool indexed, UINT start_vertex) {
  if (!vulkan::DrawReady()) {
    return;
  }
  ++vulkan::MutableDrawStats().seen;
  const uint32_t elements = TopologyEnabled(type) ? ElementCount(type, primitive_count) : 0;
  if (elements == 0) {
    ++vulkan::MutableDrawStats().skipped_topology;
    return;
  }
  // The vertex stream must be resident, or there is nothing to pull from. The INDEX buffer only
  // has to be for an indexed draw: `DrawPrimitive` reads none, and the game is entitled to leave
  // a stale one bound - requiring it here would drop every non-indexed draw whose last
  // SetIndices happened to name a buffer this layer never wrapped.
  if (stream0_ == nullptr || LiveVertexWrappers.count(stream0_) == 0 ||
      (indexed && (indices_ == nullptr || LiveIndexWrappers.count(indices_) == 0))) {
    ++vulkan::MutableDrawStats().skipped_no_slot;
    ++vulkan::MutableDrawStats().skipped_foreign_stream;
    return;
  }
  auto &vertex_buffer = *static_cast<CaptureVertexBuffer *>(stream0_);
  auto *const index_buffer = indexed ? static_cast<CaptureIndexBuffer *>(indices_) : nullptr;
  // A buffer whose only Unlock happened before the renderer existed has no slot and will never
  // be unlocked again. Being drawn is the definition of needing to be resident, so it seeds
  // itself from its own contents here - once, whether or not that works.
  if (!SkipSeeding) {
    vertex_buffer.SeedFromContents();
    if (index_buffer != nullptr) {
      index_buffer->SeedFromContents();
    }
  }
  // A live scratch version stands in for the slot: it is where this draw's contents actually
  // are, so a buffer that has one is resident whatever the arena did.
  const bool vertices_resident =
      vertex_buffer.slot_.valid || vertex_buffer.version_frame_ == TheStats.frames;
  const bool indices_resident =
      index_buffer == nullptr ||
      index_buffer->slot_.valid || index_buffer->version_frame_ == TheStats.frames;
  const bool index_stride_ok = index_buffer == nullptr || index_buffer->index_stride_ == 2;
  if (!vertices_resident || !indices_resident || !index_stride_ok) {
    ++vulkan::MutableDrawStats().skipped_no_slot;
    if (!vertex_buffer.slot_.valid) {
      ++vulkan::MutableDrawStats().skipped_unslotted_vertices;
      if (vertex_buffer.unlocks_ == 0) {
        ++vulkan::MutableDrawStats().skipped_never_unlocked;
      }
      ++UnslottedVertexBuffers[(uint64_t(vertex_buffer.fvf_) << 32) | vertex_buffer.length_];
    } else if (index_buffer != nullptr && !index_buffer->slot_.valid) {
      ++vulkan::MutableDrawStats().skipped_unslotted_indices;
    } else {
      ++vulkan::MutableDrawStats().skipped_index_stride;
    }
    return;
  }

  vulkan::DrawItem item = {};
  float mvp[16] = {};
  if (!BuildMvp(State.fvf, mvp)) {
    ++vulkan::MutableDrawStats().skipped_no_transform;
    return;
  }
  // Noted for the rewrite test in UploadLocked: this draw reads whatever the slot holds at
  // Present, so a lock arriving after it is an aliasing hazard rather than an ordinary update.
  auto note_drawn = [](auto &buffer) {
    if (buffer.drawn_frame_ != TheStats.frames) {
      buffer.drawn_frame_ = TheStats.frames;
      buffer.draws_this_frame_ = 0;
    }
    ++buffer.draws_this_frame_;
  };
  note_drawn(vertex_buffer);
  if (index_buffer != nullptr) {
    note_drawn(*index_buffer);
  }
  // Both offsets are absolute into the one arena, which is what lets the renderer bind the
  // index buffer once for the whole frame and never bind a vertex buffer at all.
  //
  // D3D's rule is `vertex = stream[index + BaseVertexIndex]`, and Slang's SV_VertexID carries
  // the same D3D semantics - the raw index, with the base excluded. So the base is folded in
  // here and `vkCmdDrawIndexed` is given a vertexOffset of 0. Passing it to Vulkan instead
  // would add it twice on any driver where gl_BaseVertex reads back as 0.
  //
  // A buffer refilled after an earlier draw this frame has its later version in the frame's
  // scratch instead of the slot (§4.23), and this draw is one of the later ones - so it reads
  // from there. Both offsets are already absolute within whichever buffer they name.
  //
  // An indexed draw adds D3D's BaseVertexIndex, which SetIndices carried; a non-indexed one adds
  // `StartVertex` instead, because its SV_VertexID counts up from 0 and the shader is what turns
  // that into a slot. They are the same field for the same reason and never both apply.
  const uint32_t vertex_bias = indexed ? base_vertex_ : start_vertex;
  if (vertex_buffer.version_frame_ == TheStats.frames) {
    item.vertex_source = vulkan::DrawSource::Scratch;
    item.base_vertex = vertex_buffer.version_offset_ + vertex_bias;
  } else {
    item.base_vertex =
        vertex_buffer.slot_.offset / static_cast<uint32_t>(sizeof(vulkan::CanonicalVertex)) +
        vertex_bias;
  }
  item.indexed = indexed;
  if (index_buffer != nullptr) {
    if (index_buffer->version_frame_ == TheStats.frames) {
      item.index_source = vulkan::DrawSource::Scratch;
      item.first_index = index_buffer->version_offset_ + start_index;
    } else {
      item.first_index = index_buffer->slot_.offset / 2 + start_index;
    }
  }
  item.count = elements;
  item.vertex_offset = 0;
  ResolveStages(item);
  ResolvePipeline(item, type);
  // Last, so a draw that is going to be skipped for any other reason does not consume a record.
  if (!BuildDrawRecord(item, mvp, TheStats.frames)) {
    ++vulkan::MutableDrawStats().skipped_no_record;
    return;
  }
  vulkan::SubmitDraw(item);
}

// The user-pointer draws: `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP`, which hand D3D their
// vertices inline. 350,000 of them a session on level01 - the text, the particles and the
// in-game menus - and none of them has a buffer, so none has an arena slot.
//
// They go through the frame's scratch instead, which is host-visible and mapped: the data is
// produced on the CPU, is different every frame, and is read once. Converting straight into the
// mapped pointer means no staging copy and no barrier.
//
// The stride comes from the CALL, not from the FVF. A user-pointer draw is entitled to pad its
// vertices, and the two agreeing is an assumption with no reason behind it.
void CaptureDevice::EmitDrawUP(D3DPRIMITIVETYPE type, UINT primitive_count,
                               const void *vertex_data, UINT vertex_stride,
                               const void *index_data, D3DFORMAT index_format,
                               UINT vertex_count) {
  if (!vulkan::DrawReady()) {
    return;
  }
  ++vulkan::MutableDrawStats().seen;
  const uint32_t elements = TopologyEnabled(type) ? ElementCount(type, primitive_count) : 0;
  if (elements == 0) {
    ++vulkan::MutableDrawStats().skipped_topology;
    return;
  }
  if (vertex_data == nullptr || !vulkan::FvfSupported(State.fvf)) {
    ++vulkan::MutableDrawStats().skipped_unconvertible;
    return;
  }

  vulkan::DrawItem item = {};
  float mvp[16] = {};
  if (!BuildMvp(State.fvf, mvp)) {
    ++vulkan::MutableDrawStats().skipped_no_transform;
    return;
  }
  item.vertex_source = vulkan::DrawSource::Scratch;
  item.index_source = vulkan::DrawSource::Scratch;

  // A non-indexed draw names its vertices directly; an indexed one is given a vertex count of
  // its own, because the indices may reach further than the primitives suggest.
  const uint32_t indices = elements;
  const uint32_t vertices = index_data != nullptr ? vertex_count : indices;
  const vulkan::ScratchAlloc vertex_scratch = vulkan::AllocateScratchVertices(vertices);
  if (!vertex_scratch.valid) {
    ++vulkan::MutableDrawStats().skipped_scratch_full;
    return;
  }
  if (!vulkan::ConvertVertices(State.fvf, vertex_data, vertices,
                               static_cast<vulkan::CanonicalVertex *>(vertex_scratch.mapped),
                               vertex_stride)) {
    ++vulkan::MutableDrawStats().skipped_unconvertible;
    return;
  }
  ++FirstVertexColours[(uint64_t(State.fvf) << 32) |
                       static_cast<const vulkan::CanonicalVertex *>(vertex_scratch.mapped)
                           ->color];
  item.base_vertex = vertex_scratch.offset;
  item.count = indices;

  if (index_data == nullptr) {
    item.indexed = false;
    item.count = vertices;
  } else {
    const uint32_t stride = index_format == D3DFMT_INDEX32 ? 4u : 2u;
    const vulkan::ScratchAlloc index_scratch =
        vulkan::AllocateScratchIndices(indices, stride);
    if (!index_scratch.valid) {
      ++vulkan::MutableDrawStats().skipped_scratch_full;
      return;
    }
    std::memcpy(index_scratch.mapped, index_data, size_t(indices) * stride);
    item.first_index = index_scratch.offset;
    item.index_stride = static_cast<uint8_t>(stride);
  }

  ResolveStages(item);
  ResolvePipeline(item, type);
  if (!BuildDrawRecord(item, mvp, TheStats.frames)) {
    ++vulkan::MutableDrawStats().skipped_no_record;
    return;
  }
  vulkan::SubmitDraw(item);
}

// Recorded whether or not the draw is drawn, so the description is available while the
// topologies are switched off - which is the state they are shipped in.
void NoteOddTopology(D3DPRIMITIVETYPE type, bool user_pointer, uint32_t primitives,
                     const void *vertex_data, uint32_t vertex_stride, uint32_t vertex_count) {
  if (type == D3DPT_TRIANGLELIST) {
    return;
  }
  OddTopology odd;
  odd.type = static_cast<uint32_t>(type);
  odd.fvf = State.fvf;
  odd.user_pointer = user_pointer ? 1u : 0u;
  odd.primitives = primitives;
  odd.stages = ActiveStages();
  odd.texture_index = vulkan::kNoTexture;
  IDirect3DBaseTexture8 *const bound = State.textures[0];
  if (bound != nullptr && LiveTextureWrappers.count(bound) != 0) {
    auto &texture = *static_cast<CaptureTexture *>(bound);
    if (texture.image_.valid) {
      odd.texture_index = texture.image_.index;
    }
  }
  odd.blend = State.render_states[D3DRS_ALPHABLENDENABLE];
  odd.depth_test = State.render_states[D3DRS_ZENABLE];
  odd.stencil = State.render_states[D3DRS_STENCILENABLE];
  odd.stencil_func = State.render_states[D3DRS_STENCILFUNC];
  odd.stencil_ref = State.render_states[D3DRS_STENCILREF];
  if (vertex_data != nullptr && vertex_count > 0 && (State.fvf & 0x004u) != 0) {
    const uint32_t stride =
        vertex_stride != 0 ? vertex_stride : vulkan::FvfStride(State.fvf);
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    for (uint32_t i = 0; i < vertex_count && stride != 0; ++i) {
      const auto *p = static_cast<const uint8_t *>(vertex_data) + size_t(i) * stride;
      float x = 0.0f;
      float y = 0.0f;
      std::memcpy(&x, p, sizeof(x));
      std::memcpy(&y, p + 4, sizeof(y));
      min_x = (std::min)(min_x, x);
      min_y = (std::min)(min_y, y);
      max_x = (std::max)(max_x, x);
      max_y = (std::max)(max_y, y);
    }
    odd.x0 = static_cast<int32_t>(min_x);
    odd.y0 = static_cast<int32_t>(min_y);
    odd.x1 = static_cast<int32_t>(max_x);
    odd.y1 = static_cast<int32_t>(max_y);
    const auto *first = static_cast<const uint8_t *>(vertex_data);
    std::memcpy(&odd.first_z, first + 8, sizeof(odd.first_z));
    std::memcpy(&odd.first_rhw, first + 12, sizeof(odd.first_rhw));
    // The first vertex's D3DCOLOR, which for an untextured draw IS the colour it paints.
    if ((State.fvf & 0x040u) != 0) {
      std::memcpy(&odd.first_colour, first + 16, sizeof(odd.first_colour));
    }
  }
  ++OddTopologies[odd];
}

void CountDraw(D3DPRIMITIVETYPE type, bool user_pointer, uint32_t primitives,
               const void *vertex_data = nullptr, uint32_t vertex_stride = 0,
               uint32_t vertex_count = 0) {
  SnapshotDraw();
  NoteOddTopology(type, user_pointer, primitives, vertex_data, vertex_stride, vertex_count);
  ++TheStats.primitive_type_counts[static_cast<uint32_t>(type)];
  ++TheStats.draws_this_frame;
  if (user_pointer) {
    ++TheStats.draws_user_ptr;
  } else {
    ++TheStats.draws_buffered;
  }
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                                        UINT StartVertex,
                                                        UINT PrimitiveCount) {
  CountDraw(PrimitiveType, false, PrimitiveCount);
  EmitDraw(PrimitiveType, 0, PrimitiveCount, false, StartVertex);
  return inner_->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex,
    UINT PrimitiveCount) {
  CountDraw(PrimitiveType, false, PrimitiveCount);
  NoteIndexedRange(PrimitiveType, MinIndex, NumVertices, StartIndex, PrimitiveCount);
  EmitDraw(PrimitiveType, StartIndex, PrimitiveCount, true, 0);
  return inner_->DrawIndexedPrimitive(PrimitiveType, MinIndex, NumVertices, StartIndex,
                                      PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  CountDraw(PrimitiveType, true, PrimitiveCount, pVertexStreamZeroData,
            VertexStreamZeroStride, ElementCount(PrimitiveType, PrimitiveCount));
  EmitDrawUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride,
             nullptr, D3DFMT_INDEX16, 0);
  return inner_->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData,
                                 VertexStreamZeroStride);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices,
    UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  CountDraw(PrimitiveType, true, PrimitiveCount, pVertexStreamZeroData,
            VertexStreamZeroStride, MinVertexIndex + NumVertexIndices);
  // MinVertexIndex + NumVertexIndices is the span the indices reach into, and the vertex
  // pointer is biased so that index 0 is its first vertex - so the whole span is copied and
  // the indices need no rebasing.
  EmitDrawUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride,
             pIndexData, IndexDataFormat, MinVertexIndex + NumVertexIndices);
  return inner_->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices,
                                        PrimitiveCount, pIndexData, IndexDataFormat,
                                        pVertexStreamZeroData, VertexStreamZeroStride);
}

// ---------------------------------------------------------------------------------------

using Direct3DCreate8Fn = IDirect3D8 *(WINAPI *)(UINT);
Direct3DCreate8Fn OriginalDirect3DCreate8 = ::Direct3DCreate8;

// `GKPLUS_RENDERER=d3d8`: forward to Windows' own D3D8 instead of to d3d8to9.
//
// **This is the ground truth this project did not have.** Every comparison up to §4.32 was
// against d3d8to9, which §4.28 and §4.29 had already caught being wrong twice - so a defect the
// translation layer shares with this renderer measures as a perfect match. That is not a
// hypothetical: the junk-pile decal on level02 matches d3d9 to 0.1 mean RGB and does not match
// the real thing (§4.33).
//
// Windows 10 still ships a 32-bit `d3d8.dll` in SysWOW64, so the original runtime is one
// LoadLibrary away.
//
// **It must be loaded by full system path.** GkPlus *is* `d3d8.dll`, sitting next to gl.exe, so
// a bare `LoadLibraryA("d3d8.dll")` resolves to the module already loaded - this one - and
// `Direct3DCreate8` would recurse into itself. `GetSystemDirectoryA` in a 32-bit process on
// 64-bit Windows returns SysWOW64, which is where the 32-bit copy lives, so no redirection
// dance is needed.
Direct3DCreate8Fn LoadSystemDirect3DCreate8() {
  char path[MAX_PATH] = {};
  const UINT length = ::GetSystemDirectoryA(path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH - 12) {
    DebugWrite("gkplus: d3d8 passthrough: no system directory\n");
    return nullptr;
  }
  std::string full = std::string(path, length) + "\\d3d8.dll";
  const HMODULE module = ::LoadLibraryA(full.c_str());
  if (module == nullptr) {
    DebugWrite("gkplus: d3d8 passthrough: could not load " + full + "\n");
    return nullptr;
  }
  auto create = reinterpret_cast<Direct3DCreate8Fn>(
      ::GetProcAddress(module, "Direct3DCreate8"));
  if (create == nullptr) {
    DebugWrite("gkplus: d3d8 passthrough: " + full + " exports no Direct3DCreate8\n");
    return nullptr;
  }
  DebugWrite("gkplus: rendering through " + full + " - the original D3D8\n");
  return create;
}

// Which implementation `Direct3DCreate8` hands the capture layer. Resolved once, on the first
// call, because reading the environment during DllMain is fine but LoadLibrary there is not -
// this runs from the game's own call to Direct3DCreate8, long after the loader lock is gone.
bool SystemD3D8 = false;

Direct3DCreate8Fn ResolveInnerCreate() {
  static Direct3DCreate8Fn resolved = nullptr;
  static bool tried = false;
  if (tried) {
    return resolved;
  }
  tried = true;
  char value[16] = {};
  const DWORD length = ::GetEnvironmentVariableA("GKPLUS_RENDERER", value, sizeof(value));
  if (length > 0 && std::string(value, length) == "d3d8") {
    resolved = LoadSystemDirect3DCreate8();
    SystemD3D8 = resolved != nullptr;
  }
  // Anything else - and a system D3D8 that would not load - keeps d3d8to9, which is what every
  // other mode is built on. A missing original is worth a log line and not worth refusing to
  // start over.
  if (resolved == nullptr) {
    resolved = OriginalDirect3DCreate8;
  }
  return resolved;
}

IDirect3D8 *WINAPI HookedDirect3DCreate8(UINT SDKVersion) {
  IDirect3D8 *const inner = ResolveInnerCreate()(SDKVersion);
  if (!inner) {
    return nullptr;
  }
  return new CaptureD3D8(inner);
}

} // namespace

const CaptureStats &Stats() { return TheStats; }

std::string ArmProbeQuad(const std::string &name, double scale, bool mipmap, double offset,
                         bool alpha) {
  ProbeTexture = nullptr;
  if (name.empty()) {
    return "probe disarmed";
  }
  std::string want = name;
  for (char &c : want) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const void *live : LiveTextureWrappers) {
    auto *const texture =
        static_cast<CaptureTexture *>(const_cast<void *>(live));
    std::string path = texture->rim_path_;
    for (char &c : path) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (path.find(want) != std::string::npos && texture->width_ > 0) {
      ProbeTexture = texture;
      break;
    }
  }
  if (ProbeTexture == nullptr) {
    return "no live texture matches \"" + name + "\"";
  }
  ProbeScale = static_cast<float>(scale);
  ProbeMipFilter = mipmap ? D3DTEXF_LINEAR : D3DTEXF_NONE;
  ProbeOffset = static_cast<float>(offset);
  ProbeAlpha = alpha;
  char line[256];
  std::snprintf(line, sizeof(line),
                "probing %s (%ux%u, %u levels) at scale %.4f -> %.0fx%.0f px at (%.2f,%.2f), "
                "mip filter %s, showing %s",
                ProbeTexture->rim_path_.c_str(), ProbeTexture->width_, ProbeTexture->height_,
                ProbeTexture->levels_, ProbeScale,
                ProbeTexture->width_ * ProbeScale, ProbeTexture->height_ * ProbeScale,
                16.0f + ProbeOffset, 16.0f + ProbeOffset, mipmap ? "LINEAR" : "NONE",
                alpha ? "ALPHA" : "colour");
  return line;
}


void SetTopologies(bool strips, bool lines) {
  DrawStrips = strips;
  DrawLines = lines;
  SkipTopologies = !strips && !lines;
}

void GetTopologies(bool &strips, bool &lines) {
  strips = DrawStrips;
  lines = DrawLines;
}

void SetLightSum(bool enabled) { DrawLightSum = enabled; }

bool GetLightSum() { return DrawLightSum; }

bool DeviceCreated() { return HaveDevice; }

void ResetStats() {
  // The live counters describe the device, not the sample, and buffers outlive a reset.
  // Zeroing them would make every subsequent destructor decrement from 0 and wrap, since
  // they are unsigned - so they are carried across, and only the peaks restart.
  const uint64_t live_vb = TheStats.live_vertex_buffers;
  const uint64_t live_ib = TheStats.live_index_buffers;
  const uint64_t live_vb_bytes = TheStats.live_vertex_bytes;
  const uint64_t live_ib_bytes = TheStats.live_index_bytes;

  TheStats = CaptureStats();

  TheStats.live_vertex_buffers = live_vb;
  TheStats.live_index_buffers = live_ib;
  TheStats.live_vertex_bytes = live_vb_bytes;
  TheStats.live_index_bytes = live_ib_bytes;
  TheStats.peak_live_vertex_bytes = live_vb_bytes;
  TheStats.peak_live_index_bytes = live_ib_bytes;
  TheStats.peak_live_buffers = live_vb + live_ib;

  MaterialKeys.clear();
  PipelineKeys.clear();
  OutOfRangeDraws.clear();
  MaterialKeysThisFrame.clear();
  StageConfigs.clear();
  PipelineConfigs.clear();
  UnslottedVertexBuffers.clear();
  FirstVertexColours.clear();
  LightingByFvf.clear();
  OddTopologies.clear();
  // Blocks are deliberately NOT cleared: they belong to the device, not to the sample, and
  // dropping them would make every later ApplyStateBlock look opaque.
}

// Which dword of a cache record holds the texture wrapper, measured rather than assumed.
//
// `AwTexture+0x00` is what rendering_notes.md derives from AwMaterial_ApplyStage, and taking
// that on trust named 5 textures out of 53 while 58 of 59 records had *something* stored. This
// scans every offset in the record against the set of live wrappers and reports the histogram,
// which says where the join really is - and whether there is more than one.
std::string RimJoinHistogram() {
  std::map<uint32_t, uint32_t> hits;
  for (AwTexture *const record : RimRecords) {
    if (record == nullptr) {
      continue;
    }
    const auto *words = reinterpret_cast<void *const *>(record);
    for (uint32_t offset = 0; offset < sizeof(AwTexture); offset += 4) {
      if (LiveTextureWrappers.count(words[offset / 4]) != 0) {
        ++hits[offset];
      }
    }
  }
  std::string out = "rim record offsets holding a live texture wrapper:";
  char line[64];
  for (const auto &[offset, count] : hits) {
    std::snprintf(line, sizeof(line), " +0x%02x=%u", offset, count);
    out += line;
  }
  return hits.empty() ? out + " none" : out;
}

std::string VerifyTextureImages() {
  // Or every blit still queued reads back as a stale texture, and the check reports its own
  // impatience as a mismatch.
  vulkan::FlushUploads();
  uint32_t checked = 0;
  uint32_t matched = 0;
  std::string first_mismatch;
  for (const void *pointer : LiveTextureWrappers) {
    auto *const texture =
        static_cast<CaptureTexture *>(const_cast<void *>(pointer));
    if (!texture->image_.valid) {
      continue;
    }
    for (uint32_t level = 0; level < texture->levels_; ++level) {
      D3DLOCKED_RECT locked = {};
      if (!ReadTextureLevel(*texture, level, locked)) {
        continue;
      }
      ++checked;
      uint64_t differing = 0;
      uint64_t first = 0;
      uint64_t total = 0;
      if (vulkan::VerifyImageLevel(texture->image_, level, locked.pBits,
                                   static_cast<uint32_t>(locked.Pitch), &differing, &first,
                                   &total)) {
        ++matched;
      } else if (first_mismatch.empty()) {
        // A one-bit experiment that separates the two things a mismatch can mean. Re-upload
        // this very level from this very data and check again: if it now matches, the upload
        // path is correct and something wrote the texture without this layer seeing it. If it
        // still does not, the conversion or the copy itself is wrong.
        vulkan::UploadIntoTextureImage(texture->image_, level, 0, 0,
                                       (std::max)(1u, texture->width_ >> level),
                                       (std::max)(1u, texture->height_ >> level),
                                       locked.pBits,
                                       static_cast<uint32_t>(locked.Pitch));
        vulkan::FlushUploads();
        const bool fixed_by_reupload = vulkan::VerifyImageLevel(
            texture->image_, level, locked.pBits, static_cast<uint32_t>(locked.Pitch));
        char line[352];
        std::snprintf(line, sizeof(line),
                      "   first mismatch: image %u level %u, %ux%u format %u, pool %u, "
                      "%llu/%llu bytes differ from offset %llu; "
                      "game locks %llu, blits in %llu (levels 0x%x); usage 0x%x, levels %u; "
                      "re-upload fixes it: %s",
                      texture->image_.index, level, texture->width_, texture->height_,
                      texture->format_, texture->pool_, (unsigned long long)differing,
                      (unsigned long long)total, (unsigned long long)first,
                      (unsigned long long)texture->own_locks_,
                      (unsigned long long)texture->blits_in_, texture->levels_blitted_,
                      texture->usage_, texture->levels_,
                      fixed_by_reupload ? "yes" : "no");
        first_mismatch = line;
      }
      texture->inner_->UnlockRect(level);
    }
  }
  char out[128];
  std::snprintf(out, sizeof(out), "%u/%u levels match", matched, checked);
  return std::string(out) + (first_mismatch.empty() ? "" : "\n" + first_mismatch);
}

// The buffer half of VerifyTextureImages: every live buffer that owns an arena slot is read
// back off the GPU and compared against its own current contents.
//
// It is worth the code for the same reason the texture check was. A draw addresses the arenas
// by offset, so a slot holding the wrong bytes does not fail anywhere a counter can see - the
// vertices are somewhere, the indices are somewhere, and the picture is merely wrong. A bad
// index is the loudest form of it: one triangle reaching a vertex from an unrelated mesh
// megabytes away, which draws as a wedge across half the screen.
std::string VerifyBufferSlots() {
  vulkan::FlushUploads();
  uint32_t checked = 0;
  uint32_t matched = 0;
  std::string first_mismatch;

  uint32_t reported = 0;
  auto report = [&](const char *what, uint32_t index, uint32_t bytes, uint64_t differing,
                    uint64_t first, uint32_t offset, uint32_t fvf, uint64_t unlocks) {
    if (++reported > 8) {
      return;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "\n   %s %u, %u bytes at arena offset %u, fvf 0x%03x, "
                  "%llu unlocks; %llu bytes differ from %llu",
                  what, index, bytes, offset, fvf, (unsigned long long)unlocks,
                  (unsigned long long)differing, (unsigned long long)first);
    first_mismatch += line;
  };

  // Do the live slots overlap each other? An arena that hands the same bytes to two buffers
  // corrupts both, and it would look exactly like a lost upload from the outside.
  std::vector<std::pair<uint64_t, uint64_t>> spans[2];
  auto note_span = [&](const vulkan::BufferSlot &slot) {
    if (slot.valid) {
      spans[slot.vertex ? 0 : 1].push_back({slot.offset, uint64_t(slot.offset) + slot.bytes});
    }
  };
  for (const void *p : LiveVertexWrappers) {
    note_span(static_cast<const CaptureVertexBuffer *>(p)->slot_);
  }
  for (const void *p : LiveIndexWrappers) {
    note_span(static_cast<const CaptureIndexBuffer *>(p)->slot_);
  }
  uint32_t overlaps = 0;
  for (auto &list : spans) {
    std::sort(list.begin(), list.end());
    for (size_t i = 1; i < list.size(); ++i) {
      if (list[i].first < list[i - 1].second) {
        ++overlaps;
      }
    }
  }

  uint32_t which = 0;
  for (const void *pointer : LiveVertexWrappers) {
    auto &buffer = *static_cast<CaptureVertexBuffer *>(const_cast<void *>(pointer));
    ++which;
    const uint32_t stride = vulkan::FvfStride(buffer.fvf_);
    if (!buffer.slot_.valid || stride == 0) {
      continue;
    }
    const uint32_t count = buffer.length_ / stride;
    BYTE *data = nullptr;
    if (count == 0 || FAILED(buffer.LockForRead(&data)) || data == nullptr) {
      continue;
    }
    std::vector<vulkan::CanonicalVertex> expected(count);
    if (vulkan::ConvertVertices(buffer.fvf_, data, count, expected.data())) {
      ++checked;
      uint64_t differing = 0;
      uint64_t first = 0;
      uint8_t got[32] = {};
      const uint32_t bytes = count * static_cast<uint32_t>(sizeof(vulkan::CanonicalVertex));
      if (vulkan::VerifySlot(buffer.slot_, expected.data(), bytes, &differing, &first, got)) {
        ++matched;
      } else {
        report("vertex buffer", which, bytes, differing, first, buffer.slot_.offset,
               buffer.fvf_, buffer.unlocks_);
        {
          const auto *want = reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(expected.data()) + first);
          const auto *have = reinterpret_cast<const float *>(got);
          char line[192];
          std::snprintf(line, sizeof(line),
                        "\n      want %.3f %.3f %.3f %.3f   arena holds %.3f %.3f %.3f %.3f",
                        want[0], want[1], want[2], want[3], have[0], have[1], have[2],
                        have[3]);
          first_mismatch += line;
        }
        // The same one-bit experiment VerifyTextureImages uses: send this very data again and
        // look once more. Fixed by a re-upload means the conversion and the copy are right and
        // something overwrote the slot afterwards; still wrong means the path itself is.
        if (reported == 1) {
          vulkan::UploadIntoSlot(buffer.slot_, 0, expected.data(), bytes);
          vulkan::FlushUploads();
          first_mismatch +=
              vulkan::VerifySlot(buffer.slot_, expected.data(), bytes)
                  ? "   (a re-upload fixes it: something overwrote the slot)"
                  : "   (a re-upload does NOT fix it: the upload path itself is wrong)";
        }
      }
    }
    buffer.UnlockAfterRead();
  }

  which = 0;
  for (const void *pointer : LiveIndexWrappers) {
    auto &buffer = *static_cast<CaptureIndexBuffer *>(const_cast<void *>(pointer));
    ++which;
    if (!buffer.slot_.valid || buffer.length_ == 0) {
      continue;
    }
    BYTE *data = nullptr;
    if (FAILED(buffer.LockForRead(&data)) || data == nullptr) {
      continue;
    }
    ++checked;
    uint64_t differing = 0;
    uint64_t first = 0;
    if (vulkan::VerifySlot(buffer.slot_, data, buffer.length_, &differing, &first)) {
      ++matched;
    } else {
      report("index buffer", which, buffer.length_, differing, first, buffer.slot_.offset, 0,
             buffer.unlocks_);
    }
    buffer.UnlockAfterRead();
  }

  char out[96];
  std::snprintf(out, sizeof(out), "%u/%u buffers match, %u overlapping live slots (must be 0)",
                matched, checked, overlaps);
  return std::string(out) + first_mismatch;
}

bool PassthroughToSystemD3D8() { return SystemD3D8; }

IDirect3DDevice9 *ResolveD3D9Device(IDirect3DDevice8 *device) {
  if (!device) {
    return nullptr;
  }
  // Under GKPLUS_RENDERER=d3d8 there is no D3D9 device to resolve: `inner_` is a genuine
  // IDirect3DDevice8 from Windows' own runtime, and `GetProxyInterface()` on it would read a
  // field d3d8to9's class has and a real device does not. That is not a hypothetical - it
  // crashed on the first launch of the passthrough mode, inside ImGui_ImplDX9_Init (§4.33).
  if (SystemD3D8) {
    return nullptr;
  }
  // The game's global holds whatever CreateDevice handed back. With the capture layer
  // installed that is a CaptureDevice; without it, d3d8to9's own Direct3DDevice8. Both
  // shapes have to work, because the capture layer is selectable at runtime.
  if (auto *const captured = dynamic_cast<CaptureDevice *>(device)) {
    return static_cast<Direct3DDevice8 *>(captured->inner_)->GetProxyInterface();
  }
  return static_cast<Direct3DDevice8 *>(device)->GetProxyInterface();
}

std::string FormatStats() {
  const CaptureStats &s = TheStats;
  std::string out;
  char line[256];

  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };

  add("device created: %s\n", HaveDevice ? "yes" : "no");
  add("frames: %llu   draws: %llu buffered + %llu user-pointer   peak/frame: %llu\n",
      (unsigned long long)s.frames, (unsigned long long)s.draws_buffered,
      (unsigned long long)s.draws_user_ptr,
      (unsigned long long)s.max_draws_per_frame);
  add("resources: %llu textures, %llu VB (%llu bytes), %llu IB (%llu bytes)\n",
      (unsigned long long)s.textures_created,
      (unsigned long long)s.vertex_buffers_created, (unsigned long long)s.vertex_bytes,
      (unsigned long long)s.index_buffers_created, (unsigned long long)s.index_bytes);
  add("max texture stage: %u   max light index: %u\n", s.max_stage_used,
      s.max_light_index);
  add("state blocks: %llu recorded (%llu states, max %llu), %llu opaque, %llu applies\n",
      (unsigned long long)s.blocks_recorded, (unsigned long long)s.block_states_total,
      (unsigned long long)s.max_block_states, (unsigned long long)s.blocks_opaque,
      (unsigned long long)s.block_applies);
  add("materials: %llu distinct (peak %llu/frame)   pipelines: %llu   active stages: %llu\n",
      (unsigned long long)s.distinct_materials,
      (unsigned long long)s.max_materials_per_frame,
      (unsigned long long)s.distinct_pipelines,
      (unsigned long long)s.max_active_stages);
  add("applies of an unwitnessed block: %llu (must be 0, or the shadow state is wrong)\n",
      (unsigned long long)s.opaque_block_applies);
  add("LIVE buffers: %llu VB (%llu KB) + %llu IB (%llu KB)   peak %llu buffers, "
      "%llu KB vtx + %llu KB idx\n",
      (unsigned long long)s.live_vertex_buffers,
      (unsigned long long)(s.live_vertex_bytes >> 10),
      (unsigned long long)s.live_index_buffers,
      (unsigned long long)(s.live_index_bytes >> 10),
      (unsigned long long)s.peak_live_buffers,
      (unsigned long long)(s.peak_live_vertex_bytes >> 10),
      (unsigned long long)(s.peak_live_index_bytes >> 10));
  add("locks: %llu   peak locked bytes per frame: %llu KB\n", (unsigned long long)s.locks,
      (unsigned long long)(s.max_locked_bytes_per_frame >> 10));
  add("textures live: %llu   LockRect: %llu   GetSurfaceLevel: %llu\n",
      (unsigned long long)s.live_textures, (unsigned long long)s.texture_lock_rects,
      (unsigned long long)s.texture_surface_levels);
  add("surfaces live: %llu   LockRect: %llu (%llu on a texture level)\n",
      (unsigned long long)s.live_surfaces, (unsigned long long)s.surface_lock_rects,
      (unsigned long long)s.surface_texture_lock_rects);
  add("pixel routes past texture LockRect: %llu surface locks + %llu render targets"
      "   (both must be 0)\n",
      (unsigned long long)s.surface_texture_lock_rects,
      (unsigned long long)s.texture_render_targets);
  add("CopyRects into a texture: %llu   untracked source: %llu (must be 0)   sub-rect: %llu"
      "   source read failures: %llu (must be 0)\n",
      (unsigned long long)s.surface_copy_rects,
      (unsigned long long)s.copy_rects_untracked,
      (unsigned long long)s.copy_rects_partial,
      (unsigned long long)s.texture_read_failures);
  out += RimJoinHistogram() + "\n";
  add("images seeded: %llu (%llu named from the rim cache)   UpdateTexture: %llu   "
      "resource GetDevice: %llu\n",
      (unsigned long long)s.images_seeded, (unsigned long long)s.textures_named,
      (unsigned long long)s.texture_updates,
      (unsigned long long)s.resource_get_devices);
  add("foreign buffers: %llu   unconvertible FVFs: %llu   failed uploads: %llu"
      "   (all must be 0)\n",
      (unsigned long long)s.foreign_buffers,
      (unsigned long long)s.unconvertible_buffers,
      (unsigned long long)s.failed_uploads);
  add("buffers rewritten after being drawn this frame: %llu (%llu vertex, %llu index;"
      " %llu draws affected)\n",
      (unsigned long long)s.buffer_rewritten_after_draw,
      (unsigned long long)s.vertex_buffer_rewritten_after_draw,
      (unsigned long long)s.index_buffer_rewritten_after_draw,
      (unsigned long long)s.draws_reading_rewritten_buffers);
  add("  ... overlapping the drawn-from range: %llu   versioned into the scratch: %llu"
      "   NOT versioned: %llu (must be 0)\n",
      (unsigned long long)s.overlapping_rewrites_after_draw,
      (unsigned long long)s.buffer_versions_in_scratch,
      (unsigned long long)s.unversioned_rewrites);
  for (const auto &entry : RewriteLocks) {
    add("  lock flags 0x%04x %-10s %llu\n", (unsigned)(entry.first >> 1),
        (entry.first & 1) ? "overlapping" : "disjoint", (unsigned long long)entry.second);
  }

  out += "FVF / vertex shader handles:\n";
  for (const auto &[fvf, count] : s.fvf_counts) {
    add("  0x%08x  %llu\n", fvf, (unsigned long long)count);
  }

  out += "primitive types:\n";
  for (const auto &[type, count] : s.primitive_type_counts) {
    add("  %u  %llu\n", type, (unsigned long long)count);
  }

  out += "texture formats:\n";
  for (const auto &[format, count] : s.texture_formats) {
    add("  %u  %llu\n", format, (unsigned long long)count);
  }

  out += "texture pools (0 default, 1 managed, 2 systemmem, 3 scratch):\n";
  for (const auto &[pool, count] : s.texture_pools) {
    add("  %u  %llu\n", pool, (unsigned long long)count);
  }

  add("render states used: %u   (state: distinct values)\n",
      (unsigned)s.render_states.size());
  for (const auto &[state, values] : s.render_states) {
    add("  %3u: %u\n", state, (unsigned)values.size());
  }

  add("texture stage states used: %u   (stage.type: distinct values)\n",
      (unsigned)s.stage_states.size());
  for (const auto &[key, values] : s.stage_states) {
    add("  %u.%-3u: %u\n", key >> 16, key & 0xffff, (unsigned)values.size());
  }

  out += "transform states:";
  for (const uint32_t state : s.transform_states) {
    add(" %u", state);
  }
  out += "\n";
  return out;
}

std::string FormatShadowState() {
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };

  // A float render state is a float's bits in a DWORD, so it has to be bit-cast rather than
  // converted - reading D3DRS_FOGSTART as an integer gives 1065353216 for 1.0.
  auto as_float = [](uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };

  // Every state printed below, with the name it is printed under and whether it is a float.
  // The history is taken from the same table, so the two halves cannot drift apart.
  struct Entry {
    uint32_t state;
    const char *name;
    bool is_float;
  };
  static const Entry kEntries[] = {
      {D3DRS_FOGENABLE, "FOGENABLE", false},
      {D3DRS_FOGCOLOR, "FOGCOLOR", false},
      {D3DRS_FOGTABLEMODE, "FOGTABLEMODE", false},
      {D3DRS_FOGVERTEXMODE, "FOGVERTEXMODE", false},
      {D3DRS_FOGSTART, "FOGSTART", true},
      {D3DRS_FOGEND, "FOGEND", true},
      {D3DRS_FOGDENSITY, "FOGDENSITY", true},
      {D3DRS_RANGEFOGENABLE, "RANGEFOGENABLE", false},
      {D3DRS_LIGHTING, "LIGHTING", false},
      {D3DRS_AMBIENT, "AMBIENT", false},
      {D3DRS_SPECULARENABLE, "SPECULARENABLE", false},
      {D3DRS_COLORVERTEX, "COLORVERTEX", false},
      {D3DRS_NORMALIZENORMALS, "NORMALIZENORMALS", false},
      {D3DRS_LOCALVIEWER, "LOCALVIEWER", false},
      {D3DRS_DIFFUSEMATERIALSOURCE, "DIFFUSEMATERIALSOURCE", false},
      {D3DRS_SPECULARMATERIALSOURCE, "SPECULARMATERIALSOURCE", false},
      {D3DRS_AMBIENTMATERIALSOURCE, "AMBIENTMATERIALSOURCE", false},
      {D3DRS_EMISSIVEMATERIALSOURCE, "EMISSIVEMATERIALSOURCE", false},
      // The two colours a draw can carry that the canonical vertex and the shader do not: the
      // per-vertex specular, added after texturing whenever SPECULARENABLE is on, and the
      // texture factor, which a stage names as D3DTA_TFACTOR.
      {D3DRS_TEXTUREFACTOR, "TEXTUREFACTOR", false},
      {D3DRS_COLORWRITEENABLE, "COLORWRITEENABLE", false},
      // The pipeline half - what a VkPipeline would have to be bucketed by.
      {D3DRS_ALPHATESTENABLE, "ALPHATESTENABLE", false},
      {D3DRS_ALPHAREF, "ALPHAREF", false},
      {D3DRS_ALPHAFUNC, "ALPHAFUNC", false},
      {D3DRS_ALPHABLENDENABLE, "ALPHABLENDENABLE", false},
      {D3DRS_SRCBLEND, "SRCBLEND", false},
      {D3DRS_DESTBLEND, "DESTBLEND", false},
      {D3DRS_ZENABLE, "ZENABLE", false},
      {D3DRS_ZWRITEENABLE, "ZWRITEENABLE", false},
      {D3DRS_ZFUNC, "ZFUNC", false},
      {D3DRS_CULLMODE, "CULLMODE", false},
      // Stencil. This renderer has no stencil buffer at all, so if the game uses one, every
      // draw it masks is drawn here unmasked - which is a whole-screen effect for anything
      // shaped like a stencil shadow.
      {D3DRS_STENCILENABLE, "STENCILENABLE", false},
      {D3DRS_STENCILFAIL, "STENCILFAIL", false},
      {D3DRS_STENCILZFAIL, "STENCILZFAIL", false},
      {D3DRS_STENCILPASS, "STENCILPASS", false},
      {D3DRS_STENCILFUNC, "STENCILFUNC", false},
      {D3DRS_STENCILREF, "STENCILREF", false},
      {D3DRS_STENCILMASK, "STENCILMASK", false},
      {D3DRS_STENCILWRITEMASK, "STENCILWRITEMASK", false},
      // The states D3D9 does not have, which is to say the ones d3d8to9 has to invent a
      // translation for - and therefore the ones on which the A/B reference can disagree with
      // both this renderer and the real D3D8 (§4.29). ZVISIBLE, LINEPATTERN and PATCHSEGMENTS it
      // drops on the floor; EDGEANTIALIAS becomes ANTIALIASEDLINEENABLE; SOFTWAREVERTEXPROCESSING
      // becomes SetSoftwareVertexProcessing, but only on a mixed-mode device; and ZBIAS becomes
      // DEPTHBIAS scaled by -0.000005. Printed because "does the game set this at all" is the
      // first question about every one of them.
      {D3DRS_ZBIAS, "ZBIAS", false},
      {D3DRS_SOFTWAREVERTEXPROCESSING, "SOFTWAREVERTEXPROC", false},
      {D3DRS_EDGEANTIALIAS, "EDGEANTIALIAS", false},
      {D3DRS_ZVISIBLE, "ZVISIBLE", false},
      {D3DRS_LINEPATTERN, "LINEPATTERN", false},
      {D3DRS_CLIPPING, "CLIPPING", false},
      {D3DRS_SHADEMODE, "SHADEMODE", false},
      {D3DRS_FILLMODE, "FILLMODE", false},
  };

  out += "render states the renderer has to reproduce (now / every value ever set):\n";
  for (const Entry &entry : kEntries) {
    const uint32_t current = State.render_states[entry.state];
    if (entry.is_float) {
      add("  %-22s %10.3f  |", entry.name, as_float(current));
    } else {
      add("  %-22s 0x%08x  |", entry.name, current);
    }
    const auto found = TheStats.render_states.find(entry.state);
    if (found == TheStats.render_states.end()) {
      out += " never set";
    } else {
      for (const uint32_t value : found->second) {
        if (entry.is_float) {
          add(" %.3f", as_float(value));
        } else {
          add(" 0x%x", value);
        }
      }
    }
    out += "\n";
  }

  add("viewport: %ux%u  depth range %.4f..%.4f   distinct ranges ever set: %u%s\n",
      State.viewport_width, State.viewport_height, State.viewport_min_z, State.viewport_max_z,
      (unsigned)ViewportDepthRanges.size(),
      ViewportDepthRanges.size() > 1 || State.viewport_min_z != 0.0f ||
              State.viewport_max_z != 1.0f
          ? "   <== the renderer hardcodes 0..1"
          : "");
  for (const uint64_t range : ViewportDepthRanges) {
    float min_z = 0.0f, max_z = 0.0f;
    const uint32_t min_bits = uint32_t(range >> 32), max_bits = uint32_t(range);
    std::memcpy(&min_z, &min_bits, sizeof(min_z));
    std::memcpy(&max_z, &max_bits, sizeof(max_z));
    add("    %.4f .. %.4f\n", min_z, max_z);
  }
  add("draws with fog on: %llu   with lighting on: %llu   of %llu\n",
      (unsigned long long)TheStats.draws_fogged, (unsigned long long)TheStats.draws_lit,
      (unsigned long long)(TheStats.draws_buffered + TheStats.draws_user_ptr));
  add("SetLight: %llu   LightEnable: %llu   SetMaterial: %llu\n",
      (unsigned long long)LightSets, (unsigned long long)LightEnables,
      (unsigned long long)MaterialSets);

  for (uint32_t i = 0; i < kLights; ++i) {
    if (!State.light_set[i] && !State.light_enabled[i]) {
      continue;
    }
    const D3DLIGHT8 &light = State.lights[i];
    add("  light %u: %s type %u  diffuse %.2f %.2f %.2f  ambient %.2f %.2f %.2f  "
        "specular %.2f %.2f %.2f\n",
        i, State.light_enabled[i] ? "ON " : "off", (unsigned)light.Type, light.Diffuse.r,
        light.Diffuse.g, light.Diffuse.b, light.Ambient.r, light.Ambient.g, light.Ambient.b,
        light.Specular.r, light.Specular.g, light.Specular.b);
    add("           direction %.3f %.3f %.3f  position %.1f %.1f %.1f  range %.1f  "
        "atten %.4f %.4f %.4f  theta %.2f phi %.2f falloff %.2f\n",
        light.Direction.x, light.Direction.y, light.Direction.z, light.Position.x,
        light.Position.y, light.Position.z, light.Range, light.Attenuation0,
        light.Attenuation1, light.Attenuation2, light.Theta, light.Phi, light.Falloff);
  }

  // The texture stages as the last draw of the frame left them. `ActiveStages` stops at the
  // first disabled COLOROP, so a stage printed past that one is configured but not in use.
  add("texture stages (%u active), FVF 0x%03x:\n", ActiveStages(), State.fvf);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t *s = State.stage_states[i];
    add("  %u: colorop %2u(%2u,%2u)  alphaop %2u(%2u,%2u)  texcoord %u  texture %p\n", i,
        s[D3DTSS_COLOROP], s[D3DTSS_COLORARG1], s[D3DTSS_COLORARG2], s[D3DTSS_ALPHAOP],
        s[D3DTSS_ALPHAARG1], s[D3DTSS_ALPHAARG2], s[D3DTSS_TEXCOORDINDEX],
        (void *)State.textures[i]);
  }

  // The sampler states, per stage, as they stand and as the game has ever set them **by a
  // direct SetTextureStageState call**. Gunlok configures its samplers inside state blocks, and
  // ApplyOp writes the shadow state without going through the recorder - so all seven read
  // "never set" here while the live values are whatever the blocks put there. The live column
  // is the one to read; "never set" only says a value was never set the direct way, which is
  // why the stage-configuration histogram below carries the filters per draw as well (§4.28).
  {
    static const std::pair<uint32_t, const char *> kSamplerStates[] = {
        {D3DTSS_MAGFILTER, "MAGFILTER"}, {D3DTSS_MINFILTER, "MINFILTER"},
        {D3DTSS_MIPFILTER, "MIPFILTER"}, {D3DTSS_ADDRESSU, "ADDRESSU"},
        {D3DTSS_ADDRESSV, "ADDRESSV"},   {D3DTSS_MIPMAPLODBIAS, "MIPMAPLODBIAS"},
        {D3DTSS_MAXMIPLEVEL, "MAXMIPLEVEL"}};
    out += "sampler stage states (now / ever set by a direct call - blocks bypass the "
           "recorder):\n";
    for (const auto &[state, name] : kSamplerStates) {
      add("  %-14s", name);
      for (uint32_t stage = 0; stage < 2; ++stage) {
        add(" s%u=%u", stage, State.stage_states[stage][state]);
      }
      out += "  |";
      bool any = false;
      for (uint32_t stage = 0; stage < kStages; ++stage) {
        const auto found = TheStats.stage_states.find((stage << 16) | state);
        if (found == TheStats.stage_states.end()) {
          continue;
        }
        any = true;
        add(" s%u:", stage);
        for (const uint32_t value : found->second) {
          add(" %u", value);
        }
      }
      out += any ? "\n" : " never set\n";
    }
  }

  // Every configuration actually drawn with, most used first. Ops are D3DTEXTUREOP (1
  // DISABLE, 2 SELECTARG1, 3 SELECTARG2, 4 MODULATE, 5 MODULATE2X, 7 ADD, ...); args are
  // D3DTA (0 DIFFUSE, 1 CURRENT, 2 TEXTURE, 3 TFACTOR, 4 SPECULAR, |0x10 COMPLEMENT,
  // |0x20 ALPHAREPLICATE).
  {
    std::vector<std::pair<uint64_t, const StageConfig *>> ordered;
    ordered.reserve(StageConfigs.size());
    for (const auto &[config, count] : StageConfigs) {
      ordered.emplace_back(count, &config);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    add("stage configurations drawn with: %u\n", (unsigned)ordered.size());
    for (const auto &[count, config] : ordered) {
      add("  %10llu draws  fvf 0x%03x  %u stage%s", (unsigned long long)count, config->fvf,
          config->stages, config->stages == 1 ? " " : "s");
      for (uint32_t i = 0; i < config->stages && i < 2; ++i) {
        const uint32_t *s = config->stage[i];
        // filter is mag/min/mip as D3DTEXTUREFILTERTYPE (0 NONE, 1 POINT, 2 LINEAR) and addr is
        // u/v as D3DTEXTUREADDRESS (1 WRAP, 2 MIRROR, 3 CLAMP). mip 0 means no mipmapping at
        // all, which is the D3D8 default and not a filter this renderer gets to choose.
        add("  | %u: c %2u(%2u,%2u) a %2u(%2u,%2u) uv%u %s filt %u%u%u addr %u%u", i, s[0], s[1],
            s[2], s[3], s[4], s[5], s[6], config->textured[i] != 0 ? "tex" : "---", s[7], s[8],
            s[9], s[10], s[11]);
      }
      out += "\n";
    }
  }

  // Blend factors are D3DBLEND (1 ZERO, 2 ONE, 5 SRCALPHA, 6 INVSRCALPHA, ...); the compare
  // functions are D3DCMPFUNC (1 NEVER, 2 LESS, ... 8 ALWAYS); cull is D3DCULL (1 NONE, 2 CW,
  // 3 CCW).
  {
    std::vector<std::pair<uint64_t, const PipelineConfig *>> ordered;
    ordered.reserve(PipelineConfigs.size());
    for (const auto &[config, count] : PipelineConfigs) {
      ordered.emplace_back(count, &config);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    add("pipeline configurations drawn with: %u\n", (unsigned)ordered.size());
    out += "                          fvf atest ref func  blend src dst   z zwrite cull cwrite"
           "   sten func pass zfail shade\n";
    for (const auto &[count, config] : ordered) {
      add("  %10llu draws  0x%03x %5u %3u %4u %6u %3u %3u %3u %6u %4u %5u %6u %4u %4u %5u %5u\n",
          (unsigned long long)count, config->fvf, config->state[0], config->state[1],
          config->state[2], config->state[3], config->state[4], config->state[5],
          config->state[6], config->state[7], config->state[8], config->state[9],
          config->state[10], config->state[11], config->state[12], config->state[13],
          config->state[14]);
    }
  }

  add("bound textures that did not reach the shader: stage 0 %llu foreign + %llu imageless, "
      "stage 1 %llu + %llu\n",
      (unsigned long long)UnresolvedForeign[0], (unsigned long long)UnresolvedNoImage[0],
      (unsigned long long)UnresolvedForeign[1], (unsigned long long)UnresolvedNoImage[1]);
  for (const auto &[key, count] : UnresolvedFormats) {
    add("  pool %u format %u: %llu draws\n", (unsigned)(key >> 32), (unsigned)(key & 0xffffffff),
        (unsigned long long)count);
  }

  {
    std::vector<std::pair<uint64_t, const LightingInputs *>> ordered;
    ordered.reserve(LightingByFvf.size());
    for (const auto &[inputs, count] : LightingByFvf) {
      ordered.emplace_back(count, &inputs);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    add("draws with NO vertex diffuse - what the fixed function colours them from: %u\n",
        (unsigned)ordered.size());
    out += "                          fvf  lit  lights  material diffuse   ambient  emissive\n";
    for (const auto &[count, in] : ordered) {
      add("  %10llu draws  0x%03x %4u %6u   0x%08x 0x%08x 0x%08x\n", (unsigned long long)count,
          in->fvf, in->lighting, in->enabled_lights, in->diffuse, in->ambient, in->emissive);
    }
  }

  // D3DPRIMITIVETYPE: 1 point list, 2 line list, 3 line strip, 4 triangle list, 5 strip, 6 fan.
  add("draws that are not triangle lists: %u distinct\n", (unsigned)OddTopologies.size());
  out += "                         type   fvf  from  prims  stages  texture  blend  ztest"
         "   screen box            colour        z      rhw  stencil(func,ref)\n";
  for (const auto &[odd, count] : OddTopologies) {
    float z = 0.0f;
    float rhw = 0.0f;
    std::memcpy(&z, &odd.first_z, sizeof(z));
    std::memcpy(&rhw, &odd.first_rhw, sizeof(rhw));
    add("  %10llu draws %5u 0x%03x %5s %6u %7u %8d %6u %6u   %5d,%-4d %5d,%-4d  0x%08x %8.3f "
        "%8.3f   %u(%u,%u)\n",
        (unsigned long long)count, odd.type, odd.fvf, odd.user_pointer != 0 ? "ptr" : "buf",
        odd.primitives, odd.stages,
        odd.texture_index == vulkan::kNoTexture ? -1 : (int)odd.texture_index, odd.blend,
        odd.depth_test, odd.x0, odd.y0, odd.x1, odd.y1, odd.first_colour, z, rhw, odd.stencil,
        odd.stencil_func, odd.stencil_ref);
  }

  add("first converted vertex colour of a user-pointer draw, by FVF: %u distinct\n",
      (unsigned)FirstVertexColours.size());
  for (const auto &[key, count] : FirstVertexColours) {
    add("  fvf 0x%03x  0x%08x: %llu draws\n", (unsigned)(key >> 32),
        (unsigned)(key & 0xffffffff), (unsigned long long)count);
  }

  add("indexed draws reaching past their bound buffer: %llu  (D3D8 tolerates, D3D9 rejects the "
      "call - the reference renderer's defect, not ours)\n",
      (unsigned long long)TheStats.draws_out_of_range);
  for (const std::string &line : OutOfRangeDraws) {
    out += line;
    out += "\n";
  }

  add("vertex buffers drawn from with no arena slot: %u distinct\n",
      (unsigned)UnslottedVertexBuffers.size());
  for (const auto &[key, count] : UnslottedVertexBuffers) {
    add("  fvf 0x%03x, %u bytes: %llu draws\n", (unsigned)(key >> 32),
        (unsigned)(key & 0xffffffff), (unsigned long long)count);
  }

  if (State.have_material) {
    const D3DMATERIAL8 &m = State.material;
    add("material: diffuse %.2f %.2f %.2f %.2f  ambient %.2f %.2f %.2f\n", m.Diffuse.r,
        m.Diffuse.g, m.Diffuse.b, m.Diffuse.a, m.Ambient.r, m.Ambient.g, m.Ambient.b);
    add("          specular %.2f %.2f %.2f power %.1f  emissive %.2f %.2f %.2f\n",
        m.Specular.r, m.Specular.g, m.Specular.b, m.Power, m.Emissive.r, m.Emissive.g,
        m.Emissive.b);
  } else {
    out += "material: never set\n";
  }
  return out;
}

// Qualified: gk::DetourAttach in src/DetourUtils.h is a member-function-pointer overload,
// and unqualified lookup from inside namespace gk would find it first.
D3D8CaptureSystem::D3D8CaptureSystem() {
  // Both bisect knobs are read here, which is the earliest point that runs before any device
  // exists. `ReadWrapMode` was defined and never called - so GKPLUS_WRAP_BUFFERS silently did
  // nothing, and a bisect run with it set would have "cleared" the buffer wrappers of a fault
  // they were still causing. Read the plan's env table as a claim to check, not a fact.
  ReadWrapMode();
  ReadTextureUploadMode();
  auto env_is_one = [](const char *name) {
    char value[8] = {};
    return ::GetEnvironmentVariableA(name, value, sizeof(value)) > 0 && value[0] == '1';
  };
  ForceLightingOff = env_is_one("GKPLUS_NO_LIGHTING");
  ForceStage1Off = env_is_one("GKPLUS_NO_STAGE1");
  ForceSpecularOff = env_is_one("GKPLUS_NO_SPECULAR");
  ForceNoMipmap = env_is_one("GKPLUS_NO_MIPMAP");
  ForceNoCull = env_is_one("GKPLUS_NO_CULL");
  ForceNoZTest = env_is_one("GKPLUS_NO_ZTEST");
  {
    char skip[16] = {};
    const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_SKIP", skip, sizeof(skip));
    const std::string letters(skip, len);
    char topologies[16] = {};
    const DWORD topologies_len =
        ::GetEnvironmentVariableA("GKPLUS_VK_TOPOLOGIES", topologies, sizeof(topologies));
    const std::string want(topologies, topologies_len);
    // Unset means all of them now, so the variable selects a SUBSET rather than opting in.
    const bool none = want == "0" || want == "none";
    const bool all = want.empty() || want == "1" || want == "all";
    DrawStrips = !none && (all || want == "strip");
    DrawLines = !none && (all || want == "line");
    SkipTopologies = (!DrawStrips && !DrawLines) || letters.find('t') != std::string::npos;
    if (SkipTopologies) {
      DrawStrips = DrawLines = false;
    }
    SkipSeeding = letters.find('s') != std::string::npos;
    SkipLitColour = letters.find('l') != std::string::npos;
    SkipStateDefaults = letters.find('d') != std::string::npos;
    if (!letters.empty()) {
      DebugWrite("gkplus: vulkan features skipped: " + letters + "\n");
    }
  }
  if (ForceLightingOff) {
    DebugWrite("gkplus: D3DRS_LIGHTING forced off (GKPLUS_NO_LIGHTING)\n");
  }
  if (ForceStage1Off) {
    DebugWrite("gkplus: texture stages past 0 forced off (GKPLUS_NO_STAGE1)\n");
  }
  if (ForceSpecularOff) {
    DebugWrite("gkplus: D3DRS_SPECULARENABLE forced off (GKPLUS_NO_SPECULAR)\n");
  }
  if (ForceNoMipmap) {
    DebugWrite("gkplus: D3DTSS_MIPFILTER forced to NONE (GKPLUS_NO_MIPMAP)\n");
  }
  if (ForceNoCull) {
    DebugWrite("gkplus: D3DRS_CULLMODE forced to NONE (GKPLUS_NO_CULL)\n");
  }
  if (ForceNoZTest) {
    DebugWrite("gkplus: D3DRS_ZENABLE forced off (GKPLUS_NO_ZTEST)\n");
  }
  ::DetourAttach(reinterpret_cast<void **>(&OriginalDirect3DCreate8),
                 reinterpret_cast<void *>(HookedDirect3DCreate8));
  GetObjectAtOffset(AcquireRimTexture, 0x005a15b0);
  DetourAttach(&AcquireRimTexture, &RimCacheAbi::HookedAcquire);
}

D3D8CaptureSystem::~D3D8CaptureSystem() {
  DetourDetach(&AcquireRimTexture, &RimCacheAbi::HookedAcquire);
  ::DetourDetach(reinterpret_cast<void **>(&OriginalDirect3DCreate8),
                 reinterpret_cast<void *>(HookedDirect3DCreate8));
}

} // namespace d3d8
} // namespace gk
