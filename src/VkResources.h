#pragma once

// GPU memory: the vertex and index arenas, and the staging ring that feeds them. Phase 2c-i
// of vulkan_renderer_notes.md.
//
// Sizes here are measured, not guessed (§4.8). Level01 holds **7.3 MB** of live buffers at
// its peak - 417 vertex buffers and 3,131 index buffers - against 859 MB ever created, so the
// arenas are sized against residency rather than churn. Steady-state upload traffic is
// **4.7 MB over 75 locks per frame**, which is what the staging ring is sized from.
//
// Three things decide the shape, in decreasing order of how much they constrain it:
//
// - **Nothing device-local is ever mapped.** On a 32-bit host the whole 2 GB of address space
//   is shared with the game, the driver and QuickJS, and this machine reports its *entire*
//   16 GB device-local heap as host-visible (full ReBAR, §4.3) - so mapping "just the arena"
//   is an easy and expensive mistake. Staging is host-visible and permanently mapped because
//   it is small and bounded; the arenas are device-local and never mapped at all.
//
// - **Uploads are deferred, not immediate.** `Unlock` happens deep inside the game's own call
//   stack, nowhere near a command buffer. So a copy is staged and queued, and the queue is
//   drained at the top of the next frame, inside the frame's command buffer. That also
//   batches 75 locks into one barrier pair.
//
// - **The staging ring is fenced by frame, not by allocation.** A region may be reused only
//   once the frame that read it has completed, which the renderer's per-frame fence already
//   tracks. Sizing it at several frames' worth of traffic is what keeps that from ever
//   blocking.

#include <cstdint>
#include <string>
#include <vector>

namespace gk {
namespace vulkan {

// A buffer's own region of an arena, held for the buffer's lifetime.
//
// This is the shape the game's model demands, and getting it wrong is instructive: the first
// version bump-allocated per *upload*, which meant every re-lock of the same buffer consumed
// fresh arena. The game re-locks ~75 buffers a frame, so 32 MB was gone in seconds and
// `arena_exhausted` hit 391,173. A D3D vertex buffer is a fixed-size allocation whose
// *contents* change - so a slot is allocated once at CreateVertexBuffer, written on every
// Unlock, and freed when the buffer is released. That is also what makes the 7.3 MB live
// measurement (§4.8) the number that sizes the arena, rather than the 859 MB of churn.
struct BufferSlot {
  uint32_t offset = 0;
  uint32_t bytes = 0;
  bool vertex = false;
  bool valid = false;
};

// A texture's image, held by the capture layer's texture wrapper for its lifetime - the same
// ownership shape as BufferSlot, and for the same reason: a D3D texture is a fixed allocation
// whose contents change.
//
// `index` is the slot this image will occupy in the bindless descriptor array, assigned at
// creation and stable until the image is destroyed. It is what goes into
// GpuMaterial::stage_tex[].
struct TextureImage {
  uint32_t index = 0;
  bool valid = false;
};

// What one image is, for the REPL and for anything that wants to address the bindless table by
// asset rather than by slot. `name` is the `.rim` path the engine acquired the texture under,
// or empty for an image with no cache record behind it (a procedural or engine-internal one).
struct TextureImageInfo {
  uint32_t index = 0;
  std::string name;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 0;
  uint32_t d3d_format = 0;
  uint64_t bytes = 0;
};

struct ResourceStats {
  bool ready = false;
  // GKPLUS_VK_HEAPS=small. Worth reporting rather than inferring from the sizes, because a
  // capture taken this way is not a capture of the configuration that normally runs.
  bool small_heaps = false;
  uint64_t vertex_arena_bytes = 0; // capacity
  uint64_t index_arena_bytes = 0;
  uint64_t staging_bytes = 0;
  uint64_t vertex_used = 0; // currently allocated, not a high-water mark
  uint64_t index_used = 0;
  uint64_t vertex_peak = 0;
  uint64_t index_peak = 0;
  uint64_t slots_live = 0;
  uint64_t uploads = 0;
  uint64_t uploaded_bytes = 0;
  uint64_t staging_wraps = 0;
  // Mid-batch flushes: the un-recorded batch reached the ring's size and had to be submitted
  // and waited for. Non-zero is normal on a level load, which stages 360 MB between two
  // Presents; steady-state play should never do it. See AllocateStaging.
  uint64_t staging_flushes = 0;
  // Times the ring had to block because the region it was about to reuse was still being read
  // by a frame in flight. See WaitForLiveFrames.
  uint64_t staging_stalls = 0;
  // Ring bytes the head skipped without handing them out - alignment padding, and the tail
  // abandoned at a wrap. They count against the batch like any other byte; the figure is here
  // because it used to not, which let the head lap its own un-recorded batch (see
  // AllocateStaging). It is the size of that former hazard, so it is worth being able to read.
  uint64_t staging_skipped_bytes = 0;
  // Copies that overwrite bytes an earlier copy in the same batch also wrote, and so had a
  // barrier put in front of them. Non-zero is normal on a level load - a freed arena slot is
  // handed to a new buffer while the old one's upload is still queued - and each one is a
  // buffer that used to reach the GPU as whichever copy the driver happened to retire last.
  uint64_t ordered_overlapping_copies = 0;
  uint64_t dropped_uploads = 0; // did not fit; see AllocateStaging
  uint64_t arena_exhausted = 0;

