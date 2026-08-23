#pragma once

namespace gk {
/// Three consecutive floats, laid out as the engine lays them out: a world
/// position, a direction or an RGB triple depending on the field it sits in.
/// World coordinates are in the same units a `.gls` writes.
struct Vec3 final {
  float x, y, z;

  bool operator==(const Vec3 &) const = default;
};

/// Four consecutive floats. Used for both quaternion orientations (which is
/// what every actor and camera stores) and RGBA colours.
struct Vec4 final {
  float x, y, z, w;

  bool operator==(const Vec4 &) const = default;
};

// Spawns the "laser fence" beam effect between two world points
// (CreateLaserFence @ 0x0051c0f0).
void CreateLaserFence(Vec3 *start, Vec3 *end);
} // namespace gk
