#include "Session.h"

#include "Console.h"
#include "Core.h"
#include "GUI.h"
#include "Memory.h"
#include "Menu.h"

#include <cstring>
#include <optional>

namespace gk {
namespace {

// --- the natives the two menu screens and LoadGame reach ---------------------

char *GameStrdup(const char *text) {
  if (!text) {
    return nullptr;
  }
  FastCall<char *, const char *> fn;
  GetObjectAtOffset(fn, 0x0044e1a0);
  return fn(text);
}

// Resets the mission-stats block SaveGame snapshots. Both menu entry points
// pass true, so a fresh start does not inherit the previous mission's accuracy
// and timer.
void ResetMissionStats(bool full) {
  FastCall<void, char> fn;
  GetObjectAtOffset(fn, 0x004fcc30);
  fn(full ? 1 : 0);
}

// GameMode = SinglePlayer, plus the two flags beside it and a team-slot reset.
void EnterSinglePlayerMode() {
  StdCall<void> fn;
  GetObjectAtOffset(fn, 0x004f94c0);
  fn();
}

// True while the front end owns its resources. SpriteScrollUp @ 0x007b7d0c is
// created by EnterMainMenuScreen and zeroed by LeaveFrontEndScreen, and those
// are its only two writers, so it says exactly what we need to know.
bool FrontEndResourcesLive() {
  void **sprite;
  GetObjectAtOffset(sprite, 0x007b7d0c);
  return *sprite != nullptr;
}

// Destroys the menu background sprites, writes GLkeys.cfg, applies pending
// video settings - and then releases a reference on ~40 menu sprites and resets
// all 36 menus.
//
// **Exactly once per front-end session.** That tail is unguarded
// (`MOV ECX,[SpriteScrollUp]; ADD ECX,0x9c; ADD [ECX+4],-1`) and every global it
// walks is zeroed on the way out, so a second call faults at 0x004e8eb7 - which
// is what starting a level from inside a level did before the check below. Only
// the first two branches are guarded, which makes the function look idempotent
// if you stop reading at the video-settings block.
void LeaveFrontEndScreen() {
  if (!FrontEndResourcesLive()) {
    return;
  }
  StdCall<int> fn;
  GetObjectAtOffset(fn, 0x004e8dd0);
  fn();
}

// Stops client routing and the executor thread. Guarded by LevelSessionStarted
// @ 0x007b6dd8, so calling it with no session running is free - which is what
// lets one code path serve both "start from the menus" and "switch levels
// mid-game".
void EndLevelSession() {
  StdCall<char> fn;
  GetObjectAtOffset(fn, 0x004e2710);
  fn();
}

// __thiscall, and ECX is dereferenced at the second instruction - both game
// callers pass the global TeamCarryOverState @ 0x007b6d70 (LoadGame does
// `MOV ECX,0x7b6d70` immediately before the call). Declared with no argument
// this faults at 0x004da242 on the first level start, which is how it was
// found; FreeAuxTeamCarryOverStates beside it genuinely takes none.
void ClearTeamCarryOverState() {
  void *state;
  GetObjectAtOffset(state, 0x007b6d70);
  FastCall<void, void *> fn;
  GetObjectAtOffset(fn, 0x004da230);
  fn(state);
}

void FreeAuxTeamCarryOverStates() {
  StdCall<void> fn;
  GetObjectAtOffset(fn, 0x004dafd0);
  fn();
}

// Sets up the session (executor thread, client routing, the gameplay console
// commands) on first use, sets GameState = 0x12, and - with true - calls
// LoadLevel(freshStart = true), which is the call that runs the level's .gcs.
bool BeginLevelSession(bool load_level) {
  FastCall<int, char> fn;
  GetObjectAtOffset(fn, 0x004e2560);
  return fn(load_level ? 1 : 0) != 0;
}

bool IsMultiplayerActive() {
  StdCall<char> fn;
  GetObjectAtOffset(fn, 0x005116f0);
  return fn() != 0;
}

void EnterMainMenuScreen() {
  StdCall<void> fn;
  GetObjectAtOffset(fn, 0x004e7e50);
  fn();
}

// LevelLoadReason @ 0x007b9cf0. 3 means "restoring a full savegame" and makes
// LoadLevel skip the fresh-start work; a level we start ourselves is a fresh
// start, so this has to be cleared - a previous LoadGame leaves it at 3.
void SetLevelLoadReason(int reason) {
  int *p;
  GetObjectAtOffset(p, 0x007b9cf0);
  *p = reason;
}

// The two globals LoadLevel reads to know what to load. Both are owned by the
// game's pool allocator; every writer frees the old value first.
void SetGameScriptFiles(const char *script, const char *console) {
  char **script_slot;
  char **console_slot;
  GetObjectAtOffset(script_slot, 0x007b6dcc);
  GetObjectAtOffset(console_slot, 0x007b6dd0);
  if (*script_slot) {
    pool_free(*script_slot);
  }
  if (*console_slot) {
    pool_free(*console_slot);
  }
  *script_slot = GameStrdup(script);
  *console_slot = GameStrdup(console);
}

// --- the deferred request ----------------------------------------------------

std::optional<LevelStartRequest> Pending;
bool PendingReturnToMenu = false;
bool CallbackInstalled = false;

void RunPending() {
  if (PendingReturnToMenu) {
    PendingReturnToMenu = false;
    EndLevelSession();
    EnterMainMenuScreen();
    return;
  }
  if (!Pending) {
    return;
  }
  LevelStartRequest request = *Pending;
  Pending.reset();
  StartLevel(request);
}

// Installed on first use rather than from a *System ctor: this subsystem
// installs no detour, and the callback must not be live before GUISystem owns
// the window procedure.
bool EnsureCallback() {
  if (!CallbackInstalled) {
    SetMessageLoopCallback(RunPending);
    CallbackInstalled = true;
  }
  return true;
}

} // namespace

const char *LevelStartRefusal(const LevelStartRequest &request) {
  if (request.script.empty()) {
    return "a level start needs a script file name";
  }
  // ScriptFileName ends up in a `# line` directive the GLS parser re-lexes, and
  // in a savegame, and on the wire to a joining client. A quote would break the
  // first of those; see CustomLevel.cpp's VirtualScriptName.
  if (request.script.find('"') != std::string::npos ||
      request.console.find('"') != std::string::npos) {
    return "a script file name may not contain a double quote";
  }
  if (request.difficulty < static_cast<int>(Difficulty::Easy) ||
      request.difficulty > static_cast<int>(Difficulty::Extreme)) {
    return "difficulty must be easy, medium, hard or extreme";
  }
  if (IsMultiplayerActive()) {
    return "a multiplayer session is active - this path forces single player";
  }
  if (Pending || PendingReturnToMenu) {
    return "a level start is already pending";
  }
  return nullptr;
}

bool LevelStartPending() { return Pending.has_value() || PendingReturnToMenu; }

bool QueueLevelStart(const LevelStartRequest &request) {
  if (LevelStartRefusal(request)) {
    return false;
  }
  EnsureCallback();
  Pending = request;
  if (!PostMessageLoopWork()) {
    Pending.reset();
    return false;
  }
  return true;
}

bool QueueReturnToMainMenu() {
  if (LevelStartPending()) {
    return false;
  }
  EnsureCallback();
  PendingReturnToMenu = true;
  if (!PostMessageLoopWork()) {
    PendingReturnToMenu = false;
    return false;
  }
  return true;
}

bool StartLevel(const LevelStartRequest &request) {
  if (const char *refusal = LevelStartRefusal(request)) {
    Print(refusal);
    return false;
  }

  // The order is the menus' own, with LoadGame's teardown in front of the
  // BeginLevelSession the briefing would have reached.
  ResetMissionStats(true);
  // Menu 7's "new game" item drops the carry-over roster; menu 5's does not,
  // because Choose Level is reached mid-campaign. Starting a level outright is
  // the former: nothing should carry in from whatever ran before.
  ClearTeamCarryOverState();
  FreeAuxTeamCarryOverStates();

  SetGameScriptFiles(request.script.c_str(), request.console.c_str());
  EnterSinglePlayerMode();
  SetGameDifficulty(request.difficulty);
  SetLevelLoadReason(0);
  LeaveFrontEndScreen();

  // No-op when nothing is running, a real teardown when a level is - which is
  // what makes this callable from inside a level as well as from the menus.
  EndLevelSession();
  return BeginLevelSession(true);
}

} // namespace gk
