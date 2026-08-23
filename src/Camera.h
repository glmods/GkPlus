#pragma once

#include "Math.h"

#include <cstddef>

namespace gk {
// The camera's own state object @ 0x007b4ba0. Everything before the Euler angles
// is matrices and quaternions that UpdateCameraMatrix rebuilds from them, so
// only the angles are modelled - the rest is opaque and must not be touched by
// hand.
//
// **This mirror is deliberately truncated, and it used to claim it was not.** The
// comment here read "the engine's object is 0x23e bytes: two more flag bytes at
// 0x23c/0x23d that the three angle setters raise". The flag bytes are real; the
// size was an undercount by 0x62 bytes, and it stopped just short of the block
// the renderer work needs. The real layout, from CameraData_Ctor @ 0x004b0190
// (which has no xrefs - it is reached from a CRT static-initialiser table sitting
// as undefined bytes, exactly CLAUDE.md's function-pointer trap):
//
//   base Camera, sizeof 0x26c, vptr 0x0066cc9c, ctor 0x00576470
//     +0x044 / +0x084 / +0x0c4  projection / view / world matrix
//     +0x19c / +0x1a0           MinZ / MaxZ, the camera's slice of the depth range
//     +0x1cc / +0x1d0           base / effective D3DCMPFUNC, both D3DCMP_LESSEQUAL
//     +0x230 / +0x234 / +0x238  yaw / roll / pitch, the three below
//     +0x240 .. +0x24c          ortho width, height, 2/w, -2/h
//     +0x250                    is_perspective (0 for every HUD camera)
//     +0x254                    D3DVIEWPORT8, MinZ at +0x264, MaxZ at +0x268
//   derived CameraData, sizeof 0x2a0, vptr 0x006644a0
//     +0x26c  CameraCoords    <- 0x007b4e0c, a MEMBER, not the separate global
//     +0x278  MapCameraPlane     the address map used to call it
//     +0x284, +0x290 CameraDistance2
//
// The arithmetic is its own check: 0x007b4ba0 + 0x26c is exactly the 0x007b4e0c
// this file's own CameraCoords accessors read, and the next camera in the run,
// Camera_Hud @ 0x007b4e40, is at +0x2a0. `rendering_notes.md` §4.4 has the slice
// table; the offsets past 0x23c stay unmodelled because nothing here reads them.
struct CameraData {
  unsigned char opaque[0x230];
  float yaw;   // 0x230, degrees - the setter converts to BAM to index SinTable
  float roll;  // 0x234, degrees
  float pitch; // 0x238, degrees
  unsigned char opaque_tail[0x64]; // 0x23c..0x2a0, see above
};
static_assert(sizeof(CameraData) == 0x2a0);
static_assert(offsetof(CameraData, yaw) == 0x230);
static_assert(offsetof(CameraData, roll) == 0x234);
static_assert(offsetof(CameraData, pitch) == 0x238);

// --- Native API over the camera ---------------------------------------------

// CameraData @ 0x007b4ba0. The angle setters below go through the engine's own
// __thiscall methods rather than writing the fields, because each one also
// rebuilds its quaternion off the sin/cos tables and raises the dirty flags.
CameraData *GetCameraData();

// CameraCoords @ 0x007b4e0c - which is `CameraData + 0x26c`, a member of the
// object above rather than a global of its own (see the layout note there); it
// is addressed absolutely here because that is how the engine's own code reads
// it. Setting it calls UpdateCameraMatrix, which is what
// `SET CAMERA POS` does - writing the global alone leaves the view matrix stale
// until something else happens to rebuild it.
Vec3 GetCameraPosition();
/// Moves the camera to \p pos in world units and rebuilds the view matrix, the
/// way `SET CAMERA POS` does.
void SetCameraPosition(Vec3 pos);

// Yaw/roll/pitch in **degrees**, the units the console and the .gcs use.
// SetCameraOrientation is `SET CAMERA ORI`: all three, then one matrix rebuild.
float GetCameraYaw();
/// The camera's roll, in degrees.
float GetCameraRoll();
/// The camera's pitch, in degrees.
float GetCameraPitch();
/// Sets the yaw, in degrees, and rebuilds the matrix. The engine converts to
/// BAM internally to index its sine table, so the stored value read back may
/// differ from \p degrees by the table's quantisation.
void SetCameraYaw(float degrees);
/// Sets the roll, in degrees, and rebuilds the matrix.
void SetCameraRoll(float degrees);
/// Sets the pitch, in degrees, and rebuilds the matrix.
void SetCameraPitch(float degrees);
/// Sets all three angles in degrees and rebuilds the matrix once, which is what
/// `SET CAMERA ORI` does. Prefer it to three separate setters.
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
/// Sets the camera's distance from its focus, writing both globals so the
/// interpolator cannot undo it.
void SetCameraDistance(float dist);

/// The zoom-out limit currently in force.
float GetMaxCameraDistance();
/// Sets the zoom-out limit, writing both globals.
void SetMaxCameraDistance(float dist);

// CameraFocus @ 0x007b3e58, plus the two mode flags SetCameraFocusLocked
// @ 0x00487f80 latches: CameraFocusLocked @ 0x007b3d11 and CameraFocusPointSet
// @ 0x007b3d0e. Setting a focus is `SET CAMERA FOCUS`, clearing it is
// `FREE CAMERA FOCUS`.
Vec3 GetCameraFocus();
/// Points the camera at \p focus and latches both mode flags, as
/// `SET CAMERA FOCUS` does.
void SetCameraFocus(Vec3 focus);
/// Releases the focus point, as `FREE CAMERA FOCUS` does. The value returned by
/// GetCameraFocus() afterwards is stale rather than cleared.
void ClearCameraFocus();
/// Whether a focus point is currently latched.
bool IsCameraFocusSet();

// The camera's actor-tracking list, CameraTrackList @ 0x007b3e90 with its count
// @ 0x007b3e94. Only the read and the teardown are exposed: *starting* a track
// is `TRACK <id>`, which the engine does by broadcasting update 0xb4 rather than
// by touching the list locally, so it has no local-only equivalent.
bool IsCameraTracking();
/// Empties the track list and clears the controls-disabled flag the engine
/// sets while tracking. There is no local-only way to *start* a track; see
/// above.
void StopCameraTracking(); // ClearCameraTrack @ 0x00487e30, + ControlsDisabled

// CurrentCameraIsPerspective @ 0x007c1470 - the engine's own record of whether the
// camera currently applied to the device is a perspective one, written by the
// canonical four-step camera switch (`rendering_notes.md` §4.4) and by
// `src/HudFix.cpp` when it re-asserts one.
//
// **It is this game's "is this draw part of the world" test**, and it is exact
// rather than a heuristic: `InitRenderCameras` @ 0x004af4d0 runs every one of the
// nine cameras through `Camera_SetOrthographic` @ 0x004b04e0 EXCEPT
// `Camera_World` and the sky camera, so orthographic means a 2D layer - the
// menus, the text, the HUD - and perspective means the scene. The depth slice
// each camera owns is *not* a substitute: one orthographic camera runs
// 0.06..0.30 and overlaps the world's 0.10..1.00.
//
// The Vulkan renderer reads it per draw, which is what lets it keep the 2D
// layers out of the HDR pipeline (`vulkan_renderer_notes.md` §4.92).
bool CurrentCameraIsPerspective();
} // namespace gk
