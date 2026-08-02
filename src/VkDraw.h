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
// vulkan_renderer_notes.md section 2 is the design this is converging on. It is NOT reached yet:
// a draw here carries its own push constants rather than indexing a GpuDraw/GpuMaterial array.
// That is a bring-up decision - one array write per draw is the next step and changes nothing
// about what is on screen - and it is recorded so the difference stays visible.

#include <cstdint>
#include <string>

namespace gk {
namespace vulkan {

// One of the game's draw calls, already reduced to what the shader needs.
//
// `mvp` is world*view*projection in D3D's own row-major, row-vector form - the shader does
// `mul(v, M)` and the module is compiled -matrix-layout-row-major, so there is no transpose
// anywhere and the CPU side reads the way the game's own matrices do. The one adjustment is a
// negated second column, which is the Y flip between D3D's clip space and Vulkan's.
// Where a draw's vertices and indices live. A buffered draw pulls from the arenas; a
// user-pointer draw pulls from the frame's scratch, which is a different buffer entirely.
enum class DrawSource : uint8_t { Arena, Scratch };

struct DrawItem {
  float mvp[16];
  uint32_t base_vertex;    // this draw's vertices, in canonical vertices into its source
  uint32_t first_index;    // in indices into its source; ignored when `indexed` is false
  uint32_t count;          // indices when indexed, vertices otherwise
  int32_t vertex_offset;   // D3D's BaseVertexIndex, added to every index
  uint32_t texture_index;  // bindless slot, kNoTexture for untextured
  uint32_t sampler_index;
  uint32_t flags;
  DrawSource source = DrawSource::Arena;
  bool indexed = true;     // DrawPrimitiveUP has no indices at all
  uint8_t index_stride = 2; // 2 or 4, for the index type to bind
};

constexpr uint32_t kNoTexture = 0xffffffffu;

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
  uint64_t skipped_no_transform = 0;
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

const DrawStats &Draws();
// The capture layer counts what it could not turn into a draw, so the skip reasons live with
// the rest of the draw statistics rather than in a second place.
DrawStats &MutableDrawStats();
std::string FormatDrawStats();

} // namespace vulkan
} // namespace gk
