#include "VkResources.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// VMA reaches Vulkan through the two getters we hand it, which volk has already resolved.
// STATIC 0 because there are no prototypes to link against (the whole point of volk here);
// DYNAMIC 1 so VMA loads the rest itself from those two.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
// VMA is header-only: exactly one translation unit must define this, and it is this one so
// the three defines above are guaranteed to apply to the implementation as well as to the
// declarations. Splitting them would repeat the volk mismatch from section 4.4.
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "Core.h"
#include "VertexFormat.h"
// For the two shader-ABI records the scratch is sized and strided by. The dependency goes this
// way round - VkDraw.cpp already reads this header - because the ABI belongs beside the draw
// list that defines it, and a scratch allocator that took the stride as an argument would put
// the one number that must not be wrong at every call site.
#include "VkDraw.h"
#include "VkCapture.h"
#include "VkContext.h"
#include "VkInternal.h"

namespace gk {
namespace vulkan {
namespace {

// Sized from §4.8's measurements with room to spare, because running out is a hard failure
// and 8 MB is not worth economising on:
//   peak live vertex 6.2 MB -> 32 MB    peak live index 586 KB -> 8 MB
//   4.7 MB/frame upload     -> 32 MB staging (about six frames' worth)
constexpr VkDeviceSize kVertexArenaBytes = 32u << 20;
constexpr VkDeviceSize kIndexArenaBytes = 8u << 20;
constexpr VkDeviceSize kStagingBytes = 32u << 20;
constexpr uint32_t kFramesInFlight = 2; // must match VkRenderer's

// One MORE scratch slice than there are frames in flight, and the extra one is not slack - it
// is what makes the allocator correct at all.
//
// The scratch is written by the *game*, during the scene, and read by the frame that Present
// then produces: the write window for scene k+1 is [end of DrawFrame(k), start of
// DrawFrame(k+1)]. The only completion the renderer has proof of during that window is the
// fence waited at the top of DrawFrame(k), which retires frame k-N. So a slice the CPU is
// about to write must not have been read more recently than that, i.e. slices must outnumber
// frames in flight. With N slices the next one round belongs to frame k+1-N, which is still in
// flight, and the CPU would be overwriting vertices the GPU is reading.
constexpr uint32_t kScratchSlices = kFramesInFlight + 1;

// `GKPLUS_VK_HEAPS=small` - the same totals cut to just above level01's measured peaks, for
// RenderDoc. A capture is taken *inside* the 32-bit game, where RenderDoc allocates a mapped
// readback buffer per resource to snapshot its initial state, and gl.exe's 2 GB of address
// space is already shared with the game, the driver and QuickJS. At full size 15 of those
// allocations fail in level and those resources are captured uninitialised (§4.17).
//
// Deliberately NOT implied by GKPLUS_RENDERDOC. Shrinking the heaps changes what the renderer
// does - an arena that no longer exhausts, a ring that wraps differently - so a capture taken
// this way is not a capture of the configuration that runs. Coupling the two would mean the
// frame being debugged is not the frame that misbehaved, which is the whole problem with
// debugging tools that alter behaviour. The warning below is the compromise.
//
// Level01's peaks are 10.8 MB vertex, 587 KB index, 800 KB + 66 KB scratch. The vertex arena
// gets 16 MB rather than the 12 that fits, because 12 leaves it at 90% full on the one level
// that has been measured and a level with more geometry would silently start dropping draws;
// `arena_full` / `scratch_exhausted` are what say so if one ever does not fit.
constexpr VkDeviceSize kSmallVertexArenaBytes = 16u << 20;
constexpr VkDeviceSize kSmallIndexArenaBytes = 2u << 20;
constexpr VkDeviceSize kSmallStagingBytes = 8u << 20;

bool SmallHeaps = false;

void ReadHeapMode() {
  char value[16] = {};
  const DWORD len = ::GetEnvironmentVariableA("GKPLUS_VK_HEAPS", value, sizeof(value));
  SmallHeaps = std::string(value, len) == "small";
}

bool Ready = false;
std::string Error;
ResourceStats TheStats;

VmaAllocator Allocator = VK_NULL_HANDLE;

// A free-list allocator over one arena. First fit, with coalescing on free.
//
// Deliberately not a bump allocator - see the note on BufferSlot. The allocation pattern is
// gentle (about 3,500 live slots, five created and five destroyed per frame), so first-fit
// over a sorted vector is both fast enough and easy to audit for the fragmentation that
// actually matters here.
struct Arena {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkDeviceSize capacity = 0;
  VkDeviceSize tail = 0; // the never-yet-allocated region starts here
  VkDeviceSize used = 0; // currently allocated
  // Every slot starts on a multiple of this. For the vertex arena it MUST be the canonical
  // vertex size, not merely 16: a draw addresses its buffer as `slot.offset / 48`, so a slot
  // that does not begin on a whole vertex silently pulls the wrong ones. That was the first
  // thing the renderer put on screen - geometry smeared into long streaks toward a point,
  // because each triangle got one right vertex and two from somewhere else.
  VkDeviceSize alignment = 16;
  std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free_list; // (offset, size), sorted
};

Arena VertexArena;
Arena IndexArena;

// Host-visible, permanently mapped, and small. This is the ONLY mapping in the renderer.
struct Staging {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  uint8_t *mapped = nullptr;
  VkDeviceSize capacity = 0;
  VkDeviceSize head = 0;
  // Bytes staged since the last time anything was recorded into a command buffer. This is
  // what bounds a wrap: see AllocateStaging.
  VkDeviceSize batch = 0;
  // Bytes that HAVE been recorded but whose copies the GPU has not run yet, per frame slot.
  // Recording does not release a region - executing does - so these have to keep counting
  // against the ring until the frame's fence retires them. `in_flight` is their sum, kept
  // beside them so the hot path adds one number rather than looping.
  VkDeviceSize frame_bytes[kFramesInFlight] = {};
  VkDeviceSize in_flight = 0;
  // Whether the frame that consumed each slot's staging is still in flight. Written when its
  // uploads are recorded, cleared when the renderer's fence for that slot has been waited on.
  //
  // This used to be a `frame_start[]` that nothing ever read, because ReleaseFrameStaging was
  // never called - so a wrap could hand a region back while the GPU was still reading it. That
  // is not theoretical: it is what corrupted exactly one texture during the startup burst,
  // once per session, in a way that looked like a missing pixel route for an afternoon.
  bool frame_live[kFramesInFlight] = {};
};

Staging Ring;

// Per-frame scratch for user-pointer draws. Host-visible and permanently mapped - see the
// header for why this is the one exception to the unmapped rule.
//
// Sized to be generous rather than measured, because nothing existed to measure: the game makes
// ~115 user-pointer draws a frame and they are text, particles and menu quads, so this is orders
// of magnitude of headroom. `scratch_exhausted` is what would say otherwise, and the two peaks
// are the numbers to resize by.
constexpr VkDeviceSize kScratchVertexBytes = 4u << 20;
constexpr VkDeviceSize kScratchIndexBytes = 1u << 20;
constexpr VkDeviceSize kSmallScratchVertexBytes = 1u << 20;
constexpr VkDeviceSize kSmallScratchIndexBytes = 256u << 10;

// The per-draw record array and the lights it points at (§4.26). The record slice holds exactly
// as many as VkDraw's kMaxDrawsPerFrame will submit, so the two limits agree rather than one
// silently biting first; level01's peak is 660 of them. Lights are deduplicated by enable mask
// within a frame, so 4096 is several hundred times what a level01 frame uses.
constexpr VkDeviceSize kScratchDrawBytes = 8192u * sizeof(GpuDrawRecord);
constexpr VkDeviceSize kScratchLightBytes = 4096u * sizeof(GpuLight);
constexpr VkDeviceSize kSmallScratchDrawBytes = 1024u * sizeof(GpuDrawRecord);
constexpr VkDeviceSize kSmallScratchLightBytes = 512u * sizeof(GpuLight);
// Materials are interned, so a frame uses far fewer of these than it has draws - but the slice
// is sized to the same kMaxDrawsPerFrame for the reason the record slice is: a frame in which
// every draw happened to have its own material must still fit, so the draw limit is the only
// one that can bite. At 48 bytes that is 384 KB a slice against the record slice's 2304 KB.
constexpr VkDeviceSize kScratchMaterialBytes = 8192u * sizeof(GpuMaterial);
constexpr VkDeviceSize kSmallScratchMaterialBytes = 1024u * sizeof(GpuMaterial);

struct Scratch {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  uint8_t *mapped = nullptr;
  VkDeviceAddress address = 0;
  VkDeviceSize slice = 0; // bytes per slice
  VkDeviceSize head = 0;  // within the current scene's slice
  VkDeviceSize base = 0;  // where the current scene's slice starts
};

Scratch ScratchVertices;
Scratch ScratchIndices;
Scratch ScratchDraws;
Scratch ScratchLights;
Scratch ScratchMaterials;
// Which slice the scene now being recorded writes into. It belongs to the *scene*, not to a
// frame in flight - see kScratchSlices.
uint32_t ScratchSlice = 0;

// A command pool and fence of the renderer's own, used only by FlushPendingNow - the frame's
// command buffer belongs to VkRenderer and is not available mid-batch. Transient because these
// buffers are allocated, submitted once and freed.
VkCommandPool UploadPool = VK_NULL_HANDLE;
VkFence UploadFence = VK_NULL_HANDLE;

struct PendingCopy {
  VkBuffer dst = VK_NULL_HANDLE;
  VkDeviceSize src_offset = 0;
  VkDeviceSize dst_offset = 0;
  VkDeviceSize bytes = 0;
  // Whether this copy writes bytes an earlier copy in the same batch also wrote, and therefore
  // needs a barrier before it. See NoteDestination.
  bool barrier_before = false;
};

std::vector<PendingCopy> Pending;

// The destination ranges already queued in this batch, per arena, as start -> end.
//
// **Two copies in one command buffer are not ordered against each other.** Vulkan orders the
// stages of a pipeline, not two transfers writing the same memory: without a barrier between
// them the result of overlapping writes is whichever the GPU happens to retire last. That is
// not a theoretical hazard here - a level load frees a buffer's arena slot and hands it to a
// new buffer within the same batch, so both buffers' uploads sit in `Pending` targeting the
// same bytes, and the older one wins often enough to leave a handful of meshes per load drawing
// somebody else's geometry. It presents as one object smeared across the screen, because a
// vertex read through a stale index lands anywhere.
//
// A range that overlaps anything already queued therefore gets a barrier before it, and the map
// is cleared at that point - the barrier orders every earlier copy, so nothing before it can
// collide again.
std::map<VkDeviceSize, VkDeviceSize> PendingDstRanges[2];

// Which staging batch is being filled. A batch is everything between two records, so this is
// the unit CaptureStagingBatch works in - and it is reproducible across runs, because a level
// load stages the same bytes in the same order every time.
uint32_t StagingBatch = 0;

// GKPLUS_VK_WATCH_DST: a vertex-arena slot offset whose uploads are logged with the batch that
// carried them, so a capture can be aimed at the right batch on the next run. -1 is off.
uint32_t WatchDst = 0xffffffffu;
std::string WatchLog;

// --- images --------------------------------------------------------------------------------
//
// The four formats notes section 4.1 enumerated and section 4.12 confirmed, and nothing else.
// A format with no entry here gets no image, which makes the texture sample as missing rather
// than as garbage - the same "refuse, do not guess" rule the FVF converter follows.
//
// A4R4G4B4 is deliberately EXPANDED to R8G8B8A8 on the CPU rather than mapped to
// VK_FORMAT_A4R4G4B4_UNORM_PACK16. That format is optional even in Vulkan 1.3 (it is gated on
// the `formatA4R4G4B4` feature), so mapping it natively would mean a per-device support matrix
// and a fallback anyway. There are 58 such textures on level01 and they are small; the
// expansion costs one pass over rows that are already being copied.
//
// A8 keeps its single channel and is fixed up in the image view instead, with a
// {ONE, ONE, ONE, R} swizzle - which is what D3DFMT_A8 means and costs nothing at sample time.
enum : uint32_t {
  kD3DFmtA4R4G4B4 = 26,
  kD3DFmtA8 = 28,
  kD3DFmtDXT1 = 0x31545844, // 'DXT1'
  kD3DFmtDXT3 = 0x33545844, // 'DXT3'
};

struct FormatMapping {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t block = 1;       // texels per block edge: 1 uncompressed, 4 for BC
  uint32_t block_bytes = 0; // bytes per block
  uint32_t src_block_bytes = 0; // ... as D3D stores it, which differs only for A4R4G4B4
  bool expand_4444 = false;
  bool alpha_swizzle = false;
};

bool MapFormat(uint32_t d3d_format, FormatMapping &out) {
  switch (d3d_format) {
  case kD3DFmtA4R4G4B4:
    out = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 2, true, false};
    return true;
  case kD3DFmtA8:
    out = {VK_FORMAT_R8_UNORM, 1, 1, 1, false, true};
    return true;
  case kD3DFmtDXT1:
    out = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 8, false, false};
    return true;
  case kD3DFmtDXT3:
    out = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 16, false, false};
    return true;
  default:
    return false;
  }
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 1;
  uint32_t d3d_format = 0;
  VkDeviceSize bytes = 0;
  FormatMapping mapping;
  // The `.rim` path the engine acquired this texture under, filled in after creation by the
  // AcquireRimTexture hook. It is what lets a mod or a shader name a texture instead of
  // pointing at a slot number that changes every run.
  std::string name;
  bool live = false;
};

