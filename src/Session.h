#pragma once
#include "Misc.h"

#include <string>

namespace gk {

// Starting a level without touching the front-end menus.
//
// The menu route is a four-screen state machine and no single call reproduces
// it. Menu 7 (SinglePlayer) or menu 5 (ChooseSinglePlayerLevel) only strdup a
// `LevelList` entry into ScriptFileName/ConsoleFileName and reset the mission
// stats; menu 9 (NewSinglePlayerGame) sets GameDifficulty, leaves the front end
// and shows the briefing; and it is the *briefing's* end - `END BRIEFING`, or
// ShowBriefingOrDebriefScreen itself - that finally calls BeginLevelSession.
// Driving that from outside means synthesizing keystrokes, which needs the
// window focused (the front end does not run a frame while it is not) and
// depends on which item happens to be selected, since GoToMenu leaves
// ChosenMenuItem at 0x100 = "nothing".
//
// **The shortcut is the game's own.** LoadGame @ 0x00505730, restoring a
// "carry to the next level" save, ends the session and calls
// `BeginLevelSession(true)` with no menu and no briefing at all - and
// BeginLevelSession is what sets GameState and calls `LoadLevel(freshStart)`.
// StartLevel does exactly that, having first set the state the two menu screens
// would have set. Nothing here reimplements a converter or a loader.
//
// Single-player only, by construction: the sequence calls
// EnterSinglePlayerMode, which forces GameMode. QueueLevelStart refuses while a
// DirectPlay session is live rather than half-applying that.
struct LevelStartRequest {
  // ScriptFileName - the level .gls. For a script-defined level this is its
  // virtual `gkplus\<slug>.gls` name, which names no file and is matched by
  // CustomLevel.cpp rather than opened.
  std::string script;
  // ConsoleFileName - the level .gcs. Empty is normal: ExecuteCommandFile on a
  // missing file is a no-op, and that is what every custom level registers.
  std::string console;
  // GameDifficulty, 0..3 (gk::Difficulty). Note the New Game menu only ever
  // sets 1 or 2 - `GameDifficulty = ChosenMenuItem + 1` over its two items -
  // while coop forces 3 and the training level forces 1.
  int difficulty = static_cast<int>(Difficulty::Medium);
};

// Why this request cannot be scheduled, as a message fit to throw, or nullptr
// when it can. Checked synchronously by QueueLevelStart so that everything the
// caller got wrong is reported at the call and only the load itself is
// deferred.
const char *LevelStartRefusal(const LevelStartRequest &request);

// Schedules `request` and posts the message that will run it. False means
// LevelStartRefusal said no, or the window was not resolved yet; the pending
// request is left alone either way.
//
// **The load does not happen here.** It happens at the next turn of the message
// loop, because LoadLevel must not run inside the renderer - see
// SetMessageLoopCallback in GUI.h. So the caller is safe anywhere: the REPL,
// draw_gui, a trigger message, a frame job. Poll gk::GetGameState() (or
// `actors.count` from a script) to see the result.
bool QueueLevelStart(const LevelStartRequest &request);

// True between QueueLevelStart and the load actually running. A second request
// while one is pending is refused rather than silently replacing it.
bool LevelStartPending();

// Ends the current level session and returns to the main menu, the way the
// front end's own quit path does. Deferred exactly like QueueLevelStart, and
// for the same reason.
bool QueueReturnToMainMenu();

// The blocking body, for a caller that already knows it is at a safe point.
// QueueLevelStart is what you want; this is what the message handler calls.
bool StartLevel(const LevelStartRequest &request);

} // namespace gk
