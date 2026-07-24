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

int GetEPWEnabled() {
  int *p;
  GetObjectAtOffset(p, 0x006a3001);
  return *p;
}
void SetEPWEnabled(int v) {
  int *p;
  GetObjectAtOffset(p, 0x006a3001);
  *p = v;
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