// Indexed by bindless slot. Entries are never erased, only marked dead and reused, so an index
// handed out stays meaningful for the life of the image and the vector can back the descriptor
// array directly.
std::vector<Image> Images;
std::vector<uint32_t> FreeImageIndices;
// See TextureRegistryGeneration in VkResources.h: bumped by create, destroy and name, which are
// the three things that change what a name-keyed lookup would resolve to.
uint64_t ImageRegistryGeneration = 1;

// --- the bindless set ------------------------------------------------------------------------
//
// Sized rather than grown: 4096 image slots is ~16x level01's live count and costs one
// descriptor each, and a set cannot be resized without recreating every pipeline layout built
// against it. `descriptors_out_of_range` is the counter that would say the guess was wrong.
constexpr uint32_t kBindlessTextures = 4096;
constexpr uint32_t kBindlessSamplers = 64;

VkDescriptorSetLayout BindlessLayout = VK_NULL_HANDLE;
VkDescriptorPool BindlessPool = VK_NULL_HANDLE;
VkDescriptorSet BindlessSet = VK_NULL_HANDLE;

// The D3D sampler state a VkSampler is built from, kept as the key so identical combinations
// collapse. These are the six stage states the recorder measured as actually varying
// (D3DTSS_ADDRESSU/V 13/14, MAG/MIN/MIPFILTER 16/17/18).
struct SamplerKey {
  uint32_t mag = 0, min = 0, mip = 0, address_u = 0, address_v = 0;
  bool operator<(const SamplerKey &o) const {
    return std::tie(mag, min, mip, address_u, address_v) <
           std::tie(o.mag, o.min, o.mip, o.address_u, o.address_v);
  }
};

std::map<SamplerKey, uint32_t> SamplerIndices;
std::vector<VkSampler> Samplers;

struct PendingImageCopy {
  uint32_t index = 0;
  VkDeviceSize src_offset = 0;
  uint32_t level = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // The same ordering problem the buffer copies have - two blits into one mip level in a batch
  // are not ordered against each other - and the game does re-blit a texture within a batch.
  // Conservative about what counts as a collision: the same image and level, whatever the
  // rectangles, because a per-rect test would buy nothing at these counts.
  bool barrier_before = false;
};

std::vector<PendingImageCopy> PendingImages;
std::set<uint64_t> PendingImageLevels; // index << 32 | level, for this batch

bool Fail(const std::string &message) {
  Error = message;
  DebugWrite("gkplus: vulkan resources: " + message + "\n");
  return false;
}

bool CreateArena(Arena &arena, VkDeviceSize bytes, VkBufferUsageFlags usage,
                 VkDeviceSize alignment, const char *what) {
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  // TRANSFER_DST because everything arrives by staged copy, and SHADER_DEVICE_ADDRESS so the
  // bindless design can pull vertices by pointer rather than binding a vertex buffer (§2).
  //
  // TRANSFER_SRC is not for the renderer either - nothing copies out of an arena in a frame.
  // It is `render.verify_buffers()`, for the same reason the texture images carry it: without
  // it, the readback's own vkCmdCopyBuffer is an invalid call, so the verifier that exists to
  // check the bytes is itself the thing validation is complaining about. It read clean here
  // only because this driver tolerates it.
  info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo alloc = {};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  // No MAPPED bit and no HOST_ACCESS: this must land in device-local memory and stay
  // unmapped. See the header - on this machine the entire device-local heap is host-visible,
  // so "it happened to be mappable" is not a reason to map it.
  alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

  if (vmaCreateBuffer(Allocator, &info, &alloc, &arena.buffer, &arena.allocation, nullptr) !=
      VK_SUCCESS) {
    return Fail(std::string("could not allocate the ") + what + " arena");
  }
  arena.capacity = bytes;
  arena.used = 0;
  arena.alignment = alignment;
  return true;
}

// 16-byte aligned, which is what the staging ring needs (see AllocateStaging).
VkDeviceSize AlignUp(VkDeviceSize v) { return (v + 15) & ~VkDeviceSize(15); }

VkDeviceSize AlignUpTo(VkDeviceSize v, VkDeviceSize alignment) {
  return ((v + alignment - 1) / alignment) * alignment;
}

bool Suballocate(Arena &arena, VkDeviceSize bytes, VkDeviceSize &offset) {
  // Both the size and every offset are rounded to the arena's alignment, so that a slot freed
  // and re-suballocated stays aligned too - rounding only the size would drift.
  bytes = AlignUpTo(bytes, arena.alignment);
  for (size_t i = 0; i < arena.free_list.size(); ++i) {
    if (arena.free_list[i].second >= bytes) {
      offset = arena.free_list[i].first;
      if (arena.free_list[i].second == bytes) {
        arena.free_list.erase(arena.free_list.begin() + i);
      } else {
        arena.free_list[i].first += bytes;
        arena.free_list[i].second -= bytes;
      }
      arena.used += bytes;
      return true;
    }
  }
  if (arena.tail + bytes > arena.capacity) {
    ++TheStats.arena_exhausted;
    return false;
  }
  offset = arena.tail;
  arena.tail += bytes;
  arena.used += bytes;
  return true;
}

void Release(Arena &arena, VkDeviceSize offset, VkDeviceSize bytes) {
  bytes = AlignUpTo(bytes, arena.alignment);
  arena.used -= bytes;
  // Insert in address order, then coalesce with either neighbour. Without coalescing the
  // free list grows without bound under this workload, which is the same leak in different
  // clothes.
  size_t i = 0;
  while (i < arena.free_list.size() && arena.free_list[i].first < offset) {
    ++i;
  }
  arena.free_list.insert(arena.free_list.begin() + i, {offset, bytes});
  if (i + 1 < arena.free_list.size() &&
      arena.free_list[i].first + arena.free_list[i].second ==
          arena.free_list[i + 1].first) {
    arena.free_list[i].second += arena.free_list[i + 1].second;
    arena.free_list.erase(arena.free_list.begin() + i + 1);
  }
  if (i > 0 && arena.free_list[i - 1].first + arena.free_list[i - 1].second ==
                   arena.free_list[i].first) {
    arena.free_list[i - 1].second += arena.free_list[i].second;
    arena.free_list.erase(arena.free_list.begin() + i);
  }
}

