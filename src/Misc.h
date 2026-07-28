#pragma once

#include <cstddef>

namespace gk {
struct Actor;

// The GLKeys "data" block: the 0x50-byte settings struct ReadGLKeys parses from
// Scripts\GLkeys.cfg and WinMain blits (five MOVUPS) onto the global block at
// 0x006abdd0. The on-disk order is scrambled and interleaves seven unrelated
// 0x007b9cxx globals; this is the in-memory layout. Full field-by-field notes,
// the on-disk order, and the persistence quirks are in menu_system_notes.md.
//
// The persisted block is exactly +0x00..+0x4f. IsAutoCrouchOn and BandwidthUse
// are contiguous in .data right after it but live *outside* the block, so they
// are saved nowhere and reset to their .data defaults every launch.
struct GLKeysSettings {
  int UnusedPrefToggle;       // 0x00  dead: menu 26 case 6 is unreachable
  int LinearMipmapOn;         // 0x04  video toggle
  int AnisotropicFilteringOn; // 0x08  video toggle (auto-cleared if ValidateDevice fails)
  int TripleBufferingOn;      // 0x0c  video toggle
  int Use32BitTextures;       // 0x10  video toggle
  int EnableRenderFlag0x400;  // 0x14  not persisted; WinMain hardcodes to 1
  int DepthStencilBits;       // 0x18  discarded on load (renegotiated per device)
  int VramTextureReduction;   // 0x1c  auto-computed from VRAM, not a user setting
  int TextureDetail;          // 0x20  video multi-value
  int ShadowQuality;          // 0x24  video multi-value (menu binds PendingShadowQuality)
  int ColourDepthIndex;       // 0x28  discarded on load (silently resets to 32-bit)
  int DynamicLightsOn;        // 0x2c  video toggle
  int ParticleFx;             // 0x30  video multi-value
  int CDMusicVolume;          // 0x34  menu 25 Audio, 0..9
  int BattleMusicVolume;      // 0x38  menu 25 Audio, 0..9
  int CinematicsVolume;       // 0x3c  menu 25 Audio, 0..9
  int SoundEffectsVolume;     // 0x40  menu 25 Audio, 0..9
  int AreHintsOn;             // 0x44  menu 26 Prefs, 0/1
  int IsFriendlyFireOn;       // 0x48  menu 26 Prefs, 0/1
  int AreFriendlyMinesOn;     // 0x4c  menu 26 Prefs, 0/1  <-- end of persisted 0x50 block
  int IsAutoCrouchOn;         // 0x50  menu 26 Prefs, 0/1; not persisted (past the block)
  int BandwidthUse;           // 0x54  menu 26 Prefs; MP net throttle 0..9, menu shows
                              //       9 - value; gates optional net updates and caps the
                              //       send backlog at 0x14 - value; not persisted
};
static_assert(sizeof(GLKeysSettings) == 0x58);
static_assert(offsetof(GLKeysSettings, CDMusicVolume) == 0x34);
static_assert(offsetof(GLKeysSettings, AreFriendlyMinesOn) == 0x4c);
static_assert(offsetof(GLKeysSettings, IsAutoCrouchOn) == 0x50);

// The god-mode / infinite-ammo cheat flags @ 0x007b9c70.
//
// **Both are single bytes**, and they are adjacent: every access in the binary
// is a `byte ptr` one (CommandGodMode, CommandInfiniteAmmo, ApplyDamage,
// SyncPositionAndBroadcast, ApplyUpdateMessage, LoadLevel). They were modelled
// as two `int`s here, which put IsInfiniteAmmo on 0x007b9c74 - that address is
// RenderStateFlags, so writing the cheat corrupted the renderer's state word.
struct Cheats {
  unsigned char IsGodMode;
  unsigned char IsInfiniteAmmo;
};
static_assert(sizeof(Cheats) == 2);
static_assert(offsetof(Cheats, IsInfiniteAmmo) == 1);

// --- Native API over game-state globals -------------------------------------

int GetGameMode();       void SetGameMode(int mode);       // 0x007b9e28
int GetGameState();      void SetGameState(int state);     // 0x006b02b4
int GetBattleNumber();   void SetBattleNumber(int n);      // 0x006a79b4
int GetGameDifficulty(); void SetGameDifficulty(int d);    // 0x007b9cc4
int GetFoobar();         void SetFoobar(int v);            // 0x007b9df0

// EPW_enabled @ 0x006a3001 is a **byte**, not an int - `CommandEPW` and its one
// other reader both use `byte ptr`. Reading four bytes there also picked up the
// low byte of selected_actor_id @ 0x006a3004, and writing them clobbered it.
bool GetEPWEnabled();    void SetEPWEnabled(bool v);       // 0x006a3001

// Difficulty, as the four settings the game's own gate commands switch on.
enum class Difficulty : int {
  Easy = 0,
  Medium = 1,
  Hard = 2,
  Extreme = 3,
};
const char *DifficultyName(int difficulty);

// The remaining single-global toggles the console exposes, in command order:
// CHROME, WIREFRAME, SWITCH DETAIL LEVEL, VISION, CONTROLS, FRIENDLY FIRE,
// SET TRAINING AREA, SELECT / GET SELECTED, PAUSE GAME, ECHO.
bool GetChromeEnabled();      void SetChromeEnabled(bool v);   // 0x006a3000, byte
bool GetWireframeEnabled();   void SetWireframeEnabled(bool v);// RenderStateFlags bit
bool GetDetailLevelToggle();  void SetDetailLevelToggle(bool v); // 0x007b9c9c

// SetVisionConesEnabled @ 0x004a0eb0 is two instructions - `MOV byte ptr
// [0x007b4708], CL; RET` - so the flag it writes is also the getter.
bool GetVisionConesEnabled(); void SetVisionConesEnabled(bool v);

bool GetControlsDisabled();   void SetControlsDisabled(bool v);// 0x007b9ca0, byte

// 0x006abe18, which is `GetSettings()->IsFriendlyFireOn`: the menu preference
// and the global the FRIENDLY FIRE command writes are one and the same address
// (0x006abdd0 + 0x48). Two names for it, one storage.
bool GetFriendlyFireOn();     void SetFriendlyFireOn(bool v);
int GetTrainingArea();        void SetTrainingArea(int area);  // 0x007b9d14
int GetSelectedActorId();     void SetSelectedActorId(int id); // 0x006a3004
bool IsActivePauseOn();                                        // 0x00738f78
bool GetConsoleEchoEnabled(); void SetConsoleEchoEnabled(bool v); // 0x007b9c9a

// DoSpawn @ 0x0044d900, __fastcall(team in ECX, amount in EDX) - what the SPAWN
// and SPAWN TEAM commands call once they have scaled `amount` by the difficulty
// (easy 1, medium 2, hard and extreme 3). Team -1 is SPAWN's own default.
void DoSpawn(int team, int amount);

// IsExecutorRunning @ 0x00502da0: whether the simulation runs in *this* process -
// single player and a multiplayer host, false on a joining client and before a
// level has started one. It is the game's own authority test, consulted by 97 of
// the 249 Command* handlers before they touch the world.
//
// The name is the engine's; what it answers is "may I mutate the world here?".
// A joining client must not - its actors arrive through the host's update stream,
// so spawning locally there produces a ghost the host knows nothing about.
bool IsSimulationRunning();

Actor *GetActorUnderCursor();   // 0x007b68e8
GLKeysSettings *GetSettings();  // 0x006abdd0
Cheats *GetCheats();            // 0x007b9c70

// Name of a game mode (0..5): "single_player", "cooperative", ...
const char *GameModeName(int mode);
} // namespace gk
