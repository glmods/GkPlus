#pragma once

namespace gk {
struct Vec3 final {
  float x, y, z;

  bool operator==(const Vec3 &) const = default;
};

struct Vec4 final {
  float x, y, z, w;

  bool operator==(const Vec4 &) const = default;
};

// Spawns the "laser fence" beam effect between two world points
// (CreateLaserFence @ 0x0051c0f0).
void CreateLaserFence(Vec3 *start, Vec3 *end);
} // namespace gk