void RecordInto(VkCommandBuffer cmd);
bool FlushPendingNow();

// How much of the ring the batch RecordInto just recorded occupied. Read by its two callers,
// which differ in when those bytes become free again.
VkDeviceSize LastRecordedBytes = 0;

// Blocks until no recorded batch is still being read by the GPU, then marks every slot free.
// Conservative on purpose: `vkDeviceWaitIdle` rather than per-slot fences, because the ring is
// only ever handed back at a wrap and the renderer's fences belong to VkRenderer.
void WaitForLiveFrames() {
  bool any = false;
  for (const bool live : Ring.frame_live) {
    any = any || live;
  }
  if (!any) {
    return;
  }
  vkDeviceWaitIdle(GetDevice());
  for (bool &live : Ring.frame_live) {
    live = false;
  }
  for (VkDeviceSize &bytes : Ring.frame_bytes) {
    bytes = 0;
  }
  Ring.in_flight = 0;
  ++TheStats.staging_stalls;
}

// Reserves staging space. A request that cannot fit even in an empty ring is dropped and
// counted rather than truncated, because a partial upload is worse than none - it would look
// like valid geometry.
//
// Every region is 16-byte aligned, and that is a requirement rather than tidiness:
// `vkCmdCopyBufferToImage` demands a `bufferOffset` that is a multiple of the texel block
// size, which is 16 for BC2 and 8 for BC1. The first version of the image upload inherited
// whatever offset the preceding buffer copy left behind, and validation rejected essentially
// every compressed copy. 16 covers all four formats; the waste is at most 15 bytes per
// allocation out of 32 MB.
//
// **A region is free again only once the GPU has RUN its copy** - not when the copy is staged,
// and not when it is recorded. Three things follow, and each of them was a real corruption
// before it was a rule.
//
// - Staged bytes are read when `RecordUploads` puts them in a command buffer, so the ring may
//   not overwrite a batch that has not been recorded yet. The steady state is ~11 MB between
//   frames and never comes close; a level load stages **360 MB between two Presents**, because
//   the game stops presenting while it loads, which is eleven laps of the ring inside one
//   batch. So a batch that would exceed the ring is recorded and waited for on the spot.
//
// - Recorded bytes are read when that command buffer EXECUTES, which is later still. Releasing
//   them at record time let the head lap the ring and overwrite the source of a copy the GPU
//   had not run - measured at 87,222 copies in one level01 session, each of them moving
//   whatever the ring held by then. That is why `frame_bytes`/`in_flight` exist: recorded
//   bytes keep counting until the frame's fence retires them.
//
// - `batch` is what the head has TRAVELLED, not the sum of the payloads. The head also skips
//   bytes it never hands out - up to 15 for alignment on every allocation, and the whole tail
//   at a wrap - and counting only the payloads let it drift ahead by the accumulated skip and
//   lap its own batch. One load skips ~140 MB that way.
//
// With all three counted, "the head may not travel further than the ring holds" is exactly the
// invariant, and lapping live data is unrepresentable rather than merely unlikely.
bool AllocateStaging(VkDeviceSize bytes, VkDeviceSize &offset) {
  if (bytes > Ring.capacity) {
    ++TheStats.dropped_uploads;
    return false;
  }
  for (uint32_t attempt = 0; attempt < 3; ++attempt) {
    VkDeviceSize head = AlignUp(Ring.head);
    VkDeviceSize skipped = head - Ring.head;
    bool wrapping = false;
    if (head + bytes > Ring.capacity) {
      skipped += Ring.capacity - head;
      head = 0;
      wrapping = true;
    }
    if (Ring.batch + Ring.in_flight + skipped + bytes <= Ring.capacity) {
      if (wrapping) {
        ++TheStats.staging_wraps;
      }
      if (Ring.batch == 0) {
        // The first allocation of a batch: everything this batch stages has to be inside the
        // capture window, and the staging writes happen here rather than at record time.
        BeginBatchCaptureIfArmed(StagingBatch, GetInstance());
      }
      offset = head;
      Ring.head = head + bytes;
      Ring.batch += skipped + bytes;
      TheStats.staging_skipped_bytes += skipped;
      return true;
    }
    // Retire what the GPU has already been given before recording anything more: a frame whose
    // copies have run is holding the ring for nothing.
    if (Ring.in_flight != 0) {
      WaitForLiveFrames();
      continue;
    }
    // The un-recorded batch has grown to the size of the ring. Record and wait for it, which
    // hands the whole ring back, then place the allocation at 0 on the second pass.
    if (!FlushPendingNow()) {
      // Nothing was reclaimed, so wrapping would still corrupt. Dropping is the honest
      // outcome and it is counted; a missing texture is diagnosable, a scrambled one is not.
      ++TheStats.dropped_uploads;
      return false;
    }
  }
  // Unreachable: a successful flush leaves head and batch at 0, and `bytes` fits the ring.
  ++TheStats.dropped_uploads;
  return false;
}

bool CreateScratch(Scratch &scratch, VkDeviceSize slice_bytes, VkBufferUsageFlags usage,
                   const char *what) {
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = slice_bytes * kScratchSlices;
  info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo alloc = {};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VmaAllocationInfo allocated = {};
  if (vmaCreateBuffer(Allocator, &info, &alloc, &scratch.buffer, &scratch.allocation,
                      &allocated) != VK_SUCCESS ||
      allocated.pMappedData == nullptr) {
    return Fail(std::string("could not allocate the ") + what + " scratch");
  }
  scratch.mapped = static_cast<uint8_t *>(allocated.pMappedData);
  scratch.slice = slice_bytes;

  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = scratch.buffer;
  scratch.address = vkGetBufferDeviceAddress(GetDevice(), &address);
  return true;
}

// A mip level's dimensions. D3D clamps to 1, and so does Vulkan, so this is the shared rule
// rather than either API's.
uint32_t LevelExtent(uint32_t base, uint32_t level) {
  const uint32_t v = base >> level;
  return v == 0 ? 1 : v;
}

// Packs one rectangle of a locked surface into tightly-packed destination rows.
//
// Tight rather than strided, so `bufferRowLength` can stay 0 and the copy region needs no
// stride arithmetic. It is also the single definition of what a texel becomes on the GPU,
// which is what lets VerifyImageLevel compare a readback against it without duplicating the
// conversion - a check that re-implemented the expansion would only ever agree with itself.
void PackLevel(const FormatMapping &map, const void *data, uint32_t pitch,
               uint32_t blocks_across, uint32_t rows, uint8_t *out) {
  const auto *src = static_cast<const uint8_t *>(data);
  const uint32_t dst_row_bytes = blocks_across * map.block_bytes;
  const uint32_t src_row_bytes = blocks_across * map.src_block_bytes;
  for (uint32_t row = 0; row < rows; ++row) {
    const uint8_t *src_row = src + VkDeviceSize(row) * pitch;
    uint8_t *dst_row = out + VkDeviceSize(row) * dst_row_bytes;
    if (map.expand_4444) {
      // ARGB4444 -> RGBA8888. Each nibble is replicated into a byte (0xf -> 0xff), which is
      // the exact widening for a UNORM: multiplying by 255/15.
      const auto *texels = reinterpret_cast<const uint16_t *>(src_row);
      for (uint32_t i = 0; i < blocks_across; ++i) {
        const uint16_t t = texels[i];
        const uint8_t a = (t >> 12) & 0xf;
        const uint8_t r = (t >> 8) & 0xf;
        const uint8_t g = (t >> 4) & 0xf;
        const uint8_t b = t & 0xf;
        dst_row[i * 4 + 0] = static_cast<uint8_t>(r | (r << 4));
        dst_row[i * 4 + 1] = static_cast<uint8_t>(g | (g << 4));
        dst_row[i * 4 + 2] = static_cast<uint8_t>(b | (b << 4));
        dst_row[i * 4 + 3] = static_cast<uint8_t>(a | (a << 4));
      }
    } else {
      std::memcpy(dst_row, src_row, src_row_bytes);
    }
  }
}

// Creates the one descriptor set the renderer uses. Two arrays, both UPDATE_AFTER_BIND so a
// texture created mid-frame can be written without rebinding, and both PARTIALLY_BOUND so the
// image array may have holes where textures have been destroyed.
bool CreateBindlessSet() {
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLER, kBindlessSamplers},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kBindlessTextures},
  };
  VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pool.maxSets = 1;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(GetDevice(), &pool, nullptr, &BindlessPool) != VK_SUCCESS) {
    return Fail("could not create the bindless descriptor pool");
  }

  const VkDescriptorBindingFlags binding_flags[] = {
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
      // VARIABLE_DESCRIPTOR_COUNT is legal only on the LAST binding, which is why the images
      // are binding 1 and the samplers binding 0 rather than the other way round.
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
  };
  VkDescriptorSetLayoutBindingFlagsCreateInfo flags = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flags.bindingCount = 2;
  flags.pBindingFlags = binding_flags;

  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_SAMPLER, kBindlessSamplers, VK_SHADER_STAGE_ALL, nullptr},
      {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kBindlessTextures, VK_SHADER_STAGE_ALL, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo layout = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout.pNext = &flags;
  layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layout.bindingCount = 2;
  layout.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(GetDevice(), &layout, nullptr, &BindlessLayout) !=
      VK_SUCCESS) {
    return Fail("could not create the bindless descriptor set layout");
  }

  uint32_t variable_count = kBindlessTextures;
  VkDescriptorSetVariableDescriptorCountAllocateInfo variable = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
  variable.descriptorSetCount = 1;
  variable.pDescriptorCounts = &variable_count;

  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.pNext = &variable;
  alloc.descriptorPool = BindlessPool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &BindlessLayout;
  if (vkAllocateDescriptorSets(GetDevice(), &alloc, &BindlessSet) != VK_SUCCESS) {
    return Fail("could not allocate the bindless descriptor set");
  }
  TheStats.descriptor_capacity = kBindlessTextures;
  return true;
}

