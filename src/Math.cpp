#include "Math.h"

#include "Core.h"

namespace gk {
void CreateLaserFence(Vec3 *start, Vec3 *end) {
  FastCall<void, Vec3 *, Vec3 *> fn;
  GetObjectAtOffset(fn, 0x0051c0f0);
  fn(start, end);
}
} // namespace gk
