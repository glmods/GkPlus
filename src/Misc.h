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
// Actor::Update, ApplyUpdateMessage, LoadLevel). They were modelled
// as two `int`s here, which put IsInfiniteAmmo on 0x007b9c74 - that address is
// RenderStateFlags, so writing the cheat corrupted the renderer's state word.
struct Cheats {
  unsigned char IsGodMode;
  unsigned char IsInfiniteAmmo;
};
static_assert(sizeof(Cheats) == 2);
static_assert(offsetof(Cheats, IsInfiniteAmmo) == 1);

// --- Native API over game-state globals -------------------------------------

// Each line below is a getter and a setter over one global, both plain reads
// and writes with no side effects. `GetGameMode` is the engine's 0..5 mode
// (see GameModeName); `GetGameState` is the coarse phase a level load moves
// through; `GetFoobar` keeps the binary's own name for a global whose meaning
// has not been recovered.
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
/// The lower-case name of a Difficulty value (`"easy"`, `"medium"`, `"hard"`,
/// `"extreme"`), or `"unknown"` outside 0..3. The pointer is to a literal and
/// is never null.
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

// `ReconModeActive` @ 0x007b9ca1, the byte the "Toggle Recon Mode on/off" binding flips
// (DIK 28, Enter by default; the command wheel is the only other route). `ToggleReconMode`
// @ 0x004976d0 has **no console command** behind it, which is the whole reason this pair
// exists - nothing else in this repo could reach it.
//
// It is more than a camera: `DrawOrderMenu`'s pass over the visible units, entered at
// 0x0049a063, is gated on this byte being non-zero, and that pass is where every vision
// cone and hearing-range ring is submitted. **Which side of the byte the player calls
// "recon" is an open question** - the name may be inverted, see ai_behaviour_notes.md
// section 11 - so this is deliberately a value and not a mode.
//
// Two things the setter cannot do anything about. It calls the game's **toggle**, which
// does far more than write the flag (camera save/restore, cursor mode, mouse picking), so
// the byte must never be written directly; and the `:= 0` direction is gated on a non-empty
// selection at 0x00497786, so `SetReconModeActive(false)` **silently does nothing** unless
// something is selected. Set the selection first.
bool GetReconModeActive();    void SetReconModeActive(bool v);

// `ShowRangeRingsToggle` @ 0x006a373e - another gate on the range-ring submit (0x0049bc72,
// and the cone's at 0x0049baca), and the key bound to "Toggle vision cones on/off"
// (DIK 83, numpad `.`). It lives in .data with an initial value of 1, so it is on until the
// player turns it off.
//
// These two plus `VISION on` are **necessary and measurably not sufficient**: with all three
// set and a role carrying both draw flags in frame, level03 submitted no cone and no ring at
// all. At least one further gate is unaccounted for; ai_behaviour_notes.md section 7 has the
// measurement and the address range still to read.
bool GetRangeRingsShown();    void SetRangeRingsShown(bool v);

bool GetControlsDisabled();   void SetControlsDisabled(bool v);// 0x007b9ca0, byte

// 0x006abe18, which is `GetSettings()->IsFriendlyFireOn`: the menu preference
// and the global the FRIENDLY FIRE command writes are one and the same address
// (0x006abdd0 + 0x48). Two names for it, one storage.
bool GetFriendlyFireOn();     void SetFriendlyFireOn(bool v);
int GetTrainingArea();        void SetTrainingArea(int area);  // 0x007b9d14
int GetSelectedActorId();     void SetSelectedActorId(int id); // 0x006a3004
/// Whether Active Pause is engaged. Read-only here: the engine has no
/// single-global setter for it.
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

// The pause handshake, and the RAII guard that is how GkPlus should use it.
//
// `SuspendExecutor` @ 0x00505290 / `ResumeExecutor` @ 0x005052d0. Despite the shape, not a
// critical section: a recursion-counted event handshake (threading_model_notes.md, "Pause
// handshake"). On the main thread the first Suspend sets the pause-request event and blocks
// until the executor acknowledges; the last Resume releases it. **On the executor thread both
// are no-ops**, tested by thread id - which is what makes the guard safe to take from a hook
// without first knowing which thread it is on.
//
// This is the engine's own answer to the two-thread split, used by 97 of the 249 Command*
// handlers before they touch the world, and it is the right tool for two jobs GkPlus has:
// mutating actors/roles from the main thread while the simulation is live, and reading a
// *consistent* multi-field or multi-object view of state the executor is mutating - walking
// the actors hash, or a list of live buffer wrappers, without it being relinked underneath.
//
// Cost is a thread round trip, so it belongs around a whole operation, never inside a loop.
void SuspendExecutor();
/// Releases the executor thread suspended by SuspendExecutor(). Must be paired
/// with it; prefer ExecutorPause, because an early return between the two
/// parks the executor forever and presents as a total game freeze.
void ResumeExecutor();

// Scope guard. Prefer this to the bare pair: an early return between them would otherwise
// leave the executor parked forever, which presents as a total game freeze.
struct ExecutorPause {
  ExecutorPause() { SuspendExecutor(); }
  ~ExecutorPause() { ResumeExecutor(); }
  ExecutorPause(const ExecutorPause &) = delete;
  ExecutorPause &operator=(const ExecutorPause &) = delete;
};

/// The actor the mouse is over, or nullptr. Borrowed; the pointer is only good
/// for the current frame.
Actor *GetActorUnderCursor();   // 0x007b68e8
/// The live GLKeys settings block, writable in place. Writes here are what the
/// front end's own menus write; they reach `GLkeys.cfg` only on a clean exit.
GLKeysSettings *GetSettings();  // 0x006abdd0
/// The two cheat bytes, writable in place. Never widen these to `int`: the
/// dword past them is the renderer's state word.
Cheats *GetCheats();            // 0x007b9c70

// Name of a game mode (0..5): "single_player", "cooperative", ...
const char *GameModeName(int mode);
} // namespace gk