// Writes one image into its own slot. Called on creation rather than per frame: the index is
// stable for the image's life, so the descriptor is written once and only rewritten if the slot
// is reused by a different image.
void WriteImageDescriptor(uint32_t index) {
  if (BindlessSet == VK_NULL_HANDLE) {
    return;
  }
  if (index >= kBindlessTextures) {
    ++TheStats.descriptors_out_of_range;
    return;
  }
  VkDescriptorImageInfo info = {};
  info.imageView = Images[index].view;
  info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = BindlessSet;
  write.dstBinding = 1;
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);
  ++TheStats.descriptors_written;
}

// D3DTEXTUREFILTERTYPE: 0 NONE, 1 POINT, 2 LINEAR, and 3/4/5 the anisotropic and gaussian
// variants the recorder never saw. Anything past LINEAR is treated as LINEAR rather than
// refused - a slightly wrong filter is a far smaller error than no texture at all, and unlike
// a format there is no way for it to corrupt anything.
VkFilter ToVkFilter(uint32_t d3d) { return d3d == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

VkSamplerMipmapMode ToVkMipmapMode(uint32_t d3d) {
  return d3d == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

// D3DTEXTUREADDRESS: 1 WRAP, 2 MIRROR, 3 CLAMP, 4 BORDER, 5 MIRRORONCE.
VkSamplerAddressMode ToVkAddressMode(uint32_t d3d) {
  switch (d3d) {
  case 2:
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case 3:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case 4:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  case 5:
    return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
  default:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

void NoteImageUse() {
  if (TheStats.image_bytes > TheStats.image_peak_bytes) {
    TheStats.image_peak_bytes = TheStats.image_bytes;
  }
}

void NoteArenaUse() {
  TheStats.vertex_used = VertexArena.used;
  TheStats.index_used = IndexArena.used;
  if (VertexArena.used > TheStats.vertex_peak) {
    TheStats.vertex_peak = VertexArena.used;
  }
  if (IndexArena.used > TheStats.index_peak) {
    TheStats.index_peak = IndexArena.used;
  }
}

} // namespace

bool StartResources() {
  if (Ready) {
    return true;
  }
  if (Initialize() != InitResult::Ok) {
    return Fail("no vulkan device");
  }
  ReadHeapMode();
  // Impossible to miss rather than silently corrected, because the symptom otherwise is a
  // capture that opens and shows resources with plausible-looking rubbish in them.
  if (RenderDocLoaded() && !SmallHeaps) {
    DebugWrite("gkplus: renderdoc is loaded with full-size heaps - an in-level capture will "
               "be missing some resources' initial state. Set GKPLUS_VK_HEAPS=small for a "
               "complete one (see vulkan_renderer_notes.md 4.17)\n");
  }

  const VkDeviceSize vertex_arena = SmallHeaps ? kSmallVertexArenaBytes : kVertexArenaBytes;
  const VkDeviceSize index_arena = SmallHeaps ? kSmallIndexArenaBytes : kIndexArenaBytes;
  const VkDeviceSize staging_bytes = SmallHeaps ? kSmallStagingBytes : kStagingBytes;
  const VkDeviceSize scratch_vertex =
      SmallHeaps ? kSmallScratchVertexBytes : kScratchVertexBytes;
  const VkDeviceSize scratch_index =
      SmallHeaps ? kSmallScratchIndexBytes : kScratchIndexBytes;
  const VkDeviceSize scratch_draw = SmallHeaps ? kSmallScratchDrawBytes : kScratchDrawBytes;
  const VkDeviceSize scratch_light = SmallHeaps ? kSmallScratchLightBytes : kScratchLightBytes;
  const VkDeviceSize scratch_material =
      SmallHeaps ? kSmallScratchMaterialBytes : kScratchMaterialBytes;

  VmaVulkanFunctions functions = {};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info = {};
  info.instance = GetInstance();
  info.physicalDevice = GetPhysicalDevice();
  info.device = GetDevice();
  info.vulkanApiVersion = VK_API_VERSION_1_3;
  info.pVulkanFunctions = &functions;
  // Must match the device feature VkContext enables, or every allocation that could back a
  // device address is rejected.
  info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

  if (vmaCreateAllocator(&info, &Allocator) != VK_SUCCESS) {
    return Fail("vmaCreateAllocator failed");
  }

  // The vertex arena aligns to a whole canonical vertex because a draw addresses its buffer as
  // a vertex index, not a byte offset. The index arena aligns to 16, which keeps it a multiple
  // of both index widths.
  if (!CreateArena(VertexArena, vertex_arena, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   sizeof(CanonicalVertex), "vertex") ||
      !CreateArena(IndexArena, index_arena, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 16, "index") ||
      !CreateScratch(ScratchVertices, scratch_vertex, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     "vertex") ||
      !CreateScratch(ScratchIndices, scratch_index, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     "index") ||
      !CreateScratch(ScratchDraws, scratch_draw, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     "draw record") ||
      !CreateScratch(ScratchLights, scratch_light, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     "light") ||
      !CreateScratch(ScratchMaterials, scratch_material, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     "material") ||
      !CreateBindlessSet()) {
    return false;
  }
  TheStats.scratch_vertex_bytes = scratch_vertex;
  TheStats.scratch_index_bytes = scratch_index;
  TheStats.scratch_draw_bytes = scratch_draw;
  TheStats.scratch_light_bytes = scratch_light;
  TheStats.scratch_material_bytes = scratch_material;

  VkBufferCreateInfo staging = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  staging.size = staging_bytes;
  staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  staging.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo staging_alloc = {};
  staging_alloc.usage = VMA_MEMORY_USAGE_AUTO;
  staging_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VmaAllocationInfo staging_info = {};
  if (vmaCreateBuffer(Allocator, &staging, &staging_alloc, &Ring.buffer, &Ring.allocation,
                      &staging_info) != VK_SUCCESS) {
    return Fail("could not allocate the staging ring");
  }
  Ring.mapped = static_cast<uint8_t *>(staging_info.pMappedData);
  Ring.capacity = staging_bytes;
  if (Ring.mapped == nullptr) {
    return Fail("staging ring came back unmapped");
  }

  VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool.queueFamilyIndex = Caps().graphics_queue_family;
  if (vkCreateCommandPool(GetDevice(), &pool, nullptr, &UploadPool) != VK_SUCCESS) {
    return Fail("could not create the upload command pool");
  }
  VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(GetDevice(), &fence, nullptr, &UploadFence) != VK_SUCCESS) {
    return Fail("could not create the upload fence");
  }

  Ready = true;
  Error.clear();
  TheStats.ready = true;
  TheStats.vertex_arena_bytes = vertex_arena;
  TheStats.index_arena_bytes = index_arena;
  {
    char watch[32] = {};
    if (::GetEnvironmentVariableA("GKPLUS_VK_WATCH_DST", watch, sizeof(watch)) != 0) {
      WatchDst = static_cast<uint32_t>(std::strtoul(watch, nullptr, 0));
    }
  }
  TheStats.staging_bytes = staging_bytes;
  TheStats.small_heaps = SmallHeaps;
  char line[160];
  std::snprintf(line, sizeof(line),
                "gkplus: vulkan arenas up (%s): %llu MB vertex, %llu MB index, "
                "%llu MB staging\n",
                SmallHeaps ? "small" : "full", (unsigned long long)(vertex_arena >> 20),
                (unsigned long long)(index_arena >> 20),
                (unsigned long long)(staging_bytes >> 20));
  DebugWrite(line);
  return true;
}

bool ResourcesReady() { return Ready; }

BufferSlot AllocateSlot(uint32_t bytes, bool vertex) {
  BufferSlot slot;
  if (!Ready || bytes == 0) {
    return slot;
  }
  VkDeviceSize offset = 0;
  if (!Suballocate(vertex ? VertexArena : IndexArena, bytes, offset)) {
    return slot;
  }
  slot.offset = static_cast<uint32_t>(offset);
  slot.bytes = bytes;
  slot.vertex = vertex;
  slot.valid = true;
  ++TheStats.slots_live;
  NoteArenaUse();
  return slot;
}

void FreeSlot(const BufferSlot &slot) {
  if (!Ready || !slot.valid) {
    return;
  }
  Release(slot.vertex ? VertexArena : IndexArena, slot.offset, slot.bytes);
  --TheStats.slots_live;
  NoteArenaUse();
}

// Records a destination range in this batch and says whether it collides with one already
// queued - i.e. whether the copy about to be pushed needs a barrier before it. See
// PendingDstRanges for why an unordered overlap is a real corruption rather than a nicety.
//
// On a collision the map is cleared rather than pruned: the barrier orders EVERY earlier copy
// in the batch, so none of them can collide with anything that follows.
bool NoteDestination(bool vertex, VkDeviceSize offset, VkDeviceSize bytes) {
  auto &ranges = PendingDstRanges[vertex ? 0 : 1];
  const VkDeviceSize end = offset + bytes;
  bool overlaps = false;
  // The first range starting at or after this one, and the one before it - between them they
  // are the only two that can overlap a range in a map of disjoint... they are not disjoint,
  // since ranges accumulate, so both neighbours are checked and that is enough for the shapes
  // this produces: a slot is written either whole or as one sub-range.
  auto after = ranges.lower_bound(offset);
  if (after != ranges.end() && after->first < end) {
    overlaps = true;
  }
  if (!overlaps && after != ranges.begin()) {
    auto before = std::prev(after);
    if (before->second > offset) {
      overlaps = true;
    }
  }
  if (overlaps) {
    ++TheStats.ordered_overlapping_copies;
    ranges.clear();
    ranges.emplace(offset, end);
    return true;
  }
  auto [it, inserted] = ranges.emplace(offset, end);
  if (!inserted && it->second < end) {
    it->second = end;
  }
  return false;
}

bool UploadIntoSlot(const BufferSlot &slot, uint32_t offset_in_slot, const void *data,
                    uint32_t bytes) {
  if (!Ready || !slot.valid || data == nullptr || bytes == 0) {
    return false;
  }
  // Never write past the slot: a larger write would silently corrupt whichever buffer
  // happens to sit next in the arena, which would show up as unrelated geometry going wrong.
  if (offset_in_slot >= slot.bytes) {
    return false;
  }
  if (offset_in_slot + bytes > slot.bytes) {
    bytes = slot.bytes - offset_in_slot;
  }
  VkDeviceSize staging_offset = 0;
  if (!AllocateStaging(bytes, staging_offset)) {
    return false;
  }
  std::memcpy(Ring.mapped + staging_offset, data, bytes);
  const bool needs_barrier =
      NoteDestination(slot.vertex, slot.offset + offset_in_slot, bytes);
  if (slot.vertex && slot.offset == WatchDst) {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "  batch %u: slot %u + %u, %u bytes, staged at %llu\n", StagingBatch,
                  slot.offset, offset_in_slot, bytes,
                  (unsigned long long)staging_offset);
    WatchLog += line;
  }
  Pending.push_back({slot.vertex ? VertexArena.buffer : IndexArena.buffer, staging_offset,
                     slot.offset + offset_in_slot, bytes, needs_barrier});
  ++TheStats.uploads;
  TheStats.uploaded_bytes += bytes;
  return true;
}

bool TextureFormatBlock(uint32_t d3d_format, uint32_t &block, uint32_t &block_bytes) {
  FormatMapping mapping;
  if (!MapFormat(d3d_format, mapping)) {
    return false;
  }
  block = mapping.block;
  block_bytes = mapping.src_block_bytes;
  return true;
}

bool CreateTextureImage(TextureImage &image, uint32_t width, uint32_t height, uint32_t levels,
                        uint32_t d3d_format) {
  image = TextureImage();
  FormatMapping mapping;
  if (!Ready || width == 0 || height == 0) {
    return false;
  }
  if (!MapFormat(d3d_format, mapping)) {
    ++TheStats.unsupported_formats;
    return false;
  }
  if (levels == 0) {
    levels = 1; // D3D's "make a full chain"; the game always passes an explicit count
  }

  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = mapping.format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_SRC is not for the renderer - nothing copies out of these in a frame. It exists so
  // VerifyImageLevel can read one back, and leaving it out made every readback an invalid
  // vkCmdCopyImageToBuffer: the check reported mismatches that were its own. A verifier needs
  // verifying, and the validation layer is what did it.
  info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo alloc = {};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;

  Image entry;
  VmaAllocationInfo allocated = {};
  if (vmaCreateImage(Allocator, &info, &alloc, &entry.image, &entry.allocation, &allocated) !=
      VK_SUCCESS) {
    return false;
  }

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = entry.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = mapping.format;
  // D3DFMT_A8 is alpha-only, and R8_UNORM is red-only. The swizzle is what makes the two the
  // same thing at sample time, rather than a shader branch per texture.
  //
  // **RGB is ZERO, not ONE.** D3D reads a channel a format does not carry as 0, except alpha,
  // which reads as 1 - so an alpha-only texture samples as (0, 0, 0, a). The ONE this replaces
  // was invisible while only stage 0 was drawn (§4.16 modulated by a white RGB, which is the
  // identity) and inverted the whole scene the moment stage 1 arrived: level01's second stage
  // blends toward the fog with BLENDTEXTUREALPHA, so the distance faded to white instead of to
  // black (§4.19).
  if (mapping.alpha_swizzle) {
    view.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                       VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
  }
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  if (vkCreateImageView(GetDevice(), &view, nullptr, &entry.view) != VK_SUCCESS) {
    vmaDestroyImage(Allocator, entry.image, entry.allocation);
    return false;
  }

  entry.width = width;
  entry.height = height;
  entry.levels = levels;
  entry.d3d_format = d3d_format;
  entry.mapping = mapping;
  entry.bytes = allocated.size;
  entry.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  entry.live = true;

  uint32_t index;
  if (!FreeImageIndices.empty()) {
    index = FreeImageIndices.back();
    FreeImageIndices.pop_back();
    Images[index] = entry;
  } else {
    index = static_cast<uint32_t>(Images.size());
    Images.push_back(entry);
  }

  image.index = index;
  image.valid = true;
  ++ImageRegistryGeneration;
  ++TheStats.images_live;
  ++TheStats.images_created;
  TheStats.image_bytes += entry.bytes;
  NoteImageUse();
  // Written once, here, rather than per frame: the index is stable for the image's life, so
  // the only thing that can invalidate the descriptor is the slot being reused - which is this
  // same path, for the next image.
  WriteImageDescriptor(index);
  return true;
}

void DestroyTextureImage(TextureImage &image) {
  if (!Ready || !image.valid || image.index >= Images.size()) {
    image = TextureImage();
    return;
  }
  Image &entry = Images[image.index];
  if (entry.live) {
    // The image may still be referenced by a copy this frame recorded but has not submitted,
    // and by the frame in flight before it. Waiting is heavy-handed but this happens at level
    // teardown, not per frame - level01 destroys 65 images in one burst and then none.
    vkDeviceWaitIdle(GetDevice());
    // Anything still queued for this image would now write a destroyed handle.
    for (size_t i = PendingImages.size(); i-- > 0;) {
      if (PendingImages[i].index == image.index) {
        PendingImages.erase(PendingImages.begin() + i);
      }
    }
    vkDestroyImageView(GetDevice(), entry.view, nullptr);
    vmaDestroyImage(Allocator, entry.image, entry.allocation);
    TheStats.image_bytes -= entry.bytes;
    --TheStats.images_live;
    entry = Image();
    FreeImageIndices.push_back(image.index);
    ++ImageRegistryGeneration;
    // The descriptor is deliberately NOT cleared, and the reasoning matters for Phase 3.
    // PARTIALLY_BOUND makes a stale descriptor legal to *have*; what would be undefined is
    // reading one, and nothing can: a draw only ever names a slot through the texture the
    // shadow state has bound, which is by definition live. The slot is rewritten when it is
    // reused. If a draw record is ever built that outlives its texture, this becomes a real
    // dangling reference and the descriptor must be nulled here instead.
  }
  image = TextureImage();
}

uint32_t AcquireSampler(uint32_t mag_filter, uint32_t min_filter, uint32_t mip_filter,
                        uint32_t address_u, uint32_t address_v) {
  const SamplerKey key{mag_filter, min_filter, mip_filter, address_u, address_v};
  const auto found = SamplerIndices.find(key);
  if (found != SamplerIndices.end()) {
    return found->second;
  }
  if (!Ready || Samplers.size() >= kBindlessSamplers) {
    return 0;
  }

  VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = ToVkFilter(mag_filter);
  info.minFilter = ToVkFilter(min_filter);
  info.mipmapMode = ToVkMipmapMode(mip_filter);
  info.addressModeU = ToVkAddressMode(address_u);
  info.addressModeV = ToVkAddressMode(address_v);
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  // The engine's textures carry their own mip chains and it never sets a LOD bias other than
  // 0 (stage state 19 has exactly one distinct value), so there is nothing to reproduce here.
  //
  // D3DTEXF_NONE as the MIP filter is not "some filter we do not model" - it means **do not
  // mipmap**, sample level 0 whatever the footprint. Vulkan has no mipmapMode that says so, so
  // it is a LOD clamp: without this a minified draw walks down the chain and comes out blurred,
  // which reads as a filtering difference rather than as a missing state (§4.28).
  info.maxLod = mip_filter == 0 /* D3DTEXF_NONE */ ? 0.25f : VK_LOD_CLAMP_NONE;
  VkSampler sampler = VK_NULL_HANDLE;
  if (vkCreateSampler(GetDevice(), &info, nullptr, &sampler) != VK_SUCCESS) {
    return 0;
  }

  const uint32_t index = static_cast<uint32_t>(Samplers.size());
  Samplers.push_back(sampler);
  SamplerIndices[key] = index;
  TheStats.samplers_live = Samplers.size();

  if (BindlessSet != VK_NULL_HANDLE) {
    VkDescriptorImageInfo image_info = {};
    image_info.sampler = sampler;
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = BindlessSet;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);
    ++TheStats.descriptors_written;
  }
  return index;
}

