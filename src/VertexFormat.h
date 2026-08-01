#pragma once

// D3D8 FVF vertices -> the renderer's one canonical layout.
//
// This is the simplification vulkan_renderer_notes.md §2 calls the highest-leverage one in
// the design: one layout means one arena, vertex *pulling* rather than a bound vertex buffer,
// and one vertex shader instead of one per format. It is only safe because Phase 0 enumerated
// the FVFs rather than assuming them - the game uses exactly six (§4.1):
//
//   0x002  XYZ                                   12 bytes
//   0x112  XYZ | NORMAL | 1 uv                   32
//   0x152  XYZ | NORMAL | DIFFUSE | 1 uv         36
//   0x1c4  XYZRHW | DIFFUSE | SPECULAR | 1 uv    32
//   0x212  XYZ | NORMAL | 2 uv                   40
//   0x252  XYZ | NORMAL | DIFFUSE | 2 uv         44   <- 10.8M of 12.6M draws
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
bool ConvertVertices(uint32_t fvf, const void *src, uint32_t count, CanonicalVertex *dst);

} // namespace vulkan
} // namespace gk
