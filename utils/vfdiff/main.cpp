// §4.82's differential harness, rebuilt: the current src/VertexFormat.cpp against the same file
// at git HEAD, re-namespaced into `refvulkan` so both link into one binary.
//
// The comparison is `memcmp` over the whole CanonicalVertex, not a float epsilon: the conversion
// is a byte transform and anything it does differently is a defect, not a rounding difference.

#include "VertexFormat.h"
#include "RefVertexFormat.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

namespace {

uint32_t Rng(uint32_t &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

int failures = 0;
int cases = 0;

void Check(bool ok, const char *what, uint32_t fvf, uint32_t stride, uint32_t count) {
  ++cases;
  if (!ok) {
    ++failures;
    if (failures <= 20) {
      std::printf("FAIL %-28s fvf 0x%03x stride %u count %u\n", what, fvf, stride, count);
    }
  }
}

} // namespace

int main() {
  uint32_t seed = 0x1234567u;
  std::vector<uint8_t> src(64 * 1024);
  for (auto &b : src) {
    b = static_cast<uint8_t>(Rng(seed));
  }

  const uint32_t counts[] = {0, 1, 2, 3, 17, 64};

  // Every FVF the position/normal/diffuse/specular/tex-count encoding can express, plus the same
  // set with unrelated bits set - which is what the masked dispatch is supposed to absorb.
  for (uint32_t extra_i = 0; extra_i < 2; ++extra_i) {
    const uint32_t extra = extra_i == 0 ? 0u : 0x8000u | 0x400000u;
    for (uint32_t pos = 0; pos < 2; ++pos) {
      for (uint32_t bits = 0; bits < 8; ++bits) {
        for (uint32_t tex = 0; tex < 3; ++tex) {
          const uint32_t fvf = (pos == 0 ? 0x002u : 0x004u) | ((bits & 1) ? 0x010u : 0u) |
                               ((bits & 2) ? 0x040u : 0u) | ((bits & 4) ? 0x080u : 0u) |
                               (tex << 8) | extra;

          Check(gk::vulkan::FvfSupported(fvf) == gk::refvulkan::FvfSupported(fvf),
                "FvfSupported", fvf, 0, 0);
          const uint32_t implied = gk::vulkan::FvfStride(fvf);
          Check(implied == gk::refvulkan::FvfStride(fvf), "FvfStride", fvf, 0, 0);
          if (implied == 0) {
            continue;
          }

          // Implied stride and three padded ones: a user-pointer draw states its own.
          const uint32_t strides[] = {0, implied, implied + 4, implied + 16};
          for (uint32_t stride : strides) {
            for (uint32_t count : counts) {
              const uint32_t step = stride != 0 ? stride : implied;
              if (static_cast<size_t>(count) * step > src.size()) {
                continue;
              }
              std::vector<gk::vulkan::CanonicalVertex> a(count + 1);
              std::vector<gk::refvulkan::CanonicalVertex> b(count + 1);
              std::memset(a.data(), 0xAA, a.size() * sizeof(a[0]));
              std::memset(b.data(), 0xAA, b.size() * sizeof(b[0]));

              const bool ra = gk::vulkan::ConvertVertices(fvf, src.data(), count, a.data(), stride);
              const bool rb =
                  gk::refvulkan::ConvertVertices(fvf, src.data(), count, b.data(), stride);
              Check(ra == rb, "ConvertVertices return", fvf, stride, count);
              if (ra && rb) {
                Check(std::memcmp(a.data(), b.data(), size_t(count) * sizeof(a[0])) == 0,
                      "ConvertVertices bytes", fvf, stride, count);
                // The guard vertex must be untouched in both.
                Check(std::memcmp(&a[count], &b[count], sizeof(a[0])) == 0,
                      "ConvertVertices overrun", fvf, stride, count);
              }

              float amin[3], amax[3], bmin[3], bmax[3];
              std::memset(amin, 0, sizeof(amin));
              std::memset(amax, 0, sizeof(amax));
              std::memset(bmin, 0, sizeof(bmin));
              std::memset(bmax, 0, sizeof(bmax));
              const bool pa =
                  gk::vulkan::PositionBounds(fvf, src.data(), count, amin, amax, stride);
              const bool pb =
                  gk::refvulkan::PositionBounds(fvf, src.data(), count, bmin, bmax, stride);
              Check(pa == pb, "PositionBounds return", fvf, stride, count);
              if (pa && pb) {
                Check(std::memcmp(amin, bmin, sizeof(amin)) == 0, "PositionBounds min", fvf,
                      stride, count);
                Check(std::memcmp(amax, bmax, sizeof(amax)) == 0, "PositionBounds max", fvf,
                      stride, count);
              }
            }
          }
        }
      }
    }
  }

  // The rejection and null-argument paths.
  {
    gk::vulkan::CanonicalVertex a{};
    gk::refvulkan::CanonicalVertex b{};
    const uint32_t bad[] = {0x000u, 0x006u, 0x00cu, 0x022u, 0x352u, 0x452u};
    for (uint32_t fvf : bad) {
      Check(gk::vulkan::ConvertVertices(fvf, src.data(), 1, &a) ==
                gk::refvulkan::ConvertVertices(fvf, src.data(), 1, &b),
            "reject", fvf, 0, 1);
    }
    Check(gk::vulkan::ConvertVertices(0x152, nullptr, 1, &a) ==
              gk::refvulkan::ConvertVertices(0x152, nullptr, 1, &b),
          "null src", 0x152, 0, 1);
    Check(gk::vulkan::ConvertVertices(0x152, src.data(), 1, nullptr) ==
              gk::refvulkan::ConvertVertices(0x152, src.data(), 1, nullptr),
          "null dst", 0x152, 0, 1);
    // A stride smaller than the FVF implies must be refused by both.
    Check(gk::vulkan::ConvertVertices(0x252, src.data(), 1, &a, 8) ==
              gk::refvulkan::ConvertVertices(0x252, src.data(), 1, &b, 8),
          "short stride", 0x252, 8, 1);
  }

  std::printf("%d cases, %d failures\n", cases, failures);
  return failures == 0 ? 0 : 1;
}