void RotateFrameScratch() {
  if (!Ready) {
    return;
  }
  ScratchSlice = (ScratchSlice + 1) % kScratchSlices;
  for (Scratch *scratch :
       {&ScratchVertices, &ScratchIndices, &ScratchDraws, &ScratchLights, &ScratchMaterials}) {
    scratch->base = scratch->slice * ScratchSlice;
    scratch->head = 0;
  }
}

void ResetFrameScratch() {
  if (!Ready) {
    return;
  }
  for (Scratch *scratch :
       {&ScratchVertices, &ScratchIndices, &ScratchDraws, &ScratchLights, &ScratchMaterials}) {
    scratch->head = 0;
  }
}

namespace {

// Bump-allocates within the current frame's slice. `element` is the stride, so the returned
// offset can be in elements - which is what both a vertex index and a first-index want.
ScratchAlloc AllocateScratch(Scratch &scratch, uint32_t count, uint32_t element,
                             uint64_t &peak) {
  ScratchAlloc out;
  if (!Ready || scratch.mapped == nullptr || count == 0) {
    return out;
  }
  // Aligned to the element so the returned offset is exact, and to 16 for the vertex case so a
  // device address stays naturally aligned - element is 48 there, which covers both.
  const VkDeviceSize head = ((scratch.head + element - 1) / element) * element;
  const VkDeviceSize bytes = VkDeviceSize(count) * element;
  if (head + bytes > scratch.slice) {
    ++TheStats.scratch_exhausted;
    return out;
  }
  out.mapped = scratch.mapped + scratch.base + head;
  out.offset = static_cast<uint32_t>(head / element);
  out.valid = true;
  scratch.head = head + bytes;
  if (scratch.head > peak) {
    peak = scratch.head;
  }
  return out;
}

} // namespace

