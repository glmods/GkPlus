#include "World.h"

#include "Core.h"

namespace gk {
namespace {
// 4096 BAM to a turn, the constant SetSunAngle multiplies degrees by before it
// indexes SinTable/CosTable.
constexpr float kBamPerDegree = 4096.0f / 360.0f;

// The scene's LightSet @ 0x007c18cc - the receiver
// LightSet_SetEmissiveColour is a method on. This was called GetRenderer() until
// the callee was read: the object is a LightSet, not a renderer.
void *GetSceneLightSet() {
  void **p;
  GetObjectAtOffset(p, 0x007c18cc);
  return *p;
}

// FogSystem @ 0x006b0144 - a pointer, null until a level is loaded.
unsigned char *GetFogSystem() {
  unsigned char **p;
  GetObjectAtOffset(p, 0x006b0144);
  return *p;
}

float *FogField(unsigned offset) {
  unsigned char *fog = GetFogSystem();
  return fog == nullptr ? nullptr
                        : reinterpret_cast<float *>(fog + offset);
}

// The AwColour LightSet_SetEmissiveColour takes (named LightInfo here until the
// callee was read). `packed` is the 0xAARRGGBB byte form the callee expands into
// `rgba` when `valid_mask & ~2` is set; with flags == 2 only `rgba` and
// `valid_mask` are read, so `packed` stays zero here - the game leaves it as
// stack garbage and the flag makes it unreachable either way.
struct AwColour {
  int packed;
  float color[4];
  int flags;
};
static_assert(sizeof(AwColour) == 0x18);
static_assert(offsetof(AwColour, color) == 0x04);
static_assert(offsetof(AwColour, flags) == 0x14);
} // namespace

float GetSunAngle() {
  // 0x0043fcd0, __fastcall(float *out) -> out. Recovers the angle in degrees
  // from the direction vector; the engine stores no angle of its own.
  FastCall<float *, float *> fn;
  GetObjectAtOffset(fn, 0x0043fcd0);
  float out = 0.0f;
  return *fn(&out);
}

void SetSunAngle(float degrees) {
  StdCall<void, float> fn;
  GetObjectAtOffset(fn, 0x0043e540);
  fn(degrees);
}

float GetSunAngle2() {
  float *p;
  GetObjectAtOffset(p, 0x006a311c);
  return *p / kBamPerDegree;
}

void SetSunAngle2(float degrees) {
  float *p;
  GetObjectAtOffset(p, 0x006a311c);
  *p = degrees * kBamPerDegree;
  // The second angle only takes effect through SetSunAngle, which rebuilds the
  // direction vector from both - so re-apply the first one, exactly as
  // CommandSunAngle2 does.
  SetSunAngle(GetSunAngle());
}

void SetSunBrightness(float r, float g, float b, float a) {
  StdCall<void, float, float, float, float> fn;
  GetObjectAtOffset(fn, 0x0043e710);
  fn(r, g, b, a);
}

Vec3 GetSunDirection() {
  Vec3 *p;
  GetObjectAtOffset(p, 0x007b9ce0);
  return *p;
}

void LightSet_SetEmissiveColour(float r, float g, float b, float a) {
  AwColour info{};
  info.color[0] = r;
  info.color[1] = g;
  info.color[2] = b;
  info.color[3] = a;
  info.flags = 2; // "the floats are authoritative" - see World.h
  ThisCall<void, void *, AwColour *> fn;
  GetObjectAtOffset(fn, 0x00579ef0);
  fn(GetSceneLightSet(), &info);
}

bool HasFog() { return GetFogSystem() != nullptr; }

void SetFogEnabled(bool enabled) {
  if (!HasFog()) {
    return;
  }
  StdCall<void, bool> fn;
  GetObjectAtOffset(fn, 0x00472230);
  fn(enabled);
}

int GetFogMode() {
  unsigned char *fog = GetFogSystem();
  if (fog == nullptr) {
    return 0;
  }
  // The mode is written through two sub-objects; +0xec is the first, and the
  // value lands at +0x08 on it.
  auto *sub = *reinterpret_cast<unsigned char **>(fog + 0xec);
  return sub == nullptr ? 0 : *reinterpret_cast<int *>(sub + 0x08);
}

float GetFogValue() {
  float *p = FogField(0xa4);
  return p == nullptr ? 0.0f : *p;
}
void SetFogValue(float value) {
  if (float *p = FogField(0xa4)) {
    *p = value;
  }
}

float GetFogUpdateRate() {
  float *p = FogField(0xa8);
  return p == nullptr ? 0.0f : *p;
}
void SetFogUpdateRate(float rate) {
  if (float *p = FogField(0xa8)) {
    *p = rate;
  }
}

float GetFogTransition() {
  float *p = FogField(0xac);
  return p == nullptr ? 0.0f : *p;
}
void SetFogTransition(float metres) {
  float *distance = FogField(0xac);
  float *reciprocal = FogField(0xb0);
  if (distance == nullptr || reciprocal == nullptr || metres <= 0.0f) {
    return;
  }
  *distance = metres;
  // CommandFogTransition derives this from the same 1.0 the range check uses.
  *reciprocal = 1.0f / metres;
}

void GetFogColor(float *rgba) {
  float *p = FogField(0xb8);
  for (int i = 0; i < 4; ++i) {
    rgba[i] = p == nullptr ? 0.0f : p[i];
  }
}

void SetFogColor(float r, float g, float b, float a) {
  unsigned char *fog = GetFogSystem();
  if (fog == nullptr) {
    return;
  }
  float rgba[4] = {r, g, b, a};
  ThisCall<void, void *, float *> fn;
  GetObjectAtOffset(fn, 0x00469270);
  fn(fog, rgba);
}
} // namespace gk
