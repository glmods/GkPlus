#pragma once

#include "Math.h"

namespace gk {
// Camera globals: CameraPosition @ 0x007b4e0c, CameraDistance @ 0x007b3e78,
// MaxCameraDistance @ 0x006a5748.
Vec3 GetCameraPosition();
void SetCameraPosition(Vec3 pos);

float GetCameraDistance();
void SetCameraDistance(float dist);

float GetMaxCameraDistance();
void SetMaxCameraDistance(float dist);
} // namespace gk
