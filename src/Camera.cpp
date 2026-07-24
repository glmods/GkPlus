#include "Camera.h"

#include "Core.h"

namespace gk {
Vec3 GetCameraPosition() {
  Vec3 *p;
  GetObjectAtOffset(p, 0x007b4e0c);
  return *p;
}
void SetCameraPosition(Vec3 pos) {
  Vec3 *p;
  GetObjectAtOffset(p, 0x007b4e0c);
  *p = pos;
}

float GetCameraDistance() {
  float *p;
  GetObjectAtOffset(p, 0x007b3e78);
  return *p;
}
void SetCameraDistance(float dist) {
  float *p;
  GetObjectAtOffset(p, 0x007b3e78);
  *p = dist;
}

float GetMaxCameraDistance() {
  float *p;
  GetObjectAtOffset(p, 0x006a5748);
  return *p;
}
void SetMaxCameraDistance(float dist) {
  float *p;
  GetObjectAtOffset(p, 0x006a5748);
  *p = dist;
}
} // namespace gk
