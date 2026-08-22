#include "Misc.h"

#include "Actors.h"
#include "JsBindings.h"

#include <cstring>
#include <iterator>

namespace gk::js {
namespace {

// Why accessors on a namespace object rather than plain exports: a C module's
// named exports are values fixed at instantiation, never live bindings, so
// `import { simulation_running } from "gk"` could only ever be a snapshot of
// whatever was true when the module was linked - which is before any level has
// started, i.e. always false. See the QuickJS conventions in CLAUDE.md.
JSValue GetSimulationRunning(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, IsSimulationRunning());
}

JSValue GetMode(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, GameModeName(GetGameMode()));
}

JSValue GetState(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, GetGameState());
}

// --- difficulty --------------------------------------------------------------
//
// The four EASY / MEDIUM / HARD / EXTREME gate commands are `if difficulty ==
// mine then re-run the rest of the line`, which is an `if` in JS once the value
// is readable. Reads as a name; accepts a name or the raw 0..3.
JSValue GetDifficulty(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, DifficultyName(GetGameDifficulty()));
}

JSValue SetDifficulty(JSContext *ctx, JSValueConst, JSValueConst v) {
  if (JS_IsNumber(v)) {
    int32_t d = 0;
    if (JS_ToInt32(ctx, &d, v)) {
      return JS_EXCEPTION;
    }
    if (d < 0 || d > 3) {
      return JS_ThrowRangeError(ctx, "difficulty must be 0..3");
    }
    SetGameDifficulty(d);
    return JS_UNDEFINED;
  }
  const char *name = JS_ToCString(ctx, v);
  if (name == nullptr) {
    return JS_EXCEPTION;
  }
  int found = -1;
  for (int i = 0; i < 4; ++i) {
    if (std::strcmp(name, DifficultyName(i)) == 0) {
      found = i;
      break;
    }
  }
  JS_FreeCString(ctx, name);
  if (found < 0) {
    return JS_ThrowRangeError(
        ctx, "difficulty must be easy, medium, hard or extreme");
  }
  SetGameDifficulty(found);
  return JS_UNDEFINED;
}

// --- the byte/int toggles ----------------------------------------------------
//
// One accessor pair per global; the getter/setter pointers keep them to two
// lines each. Everything here is client-side state the engine reads on the main
// thread, so none of it needs the executor suspended or a broadcast.
using BoolGetter = bool (*)();
using BoolSetter = void (*)(bool);

JSValue BoolFromSetter(JSContext *ctx, JSValueConst v, BoolSetter set) {
  set(JS_ToBool(ctx, v) != 0);
  return JS_UNDEFINED;
}

#define GK_BOOL_ACCESSORS(js_get, js_set, get_fn, set_fn)                      \
  JSValue js_get(JSContext *ctx, JSValueConst) {                               \
    return JS_NewBool(ctx, get_fn());                                          \
  }                                                                            \
  JSValue js_set(JSContext *ctx, JSValueConst, JSValueConst v) {               \
    return BoolFromSetter(ctx, v, set_fn);                                     \
  }

bool GodMode() { return GetCheats()->IsGodMode != 0; }
void SetGodModeFlag(bool v) { GetCheats()->IsGodMode = v ? 1 : 0; }
bool InfiniteAmmo() { return GetCheats()->IsInfiniteAmmo != 0; }
void SetInfiniteAmmoFlag(bool v) { GetCheats()->IsInfiniteAmmo = v ? 1 : 0; }

GK_BOOL_ACCESSORS(GetGodMode, SetGodMode, GodMode, SetGodModeFlag)
GK_BOOL_ACCESSORS(GetInfiniteAmmo, SetInfiniteAmmo, InfiniteAmmo,
                  SetInfiniteAmmoFlag)
GK_BOOL_ACCESSORS(GetFriendlyFire, SetFriendlyFire, GetFriendlyFireOn,
                  SetFriendlyFireOn)
