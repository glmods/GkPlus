#pragma once

#include "Math.h"

#include <cstddef>

namespace gk {
// The camera's own state object @ 0x007b4ba0. Everything before the Euler angles
// is matrices and quaternions that UpdateCameraMatrix rebuilds from them, so
// only the angles are modelled - the rest is opaque and must not be touched by
// hand.
//
// The engine's object is **0x23e** bytes: two more flag bytes at 0x23c/0x23d
// that the three angle setters raise. They are deliberately left off the mirror,
// because only those setters ever write them and including them would push the
// struct to 0x240 under 4-byte alignment - a size assert that no longer matches
// anything real. The three offset asserts are what pin the layout here.
struct CameraData {
  unsigned char opaque[0x230];
  float yaw;   // 0x230, degrees - the setter converts to BAM to index SinTable
  float roll;  // 0x234, degrees
  float pitch; // 0x238, degrees
};
static_assert(sizeof(CameraData) == 0x23c);
static_assert(offsetof(CameraData, yaw) == 0x230);
static_assert(offsetof(CameraData, roll) == 0x234);
static_assert(offsetof(CameraData, pitch) == 0x238);

// --- Native API over the camera ---------------------------------------------

// CameraData @ 0x007b4ba0. The angle setters below go through the engine's own
// __thiscall methods rather than writing the fields, because each one also
// rebuilds its quaternion off the sin/cos tables and raises the dirty flags.
CameraData *GetCameraData();

// CameraCoords @ 0x007b4e0c. Setting it calls UpdateCameraMatrix, which is what
// `SET CAMERA POS` does - writing the global alone leaves the view matrix stale
// until something else happens to rebuild it.
Vec3 GetCameraPosition();
void SetCameraPosition(Vec3 pos);

// Yaw/roll/pitch in **degrees**, the units the console and the .gcs use.
// SetCameraOrientation is `SET CAMERA ORI`: all three, then one matrix rebuild.
float GetCameraYaw();
float GetCameraRoll();
float GetCameraPitch();
void SetCameraYaw(float degrees);
void SetCameraRoll(float degrees);
void SetCameraPitch(float degrees);
void SetCameraOrientation(float yaw, float roll, float pitch);

// CameraData::UpdateCameraMatrix @ 0x0044e560. Rebuilds the view matrix from the
// position and the three quaternions.
void UpdateCameraMatrix();

// The camera keeps its zoom in **two** globals and the game writes both whenever
// it snaps: CameraDistance1 @ 0x007b3e78 is the target the smoothing runs
// towards, CameraDistance2 @ 0x007b4e30 the value in force this frame. Writing
// only the first is a request that the interpolator can overwrite; `SET CAMERA
// DISTANCE` writes both, so these do too. Same story for the zoom-out limit:
// MaxCameraDist1 @ 0x006a5748 and MaxCameraDist2 @ 0x007b9d18.
float GetCameraDistance();
void SetCameraDistance(float dist);

float GetMaxCameraDistance();
void SetMaxCameraDistance(float dist);

// CameraFocus @ 0x007b3e58, plus the two mode flags SetCameraFocusLocked
// @ 0x00487f80 latches: CameraFocusLocked @ 0x007b3d11 and CameraFocusPointSet
// @ 0x007b3d0e. Setting a focus is `SET CAMERA FOCUS`, clearing it is
// `FREE CAMERA FOCUS`.
Vec3 GetCameraFocus();
void SetCameraFocus(Vec3 focus);
void ClearCameraFocus();
bool IsCameraFocusSet();

// The camera's actor-tracking list, CameraTrackList @ 0x007b3e90 with its count
// @ 0x007b3e94. Only the read and the teardown are exposed: *starting* a track
// is `TRACK <id>`, which the engine does by broadcasting update 0xb4 rather than
// by touching the list locally, so it has no local-only equivalent.
bool IsCameraTracking();
void StopCameraTracking(); // ClearCameraTrack @ 0x00487e30, + ControlsDisabled
} // namespace gk
