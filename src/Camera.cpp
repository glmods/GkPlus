#include "Camera.h"

#include "Core.h"
#include "Misc.h"

namespace gk {
namespace {
// CameraData::SetCameraYaw/Roll/Pitch @ 0x0044e440 / 0x0044e200 / 0x0044e320 and
// CameraData::UpdateCameraMatrix @ 0x0044e560. All __thiscall: `this` in ECX and
// the angle on the stack, each ending in RET 0x4 (RET for the no-argument one).
using AngleSetter = ThisCall<void, CameraData *, float>;

void CallAngleSetter(unsigned offset, float degrees) {
  AngleSetter fn;
  GetObjectAtOffset(fn, offset);
  fn(GetCameraData(), degrees);
}
} // namespace

CameraData *GetCameraData() {
  CameraData *p;
  GetObjectAtOffset(p, 0x007b4ba0);
  return p;
}

Vec3 GetCameraPosition() {
  Vec3 *p;
  GetObjectAtOffset(p, 0x007b4e0c);
  return *p;
}
void SetCameraPosition(Vec3 pos) {
  Vec3 *p;
  GetObjectAtOffset(p, 0x007b4e0c);
  *p = pos;
  UpdateCameraMatrix();
}

float GetCameraYaw() { return GetCameraData()->yaw; }
float GetCameraRoll() { return GetCameraData()->roll; }
float GetCameraPitch() { return GetCameraData()->pitch; }

void SetCameraYaw(float degrees) { CallAngleSetter(0x0044e440, degrees); }
void SetCameraRoll(float degrees) { CallAngleSetter(0x0044e200, degrees); }
void SetCameraPitch(float degrees) { CallAngleSetter(0x0044e320, degrees); }

void SetCameraOrientation(float yaw, float roll, float pitch) {
  SetCameraYaw(yaw);
  SetCameraRoll(roll);
  SetCameraPitch(pitch);
  UpdateCameraMatrix();
}

void UpdateCameraMatrix() {
  ThisCall<void, CameraData *> fn;
  GetObjectAtOffset(fn, 0x0044e560);
  fn(GetCameraData());
}

float GetCameraDistance() {
  float *p;
  GetObjectAtOffset(p, 0x007b3e78);
  return *p;
}
void SetCameraDistance(float dist) {
  float *target;
  float *current;
  GetObjectAtOffset(target, 0x007b3e78);
  GetObjectAtOffset(current, 0x007b4e30);
  *target = dist;
  *current = dist;
}

float GetMaxCameraDistance() {
  float *p;
  GetObjectAtOffset(p, 0x006a5748);
  return *p;
}
void SetMaxCameraDistance(float dist) {
  float *a;
  float *b;
  GetObjectAtOffset(a, 0x006a5748);
  GetObjectAtOffset(b, 0x007b9d18);
  *a = dist;
  *b = dist;
}

Vec3 GetCameraFocus() {
  Vec3 *p;
  GetObjectAtOffset(p, 0x007b3e58);
  return *p;
}

void SetCameraFocus(Vec3 focus) {
  // Order matters: SetCameraFocusLocked snaps the interpolation state off the
  // *old* focus on the 0->1 edge, so it has to run before the new one lands.
  FastCall<void, bool> lock;
  GetObjectAtOffset(lock, 0x00487f80);
  lock(true);

  unsigned char *pointSet;
  GetObjectAtOffset(pointSet, 0x007b3d0e);
  *pointSet = 1;

  Vec3 *p;
  GetObjectAtOffset(p, 0x007b3e58);
  *p = focus;
}

void ClearCameraFocus() {
  FastCall<void, bool> lock;
  GetObjectAtOffset(lock, 0x00487f80);
  lock(false);

  unsigned char *pointSet;
  GetObjectAtOffset(pointSet, 0x007b3d0e);
  *pointSet = 0;
}

bool IsCameraFocusSet() {
  unsigned char *pointSet;
  GetObjectAtOffset(pointSet, 0x007b3d0e);
  return *pointSet != 0;
}

bool IsCameraTracking() {
  // IsCameraTracking @ 0x00487ed0 is `CameraTrackCount > 0` and takes no
  // arguments, so it ends in a bare RET and the convention is a formality.
  CDecl<bool> fn;
  GetObjectAtOffset(fn, 0x00487ed0);
  return fn();
}

void StopCameraTracking() {
  // Exactly what CommandStopTracking does: drop the track list, then hand the
  // controls back - ClearCameraTrack alone leaves the player unable to steer.
  CDecl<void> fn;
  GetObjectAtOffset(fn, 0x00487e30);
  fn();
  SetControlsDisabled(false);
}
} // namespace gk