GK_BOOL_ACCESSORS(GetEPW, SetEPW, GetEPWEnabled, SetEPWEnabled)
GK_BOOL_ACCESSORS(GetChrome, SetChrome, GetChromeEnabled, SetChromeEnabled)
GK_BOOL_ACCESSORS(GetWireframe, SetWireframe, GetWireframeEnabled,
                  SetWireframeEnabled)
GK_BOOL_ACCESSORS(GetLowDetail, SetLowDetail, GetDetailLevelToggle,
                  SetDetailLevelToggle)
GK_BOOL_ACCESSORS(GetVisionCones, SetVisionCones, GetVisionConesEnabled,
                  SetVisionConesEnabled)
GK_BOOL_ACCESSORS(GetReconMode, SetReconMode, GetReconModeActive,
                  SetReconModeActive)
GK_BOOL_ACCESSORS(GetRangeRings, SetRangeRings, GetRangeRingsShown,
                  SetRangeRingsShown)

#undef GK_BOOL_ACCESSORS

// `CONTROLS ON|OFF`, inverted so the property reads as the affirmative.
JSValue GetControlsEnabled(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, !GetControlsDisabled());
}
JSValue SetControlsEnabled(JSContext *ctx, JSValueConst, JSValueConst v) {
  SetControlsDisabled(JS_ToBool(ctx, v) == 0);
  return JS_UNDEFINED;
}

JSValue GetPaused(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, IsActivePauseOn());
}

// --- ints --------------------------------------------------------------------

JSValue GetBattleNumberJs(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, GetBattleNumber());
}
JSValue SetBattleNumberJs(JSContext *ctx, JSValueConst, JSValueConst v) {
  int32_t n = 0;
  if (JS_ToInt32(ctx, &n, v)) {
    return JS_EXCEPTION;
  }
  if (n <= 0) {
    return JS_ThrowRangeError(ctx, "battle_number must be positive");
  }
  SetBattleNumber(n);
  return JS_UNDEFINED;
}

JSValue GetTrainingAreaJs(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, GetTrainingArea());
}
JSValue SetTrainingAreaJs(JSContext *ctx, JSValueConst, JSValueConst v) {
  int32_t n = 0;
  if (JS_ToInt32(ctx, &n, v)) {
    return JS_EXCEPTION;
  }
  // The command's own bound: `SET TRAINING AREA` ignores anything outside 1..6.
  if (n < 1 || n > 6) {
    return JS_ThrowRangeError(ctx, "training_area must be 1..6");
  }
  SetTrainingArea(n);
  return JS_UNDEFINED;
}

// --- actor selection ---------------------------------------------------------

JSValue GetSelectedActor(JSContext *ctx, JSValueConst) {
  Actor *actor = GetActorById(GetSelectedActorId());
  return actor == nullptr ? JS_NULL : NewActorWrapper(ctx, actor);
}

JSValue SetSelectedActor(JSContext *ctx, JSValueConst, JSValueConst v) {
  if (JS_IsNull(v) || JS_IsUndefined(v)) {
    SetSelectedActorId(-1);
    return JS_UNDEFINED;
  }
  // An actor, so the getter and the setter agree on their type. It used to take
  // an id, which made this the one accessor in the surface whose two halves
  // disagreed - and because JS_ToInt32 on an object yields 0 rather than
  // throwing, assigning the actor the getter had just returned selected actor 0.
  Actor *actor = ActorFromValue(ctx, v);
  if (!actor) {
    return JS_EXCEPTION;
  }
  SetSelectedActorId(actor->id);
  return JS_UNDEFINED;
}

JSValue GetActorUnderCursorJs(JSContext *ctx, JSValueConst) {
  Actor *actor = GetActorUnderCursor();
  return actor == nullptr ? JS_NULL : NewActorWrapper(ctx, actor);
}

// --- spawn -------------------------------------------------------------------

// `SPAWN` and `SPAWN TEAM` are DoSpawn with the count picked off the difficulty
// (easy 1, medium 2, hard and extreme 3). The scaling is a one-liner in JS once
// `difficulty` is readable, so this takes the count outright.
//
// Deliberately does not throw off the simulation: DoSpawn holds the authority
// gate itself, so on a joining client this is the same silent no-op the console
// command is, and raising here would break a `message_received` that every
// machine runs.
JSValue Spawn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  int32_t amount = 1;
  int32_t team = -1;
  if (argc > 0 && JS_ToInt32(ctx, &amount, argv[0])) {
    return JS_EXCEPTION;
  }
  if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &team, argv[1])) {
    return JS_EXCEPTION;
  }
  DoSpawn(team, amount);
  return JS_UNDEFINED;
}