ScratchAlloc AllocateScratchVertices(uint32_t count) {
  return AllocateScratch(ScratchVertices, count,
                         static_cast<uint32_t>(sizeof(CanonicalVertex)),
                         TheStats.scratch_vertices_peak);
}

ScratchAlloc AllocateScratchIndices(uint32_t count, uint32_t stride) {
  ScratchAlloc out =
      AllocateScratch(ScratchIndices, count, stride, TheStats.scratch_indices_peak);
  // Made absolute from the buffer's start, unlike the vertex case. Vertices are reached through
  // an address that already has the frame's slice folded in; indices go through
  // vkCmdBindIndexBuffer, which the renderer binds at offset 0, so the first-index has to carry
  // the slice itself. The slice size is a power of two, so it divides both index widths.
  if (out.valid) {
    out.offset += static_cast<uint32_t>(ScratchIndices.base / stride);
  }
  return out;
}

ScratchAlloc AllocateScratchDraws(uint32_t count) {
  return AllocateScratch(ScratchDraws, count, static_cast<uint32_t>(sizeof(GpuDrawRecord)),
                         TheStats.scratch_draws_peak);
}

ScratchAlloc AllocateScratchLights(uint32_t count) {
  return AllocateScratch(ScratchLights, count, static_cast<uint32_t>(sizeof(GpuLight)),
                         TheStats.scratch_lights_peak);
}

ScratchAlloc AllocateScratchMaterials(uint32_t count) {
  return AllocateScratch(ScratchMaterials, count, static_cast<uint32_t>(sizeof(GpuMaterial)),
                         TheStats.scratch_materials_peak);
}

uint64_t ScratchVertexAddress() {
  return Ready ? ScratchVertices.address + ScratchVertices.base : 0;
}

uint64_t ScratchIndexBuffer() {
  return Ready ? reinterpret_cast<uint64_t>(ScratchIndices.buffer) : 0;
}

// Both carry the current slice, like the vertex address and unlike the index one: they are read
// by device address rather than bound, so the offsets in a DrawItem stay slice-relative.
uint64_t ScratchDrawAddress() {
  return Ready ? ScratchDraws.address + ScratchDraws.base : 0;
}

uint64_t ScratchLightAddress() {
  return Ready ? ScratchLights.address + ScratchLights.base : 0;
}

uint64_t ScratchMaterialAddress() {
  return Ready ? ScratchMaterials.address + ScratchMaterials.base : 0;
}

const void *ScratchVertexMapped() {
  return Ready && ScratchVertices.mapped != nullptr
             ? ScratchVertices.mapped + ScratchVertices.base
             : nullptr;
}

const void *ScratchIndexMappedBase() {
  return Ready ? ScratchIndices.mapped : nullptr;
}

uint64_t VertexArenaAddress() {
  if (!Ready || VertexArena.buffer == VK_NULL_HANDLE) {
    return 0;
  }
  VkBufferDeviceAddressInfo info = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = VertexArena.buffer;
  return vkGetBufferDeviceAddress(GetDevice(), &info);
}

uint64_t IndexArenaBuffer() {
  return Ready ? reinterpret_cast<uint64_t>(IndexArena.buffer) : 0;
}

uint64_t BindlessDescriptorSet() { return reinterpret_cast<uint64_t>(BindlessSet); }

uint64_t BindlessDescriptorSetLayout() {
  return reinterpret_cast<uint64_t>(BindlessLayout);
}

void NameTextureImage(const TextureImage &image, const std::string &name) {
  if (Ready && image.valid && image.index < Images.size() && Images[image.index].live) {
    Images[image.index].name = name;
    ++ImageRegistryGeneration;
  }
}

uint64_t TextureRegistryGeneration() { return ImageRegistryGeneration; }

std::vector<TextureImageInfo> TextureImages() {
  std::vector<TextureImageInfo> out;
  for (uint32_t i = 0; i < Images.size(); ++i) {
    const Image &entry = Images[i];
    if (!entry.live) {
      continue;
    }
    TextureImageInfo info;
    info.index = i;
    info.name = entry.name;
    info.width = entry.width;
    info.height = entry.height;
    info.levels = entry.levels;
    info.d3d_format = entry.d3d_format;
    info.bytes = entry.bytes;
    out.push_back(info);
  }
  return out;
}

bool UploadIntoTextureImage(const TextureImage &image, uint32_t level, int32_t x, int32_t y,
                            uint32_t width, uint32_t height, const void *data,
                            uint32_t pitch) {
  if (!Ready || !image.valid || image.index >= Images.size() || data == nullptr ||
      width == 0 || height == 0) {
    return false;
  }
  Image &entry = Images[image.index];
  if (!entry.live || level >= entry.levels) {
    return false;
  }
  const FormatMapping &map = entry.mapping;

  // A compressed image is addressed in blocks, and a rectangle that does not land on block
  // boundaries cannot be expressed as a copy at all. D3D requires the same alignment, so this
  // should be unreachable - but copying a misaligned rect would silently corrupt the blocks
  // either side, so it is refused and counted.
  if (map.block > 1 && ((x % static_cast<int32_t>(map.block)) != 0 ||
                        (y % static_cast<int32_t>(map.block)) != 0 ||
                        (width % map.block) != 0 || (height % map.block) != 0)) {
    ++TheStats.unaligned_rects;
    return false;
  }
  // Except at the edge of a level whose size is not a multiple of the block, where D3D rounds
  // up and so does Vulkan.
  const uint32_t level_w = LevelExtent(entry.width, level);
  const uint32_t level_h = LevelExtent(entry.height, level);
  if (static_cast<uint32_t>(x) + width > level_w) {
    width = level_w - static_cast<uint32_t>(x);
  }
  if (static_cast<uint32_t>(y) + height > level_h) {
    height = level_h - static_cast<uint32_t>(y);
  }

  const uint32_t rows = (height + map.block - 1) / map.block;
  const uint32_t blocks_across = (width + map.block - 1) / map.block;
  const uint32_t dst_row_bytes = blocks_across * map.block_bytes;
  const VkDeviceSize total = VkDeviceSize(dst_row_bytes) * rows;

  VkDeviceSize staging_offset = 0;
  if (!AllocateStaging(total, staging_offset)) {
    ++TheStats.image_uploads_dropped;
    return false;
  }
  PackLevel(map, data, pitch, blocks_across, rows, Ring.mapped + staging_offset);

  const uint64_t key = (uint64_t(image.index) << 32) | level;
  const bool repeat = !PendingImageLevels.insert(key).second;
  if (repeat) {
    ++TheStats.ordered_overlapping_copies;
    PendingImageLevels.clear();
    PendingImageLevels.insert(key);
  }
  PendingImages.push_back(
      {image.index, staging_offset, level, x, y, width, height, repeat});
  ++TheStats.image_uploads;
  TheStats.image_uploaded_bytes += total;
  return true;
}

void FlushUploads() {
  if (Ready) {
    FlushPendingNow();
  }
}

// Reads a whole mip level back off the GPU and compares it against what `data` should have
// become. This is the only thing that checks the *contents* rather than the plumbing: every
// counter in ResourceStats can read perfectly while the image holds the wrong bytes, and the
// whole point of section 4.12 was to avoid building on a texture path nobody had verified.
//
// The expected bytes come from PackLevel, the same function the upload uses, so this compares
// "what the GPU holds" against "what we meant to send" - a swapped channel or a wrong VkFormat
// would show up as a mismatch, a bad layout transition or a lost copy as a difference too.
bool VerifyImageLevel(const TextureImage &image, uint32_t level, const void *data,
                      uint32_t pitch, uint64_t *differing_bytes, uint64_t *first_difference,
                      uint64_t *total_bytes) {
  if (!Ready || !image.valid || image.index >= Images.size() || data == nullptr) {
    return false;
  }
  const Image &entry = Images[image.index];
  if (!entry.live || level >= entry.levels) {
    return false;
  }
  const FormatMapping &map = entry.mapping;
  const uint32_t level_w = LevelExtent(entry.width, level);
  const uint32_t level_h = LevelExtent(entry.height, level);
  const uint32_t blocks_across = (level_w + map.block - 1) / map.block;
  const uint32_t rows = (level_h + map.block - 1) / map.block;
  const VkDeviceSize total = VkDeviceSize(blocks_across) * map.block_bytes * rows;

  // Its own buffer rather than the staging ring: that one is HOST_ACCESS_SEQUENTIAL_WRITE and
  // reading from write-combined memory is correct but pathologically slow.
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = total;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo alloc = {};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer readback = VK_NULL_HANDLE;
  VmaAllocation readback_alloc = VK_NULL_HANDLE;
  VmaAllocationInfo readback_info = {};
  if (vmaCreateBuffer(Allocator, &info, &alloc, &readback, &readback_alloc,
                      &readback_info) != VK_SUCCESS ||
      readback_info.pMappedData == nullptr) {
    return false;
  }

  bool equal = false;
  VkCommandBufferAllocateInfo cmd_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = UploadPool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(GetDevice(), &cmd_alloc, &cmd) == VK_SUCCESS) {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // SHADER_READ_ONLY is where every image sits between frames; anything still queued for
    // this one is irrelevant because we compare against what is already there.
    auto transition = [&](VkImageLayout from, VkImageLayout to) {
      VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      barrier.oldLayout = from;
      barrier.newLayout = to;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = entry.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, entry.levels, 0, 1};
      VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.imageMemoryBarrierCount = 1;
      dependency.pImageMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dependency);
    };
    transition(entry.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {level_w, level_h, 1};
    vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                           &region);
    transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.layout);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkResetFences(GetDevice(), 1, &UploadFence);
    if (vkQueueSubmit(GetGraphicsQueue(), 1, &submit, UploadFence) == VK_SUCCESS) {
      vkWaitForFences(GetDevice(), 1, &UploadFence, VK_TRUE, UINT64_MAX);
      std::vector<uint8_t> expected(static_cast<size_t>(total));
      PackLevel(map, data, pitch, blocks_across, rows, expected.data());
      const auto *actual = static_cast<const uint8_t *>(readback_info.pMappedData);
      uint64_t differing = 0;
      uint64_t first = total;
      for (VkDeviceSize i = 0; i < total; ++i) {
        if (expected[static_cast<size_t>(i)] != actual[i]) {
          ++differing;
          if (first == total) {
            first = i;
          }
        }
      }
      equal = differing == 0;
      if (differing_bytes != nullptr) {
        *differing_bytes = differing;
      }
      if (first_difference != nullptr) {
        *first_difference = first;
      }
      if (total_bytes != nullptr) {
        *total_bytes = total;
      }
    }
    vkFreeCommandBuffers(GetDevice(), UploadPool, 1, &cmd);
  }

  vmaDestroyBuffer(Allocator, readback, readback_alloc);
  return equal;
}

