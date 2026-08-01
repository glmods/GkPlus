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

struct ShadowState {
  uint32_t render_states[kMaxRenderState] = {};
  uint32_t stage_states[kStages][kMaxStageState] = {};
  IDirect3DBaseTexture8 *textures[kStages] = {};
  uint32_t fvf = 0;
};

ShadowState State;

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

std::set<uint32_t> MaterialKeys;
std::set<uint32_t> PipelineKeys;
std::set<uint32_t> MaterialKeysThisFrame;

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
    // The pointer identity of the texture, not its contents: two draws with different
    // textures are different materials in a bindless table, since the index differs.
    IDirect3DBaseTexture8 *texture = State.textures[i];
    hash = HashBytes(&texture, sizeof(texture), hash);
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

  void NoteLock(uint32_t offset, uint32_t size, BYTE **data) {
    ++TheStats.locks;
    // A zero SizeToLock means "the whole buffer" in D3D8.
    locked_offset_ = offset;
    locked_bytes_ = size == 0 ? length_ - offset : size;
    locked_ = data != nullptr ? *data : nullptr;
    TheStats.locked_bytes_this_frame += locked_bytes_;
  }

  // Read on the way OUT of the lock, not on the way in: the game writes its geometry between
  // Lock and Unlock, so before Unlock is the only moment the data is both complete and still
  // mapped. Uploading here means the arenas see exactly what D3D9 sees.
  void UploadLocked() {
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
    if (slot_.valid) {
      if (vertex_) {
        UploadConvertedVertices();
      } else if (!vulkan::UploadIntoSlot(slot_, locked_offset_, locked_, locked_bytes_)) {
        ++TheStats.failed_uploads;
      }
    }
    locked_ = nullptr;
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
  // This buffer's own region of the arena, held for its whole lifetime and released in the
  // destructor. See BufferSlot in VkResources.h for why it is per-buffer and not per-upload.
  vulkan::BufferSlot slot_;
};

// Declared before the wrappers so their constructors can register themselves; defined with
// Unwrap, which is where the reason they exist is written up.
std::set<const void *> LiveVertexWrappers;
std::set<const void *> LiveIndexWrappers;

struct CaptureVertexBuffer final : BufferWrapper<IDirect3DVertexBuffer8> {
  CaptureVertexBuffer(IDirect3DVertexBuffer8 *inner, uint32_t length, uint32_t fvf)
      : BufferWrapper(inner, length, true, fvf) {
    LiveVertexWrappers.insert(this);
  }
  ~CaptureVertexBuffer() override { LiveVertexWrappers.erase(this); }

#define GK_DECL(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
  GK_IDIRECT3DVERTEXBUFFER8_METHODS(GK_DECL)
#undef GK_DECL
};

struct CaptureIndexBuffer final : BufferWrapper<IDirect3DIndexBuffer8> {
  CaptureIndexBuffer(IDirect3DIndexBuffer8 *inner, uint32_t length)
      : BufferWrapper(inner, length, false, 0) {
    LiveIndexWrappers.insert(this);
  }
  ~CaptureIndexBuffer() override { LiveIndexWrappers.erase(this); }

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
    NoteLock(OffsetToLock, SizeToLock, ppbData);
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
    NoteLock(OffsetToLock, SizeToLock, ppbData);
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
      WrapVertexBuffers ? new CaptureVertexBuffer(buffer, Length, FVF) : buffer;
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
  *ppIndexBuffer = WrapIndexBuffers ? new CaptureIndexBuffer(buffer, Length) : buffer;
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
  return inner_->SetTransform(State, pMatrix);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetViewport(const D3DVIEWPORT8 *pViewport) {
  NoteBlockState();
  return inner_->SetViewport(pViewport);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetMaterial(const D3DMATERIAL8 *pMaterial) {
  NoteBlockState();
  return inner_->SetMaterial(pMaterial);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetLight(DWORD Index, const D3DLIGHT8 *pLight) {
  NoteBlockState();
  if (Index > TheStats.max_light_index) {
    TheStats.max_light_index = Index;
  }
  return inner_->SetLight(Index, pLight);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::LightEnable(DWORD Index, BOOL Enable) {
  NoteBlockState();
  if (Index > TheStats.max_light_index) {
    TheStats.max_light_index = Index;
  }
  return inner_->LightEnable(Index, Enable);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::SetRenderState(D3DRENDERSTATETYPE State,
                                                        DWORD Value) {
  NoteBlockState();
  TheStats.render_states[static_cast<uint32_t>(State)].insert(
      static_cast<uint32_t>(Value));
  Record({OpKind::RenderState, static_cast<uint32_t>(State), 0,
          static_cast<uint32_t>(Value), nullptr});
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
  Record({OpKind::Texture, Stage, 0, 0, pTexture});
  // Being bound is the definition of "this texture will be sampled", so it is the right
  // trigger for giving it an image - and the only one that covers a texture the game never
  // blits into. `EnsureTextureImage` is a single predictable branch once the image exists.
  if (pTexture != nullptr && LiveTextureWrappers.count(pTexture) != 0) {
    EnsureTextureImage(*static_cast<CaptureTexture *>(pTexture));
  }
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

void CountDraw(D3DPRIMITIVETYPE type, bool user_pointer) {
  SnapshotDraw();
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
  CountDraw(PrimitiveType, false);
  return inner_->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex,
    UINT PrimitiveCount) {
  CountDraw(PrimitiveType, false);
  return inner_->DrawIndexedPrimitive(PrimitiveType, MinIndex, NumVertices, StartIndex,
                                      PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  CountDraw(PrimitiveType, true);
  return inner_->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData,
                                 VertexStreamZeroStride);
}

HRESULT STDMETHODCALLTYPE CaptureDevice::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices,
    UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  CountDraw(PrimitiveType, true);
  return inner_->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices,
                                        PrimitiveCount, pIndexData, IndexDataFormat,
                                        pVertexStreamZeroData, VertexStreamZeroStride);
}

// ---------------------------------------------------------------------------------------

using Direct3DCreate8Fn = IDirect3D8 *(WINAPI *)(UINT);
Direct3DCreate8Fn OriginalDirect3DCreate8 = ::Direct3DCreate8;

IDirect3D8 *WINAPI HookedDirect3DCreate8(UINT SDKVersion) {
  IDirect3D8 *const inner = OriginalDirect3DCreate8(SDKVersion);
  if (!inner) {
    return nullptr;
  }
  return new CaptureD3D8(inner);
}

} // namespace

const CaptureStats &Stats() { return TheStats; }

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
  MaterialKeysThisFrame.clear();
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

IDirect3DDevice9 *ResolveD3D9Device(IDirect3DDevice8 *device) {
  if (!device) {
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

// Qualified: gk::DetourAttach in src/DetourUtils.h is a member-function-pointer overload,
// and unqualified lookup from inside namespace gk would find it first.
D3D8CaptureSystem::D3D8CaptureSystem() {
  // Both bisect knobs are read here, which is the earliest point that runs before any device
  // exists. `ReadWrapMode` was defined and never called - so GKPLUS_WRAP_BUFFERS silently did
  // nothing, and a bisect run with it set would have "cleared" the buffer wrappers of a fault
  // they were still causing. Read the plan's env table as a claim to check, not a fact.
  ReadWrapMode();
  ReadTextureUploadMode();
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
