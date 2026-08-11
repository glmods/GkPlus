#pragma once

// D3D8 FVF vertices -> the renderer's one canonical layout.
//
// This is the simplification vulkan_renderer_notes.md §2 calls the highest-leverage one in
// the design: one layout means one arena, vertex *pulling* rather than a bound vertex buffer,
// and one vertex shader instead of one per format. It is only safe because Phase 0 enumerated
// the FVFs rather than assuming them - six by *draw* count (§4.1):
//
//   0x002  XYZ                                   12 bytes
//   0x112  XYZ | NORMAL | 1 uv                   32
//   0x152  XYZ | NORMAL | DIFFUSE | 1 uv         36
//   0x1c4  XYZRHW | DIFFUSE | SPECULAR | 1 uv    32
//   0x212  XYZ | NORMAL | 2 uv                   40
//   0x252  XYZ | NORMAL | DIFFUSE | 2 uv         44   <- 10.8M of 12.6M draws
//
// **That table is by draws, and by VERTICES it is not the ranking at all.** `ReadLayoutCensus`
// below counts what this converter is actually handed, and a level02 session says the largest
// layout by far is one the table above does not list:
//
//   0x004  XYZRHW alone                          16      <- 66% of all vertices converted
//   0x142  XYZ | DIFFUSE | 1 uv                  24
//
// 0x004 is 222M of 337M vertices at ~4,500 a call, against 0x252's 28M at 45 a call. Neither
// appears in `render.stats.fvfs`, which is keyed on the `SetVertexShader` handle - the buffered
// path converts with whatever FVF `CreateVertexBuffer` was given. A per-draw census cannot see
// this and a per-draw census is what §4.1 was; see §4.82 and §4.83.
//
// Pure CPU, no Vulkan: this is a byte transform, and keeping it that way makes it the one
// piece of the renderer that could be unit-tested outside the game.

#include <cstdint>

namespace gk {
namespace vulkan {

// 48 bytes, and every field naturally aligned.
//
// `pos` is float4 rather than float3 so that XYZRHW survives: those vertices are already in
// screen space with a reciprocal w, and the fourth component carries it (1.0 for ordinary
// XYZ). Which of the two a draw is using is a per-*draw* property, not a per-vertex one, so
// the shader learns it from the draw record rather than from the vertex.
struct CanonicalVertex {
  float pos[4];
  float normal[3];
  uint32_t color; // D3DCOLOR, i.e. BGRA bytes in a little-endian DWORD
  float uv0[2];
  float uv1[2];
};

static_assert(sizeof(CanonicalVertex) == 48, "the arena stride is part of the shader ABI");

// Bytes per vertex for `fvf`, or 0 if this layout is not one the converter handles.
//
// Deliberately reproduces the arithmetic inlined in `Aw_DrawIndexedPrimitive` @ 0x005a3c20
// rather than deriving it from the D3D docs, so our stride and the engine's agree by
// construction. Note the engine treats XYZ (+12) and XYZRHW (+16) as independent adders and
// ignores PSIZE entirely - it never emits one.
uint32_t FvfStride(uint32_t fvf);

// True when ConvertVertices can handle `fvf`. False for the blend-weight layouts
// (D3DFVF_XYZB1..5) and anything with a non-default texture coordinate size, none of which
// Gunlok emits - the point is to refuse rather than to silently mis-decode.
bool FvfSupported(uint32_t fvf);

// Converts `count` vertices from `src` into `dst`. Missing fields take neutral defaults:
// w = 1, normal = (0,0,0), colour = opaque white, uvs = 0. Returns false without writing
// anything if the layout is unsupported.
//
// `src_stride` of 0 means "whatever the FVF implies", which is the buffered case. A
// user-pointer draw states its stride explicitly in the call and is entitled to pad, so it
// passes the value it was given rather than letting the FVF speak for it.
bool ConvertVertices(uint32_t fvf, const void *src, uint32_t count, CanonicalVertex *dst,
                     uint32_t src_stride = 0);

// --- the layout census -----------------------------------------------------------------------
//
// What `ConvertVertices` actually sees, which is a different question from `render.stats.fvfs`
// and the reason this exists. That one is keyed on the handle passed to `SetVertexShader`; the
// buffered path converts with the FVF `CreateVertexBuffer` was given, and nothing counted it. The
// gap is not academic - it is why the generic conversion loop was the hottest function left in
// the profile while the existing census showed only the six layouts that are all specialized
// (vulkan_renderer_notes.md §4.82).
//
// Counted inside the converter rather than at its call sites, so a caller cannot be forgotten.
// The table is a fixed 64 entries of atomics rather than a map behind a lock: `ConvertVertices`
// runs on **both** game threads (§4.72), it is on the hot path, and 64 covers the encoding
// exactly - position is one bit because only XYZ and XYZRHW are admitted, then normal, diffuse
// and specular, then two bits of texture-coordinate count.
struct LayoutCensusEntry {
  uint32_t layout;    // fvf & the bits the conversion depends on
  uint64_t calls;
  uint64_t vertices;
  bool specialized;   // false means it took the generic loop
};

// Fills `out` with every layout seen at least once, most vertices first, and returns how many
// were written. 64 entries is always enough for the whole table.
uint32_t ReadLayoutCensus(LayoutCensusEntry *out, uint32_t capacity);

// The object-space box `count` vertices of `src` occupy, without converting them.
//
// Position is the first 12 bytes of every layout ConvertVertices accepts, so this needs only the
// stride - which is why it can run over the game's own vertices rather than over the converted
// copy. That matters where it is used: both callers have already written their canonical copy
// into mapped **write-combined** scratch, where reading a position back costs far more than
// walking the source again.
//
// **Refuses a pre-transformed layout by name.** An XYZRHW vertex is already in screen space, so
// its box is not in any world the shadow bakes project from, and the only safe answer is none.
// `src_stride` means what it does above.
bool PositionBounds(uint32_t fvf, const void *src, uint32_t count, float out_min[3],
                    float out_max[3], uint32_t src_stride = 0);

} // namespace vulkan
} // namespace gk