  // --- images (Phase 2c-iv) ---------------------------------------------------------------
  uint64_t images_live = 0;
  uint64_t images_created = 0;
  uint64_t image_bytes = 0; // device memory currently held by images
  uint64_t image_peak_bytes = 0;
  uint64_t image_uploads = 0;
  uint64_t image_uploaded_bytes = 0;

  // Must be 0. A texture whose D3D format has no VkFormat gets no image at all, so it would
  // sample as missing rather than as wrong - which is the right failure, but it is still a
  // format the section 4.1 enumeration did not predict.
  uint64_t unsupported_formats = 0;
  // A blit whose rectangle is not a multiple of the compressed block size. D3D requires block
  // alignment too, so this should be structurally impossible; counted rather than assumed,
  // because copying a misaligned rect would corrupt the neighbouring blocks.
  uint64_t unaligned_rects = 0;
  uint64_t image_uploads_dropped = 0; // staging could not take it

  // --- the bindless set --------------------------------------------------------------------
  uint64_t descriptor_capacity = 0; // image slots the set was allocated with
  uint64_t descriptors_written = 0; // cumulative writes, not live descriptors
  uint64_t samplers_live = 0;
  // Images whose slot index is past the set's capacity, so they have no descriptor and would
  // sample as missing. Must be 0; if it ever is not, raise kBindlessTextures.
  uint64_t descriptors_out_of_range = 0;