// --- session control ---------------------------------------------------------
//
// Moved here from `screen`, which had collected it because these are all console
// commands. `screen` is the presentation layer - borders, briefing text, FMVs,
// the cursor - and pausing, chatting and exiting are not that. Where a member
// lives should say what it affects, not which of the two binding styles it
// happens to use.
//
// Still command-backed, for the reason src/JsCommands.cpp gives: the handler is
// the argument parser and dispatching it keeps the defaults and the executor
// handshake. Three of the four names come out of glres<lang>.dll, so they cannot
// be spelled as literals.

// Single player only; in multiplayer it is a vote sent to the server. `paused`
// reads the result - the write stays a command because toggling it is a clock
// handshake.
JSValue TogglePause(JSContext *ctx, JSValueConst, int argc,
                    JSValueConst *argv) {
  return RunConsoleCommand(ctx, "PAUSE GAME", argc, argv);
}

// The argument is required, unlike the console's, where omitting it *prints* the
// current value. A script asking for a value it cannot read back is a mistake
// worth catching rather than a line in the console.
JSValue SetSpeed(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "set_speed(speed) expects a speed - 1 is normal, 0 is the active "
             "pause. The console prints the current value when it is omitted; "
             "there is nothing here for that to return.");
  }
  return RunConsoleCommand(ctx, "GAMESPEED", argc, argv);
}

// Broadcasts a chat message. Takes the rest of the line, so whitespace is
// allowed here where every other console argument refuses it.
JSValue Say(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return RunLocalizedConsoleCommand(ctx, 10014, argc, argv);
}

// Exits to the desktop. `levels.quit()` is the one that ends the *session* and
// returns to the front end.
JSValue Quit(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return RunLocalizedConsoleCommand(ctx, 10001, argc, argv);
}

const JSCFunctionListEntry GameProps[] = {
    JS_CGETSET_DEF("simulation_running", GetSimulationRunning, nullptr),
    JS_CGETSET_DEF("mode", GetMode, nullptr),
    JS_CGETSET_DEF("state", GetState, nullptr),
    JS_CGETSET_DEF("paused", GetPaused, nullptr),
    JS_CGETSET_DEF2("difficulty", GetDifficulty, SetDifficulty,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("god_mode", GetGodMode, SetGodMode,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("infinite_ammo", GetInfiniteAmmo, SetInfiniteAmmo,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("friendly_fire", GetFriendlyFire, SetFriendlyFire,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("controls_enabled", GetControlsEnabled, SetControlsEnabled,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("epw", GetEPW, SetEPW,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("chrome", GetChrome, SetChrome,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("wireframe", GetWireframe, SetWireframe,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("low_detail", GetLowDetail, SetLowDetail,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("vision_cones", GetVisionCones, SetVisionCones,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("recon_mode", GetReconMode, SetReconMode,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("range_rings", GetRangeRings, SetRangeRings,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("battle_number", GetBattleNumberJs, SetBattleNumberJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("training_area", GetTrainingAreaJs, SetTrainingAreaJs,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("selected_actor", GetSelectedActor, SetSelectedActor,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF("actor_under_cursor", GetActorUnderCursorJs, nullptr),
    JS_CFUNC_DEF("spawn", 2, Spawn),
    JS_CFUNC_DEF("toggle_pause", 0, TogglePause),
    JS_CFUNC_DEF("set_speed", 1, SetSpeed),
    JS_CFUNC_DEF("say", 1, Say),
    JS_CFUNC_DEF("quit", 0, Quit),
};

} // namespace

JSValue NewGameNamespace(JSContext *ctx) {
  return NewNamespace(ctx, GameProps, static_cast<int>(std::size(GameProps)));
}

} // namespace gk::js
