#include "Misc.h"

#include "Actors.h"
#include "Core.h"

#include <array>

namespace gk {
int GetGameMode() {
  int *p;
  GetObjectAtOffset(p, 0x007b9e28);
  return *p;
}
void SetGameMode(int mode) {
  int *p;
  GetObjectAtOffset(p, 0x007b9e28);
  *p = mode;
}

int GetGameState() {
  int *p;
  GetObjectAtOffset(p, 0x006b02b4);
  return *p;
}
void SetGameState(int state) {
  int *p;
  GetObjectAtOffset(p, 0x006b02b4);
  *p = state;
}

int GetBattleNumber() {
  int *p;
  GetObjectAtOffset(p, 0x006a79b4);
  return *p;
}
void SetBattleNumber(int n) {
  int *p;
  GetObjectAtOffset(p, 0x006a79b4);
  *p = n;
}

int GetGameDifficulty() {
  int *p;
  GetObjectAtOffset(p, 0x007b9cc4);
  return *p;
}
void SetGameDifficulty(int d) {
  int *p;
  GetObjectAtOffset(p, 0x007b9cc4);
  *p = d;
}

int GetFoobar() {
  int *p;
  GetObjectAtOffset(p, 0x007b9df0);
  return *p;
}
void SetFoobar(int v) {
  int *p;
  GetObjectAtOffset(p, 0x007b9df0);
  *p = v;
}

namespace {
// Every one-byte toggle below follows the same shape; the game reads them all
// through `byte ptr`, so an `int` mirror would spill into the next global.
bool GetByteFlag(unsigned offset) {
  unsigned char *p;
  GetObjectAtOffset(p, offset);
  return *p != 0;
}
void SetByteFlag(unsigned offset, bool v) {
  unsigned char *p;
  GetObjectAtOffset(p, offset);
  *p = v ? 1 : 0;
}
} // namespace

bool GetEPWEnabled() { return GetByteFlag(0x006a3001); }
void SetEPWEnabled(bool v) { SetByteFlag(0x006a3001, v); }

const char *DifficultyName(int difficulty) {
  static constexpr std::array<const char *, 4> names = {
      "easy",
      "medium",
      "hard",
      "extreme",
  };
  if (difficulty < 0 || difficulty >= static_cast<int>(names.size())) {
    return "unknown";
  }
  return names[static_cast<size_t>(difficulty)];
}

bool GetChromeEnabled() { return GetByteFlag(0x006a3000); }
void SetChromeEnabled(bool v) { SetByteFlag(0x006a3000, v); }

// WIREFRAME is not its own global: it is bit 0x200000 of RenderStateFlags
// @ 0x007b9c74, which the command sets, clears or toggles.
bool GetWireframeEnabled() {
  unsigned *p;
  GetObjectAtOffset(p, 0x007b9c74);
  return (*p & 0x200000u) != 0;
}
void SetWireframeEnabled(bool v) {
  unsigned *p;
  GetObjectAtOffset(p, 0x007b9c74);
  *p = v ? (*p | 0x200000u) : (*p & ~0x200000u);
}

bool GetDetailLevelToggle() {
  int *p;
  GetObjectAtOffset(p, 0x007b9c9c);
  return *p != 0;
}
void SetDetailLevelToggle(bool v) {
  int *p;
  GetObjectAtOffset(p, 0x007b9c9c);
  *p = v ? 1 : 0;
}

bool GetVisionConesEnabled() { return GetByteFlag(0x007b4708); }
void SetVisionConesEnabled(bool v) {
  // 0x004a0eb0, __fastcall with the flag in CL and a bare RET.
  FastCall<void, bool> fn;
  GetObjectAtOffset(fn, 0x004a0eb0);
  fn(v);
}

bool GetControlsDisabled() { return GetByteFlag(0x007b9ca0); }
void SetControlsDisabled(bool v) { SetByteFlag(0x007b9ca0, v); }

bool GetFriendlyFireOn() {
  int *p;
  GetObjectAtOffset(p, 0x006abe18);
  return *p != 0;
}
void SetFriendlyFireOn(bool v) {
  int *p;
  GetObjectAtOffset(p, 0x006abe18);
  *p = v ? 1 : 0;
}

int GetTrainingArea() {
  int *p;
  GetObjectAtOffset(p, 0x007b9d14);
  return *p;
}
void SetTrainingArea(int area) {
  int *p;
  GetObjectAtOffset(p, 0x007b9d14);
  *p = area;
}

int GetSelectedActorId() {
  int *p;
  GetObjectAtOffset(p, 0x006a3004);
  return *p;
}
void SetSelectedActorId(int id) {
  int *p;
  GetObjectAtOffset(p, 0x006a3004);
  *p = id;
}

bool IsActivePauseOn() {
  int *p;
  GetObjectAtOffset(p, 0x00738f78);
  return *p != 0;
}

bool GetConsoleEchoEnabled() { return GetByteFlag(0x007b9c9a); }
void SetConsoleEchoEnabled(bool v) { SetByteFlag(0x007b9c9a, v); }

void DoSpawn(int team, int amount) {
  FastCall<void, int, int> fn;
  GetObjectAtOffset(fn, 0x0044d900);
  fn(team, amount);
}

bool IsSimulationRunning() {
  // __stdcall, no arguments; it reads the executor-running flag @ 0x007b9df0.
  StdCall<bool> fn;
  GetObjectAtOffset(fn, 0x00502da0);
  return fn();
}

Actor *GetActorUnderCursor() {
  Actor **p;
  GetObjectAtOffset(p, 0x007b68e8);
  return *p;
}

GLKeysSettings *GetSettings() {
  GLKeysSettings *p;
  GetObjectAtOffset(p, 0x006abdd0);
  return p;
}

Cheats *GetCheats() {
  Cheats *p;
  GetObjectAtOffset(p, 0x007b9c70);
  return p;
}

const char *GameModeName(int mode) {
  static constexpr std::array<const char *, 6> names = {
      "single_player", "cooperative", "last_man_standing",
      "president",     "deathmatch",  "capture_the_flag",
  };
  if (mode < 0 || mode >= static_cast<int>(names.size())) {
    return "unknown";
  }
  return names[static_cast<size_t>(mode)];
}
} // namespace gk