  // --- per-frame scratch -------------------------------------------------------------------
  uint64_t scratch_vertex_bytes = 0; // capacity of one frame's slice
  uint64_t scratch_index_bytes = 0;
  uint64_t scratch_draw_bytes = 0;
  uint64_t scratch_light_bytes = 0;
  uint64_t scratch_material_bytes = 0;
  uint64_t scratch_vertices_peak = 0; // high-water within a single frame
  uint64_t scratch_indices_peak = 0;
  uint64_t scratch_draws_peak = 0;
  uint64_t scratch_lights_peak = 0;
  uint64_t scratch_materials_peak = 0;
  // Allocations that did not fit in the frame's slice, so those draws were dropped. Must be 0;
  // if it ever is not, the slice is too small - the numbers to size it by are the two peaks.
  uint64_t scratch_exhausted = 0;
};

// Creates the arenas and the staging ring. Requires vulkan::Initialize() to have succeeded.
// Idempotent; returns false and records a reason on failure.
bool StartResources();

bool ResourcesReady();

void ShutdownResources();

// Reserves a slot for a buffer's whole lifetime. Returns an invalid slot if the arena is
// full, which the caller should treat as "this buffer is not resident" rather than an error.
BufferSlot AllocateSlot(uint32_t bytes, bool vertex);
void FreeSlot(const BufferSlot &slot);

// Copies `bytes` from `data` into the staging ring and queues a copy into `slot`. Safe to
// call at any time from the main thread; nothing is recorded into a command buffer here. The
// data is copied immediately, so the caller's pointer need not outlive the call - which
// matters because it is a buffer the game is about to unlock.
// `offset_in_slot` is where the data lands inside the slot, which is how a partial
// Lock is honoured - the game may re-lock a sub-range and expects the rest to keep its
// previous contents.
bool UploadIntoSlot(const BufferSlot &slot, uint32_t offset_in_slot, const void *data,
                    uint32_t bytes);

// --- textures ------------------------------------------------------------------------------
//
// Images are created lazily, on the first blit into a texture, because a texture may exist
// long before the renderer comes up - the same reason a BufferSlot is claimed on first use.
//
// `d3d_format` is the D3DFORMAT the game created the texture with. Only the four measured in
// notes section 4.1 map to a VkFormat; anything else is refused and counted rather than
// guessed at, which is the same rule the FVF converter follows.
bool CreateTextureImage(TextureImage &image, uint32_t width, uint32_t height, uint32_t levels,
                        uint32_t d3d_format);
void DestroyTextureImage(TextureImage &image);

// Records the asset this image came from. Separate from creation because the name is recovered
// from the engine's texture cache, which only links a path to a D3D texture *after* the texture
// exists - see the AcquireRimTexture hook in src/D3D8Capture.cpp.
void NameTextureImage(const TextureImage &image, const std::string &name);

// Every live image, in slot order. The input to a bindless table addressed by asset name, and
// what `render.textures` reports.
std::vector<TextureImageInfo> TextureImages();

// --- the bindless descriptor set -------------------------------------------------------------
//
// One set for the whole renderer: a sampler array at binding 0 and a sampled-image array at
// binding 1, both `UPDATE_AFTER_BIND | PARTIALLY_BOUND`, the image array additionally
// `VARIABLE_DESCRIPTOR_COUNT`. `TextureImage::index` IS the descriptor index - there is no
// second mapping to keep in step, which is the whole point of assigning the slot at creation.
//
// UPDATE_AFTER_BIND is what makes this workable at all: a texture can be created mid-frame,
// after the set is already bound, and its descriptor written without touching the set's
// binding. PARTIALLY_BOUND is what lets the array have holes where images have been destroyed.

// A sampler, described in the D3D enum values the shadow state already records - so the caller
// passes what it has rather than translating, and the translation lives in one place. Returns a
// stable index into the sampler array; identical combinations share one.
uint32_t AcquireSampler(uint32_t mag_filter, uint32_t min_filter, uint32_t mip_filter,
                        uint32_t address_u, uint32_t address_v);

// The set and its layout, as opaque handles so this header keeps mentioning no Vulkan type
// (the same reason RecordUploads takes a `void *`). Zero until the resources are up.
//
// `uint64_t` and NOT `void *`, which is the 32-bit trap: a non-dispatchable Vulkan handle is
// `uint64_t` on every platform, so on x86 it is *wider* than a pointer. `RecordUploads` can
// take a `void *` because a VkCommandBuffer is dispatchable and really is a pointer; these two
// are not, and typing them as one truncates the handle in half.
uint64_t BindlessDescriptorSet();
uint64_t BindlessDescriptorSetLayout();

// --- per-frame scratch, for user-pointer draws ------------------------------------------------
//
// `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` hand D3D their vertices inline: there is no
// buffer, so there is nothing for a BufferSlot to attach to and nothing whose Release says when
// the data dies. It is per-frame data by construction, so it gets a per-frame allocator.
//
// **Host-visible and mapped, unlike the arenas**, which is the one deliberate exception to the
// rule in this header. The data is produced on the CPU, is different every frame, and is small;
// staging it would mean a copy and a barrier per draw to move bytes that the GPU will read once.
// The arenas stay device-local because they hold a level's worth of geometry that is read every
// frame and written rarely - exactly the opposite trade.
//
// **A slice belongs to a scene, not to a frame in flight**, and the difference is the whole
// reason there is one more slice than there are frames. The game writes here while it draws,
// which is before the Present that renders what it wrote; rotating on the renderer's own frame
// index instead made every user-pointer draw read the slice the *previous* scene had written,
// and made the next scene overwrite a slice the GPU was still reading. See kScratchSlices in
// the .cpp for why the count is what it is.

struct ScratchAlloc {
  void *mapped = nullptr; // write the data here
  uint32_t offset = 0;    // in elements: vertices for the vertex scratch, indices for the index
  bool valid = false;
};

// Moves to the next slice and empties it, for the scene that is about to be recorded. Called by
// the renderer at the END of a frame - after the draws that read the outgoing slice have been
// submitted, and while the fence waited at the top of that frame is the proof that the incoming
// one is no longer being read.
void RotateFrameScratch();
// Empties the current slice without moving off it, for a frame whose draws were dropped. Safe
// wherever RotateFrameScratch is not, because nothing in flight is reading this slice.
void ResetFrameScratch();

ScratchAlloc AllocateScratchVertices(uint32_t count);
// `stride` is 2 or 4 bytes; the two index widths are kept apart so a draw can be recorded with
// the type it actually has rather than everything being widened.
ScratchAlloc AllocateScratchIndices(uint32_t count, uint32_t stride);

// The same allocator for the three arrays the shader reads per draw: one GpuDrawRecord each,
// the enabled lights each draw sees, and the interned GpuMaterials. They are per-frame data
// written at draw time, exactly like the user-pointer vertices, so they take the same slices and
// rotate with them - which is also what makes them safe to write while a previous frame is in
// flight.
//
// Sized in *records* rather than bytes, so the caller never spells the stride: getting that
// wrong would address the array past its own elements, which is the failure §4.16 spent a
// session on.
ScratchAlloc AllocateScratchDraws(uint32_t count);
ScratchAlloc AllocateScratchLights(uint32_t count);
ScratchAlloc AllocateScratchMaterials(uint32_t count);

// The scratch vertex buffer's shader-readable address, and the index scratch as a handle. The
// address carries the current slice, so it is only valid for the scene now being recorded -
// read it while recording that scene's draws, never after RotateFrameScratch.
uint64_t ScratchVertexAddress();
uint64_t ScratchIndexBuffer();
uint64_t ScratchDrawAddress();
uint64_t ScratchLightAddress();
uint64_t ScratchMaterialAddress();

// The host side of the same two slices, for reading back what a user-pointer draw was actually
// given. Only the scratch has these: it is host-visible and mapped by design, where the arenas
// are device-local and deliberately never mapped, so this is a diagnostic for the user-pointer
// path and nothing else. Valid only for the scene now being recorded, exactly like the
// addresses above.
//
// The vertex pointer already carries the current slice, so a DrawItem's `base_vertex` indexes it
// directly. The index pointer does NOT - `first_index` is absolute from the buffer's start,
// because the renderer binds the index buffer at offset 0 - so it is the buffer base.
const void *ScratchVertexMapped();
const void *ScratchIndexMappedBase();

// The vertex arena as a shader-readable address, and the index arena as a VkBuffer handle.
// Vertices are *pulled* by address (so a draw binds no vertex buffer at all) while indices
// still go through vkCmdBindIndexBuffer, which takes a handle - hence the two shapes. Both
// zero until the resources are up.
uint64_t VertexArenaAddress();
uint64_t IndexArenaBuffer();

// Stages one rectangle of one mip level and queues the copy. `data` points at the top-left
// texel (or block) of the rectangle inside a locked surface of `pitch` bytes per row - which
// for a block-compressed format is bytes per row of BLOCKS, exactly as D3D reports it.
//
// The rectangle is honoured rather than assumed to be the whole level: notes section 4.12
// measured 94% of the engine's blits as sub-rect, so uploading whole surfaces would give
// textures that are correct on the frame they load and stale ever after.
bool UploadIntoTextureImage(const TextureImage &image, uint32_t level, int32_t x, int32_t y,
                            uint32_t width, uint32_t height, const void *data,
                            uint32_t pitch);

// Submits everything queued right now and waits for it. The frame path does not need this -
// RecordUploads runs before anything reads - but a diagnostic that reads the GPU's copy does:
// without it, a blit still sitting in the queue reads back as a stale texture and the check
// reports a mismatch that is really its own impatience.
void FlushUploads();

// Reads one mip level back off the GPU and compares it against what `data` should have become,
// where `data`/`pitch` are a locked surface of the same level. True means the image holds
// exactly the bytes the upload path was supposed to put there.
//
// This is the only check on the *contents* rather than the plumbing: every counter can read
// perfectly while the image holds swapped channels or stale texels. Slow by construction - it
// submits and waits - so it is a REPL diagnostic, not something a frame does.
// `differing_bytes` and `first_difference` are filled in on a mismatch. They are what separate
// the three things a mismatch can mean: everything different (the image was never populated),
// a contiguous run different (a blit's rectangle mishandled), or a scattering (a format or
// swizzle error).
bool VerifyImageLevel(const TextureImage &image, uint32_t level, const void *data,
                      uint32_t pitch, uint64_t *differing_bytes = nullptr,
                      uint64_t *first_difference = nullptr, uint64_t *total_bytes = nullptr);

// The same check for a buffer slot: read the arena back and compare it against what the slot
// should hold - converted vertices, or indices verbatim. A slot that took another buffer's
// bytes cannot fail visibly on its own, because a draw addresses the arena by offset: it draws
// the wrong geometry, or, through a corrupt index, one triangle stretched across the screen.
// Every upload GKPLUS_VK_WATCH_DST named, with the staging batch that carried it. One run
// says which batch to capture; see CaptureStagingBatch in VkCapture.h.
const std::string &StagingWatchLog();

bool VerifySlot(const BufferSlot &slot, const void *expected, uint32_t bytes,
                uint64_t *differing_bytes = nullptr, uint64_t *first_difference = nullptr,
                uint8_t *got_prefix = nullptr);

// Copies `bytes` out of the vertex or index arena at an ABSOLUTE offset.
//
// Deliberately not expressed in terms of a slot, because the question it exists to answer is not
// about one: a draw addresses the arena by absolute offset, and `VerifySlot` proves a slot holds
// the right bytes without proving any draw *reads* that slot. This is what lets a caller ask
// "what did draw N actually pull", which is the gap §4.16 named and nothing here could answer.
bool ReadArena(bool vertex, uint64_t offset, uint32_t bytes, void *out);


// Whether a D3DFORMAT has an image mapping at all, and how it is addressed. `block` is texels
// per block edge (1 for uncompressed, 4 for DXT) and `block_bytes` is what D3D stores per
// block - which is what the caller needs to index into a locked surface, and is NOT always
// what the VkFormat uses (A4R4G4B4 is 2 bytes here and 4 in the image).
bool TextureFormatBlock(uint32_t d3d_format, uint32_t &block, uint32_t &block_bytes);

// Records every queued copy into `cmd`, with one barrier pair for the whole batch, and marks
// the staging used by this frame. Called by the renderer at the top of its frame.
void RecordUploads(void *command_buffer, uint32_t frame_index);

// Releases the staging a completed frame was holding, so it can be reused.
void ReleaseFrameStaging(uint32_t frame_index);

// Named apart from vulkan::Stats() in VkRenderer.h, which returns RendererStats: same
// namespace, and two functions differing only in return type cannot coexist. The same
// collision already cost a build with LastError().
const ResourceStats &Resources();
const std::string &ResourceError();
std::string FormatResourceStats();

} // namespace vulkan
} // namespace gk
