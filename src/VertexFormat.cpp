#include "VertexFormat.h"

#include <cstring>

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

bool ConvertVertices(uint32_t fvf, const void *src, uint32_t count, CanonicalVertex *dst) {
  const uint32_t stride = FvfStride(fvf);
  if (stride == 0 || src == nullptr || dst == nullptr) {
    return false;
  }

  const bool has_xyz = (fvf & kXyz) != 0;
  const bool has_rhw = (fvf & kXyzRhw) != 0;
  const bool has_normal = (fvf & kNormal) != 0;
  const bool has_diffuse = (fvf & kDiffuse) != 0;
  const bool has_specular = (fvf & kSpecular) != 0;
  const uint32_t tex_count = (fvf & kTexCountMask) >> kTexCountShift;

  const auto *bytes = static_cast<const uint8_t *>(src);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *p = bytes + static_cast<size_t>(i) * stride;
    CanonicalVertex &v = dst[i];

    // Neutral defaults for everything the source layout omits, so a missing field never
    // reads as garbage downstream: opaque white is the identity for a modulate, and a zero
    // normal is distinguishable from a real one.
    v = CanonicalVertex{};
    v.pos[3] = 1.0f;
    v.color = 0xffffffffu;

    if (has_xyz) {
      v.pos[0] = ReadFloat(p);
      v.pos[1] = ReadFloat(p + 4);
      v.pos[2] = ReadFloat(p + 8);
      p += 12;
    } else if (has_rhw) {
      // Already in screen space; the fourth component is 1/w, not w.
      v.pos[0] = ReadFloat(p);
      v.pos[1] = ReadFloat(p + 4);
      v.pos[2] = ReadFloat(p + 8);
      v.pos[3] = ReadFloat(p + 12);
      p += 16;
    }
    if (has_normal) {
      v.normal[0] = ReadFloat(p);
      v.normal[1] = ReadFloat(p + 4);
      v.normal[2] = ReadFloat(p + 8);
      p += 12;
    }
    if (has_diffuse) {
      v.color = ReadDword(p);
      p += 4;
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
    }
    if (tex_count >= 2) {
      v.uv1[0] = ReadFloat(p);
      v.uv1[1] = ReadFloat(p + 4);
    }
  }
  return true;
}

} // namespace vulkan
} // namespace gk
