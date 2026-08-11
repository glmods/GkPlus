#include "VertexFormat.h"

#include <cstring>
#include <type_traits>

namespace gk {
namespace vulkan {
namespace {

// The D3DFVF bits this converter understands. Spelled out rather than included from d3d8.hpp
// so that this file stays free of any D3D dependency and could be exercised outside the game.
constexpr uint32_t kXyz = 0x002;
constexpr uint32_t kXyzRhw = 0x004;
constexpr uint32_t kNormal = 0x010;
constexpr uint32_t kPSize = 0x020;
constexpr uint32_t kDiffuse = 0x040;
constexpr uint32_t kSpecular = 0x080;
constexpr uint32_t kTexCountMask = 0xf00;
constexpr uint32_t kTexCountShift = 8;
// D3DFVF_POSITION_MASK covers XYZ, XYZRHW and the XYZB1..5 blend layouts; anything in it that
// is not one of the first two is a layout the game never emits.
constexpr uint32_t kPositionMask = 0x00e;
// Every bit the conversion actually depends on, and no others. Two FVFs that agree on these
// produce byte-identical output and have the same stride, which is what lets the dispatch in
// ConvertVertices match on the masked value. PSIZE is absent on purpose: FvfSupported refuses
// it outright, so it never reaches a conversion to influence one.
constexpr uint32_t kLayoutMask = kPositionMask | kNormal | kDiffuse | kSpecular | kTexCountMask;

float ReadFloat(const uint8_t *p) {
  float v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint32_t ReadDword(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

} // namespace

uint32_t FvfStride(uint32_t fvf) {
  if (!FvfSupported(fvf)) {
    return 0;
  }
  uint32_t stride = 0;
  if ((fvf & kXyz) != 0) {
    stride += 12;
  }
  if ((fvf & kXyzRhw) != 0) {
    stride += 16;
  }
  if ((fvf & kNormal) != 0) {
    stride += 12;
  }
  if ((fvf & kDiffuse) != 0) {
    stride += 4;
  }
  if ((fvf & kSpecular) != 0) {
    stride += 4;
  }
  stride += ((fvf & kTexCountMask) >> kTexCountShift) * 8;
  return stride;
}

bool FvfSupported(uint32_t fvf) {
  const uint32_t position = fvf & kPositionMask;
  if (position != kXyz && position != kXyzRhw) {
    return false; // untransformed-with-blend-weights, or no position at all
  }
  if ((fvf & kPSize) != 0) {
    return false; // the engine never emits one, and its offset would shift everything after
  }
  // More than two texture coordinate sets would not fit the canonical layout. Measured max
  // is two (§4.1), so this is a guard rather than a limitation being accepted.
  return ((fvf & kTexCountMask) >> kTexCountShift) <= 2;
}

bool PositionBounds(uint32_t fvf, const void *src, uint32_t count, float out_min[3],
                    float out_max[3], uint32_t src_stride) {
  const uint32_t implied = FvfStride(fvf);
  if (implied == 0 || src == nullptr || count == 0) {
    return false;
  }
  // No position at all, or one that is already in screen space. Either way there is nothing here
  // a world-space consumer may use.
  if ((fvf & kXyz) == 0 || (fvf & kXyzRhw) != 0) {
    return false;
  }
  const uint32_t stride = src_stride != 0 ? src_stride : implied;
  if (stride < implied) {
    return false;
  }
  const auto *bytes = static_cast<const uint8_t *>(src);
  for (uint32_t i = 0; i < count; ++i) {
    float p[3];
    std::memcpy(p, bytes + static_cast<size_t>(i) * stride, sizeof(p));
    for (int j = 0; j < 3; ++j) {
      if (i == 0) {
        out_min[j] = out_max[j] = p[j];
      } else {
        out_min[j] = p[j] < out_min[j] ? p[j] : out_min[j];
        out_max[j] = p[j] > out_max[j] ? p[j] : out_max[j];
      }
    }
  }
  return true;
}

namespace {

// The conversion loop, with the FVF as a *template* parameter.
//
// `Fvf` is instantiated two ways and the body is written once, which is the whole point: pass a
// `std::integral_constant` and every field test below folds at compile time into a straight-line
// copy; pass a plain `uint32_t` and the identical source runs with the tests live. There is no
// second implementation to drift out of step with the first, so the specialized paths cannot
// disagree with the general one about what a layout means - they are the same statements.
//
// Two things about the shape, both of which matter more than they look:
//
//   * **Every field is written exactly once, in address order.** The previous version zeroed all
//     48 bytes with `v = CanonicalVertex{}` and then overwrote most of them, which is two passes
//     over the vertex. For a user-pointer draw `dst` is mapped **write-combined** scratch, where
//     a partial rewrite of a line the WC buffer has already started assembling is exactly the
//     access pattern that forces it out early. The `else` branches below write the same neutral
//     defaults the zero-fill used to supply - w = 1, normal 0, colour opaque white, uv 0 - so
//     the result is identical byte for byte.
//   * The reads stay `memcpy`-based (`ReadFloat`/`ReadDword`). The source is the game's own
//     vertex data at whatever alignment it chose, and these compile to a single unaligned load.
template <typename FvfT>
void ConvertRun(FvfT fvf, const uint8_t *bytes, uint32_t stride, uint32_t count,
                CanonicalVertex *dst) {
  const bool has_xyz = (fvf & kXyz) != 0;
  const bool has_rhw = (fvf & kXyzRhw) != 0;
  const bool has_normal = (fvf & kNormal) != 0;
  const bool has_diffuse = (fvf & kDiffuse) != 0;
  const bool has_specular = (fvf & kSpecular) != 0;
  const uint32_t tex_count = (fvf & kTexCountMask) >> kTexCountShift;

  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *p = bytes + static_cast<size_t>(i) * stride;
    CanonicalVertex &v = dst[i];

    if (has_xyz) {
      v.pos[0] = ReadFloat(p);
      v.pos[1] = ReadFloat(p + 4);
      v.pos[2] = ReadFloat(p + 8);
      v.pos[3] = 1.0f;
      p += 12;
    } else if (has_rhw) {
      // Already in screen space; the fourth component is 1/w, not w.
      v.pos[0] = ReadFloat(p);
      v.pos[1] = ReadFloat(p + 4);
      v.pos[2] = ReadFloat(p + 8);
      v.pos[3] = ReadFloat(p + 12);
      p += 16;
    } else {
      v.pos[0] = v.pos[1] = v.pos[2] = 0.0f;
      v.pos[3] = 1.0f;
    }

    if (has_normal) {
      v.normal[0] = ReadFloat(p);
      v.normal[1] = ReadFloat(p + 4);
      v.normal[2] = ReadFloat(p + 8);
      p += 12;
    } else {
      // A zero normal is distinguishable from a real one, which is what the shader needs.
      v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
    }

    if (has_diffuse) {
      v.color = ReadDword(p);
      p += 4;
    } else {
      // Opaque white is the identity for a modulate.
      v.color = 0xffffffffu;
    }

    if (has_specular) {
      // Dropped on purpose: the canonical vertex carries one colour, and D3D8 fixed function
      // only uses specular when lighting is on with a specular material - which
      // `AwMaterial`'s nine render states never enable. Recorded here so the omission is a
      // decision rather than an oversight.
      p += 4;
    }

    if (tex_count >= 1) {
      v.uv0[0] = ReadFloat(p);
      v.uv0[1] = ReadFloat(p + 4);
      p += 8;
    } else {
      v.uv0[0] = v.uv0[1] = 0.0f;
    }

    if (tex_count >= 2) {
      v.uv1[0] = ReadFloat(p);
      v.uv1[1] = ReadFloat(p + 4);
    } else {
      v.uv1[0] = v.uv1[1] = 0.0f;
    }
  }
}

// The layouts that get an instantiation of their own, listed once so the dispatch below and
// `IsSpecializedLayout` cannot disagree about which those are - the census reports that flag, and
// a census that lied about it would send someone to specialize something already specialized.
//
// These are the six VertexFormat.h documents, taken from the *draw* census.
//
// **0x004 and 0x142 are deliberately NOT here, and that is a measurement rather than an
// oversight.** `render.stats.converted_layouts` - which exists because of this - said 0x004
// (XYZRHW alone, untextured and uncoloured) was **66% of every vertex the converter touches** in a
// level02 session, 222M of 337M at ~4,500 vertices a call, running the generic loop the whole
// time: a layout can dominate by vertex count while being invisible in a per-draw table. Adding
// both was the obvious move and the measurement refuses it - medians 5.72/5.71 with against
// 5.72/5.81/5.71 without, distributions fully overlapping, no effect to see.
//
// So the generic loop's cost was never the branching it was assumed to be. For 0x004 the body is
// 16 bytes read and 48 written, 32 of them constants, and what that loop is doing is moving bytes
// - which a ninth instantiation cannot speed up and does make the code footprint worse. The win
// was never a faster conversion of 222M vertices; it was **not converting them**, and §4.84 found
// where: they were `ProcessVertices` output being read back through a `D3DLOCK_READONLY` lock,
// and `BufferWrapper::UploadLocked` now returns early on one. 0x004 reaches this function zero
// times per frame, so the case that motivated specializing it does not exist any more.
//
// See vulkan_renderer_notes.md §4.83 and §4.84.
#define GK_SPECIALIZED_FVFS(X)                                                                 \
  X(0x002) /* XYZ */                                                                           \
  X(0x112) /* XYZ | NORMAL | 1 uv */                                                           \
  X(0x152) /* XYZ | NORMAL | DIFFUSE | 1 uv */                                                 \
  X(0x1c4) /* XYZRHW | DIFFUSE | SPECULAR | 1 uv */                                            \
  X(0x212) /* XYZ | NORMAL | 2 uv */                                                           \
  X(0x252) /* XYZ | NORMAL | DIFFUSE | 2 uv */

bool IsSpecializedLayout(uint32_t layout) {
  switch (layout) {
#define GK_FVF_TEST(bits)                                                                      \
  case bits:                                                                                   \
    return true;
    GK_SPECIALIZED_FVFS(GK_FVF_TEST)
#undef GK_FVF_TEST
  default:
    return false;
  }
}

// The census table. See VertexFormat.h for why it is a fixed array of atomics and not a map.
//
// The encoding is dense and reversible: one bit for the position type (only XYZ and XYZRHW reach
// here), one each for normal, diffuse and specular, then two bits of texture-coordinate count.
// Six bits, 64 slots, every supported layout distinct.
// Plain counters, raced on purpose, and measured rather than assumed: as `std::atomic` with
// relaxed adds this cost **0.15 ms of a 5.7 ms frame** - 2.6% - because `ConvertVertices` runs on
// both game threads (§4.72) and two `lock xadd`s land on the same two lines from two cores, which
// is cache-line ping-pong rather than the ~20 cycles the instructions themselves cost.
//
// That is exactly the trade CaptureDiagLock's comment already draws, and this falls on the same
// side of it: a lost increment costs a count in a diagnostic and nothing reads one to decide
// anything, while the failure a lock would prevent - a container corrupting - cannot happen to a
// fixed array that never allocates, rehashes or rebalances. The array is `constinit` for the same
// reason the rest of the file has no initialisation order to worry about.
constexpr uint32_t kCensusSlots = 64;
constexpr uint32_t kCensusSources = static_cast<uint32_t>(ConvertSource::Count);
uint64_t CensusCalls[kCensusSlots][kCensusSources];
uint64_t CensusVertices[kCensusSlots][kCensusSources];

uint32_t CensusSlot(uint32_t fvf) {
  const uint32_t tex = (fvf & kTexCountMask) >> kTexCountShift;
  return ((fvf & kXyzRhw) != 0 ? 1u : 0u) | ((fvf & kNormal) != 0 ? 2u : 0u) |
         ((fvf & kDiffuse) != 0 ? 4u : 0u) | ((fvf & kSpecular) != 0 ? 8u : 0u) |
         ((tex & 3u) << 4);
}

uint32_t CensusLayout(uint32_t slot) {
  return ((slot & 1u) != 0 ? kXyzRhw : kXyz) | ((slot & 2u) != 0 ? kNormal : 0u) |
         ((slot & 4u) != 0 ? kDiffuse : 0u) | ((slot & 8u) != 0 ? kSpecular : 0u) |
         (((slot >> 4) & 3u) << kTexCountShift);
}

} // namespace

uint32_t ReadLayoutCensus(LayoutCensusEntry *out, uint32_t capacity) {
  if (out == nullptr || capacity == 0) {
    return 0;
  }
  uint32_t written = 0;
  for (uint32_t slot = 0; slot < kCensusSlots && written < capacity; ++slot) {
    uint64_t calls = 0;
    uint64_t vertices = 0;
    for (uint32_t s = 0; s < kCensusSources; ++s) {
      calls += CensusCalls[slot][s];
      vertices += CensusVertices[slot][s];
    }
    if (calls == 0) {
      continue;
    }
    const uint32_t layout = CensusLayout(slot);
    out[written].layout = layout;
    out[written].calls = calls;
    out[written].vertices = vertices;
    out[written].specialized = IsSpecializedLayout(layout);
    for (uint32_t s = 0; s < kCensusSources; ++s) {
      out[written].calls_by_source[s] = CensusCalls[slot][s];
      out[written].vertices_by_source[s] = CensusVertices[slot][s];
    }
    ++written;
  }
  // Most vertices first: the question this answers is always "what is the converter spending its
  // time on", and a layout with many calls but few vertices each is not that.
  for (uint32_t i = 1; i < written; ++i) {
    LayoutCensusEntry key = out[i];
    uint32_t j = i;
    for (; j > 0 && out[j - 1].vertices < key.vertices; --j) {
      out[j] = out[j - 1];
    }
    out[j] = key;
  }
  return written;
}

bool ConvertVertices(uint32_t fvf, const void *src, uint32_t count, CanonicalVertex *dst,
                     uint32_t src_stride, ConvertSource source) {
  const uint32_t implied = FvfStride(fvf);
  if (implied == 0 || src == nullptr || dst == nullptr) {
    return false;
  }
  // A caller-supplied stride may be larger than the FVF's - padding is legal - but never
  // smaller, which would mean the fields the FVF promises do not fit in the vertex.
  const uint32_t stride = src_stride != 0 ? src_stride : implied;
  if (stride < implied) {
    return false;
  }

  const auto *bytes = static_cast<const uint8_t *>(src);
  // The six layouts Gunlok actually emits (VertexFormat.h), each given its own instantiation so
  // the field tests become nothing. 0x252 alone is 10.8M of 12.6M draws, so the switch is very
  // nearly a single predicted branch. `default` is the same loop with the tests live, which is
  // what keeps every other layout `FvfSupported` admits correct rather than merely unhandled.
  //
  // Switched on the layout bits ALONE, not on the raw FVF. `ConvertRun` reads exactly these five
  // groups and nothing else, so two FVFs that agree here convert identically - and `FvfStride`
  // is computed from the same bits, so the stride agrees too. Matching the raw value instead
  // sent anything carrying an unrelated bit to the generic loop, and that is not hypothetical:
  // the buffered path takes its FVF from `CreateVertexBuffer` rather than from
  // `SetVertexShader`, and the generic instantiation was the single hottest function left in the
  // profile - hotter than every specialization put together - while the FVF census showed only
  // the six. The masked switch is also what makes those six exhaustive rather than merely likely.
  const uint32_t layout = fvf & kLayoutMask;

  // Counted here, once per call, before the dispatch - so every caller is covered and the
  // "specialized" column of the census is answering about the branch actually taken below.
  const uint32_t slot = CensusSlot(layout);
  const uint32_t src_index =
      static_cast<uint32_t>(source) < kCensusSources ? static_cast<uint32_t>(source) : 3u;
  ++CensusCalls[slot][src_index];
  CensusVertices[slot][src_index] += count;

  switch (layout) {
#define GK_FVF_CASE(bits)                                                                      \
  case bits:                                                                                   \
    ConvertRun(std::integral_constant<uint32_t, bits>{}, bytes, stride, count, dst);           \
    break;
    GK_SPECIALIZED_FVFS(GK_FVF_CASE)
#undef GK_FVF_CASE
  default:
    ConvertRun(fvf, bytes, stride, count, dst);
    break;
  }
  return true;
}

} // namespace vulkan
} // namespace gk