// Reads a slot back off the GPU and compares it against what the arena should hold. The buffer
// half of VerifyImageLevel, and it exists for the same reason: every counter in ResourceStats
// can read perfectly while a slot holds another buffer's bytes. A draw reads its vertices and
// indices by offset into one big arena, so a slot that took the wrong data does not fail - it
// draws somebody else's geometry, or, through a corrupt index, a triangle stretched across the
// screen.
// Copies `bytes` out of one arena at an ABSOLUTE offset. The unit both VerifySlot and ReadArena
// are built on, and the reason it takes an offset rather than a slot: a draw addresses the arena
// by absolute offset, so "what does this draw actually read" is a question about the arena and
// not about any particular slot - which is exactly the question VerifySlot cannot answer.
bool CopyOutOfArena(bool vertex, uint64_t offset, uint32_t bytes, void *out) {
  if (!Ready || out == nullptr || bytes == 0) {
    return false;
  }
  const Arena &arena = vertex ? VertexArena : IndexArena;
  if (arena.buffer == VK_NULL_HANDLE || offset + bytes > arena.capacity) {
    return false;
  }

  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo alloc = {};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  alloc.flags =
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer readback = VK_NULL_HANDLE;
  VmaAllocation readback_alloc = VK_NULL_HANDLE;
  VmaAllocationInfo readback_info = {};
  if (vmaCreateBuffer(Allocator, &info, &alloc, &readback, &readback_alloc, &readback_info) !=
          VK_SUCCESS ||
      readback_info.pMappedData == nullptr) {
    return false;
  }

  bool ok = false;
  VkCommandBufferAllocateInfo cmd_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = UploadPool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(GetDevice(), &cmd_alloc, &cmd) == VK_SUCCESS) {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkBufferCopy region = {offset, 0, bytes};
    vkCmdCopyBuffer(cmd, arena.buffer, readback, 1, &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkResetFences(GetDevice(), 1, &UploadFence);
    if (vkQueueSubmit(GetGraphicsQueue(), 1, &submit, UploadFence) == VK_SUCCESS) {
      vkWaitForFences(GetDevice(), 1, &UploadFence, VK_TRUE, UINT64_MAX);
      std::memcpy(out, readback_info.pMappedData, bytes);
      ok = true;
    }
    vkFreeCommandBuffers(GetDevice(), UploadPool, 1, &cmd);
  }
  vmaDestroyBuffer(Allocator, readback, readback_alloc);
  return ok;
}

bool ReadArena(bool vertex, uint64_t offset, uint32_t bytes, void *out) {
  FlushUploads();
  return CopyOutOfArena(vertex, offset, bytes, out);
}

bool VerifySlot(const BufferSlot &slot, const void *expected, uint32_t bytes,
                uint64_t *differing_bytes, uint64_t *first_difference, uint8_t *got_prefix) {
  if (!Ready || !slot.valid || expected == nullptr || bytes == 0 || bytes > slot.bytes) {
    return false;
  }
  const Arena &arena = slot.vertex ? VertexArena : IndexArena;
  if (arena.buffer == VK_NULL_HANDLE) {
    return false;
  }

  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo alloc = {};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  alloc.flags =
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer readback = VK_NULL_HANDLE;
  VmaAllocation readback_alloc = VK_NULL_HANDLE;
  VmaAllocationInfo readback_info = {};
  if (vmaCreateBuffer(Allocator, &info, &alloc, &readback, &readback_alloc, &readback_info) !=
          VK_SUCCESS ||
      readback_info.pMappedData == nullptr) {
    return false;
  }

  bool equal = false;
  VkCommandBufferAllocateInfo cmd_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = UploadPool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(GetDevice(), &cmd_alloc, &cmd) == VK_SUCCESS) {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkBufferCopy region = {slot.offset, 0, bytes};
    vkCmdCopyBuffer(cmd, arena.buffer, readback, 1, &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkResetFences(GetDevice(), 1, &UploadFence);
    if (vkQueueSubmit(GetGraphicsQueue(), 1, &submit, UploadFence) == VK_SUCCESS) {
      vkWaitForFences(GetDevice(), 1, &UploadFence, VK_TRUE, UINT64_MAX);
      const auto *got = static_cast<const uint8_t *>(readback_info.pMappedData);
      const auto *want = static_cast<const uint8_t *>(expected);
      uint64_t differing = 0;
      uint64_t first = bytes;
      for (uint32_t i = 0; i < bytes; ++i) {
        if (got[i] != want[i]) {
          if (differing == 0) {
            first = i;
          }
          ++differing;
        }
      }
      equal = differing == 0;
      if (differing_bytes != nullptr) {
        *differing_bytes = differing;
      }
      if (first_difference != nullptr) {
        *first_difference = first;
      }
      // What the arena actually holds at the first difference, which separates two very
      // different failures: plausible geometry says another buffer's bytes landed here, zeros
      // or noise says the copy never happened at all.
      if (got_prefix != nullptr && first < bytes) {
        const uint32_t n = bytes - static_cast<uint32_t>(first) < 32u
                               ? bytes - static_cast<uint32_t>(first)
                               : 32u;
        std::memcpy(got_prefix, got + first, n);
      }
    }
    vkFreeCommandBuffers(GetDevice(), UploadPool, 1, &cmd);
  }

  vmaDestroyBuffer(Allocator, readback, readback_alloc);
  return equal;
}

const std::string &StagingWatchLog() { return WatchLog; }

void RecordUploads(void *command_buffer, uint32_t frame_index) {
  if (!Ready || (Pending.empty() && PendingImages.empty())) {
    return;
  }
  RecordInto(static_cast<VkCommandBuffer>(command_buffer));
  if (frame_index < kFramesInFlight) {
    Ring.frame_live[frame_index] = true;
    Ring.frame_bytes[frame_index] += LastRecordedBytes;
    Ring.in_flight += LastRecordedBytes;
  }
}

void ReleaseFrameStaging(uint32_t frame_index) {
  // Called once the renderer has waited on this slot's fence, so everything it staged has been
  // read and the ring may hand those bytes back.
  if (Ready && frame_index < kFramesInFlight) {
    Ring.frame_live[frame_index] = false;
    Ring.in_flight -= Ring.frame_bytes[frame_index];
    Ring.frame_bytes[frame_index] = 0;
  }
}

namespace {

// Records everything queued, with one barrier pair around the whole batch. Shared by the
// frame path and by the mid-batch flush, so the two cannot drift.
void RecordInto(VkCommandBuffer cmd) {
  // The ring is host-written and GPU-read, and nothing else makes those writes visible: on a
  // memory type that is not HOST_COHERENT the CPU's stores sit in its cache and the copy reads
  // whatever was there before. VMA turns this into a no-op when the type is coherent, so it
  // costs nothing to be right about it.
  vmaFlushAllocation(Allocator, Ring.allocation, 0, VK_WHOLE_SIZE);
  for (const PendingCopy &copy : Pending) {
    if (copy.barrier_before) {
      // This copy overwrites bytes an earlier one in this batch also wrote, and two transfers
      // in a command buffer are not ordered against each other. Without this the later upload
      // does not reliably win, which is the whole defect - see PendingDstRanges.
      VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
      VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dependency);
    }
    VkBufferCopy region = {copy.src_offset, copy.dst_offset, copy.bytes};
    vkCmdCopyBuffer(cmd, Ring.buffer, copy.dst, 1, &region);
  }

  // Images need a layout on either side of their copies, which buffers do not. Collected into
  // two batched barriers rather than one pair per image: several blits usually land on the
  // same texture in a frame, and a per-copy transition would also force each into
  // SHADER_READ_ONLY only to pull it straight back out.
  std::vector<VkImageMemoryBarrier2> to_transfer;
  std::vector<VkImageMemoryBarrier2> to_read;
  for (const PendingImageCopy &copy : PendingImages) {
    Image &entry = Images[copy.index];
    if (entry.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
      continue; // already in this batch
    }
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = entry.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, entry.levels, 0, 1};
    to_transfer.push_back(barrier);

    VkImageMemoryBarrier2 back = barrier;
    back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    back.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    back.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.push_back(back);

    entry.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }

  if (!to_transfer.empty()) {
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(to_transfer.size());
    dependency.pImageMemoryBarriers = to_transfer.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
  }

  for (const PendingImageCopy &copy : PendingImages) {
    const Image &entry = Images[copy.index];
    if (copy.barrier_before) {
      VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
      VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dependency);
    }
    VkBufferImageCopy region = {};
    region.bufferOffset = copy.src_offset;
    // 0/0 mean "tightly packed", which is what UploadIntoTextureImage guarantees.
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, copy.level, 0, 1};
    region.imageOffset = {copy.x, copy.y, 0};
    region.imageExtent = {copy.width, copy.height, 1};
    vkCmdCopyBufferToImage(cmd, Ring.buffer, entry.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }

  // One barrier for the whole buffer batch rather than one per copy: 75 uploads a frame would
  // otherwise be 75 pipeline barriers for no benefit, since they all target the same two
  // buffers and are all read at the same stage.
  VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;

  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  if (!to_read.empty()) {
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(to_read.size());
    dependency.pImageMemoryBarriers = to_read.data();
  }
  vkCmdPipelineBarrier2(cmd, &dependency);

  for (const PendingImageCopy &copy : PendingImages) {
    Images[copy.index].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  Pending.clear();
  PendingImages.clear();
  PendingImageLevels.clear();
  for (auto &ranges : PendingDstRanges) {
    ranges.clear();
  }
  // The batch is not released here: its bytes are only free once the GPU has run these copies.
  // The caller moves them into `frame_bytes` (the frame path, retired by that frame's fence) or
  // waits for them itself (FlushPendingNow).
  LastRecordedBytes = Ring.batch;
  Ring.batch = 0;
  ++StagingBatch;
}

// Records and submits everything queued right now, on a throwaway command buffer, and waits.
//
// Only ever called from AllocateStaging, when the un-recorded batch has grown to the size of
// the whole ring - which in practice means a level load. Once the wait returns, every staged
// byte has been consumed, so the ring is entirely free again.
bool FlushPendingNow() {
  if (Pending.empty() && PendingImages.empty()) {
    return false; // nothing to reclaim, so wrapping would still corrupt
  }
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = UploadPool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(GetDevice(), &alloc, &cmd) != VK_SUCCESS) {
    return false;
  }

  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  const uint32_t recorded_batch = StagingBatch;
  RecordInto(cmd);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkResetFences(GetDevice(), 1, &UploadFence);
  const bool ok =
      vkQueueSubmit(GetGraphicsQueue(), 1, &submit, UploadFence) == VK_SUCCESS;
  if (ok) {
    vkWaitForFences(GetDevice(), 1, &UploadFence, VK_TRUE, UINT64_MAX);
  }
  // After the wait, so the capture contains the copies AND their completion.
  EndBatchCaptureIfArmed(recorded_batch);
  vkFreeCommandBuffers(GetDevice(), UploadPool, 1, &cmd);
  if (ok) {
    // The wait above covers only what this call submitted, so anything a frame still holds has
    // to be waited for separately before the head may rewind over it.
    LastRecordedBytes = 0;
    WaitForLiveFrames();
    Ring.head = 0;
    ++TheStats.staging_flushes;
  }
  return ok;
}

} // namespace

const ResourceStats &Resources() { return TheStats; }

const std::string &ResourceError() { return Error; }

std::string FormatResourceStats() {
  std::string out;
  char line[256];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  add("arenas: %s (%s)\n", Ready ? "up" : "down",
      TheStats.small_heaps ? "SMALL heaps - not the configuration that normally runs"
                           : "full heaps");
  if (!Error.empty()) {
    out += "error: " + Error + "\n";
  }
  if (!Ready) {
    return out;
  }
  add("vertex: %llu live / %llu peak / %llu KB   index: %llu / %llu / %llu KB\n",
      (unsigned long long)(TheStats.vertex_used >> 10),
      (unsigned long long)(TheStats.vertex_peak >> 10),
      (unsigned long long)(TheStats.vertex_arena_bytes >> 10),
      (unsigned long long)(TheStats.index_used >> 10),
      (unsigned long long)(TheStats.index_peak >> 10),
      (unsigned long long)(TheStats.index_arena_bytes >> 10));
  add("slots live: %llu   staging: %llu KB\n", (unsigned long long)TheStats.slots_live,
      (unsigned long long)(TheStats.staging_bytes >> 10));
  add("uploads: %llu (%llu KB)   staging wraps: %llu   flushes: %llu   stalls: %llu   "
      "dropped: %llu   arena full: %llu\n",
      (unsigned long long)TheStats.uploads,
      (unsigned long long)(TheStats.uploaded_bytes >> 10),
      (unsigned long long)TheStats.staging_wraps,
      (unsigned long long)TheStats.staging_flushes,
      (unsigned long long)TheStats.staging_stalls,
      (unsigned long long)TheStats.dropped_uploads,
      (unsigned long long)TheStats.arena_exhausted);
  add("   ... ring bytes skipped for alignment and wraps: %llu KB (they count against the "
      "batch)   overlapping copies ordered: %llu\n",
      (unsigned long long)(TheStats.staging_skipped_bytes >> 10),
      (unsigned long long)TheStats.ordered_overlapping_copies);
  add("images: %llu live / %llu created   %llu KB (peak %llu KB)\n",
      (unsigned long long)TheStats.images_live,
      (unsigned long long)TheStats.images_created,
      (unsigned long long)(TheStats.image_bytes >> 10),
      (unsigned long long)(TheStats.image_peak_bytes >> 10));
  add("image uploads: %llu (%llu KB)   unsupported formats: %llu   unaligned rects: %llu"
      "   dropped: %llu   (last three must be 0)\n",
      (unsigned long long)TheStats.image_uploads,
      (unsigned long long)(TheStats.image_uploaded_bytes >> 10),
      (unsigned long long)TheStats.unsupported_formats,
      (unsigned long long)TheStats.unaligned_rects,
      (unsigned long long)TheStats.image_uploads_dropped);
  add("scratch: %llu KB vtx (peak %llu KB) + %llu KB idx (peak %llu KB) per frame   "
      "exhausted: %llu (must be 0)\n",
      (unsigned long long)(TheStats.scratch_vertex_bytes >> 10),
      (unsigned long long)(TheStats.scratch_vertices_peak >> 10),
      (unsigned long long)(TheStats.scratch_index_bytes >> 10),
      (unsigned long long)(TheStats.scratch_indices_peak >> 10),
      (unsigned long long)TheStats.scratch_exhausted);
  add("  ... plus %llu KB draw records (peak %llu KB) + %llu KB lights (peak %llu KB)"
      " + %llu KB materials (peak %llu KB)\n",
      (unsigned long long)(TheStats.scratch_draw_bytes >> 10),
      (unsigned long long)(TheStats.scratch_draws_peak >> 10),
      (unsigned long long)(TheStats.scratch_light_bytes >> 10),
      (unsigned long long)(TheStats.scratch_lights_peak >> 10),
      (unsigned long long)(TheStats.scratch_material_bytes >> 10),
      (unsigned long long)(TheStats.scratch_materials_peak >> 10));
  add("bindless: %s   %llu image slots   %llu samplers   %llu writes   out of range: %llu"
      " (must be 0)\n",
      BindlessSet != VK_NULL_HANDLE ? "up" : "down",
      (unsigned long long)TheStats.descriptor_capacity,
      (unsigned long long)TheStats.samplers_live,
      (unsigned long long)TheStats.descriptors_written,
      (unsigned long long)TheStats.descriptors_out_of_range);
  return out;
}

void ShutdownResources() {
  if (Allocator == VK_NULL_HANDLE) {
    return;
  }
  vkDeviceWaitIdle(GetDevice());
  Pending.clear();
  PendingImages.clear();
  PendingImageLevels.clear();
  for (auto &ranges : PendingDstRanges) {
    ranges.clear();
  }
  for (Image &entry : Images) {
    if (entry.live) {
      vkDestroyImageView(GetDevice(), entry.view, nullptr);
      vmaDestroyImage(Allocator, entry.image, entry.allocation);
      entry = Image();
    }
  }
  Images.clear();
  FreeImageIndices.clear();
  TheStats.images_live = 0;
  TheStats.image_bytes = 0;
  for (VkSampler sampler : Samplers) {
    vkDestroySampler(GetDevice(), sampler, nullptr);
  }
  Samplers.clear();
  SamplerIndices.clear();
  TheStats.samplers_live = 0;
  // The pool owns the set, so it is freed with it rather than separately.
  if (BindlessPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(GetDevice(), BindlessPool, nullptr);
    BindlessPool = VK_NULL_HANDLE;
    BindlessSet = VK_NULL_HANDLE;
  }
  if (BindlessLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(GetDevice(), BindlessLayout, nullptr);
    BindlessLayout = VK_NULL_HANDLE;
  }
  if (UploadFence != VK_NULL_HANDLE) {
    vkDestroyFence(GetDevice(), UploadFence, nullptr);
    UploadFence = VK_NULL_HANDLE;
  }
  if (UploadPool != VK_NULL_HANDLE) {
    vkDestroyCommandPool(GetDevice(), UploadPool, nullptr);
    UploadPool = VK_NULL_HANDLE;
  }
  if (Ring.buffer != VK_NULL_HANDLE) {
    vmaDestroyBuffer(Allocator, Ring.buffer, Ring.allocation);
    Ring = Staging();
  }
  for (Scratch *scratch :
       {&ScratchVertices, &ScratchIndices, &ScratchDraws, &ScratchLights, &ScratchMaterials}) {
    if (scratch->buffer != VK_NULL_HANDLE) {
      vmaDestroyBuffer(Allocator, scratch->buffer, scratch->allocation);
      *scratch = Scratch();
    }
  }
  for (Arena *arena : {&VertexArena, &IndexArena}) {
    if (arena->buffer != VK_NULL_HANDLE) {
      vmaDestroyBuffer(Allocator, arena->buffer, arena->allocation);
      *arena = Arena();
    }
  }
  vmaDestroyAllocator(Allocator);
  Allocator = VK_NULL_HANDLE;
  Ready = false;
  TheStats.ready = false;
}

} // namespace vulkan
} // namespace gk
