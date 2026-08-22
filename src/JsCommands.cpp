// The typed surface over every console command that is not plain JavaScript or
// developer scaffolding - broadcasters included.
//
// Why these are formatters over ExecuteCommand rather than native wrappers, when
// `camera`, `game` and `world` are the opposite:
//
//   * Every one of these handlers *is* an argument parser. `CommandWater` is
//     eight ConsoleParse* calls whose defaults come from `TheMap->bounds_min`
//     and `bounds_max`; `CommandExplode` falls back to the cursor position;
//     `CommandGive` resolves a role by name and walks an inventory. Rebuilding
//     140 of those in C++ is 140 chances to diverge on a default, a clamp or an
//     omitted-argument rule - and the divergence would be silent.
//   * Dispatching the real handler is faithful **by construction**: the game's
//     own parsing, defaults, range checks and executor handshake all still run.
//   * What a binding adds over a raw string is what these keep: typed and named
//     arguments, locale-independent number formatting, a name that does not
//     depend on the language DLL, and a length check the engine does not do.
//
// So the split is not arbitrary. Anything with **state to read back** is native
// (see Camera.h, Misc.h, World.h); anything that is a fire-and-forget effect is
// here. Nothing here is reachable on a joining client that would not also be
// reachable by typing the command, because it is the same code path.
//
// **The commands that broadcast are here too, and they cost nothing extra.**
// That is the other half of the argument above: an update id and payload only
// have to be reproduced by a *native* binding. Dispatching runs the handler,
// which does its own `IsExecutorRunning` check, its own `SuspendExecutor`
// bracket and its own `BroadcastToPlayers`.
//
// It also means they are safe to call unguarded. 24 of the 25 gate on
// `IsExecutorRunning`, so on a joining client they are a silent no-op while the
// authority machine mutates and broadcasts - which is exactly the behaviour a
// script wants, and the opposite of the trap that `actor.set_position` is.
// (`STATS SCREEN` is the exception and needs no gate: it sends *to* the server,
// which is the right direction from a client.)

#include "Console.h"

#include "ActorArg.h"
#include "Actors.h"
#include "Encoding.h"
#include "JsBindings.h"
#include "Menu.h"
#include "Roles.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace gk::js {
namespace {

// --- argument formatting ------------------------------------------------------

// The console's own number syntax is whatever `atof` accepts, and `atof` here is
// the game's CRT in the "C" locale. Formatting through snprintf would follow
// *this* DLL's locale, so any decimal comma is rewritten - cheaper and more
// robust than swapping the locale around every call.
void AppendNumber(std::string *out, double value) {
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%.9g", value);
  for (char &c : buffer) {
    if (c == ',') {
      c = '.';
    }
  }
  *out += buffer;
}

// Some commands consume the **rest of the line** rather than a word -
// `CopyRemainingArgs` instead of `ConsoleParseWord` - so a space in the argument
// is meaningful there and an error everywhere else. `free_text` is that
// distinction, and it is per command rather than global.
bool AppendValue(JSContext *ctx, std::string *out, JSValueConst v,
                 bool free_text, ActorArgScope *actors);

// A {x, y, z} object becomes the three numbers the console expects, in order.
// Missing components are 0 rather than an error: `ConsoleParsePosition` treats a
// short argument list the same way.
bool AppendVec3Like(JSContext *ctx, std::string *out, JSValueConst v) {
  static const char *kNames[] = {"x", "y", "z"};
  for (const char *name : kNames) {
    JSValue prop = JS_GetPropertyStr(ctx, v, name);
    if (JS_IsException(prop)) {
      return false;
    }
    double d = 0.0;
    if (!JS_IsUndefined(prop) && JS_ToFloat64(ctx, &d, prop)) {
      JS_FreeValue(ctx, prop);
      return false;
    }
    JS_FreeValue(ctx, prop);
    if (!out->empty()) {
      *out += ' ';
    }
    AppendNumber(out, d);
  }
  return true;
}

bool AppendArray(JSContext *ctx, std::string *out, JSValueConst v,
                 bool free_text, ActorArgScope *actors) {
  JSValue lengthValue = JS_GetPropertyStr(ctx, v, "length");
  if (JS_IsException(lengthValue)) {
    return false;
  }
  uint32_t length = 0;
  int failed = JS_ToUint32(ctx, &length, lengthValue);
  JS_FreeValue(ctx, lengthValue);
  if (failed) {
    return false;
  }
  for (uint32_t i = 0; i < length; ++i) {
    JSValue item = JS_GetPropertyUint32(ctx, v, i);
    if (JS_IsException(item)) {
      return false;
    }
    bool ok = AppendValue(ctx, out, item, free_text, actors);
    JS_FreeValue(ctx, item);
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool AppendValue(JSContext *ctx, std::string *out, JSValueConst v,
                 bool free_text, ActorArgScope *actors) {
  // undefined and null are "argument omitted", which is how every optional
  // console argument is spelled: stop rather than emit a placeholder.
  if (JS_IsUndefined(v) || JS_IsNull(v)) {
    return true;
  }
  if (JS_IsBool(v)) {
    // Every ON/OFF command goes through ConsoleParseBool, which matches those
    // two words and nothing else.
    if (!out->empty()) {
      *out += ' ';
    }
    *out += JS_ToBool(ctx, v) ? "ON" : "OFF";
    return true;
  }
  if (JS_IsNumber(v)) {
    double d = 0.0;
    if (JS_ToFloat64(ctx, &d, v)) {
      return false;
    }
    if (!out->empty()) {
      *out += ' ';
    }
    AppendNumber(out, d);
    return true;
  }
  if (JS_IsString(v)) {
    const char *text = JS_ToCString(ctx, v);
    if (text == nullptr) {
      return false;
    }
    // The console splits arguments on whitespace, so an embedded space would
    // silently become two arguments. Refusing is the only honest option: there
    // is no quoting syntax to escape into.
    bool hasSpace = !free_text && std::strpbrk(text, " \t\r\n") != nullptr;
    std::string encoded = GameTextFromUtf8(text);
    JS_FreeCString(ctx, text);
    if (hasSpace) {
      JS_ThrowTypeError(
          ctx, "console arguments cannot contain whitespace: the command line "
               "is split on it and there is no quoting syntax");
      return false;
    }
    if (!out->empty()) {
      *out += ' ';
    }
    *out += encoded;
    return true;
  }
  // BEFORE the object cases, and that ordering is load-bearing: an actor
  // wrapper is an ordinary object, so without this it falls into AppendVec3Like
  // and silently appends "0 0 0".
  //
  // An actor is spelled as its decimal id and also queued on `actors`. The two
  // together cover both kinds of handler with no per-command declaration: one
  // group resolves its actor through ConsoleParseActorName, which the queue
  // substitutes into, and the other reads the digits directly with
  // ConsoleParseInt and never consults the queue. See src/ActorArg.h.
  if (IsActorValue(v)) {
    Actor *actor = ActorFromValue(ctx, v);
    if (!actor) {
      return false;
    }
    if (!actors) {
      JS_ThrowTypeError(ctx, "this command does not take an actor");
      return false;
    }
    if (!out->empty()) {
      *out += ' ';
    }
    AppendNumber(out, static_cast<double>(actor->id));
    actors->Push(actor);
    return true;
  }
  // Also before the object cases, and for the same reason. A role is spelled as
  // its name because that is what the handlers resolve (`GetRoleByName`), and
  // unlike an actor a role reliably has one - roles are the registry the `.gls`
  // named, so a nameless one would be a defect rather than an ordinary case.
  if (Role *role = RoleFromWrapper(v)) {
    const char *name = role->name.get();
    if (!name || !*name) {
      JS_ThrowTypeError(ctx, "role %d has no name, so it cannot be named on a "
                             "console command line",
                        role->id);
      return false;
    }
    if (!out->empty()) {
      *out += ' ';
    }
    *out += name;
    return true;
  }
  if (JS_IsArray(v)) {
    return AppendArray(ctx, out, v, free_text, actors);
  }
  if (JS_IsObject(v)) {
    return AppendVec3Like(ctx, out, v);
  }
  JS_ThrowTypeError(ctx, "unsupported console argument: expected an actor, a "
                         "number, boolean, string, {x, y, z} or an array");
  return false;
}

// Runs `name` with `argv` appended; exposed as js::RunConsoleCommand. `name` is
// already game text (either a literal from the binary or a resource string), so
// only the arguments are transcoded.
JSValue RunCommandImpl(JSContext *ctx, const char *name, int argc,
                       JSValueConst *argv, bool free_text = false) {
  // Armed for every command rather than only for the ones that take an actor.
  // It costs two thread-local reads, it has to outlive the ExecuteCommand below
  // (that is what the handler reads it through), and it discards anything the
  // handler did not consume - so a command that takes no actor cannot inherit a
  // substitution from one that did.
  ActorArgScope actors;
  std::string line = name;
  for (int i = 0; i < argc; ++i) {
    if (!AppendValue(ctx, &line, argv[i], free_text, &actors)) {
      return JS_EXCEPTION;
    }
  }
  if (!ExecuteCommand(line.c_str())) {
    // Not a silent truncation: see kConsoleCommandLineMax in Console.h.
    return JS_ThrowRangeError(
        ctx, "console command line is %d characters; the game's buffer holds %d",
        static_cast<int>(line.size()), kConsoleCommandLineMax);
  }
  return JS_UNDEFINED;
}

// A command whose *name* comes from glres<lang>.dll, so it cannot be spelled as
// a literal. Ids 10000..10014, contiguous, in the order SetupConsoleCommands
// registers them.
JSValue RunLocalizedCommandImpl(JSContext *ctx, unsigned resourceId, int argc,
                                JSValueConst *argv, bool free_text = false) {
  const char *name = ResourceString(resourceId);
  if (name == nullptr || name[0] == '\0') {
    return JS_ThrowInternalError(
        ctx, "command name resource %u is missing from the language DLL",
        resourceId);
  }
  return RunCommandImpl(ctx, name, argc, argv, free_text);
}

// One C function per command, built from a table. The magic argument count is
// deliberately generous - the binding passes through whatever it is given and
// the game's parser decides what it needs.
#define GK_COMMAND(fn, command)                                                \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunCommandImpl(ctx, command, argc, argv);                           \
  }
#define GK_LOCALIZED_COMMAND(fn, id)                                           \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunLocalizedCommandImpl(ctx, id, argc, argv);                       \
  }
// The free-text variants, for the handlers that consume the rest of the line
// (`CopyRemainingArgs`) instead of one word.
#define GK_TEXT_COMMAND(fn, command)                                           \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunCommandImpl(ctx, command, argc, argv, true);                     \
  }
#define GK_LOCALIZED_TEXT_COMMAND(fn, id)                                      \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunLocalizedCommandImpl(ctx, id, argc, argv, true);                 \
  }

// --- fx: world effects and particles (26) -------------------------------------

GK_COMMAND(FxWater, "WATER")
GK_COMMAND(FxLava, "LAVA")
GK_COMMAND(FxOil, "OIL")
GK_COMMAND(FxSea, "SEA")
GK_COMMAND(FxSwamp, "SWAMP")
GK_COMMAND(FxRaiseWater, "RAISE WATER")
GK_COMMAND(FxLowerWater, "LOWER WATER")
GK_COMMAND(FxRaiseLava, "RAISE LAVA")
GK_COMMAND(FxLowerLava, "LOWER LAVA")
GK_COMMAND(FxSetWaterSpeed, "SET WATER SPEED")
GK_COMMAND(FxSetWaterDirection, "SET WATER DIRECTION")
GK_COMMAND(FxElectricity, "ELECTRICITY")
GK_COMMAND(FxDeactivateElectricity, "DEACTIVATE ELECTRICITY")
GK_COMMAND(FxLaserFence, "LASER FENCE")
GK_COMMAND(FxPulseRings, "PULSE RINGS")
GK_COMMAND(FxRemovePulseRings, "REMOVE PULSE RINGS")
GK_COMMAND(FxRing, "RING")
GK_COMMAND(FxBasin, "BASIN")
GK_COMMAND(FxExplode, "EXPLODE")
GK_COMMAND(FxExplodeWithSmoke, "EWS")
GK_COMMAND(FxSparks, "SPARKS")
GK_COMMAND(FxRain, "RAIN")
GK_COMMAND(FxSnow, "SNOW")
GK_COMMAND(FxLightning, "LIGHTNING")
GK_COMMAND(FxParticleTester, "PARTICLE TESTER")
GK_COMMAND(FxParticleRate, "RATE")
GK_COMMAND(FxAirstrike, "AIRSTRIKE")
GK_COMMAND(FxSmoke, "SMOKE")
GK_COMMAND(FxStopParticles, "STOP PARTICLES")
GK_COMMAND(FxTextureAnimate, "TEXTURE ANIMATE")

const JSCFunctionListEntry FxProps[] = {
    JS_CFUNC_DEF("water", 5, FxWater),
    JS_CFUNC_DEF("lava", 5, FxLava),
    JS_CFUNC_DEF("oil", 5, FxOil),
    JS_CFUNC_DEF("sea", 5, FxSea),
    JS_CFUNC_DEF("swamp", 5, FxSwamp),
    JS_CFUNC_DEF("raise_water", 2, FxRaiseWater),
    JS_CFUNC_DEF("lower_water", 2, FxLowerWater),
    JS_CFUNC_DEF("raise_lava", 2, FxRaiseLava),
    JS_CFUNC_DEF("lower_lava", 2, FxLowerLava),
    JS_CFUNC_DEF("set_water_speed", 2, FxSetWaterSpeed),
    JS_CFUNC_DEF("set_water_direction", 2, FxSetWaterDirection),
    JS_CFUNC_DEF("electricity", 3, FxElectricity),
    JS_CFUNC_DEF("deactivate_electricity", 1, FxDeactivateElectricity),
    JS_CFUNC_DEF("laser_fence", 2, FxLaserFence),
    JS_CFUNC_DEF("pulse_rings", 5, FxPulseRings),
    JS_CFUNC_DEF("remove_pulse_rings", 1, FxRemovePulseRings),
    JS_CFUNC_DEF("ring", 4, FxRing),
    JS_CFUNC_DEF("basin", 4, FxBasin),
    JS_CFUNC_DEF("explode", 1, FxExplode),
    JS_CFUNC_DEF("explode_with_smoke", 1, FxExplodeWithSmoke),
    JS_CFUNC_DEF("sparks", 1, FxSparks),
    JS_CFUNC_DEF("rain", 1, FxRain),
    JS_CFUNC_DEF("snow", 1, FxSnow),
    JS_CFUNC_DEF("lightning", 1, FxLightning),
    JS_CFUNC_DEF("particle_tester", 2, FxParticleTester),
    JS_CFUNC_DEF("particle_rate", 1, FxParticleRate),
    JS_CFUNC_DEF("airstrike", 2, FxAirstrike),
    JS_CFUNC_DEF("smoke", 1, FxSmoke),
    JS_CFUNC_DEF("stop_particles", 1, FxStopParticles),
    JS_CFUNC_DEF("texture_animate", 3, FxTextureAnimate),
};

// --- light: the lighting commands with no broadcast (13) ----------------------

GK_COMMAND(LightDark, "DARK")
GK_COMMAND(LightFadeIn, "FADE IN")
GK_COMMAND(LightFadeOut, "FADE OUT")
GK_COMMAND(LightFadeToBlack, "FADE TO BLACK")
GK_COMMAND(LightFadeFromBlack, "FADE FROM BLACK")
GK_COMMAND(LightCorona, "CORONA")
GK_COMMAND(LightSpotlight, "SPOTLIGHT")
GK_COMMAND(LightRay, "RAY")
GK_COMMAND(LightRayColor, "RAY COLOUR")
GK_COMMAND(LightCylinder, "LIGHT CYLINDER")
GK_COMMAND(LightRemoveCylinder, "REMOVE LIGHT CYLINDER")
GK_COMMAND(LightOn, "LIGHTON")
GK_COMMAND(LightReflect, "REFLECT")
GK_COMMAND(LightAssociate, "ASSOCIATELIGHT")
GK_COMMAND(LightAdd, "ADD LIGHT")
GK_COMMAND(LightAddBlinking, "ADD BLINKING LIGHT")
GK_COMMAND(LightShadow, "SHADOW")

const JSCFunctionListEntry LightProps[] = {
    JS_CFUNC_DEF("dark", 0, LightDark),
    JS_CFUNC_DEF("fade_in", 0, LightFadeIn),
    JS_CFUNC_DEF("fade_out", 0, LightFadeOut),
    JS_CFUNC_DEF("fade_to_black", 2, LightFadeToBlack),
    JS_CFUNC_DEF("fade_from_black", 1, LightFadeFromBlack),
    JS_CFUNC_DEF("corona", 2, LightCorona),
    JS_CFUNC_DEF("spotlight", 1, LightSpotlight),
    JS_CFUNC_DEF("ray", 1, LightRay),
    JS_CFUNC_DEF("ray_color", 3, LightRayColor),
    JS_CFUNC_DEF("cylinder", 1, LightCylinder),
    JS_CFUNC_DEF("remove_cylinders", 0, LightRemoveCylinder),
    JS_CFUNC_DEF("light_on", 6, LightOn),
    JS_CFUNC_DEF("reflect", 0, LightReflect),
    JS_CFUNC_DEF("associate", 7, LightAssociate),
    JS_CFUNC_DEF("add", 4, LightAdd),
    JS_CFUNC_DEF("add_blinking", 4, LightAddBlinking),
    JS_CFUNC_DEF("shadow", 1, LightShadow),
};

// --- objectives (7) -----------------------------------------------------------

// The last of the untyped `CommandArg` spreads, recovered from
// `CommandAddObjective` @ 0x00446300 rather than guessed:
//
//   ADD OBJECTIVE <team slot> <PRIMARY|SECONDARY|TERTIARY> <id> -1 <title> <body>
//
// Every argument is gated and the handler **returns silently** when one fails,
// which is why this was worth typing rather than leaving as varargs:
//
//   * the team slot must be `0 <= n < NumTeamSlots`;
//   * the priority word is compared with `__mbsicmp` against exactly those three
//     spellings, and anything else returns;
//   * the id must be non-negative;
//   * the **fourth number must be literally -1**. It is a hard-coded sentinel
//     with no other accepted value, so the binding writes it and the caller
//     never sees it - which is the whole reason a typed wrapper beats the raw
//     command here. (`ConsoleParseInt` does read a negative: a leading '-'
//     toggles a sign flag applied on the way out. The decompiler's `extraout_EAX`
//     hides that, and reading it there suggested the gate was unsatisfiable and
//     the command dead. It is not.)
//   * `title` and `body` are offsets from `GL_OBJECTIVE_0` into the localized
//     string table, not text.
//
// The two `add objective` lines in the shipped `.gcs` files are both `rem`'d out
// and would not have worked: each puts a `2` where the -1 belongs.
JSValue ObjectiveAdd(JSContext *ctx, JSValueConst, int argc,
                     JSValueConst *argv) {
  if (argc < 5) {
    return JS_ThrowTypeError(
        ctx, "add(team_slot, priority, id, title, body) expects five arguments");
  }
  const char *priority = JS_ToCString(ctx, argv[1]);
  if (!priority) {
    return JS_EXCEPTION;
  }
  const bool known = std::strcmp(priority, "primary") == 0 ||
                     std::strcmp(priority, "secondary") == 0 ||
                     std::strcmp(priority, "tertiary") == 0;
  JS_FreeCString(ctx, priority);
  if (!known) {
    return JS_ThrowTypeError(
        ctx, "priority must be \"primary\", \"secondary\" or \"tertiary\"");
  }
  JSValue sentinel = JS_NewInt32(ctx, -1);
  JSValueConst forwarded[] = {argv[0], argv[1], argv[2], sentinel, argv[3],
                              argv[4]};
  JSValue result = RunConsoleCommand(ctx, "ADD OBJECTIVE",
                                     static_cast<int>(std::size(forwarded)),
                                     forwarded);
  JS_FreeValue(ctx, sentinel);
  return result;
}
GK_COMMAND(ObjectiveComplete, "COMPLETE OBJECTIVE")
GK_COMMAND(ObjectiveFail, "FAIL OBJECTIVE")
GK_COMMAND(ObjectiveText, "OBJECTIVE TEXT")
GK_COMMAND(ObjectiveStartPrinting, "START PRINTING OBJECTIVES")
GK_COMMAND(ObjectiveTrainingText, "TRNTXT")
GK_COMMAND(ObjectiveRepeatText, "REPTXT")

const JSCFunctionListEntry ObjectiveProps[] = {
    JS_CFUNC_DEF("add", 5, ObjectiveAdd),
    JS_CFUNC_DEF("complete", 1, ObjectiveComplete),
    JS_CFUNC_DEF("fail", 1, ObjectiveFail),
    JS_CFUNC_DEF("print", 1, ObjectiveText),
    JS_CFUNC_DEF("start_printing", 1, ObjectiveStartPrinting),
    JS_CFUNC_DEF("training_text", 1, ObjectiveTrainingText),
    JS_CFUNC_DEF("repeat_text", 1, ObjectiveRepeatText),
};

// --- music: CD tracks and sound effects (9) -----------------------------------
//
// `PLAY ENVIRONMENTAL SOUND` is bound but **broken in the game**: its handler
// (0x004464b0) parses the sound id, tests it non-zero, and then calls the player
// with a hard-coded argument, discarding the id. So it plays one fixed sound for
// every id. Bound anyway - it is a registered command with no broadcast, and
// hiding it would misrepresent what the console can do - with the defect
// recorded here and in the .d.ts.

GK_COMMAND(MusicPlaySound, "PLAY SOUND")
GK_COMMAND(MusicEnvironmentalSound, "PLAY ENVIRONMENTAL SOUND")
GK_COMMAND(MusicCDPlay, "CD PLAY")
GK_COMMAND(MusicCDStop, "CD STOP")
GK_COMMAND(MusicCDFade, "CD FADE")
GK_COMMAND(MusicCDAuto, "CD AUTO")
GK_COMMAND(MusicCDVolume, "CD SET VOLUME")
GK_COMMAND(MusicCDTracks, "CD TRACKS")
GK_COMMAND(MusicVictoryKills, "CD VICTORY KILLS")

const JSCFunctionListEntry MusicProps[] = {
    JS_CFUNC_DEF("play_sound", 1, MusicPlaySound),
    JS_CFUNC_DEF("play_environmental_sound", 1, MusicEnvironmentalSound),
    JS_CFUNC_DEF("cd_play", 2, MusicCDPlay),
    JS_CFUNC_DEF("cd_stop", 0, MusicCDStop),
    JS_CFUNC_DEF("cd_fade", 2, MusicCDFade),
    JS_CFUNC_DEF("cd_auto", 1, MusicCDAuto),
    JS_CFUNC_DEF("cd_set_volume", 1, MusicCDVolume),
    JS_CFUNC_DEF("cd_tracks", 2, MusicCDTracks),
    JS_CFUNC_DEF("victory_kills", 1, MusicVictoryKills),
};

// --- screen: presentation, briefing and session (21) --------------------------

GK_COMMAND(ScreenBorders, "BORDERS ")
GK_COMMAND(ScreenBordersOff, "BORDERS OFF")
GK_COMMAND(ScreenCursor, "CURSOR")
GK_COMMAND(ScreenSystemCursor, "SYSTEM CURSOR")
GK_COMMAND(ScreenClear, "CLS")
GK_TEXT_COMMAND(ScreenBitmap, "BITMAP")
GK_COMMAND(ScreenBriefingText, "BRIEFING TEXT")
GK_COMMAND(ScreenTrainingDebriefText, "TRAINING DEBRIEF TEXT")
GK_COMMAND(ScreenEndBriefing, "END BRIEFING")
GK_COMMAND(ScreenCredits, "CREDITS")
GK_COMMAND(ScreenFMV, "FMV")
GK_TEXT_COMMAND(ScreenPlayFMV, "PLAY FMV")
GK_TEXT_COMMAND(ScreenPlayCutscene, "PLAY CUTSCENE")
GK_COMMAND(ScreenStatusWindow, "STATUS WINDOW")
GK_COMMAND(ScreenStats, "STATS SCREEN")

const JSCFunctionListEntry ScreenProps[] = {
    JS_CFUNC_DEF("borders", 2, ScreenBorders),
    JS_CFUNC_DEF("borders_off", 0, ScreenBordersOff),
    JS_CFUNC_DEF("cursor", 1, ScreenCursor),
    JS_CFUNC_DEF("system_cursor", 0, ScreenSystemCursor),
    JS_CFUNC_DEF("clear", 0, ScreenClear),
    JS_CFUNC_DEF("bitmap", 1, ScreenBitmap),
    JS_CFUNC_DEF("briefing_text", 1, ScreenBriefingText),
    JS_CFUNC_DEF("training_debrief_text", 1, ScreenTrainingDebriefText),
    JS_CFUNC_DEF("end_briefing", 0, ScreenEndBriefing),
    JS_CFUNC_DEF("credits", 0, ScreenCredits),
    JS_CFUNC_DEF("end_game_fmv", 1, ScreenFMV),
    JS_CFUNC_DEF("play_fmv", 1, ScreenPlayFMV),
    JS_CFUNC_DEF("play_cutscene", 1, ScreenPlayCutscene),
    JS_CFUNC_DEF("status_window", 1, ScreenStatusWindow),
    JS_CFUNC_DEF("stats", 0, ScreenStats),
};

// --- units: AI and per-actor behaviour (20) -----------------------------------
//
// The broadcasting members of this cluster - ANIM, BOARD, DEFOGGER, FOGGER,
// GIVE CONTROL, PLAYER SELECT, REMOVEBB - are absent for the usual reason.

GK_COMMAND(UnitsSetAI, "AI")
GK_COMMAND(UnitsAlertNode, "ALERT NODE")
// Takes NO actor, and the binding used to claim one. CommandSetActivity
// @ 0x00445940 parses a single word - the activity keyword - and operates on the
// global `selected_actor_id`, printing GL_ERROR_NO_ACTOR_SELECTED when nothing
// is selected. So `set_activity(actor, activity)` sent `SET ACTIVITY <actor>
// <activity>`, the handler read `<actor>` as the keyword, all three
// `__mbsicmp`s against GOTO/PATROL/STOP failed and the call did nothing at all.
// Set `game.selected_actor` first.
GK_COMMAND(UnitsSetActivity, "SET ACTIVITY")
GK_COMMAND(UnitsAddWaypoint, "ADD WAYPOINT")
GK_COMMAND(UnitsAddPatrolPoint, "ADD PATROLPOINT")
GK_COMMAND(UnitsNewNodeWaypointList, "NEW NODE WAYPOINT LIST")
GK_COMMAND(UnitsHunter, "HUNTER")
GK_COMMAND(UnitsFlareFirer, "FLARE FIRER")
GK_COMMAND(UnitsTurnVisionCone, "TURN VISION CONE")
GK_COMMAND(UnitsTurnHearingRange, "TURN HEARING RANGE")
GK_COMMAND(UnitsTurretLOS, "TURRET LOS")
// Also takes no actor. CommandSetScale @ 0x00447370 parses one float and
// applies it to `ActorUnderCursor`; the second parameter the binding used to
// declare was read by nothing. `game.actor_under_cursor` is what it acts on.
GK_COMMAND(UnitsSetScale, "SET SCALE")
GK_COMMAND(UnitsDeleteTeam, "DELETE TEAM")
GK_COMMAND(UnitsWatch, "WATCH")
GK_COMMAND(UnitsPresident, "PRESIDENT")
GK_COMMAND(UnitsListTeam, "LIST TEAM")
GK_COMMAND(UnitsUpperLeftBound, "SET UPPER LEFT BOUND")
GK_COMMAND(UnitsLowerRightBound, "SET LOWER RIGHT BOUND")
GK_COMMAND(UnitsVulnerability, "VULNERABILITY")
GK_COMMAND(UnitsAnim, "ANIM")
GK_COMMAND(UnitsBoard, "BOARD")
GK_COMMAND(UnitsDefogger, "DEFOGGER")
GK_COMMAND(UnitsFogger, "FOGGER")
GK_COMMAND(UnitsGiveControl, "GIVE CONTROL")
GK_COMMAND(UnitsPlayerSelect, "PLAYER SELECT")
GK_COMMAND(UnitsRemoveBB, "REMOVEBB")
GK_TEXT_COMMAND(UnitsSpeak, "SPEAK")

const JSCFunctionListEntry UnitsProps[] = {
    JS_CFUNC_DEF("set_ai", 2, UnitsSetAI),
    JS_CFUNC_DEF("alert_node", 1, UnitsAlertNode),
    JS_CFUNC_DEF("set_activity", 1, UnitsSetActivity),
    JS_CFUNC_DEF("add_waypoint", 1, UnitsAddWaypoint),
    JS_CFUNC_DEF("add_patrol_point", 1, UnitsAddPatrolPoint),
    JS_CFUNC_DEF("new_node_waypoint_list", 1, UnitsNewNodeWaypointList),
    JS_CFUNC_DEF("make_hunter", 1, UnitsHunter),
    JS_CFUNC_DEF("make_flare_firer", 1, UnitsFlareFirer),
    JS_CFUNC_DEF("turn_vision_cone", 2, UnitsTurnVisionCone),
    JS_CFUNC_DEF("turn_hearing_range", 2, UnitsTurnHearingRange),
    JS_CFUNC_DEF("turret_los", 2, UnitsTurretLOS),
    JS_CFUNC_DEF("set_scale", 1, UnitsSetScale),
    JS_CFUNC_DEF("delete_team", 1, UnitsDeleteTeam),
    JS_CFUNC_DEF("watch", 1, UnitsWatch),
    JS_CFUNC_DEF("create_president", 0, UnitsPresident),
    JS_CFUNC_DEF("list_team", 1, UnitsListTeam),
    JS_CFUNC_DEF("set_upper_left_bound", 1, UnitsUpperLeftBound),
    JS_CFUNC_DEF("set_lower_right_bound", 1, UnitsLowerRightBound),
    JS_CFUNC_DEF("set_vulnerability", 4, UnitsVulnerability),
    JS_CFUNC_DEF("play_animation", 2, UnitsAnim),
    JS_CFUNC_DEF("board", 1, UnitsBoard),
    JS_CFUNC_DEF("make_defogger", 1, UnitsDefogger),
    JS_CFUNC_DEF("clear_defogger", 1, UnitsFogger),
    JS_CFUNC_DEF("give_control", 2, UnitsGiveControl),
    JS_CFUNC_DEF("player_select", 1, UnitsPlayerSelect),
    JS_CFUNC_DEF("remove_bounding_box", 1, UnitsRemoveBB),
    JS_CFUNC_DEF("speak", 2, UnitsSpeak),
};

// --- inventory: the GIVE family (14) ------------------------------------------
//
// REMOVE ITEM broadcasts (update 0x7d) and is therefore absent.

GK_COMMAND(InvGive, "GIVE")
GK_COMMAND(InvGiveAndSay, "GIVE AND SAY")
GK_COMMAND(InvGiveAndEquip, "GIVE AND EQUIP")
GK_COMMAND(InvGiveAndEquipAndSay, "GIVE AND EQUIP AND SAY")
GK_COMMAND(InvGiveRole, "GIVE ROLE")
GK_COMMAND(InvGiveRoleId, "GIVE ROLE ID")
GK_COMMAND(InvGiveRoleTeam, "GIVE ROLE TEAM")
GK_COMMAND(InvGiveAndEquipRole, "GIVE AND EQUIP ROLE")
GK_COMMAND(InvGiveAndEquipRoleId, "GIVE AND EQUIP ROLE ID")
GK_COMMAND(InvGiveAndEquipRoleTeam, "GIVE AND EQUIP ROLE TEAM")
GK_COMMAND(InvHeap, "HEAP")
GK_COMMAND(InvRespawnHeap, "RESPAWN HEAP")
GK_COMMAND(InvNextRespawnId, "NEXT RESPAWN ID")
GK_TEXT_COMMAND(InvIfCarrying, "IF CARRYING")
GK_COMMAND(InvRemoveItem, "REMOVE ITEM")

const JSCFunctionListEntry InventoryProps[] = {
    JS_CFUNC_DEF("give", 2, InvGive),
    JS_CFUNC_DEF("give_and_say", 2, InvGiveAndSay),
    JS_CFUNC_DEF("give_and_equip", 2, InvGiveAndEquip),
    JS_CFUNC_DEF("give_and_equip_and_say", 2, InvGiveAndEquipAndSay),
    JS_CFUNC_DEF("give_role", 2, InvGiveRole),
    JS_CFUNC_DEF("give_role_id", 1, InvGiveRoleId),
    JS_CFUNC_DEF("give_role_team", 3, InvGiveRoleTeam),
    JS_CFUNC_DEF("give_and_equip_role", 2, InvGiveAndEquipRole),
    JS_CFUNC_DEF("give_and_equip_role_id", 1, InvGiveAndEquipRoleId),
    JS_CFUNC_DEF("give_and_equip_role_team", 3, InvGiveAndEquipRoleTeam),
    JS_CFUNC_DEF("heap", 2, InvHeap),
    JS_CFUNC_DEF("respawn_heap", 3, InvRespawnHeap),
    JS_CFUNC_DEF("next_respawn_id", 0, InvNextRespawnId),
    JS_CFUNC_DEF("if_carrying", 2, InvIfCarrying),
    JS_CFUNC_DEF("remove_item", 1, InvRemoveItem),
};

// --- tracks: track objects, doors and attachments (7) -------------------------
//
// SET TRACK, SET SPEED and SET LOOP TIME broadcast; OPEN/CLOSE/TOGGLE DOOR do
// too. `DOOR` (declaring one) and ATTACH/DETACH do not.

GK_COMMAND(TrackRun, "RUN TRACK")
GK_COMMAND(TrackPause, "PAUSE")
GK_COMMAND(TrackUnpause, "UNPAUSE")
GK_COMMAND(TrackDeclareDoor, "DOOR")
GK_COMMAND(TrackAttach, "ATTACH")
GK_COMMAND(TrackDetach, "DETACH")
GK_COMMAND(TrackSet, "SET TRACK")
GK_COMMAND(TrackSetSpeed, "SET SPEED")
GK_COMMAND(TrackSetLoopTime, "SET LOOP TIME")
GK_COMMAND(TrackOpenDoor, "OPEN DOOR")
GK_COMMAND(TrackCloseDoor, "CLOSE DOOR")
GK_COMMAND(TrackToggleDoor, "TOGGLE DOOR")

const JSCFunctionListEntry TrackProps[] = {
    JS_CFUNC_DEF("run", 1, TrackRun),
    JS_CFUNC_DEF("pause", 1, TrackPause),
    JS_CFUNC_DEF("unpause", 1, TrackUnpause),
    JS_CFUNC_DEF("declare_door", 2, TrackDeclareDoor),
    JS_CFUNC_DEF("attach", 1, TrackAttach),
    JS_CFUNC_DEF("detach", 1, TrackDetach),
    JS_CFUNC_DEF("set", 7, TrackSet),
    JS_CFUNC_DEF("set_speed", 2, TrackSetSpeed),
    JS_CFUNC_DEF("set_loop_time", 1, TrackSetLoopTime),
    JS_CFUNC_DEF("open_door", 1, TrackOpenDoor),
    JS_CFUNC_DEF("close_door", 1, TrackCloseDoor),
    JS_CFUNC_DEF("toggle_door", 1, TrackToggleDoor),
};

// --- demo recording (4) --------------------------------------------------------

GK_COMMAND(DemoRecord, "RECORD")
GK_COMMAND(DemoPlayback, "PLAYBACK")
GK_COMMAND(DemoSave, "SAVE")
GK_COMMAND(DemoLoad, "LOAD")

const JSCFunctionListEntry DemoProps[] = {
    JS_CFUNC_DEF("record", 0, DemoRecord),
    JS_CFUNC_DEF("playback", 0, DemoPlayback),
    JS_CFUNC_DEF("save", 1, DemoSave),
    JS_CFUNC_DEF("load", 1, DemoLoad),
};

// --- script: the console queue's own pacing (7) -------------------------------
//
// These suspend the *console command queue*, which a script does not use, so
// they only do anything to work queued with console.execute_file. A script's own
// sequencing is ordinary JavaScript.

GK_COMMAND(ScriptWait, "WAIT")
GK_COMMAND(ScriptRealWait, "REAL WAIT")
GK_COMMAND(ScriptRealWaitOrClick, "REAL WAIT OR CLICK")
GK_TEXT_COMMAND(ScriptWaitFor, "WAIT FOR")
GK_COMMAND(ScriptCheckWaitFor, "CHECK WAIT FOR")
GK_COMMAND(ScriptCancelWaitFor, "CANCEL WAIT FOR")
GK_COMMAND(ScriptGetWaitFor, "GET WAIT FOR")

const JSCFunctionListEntry ScriptProps[] = {
    JS_CFUNC_DEF("wait", 1, ScriptWait),
    JS_CFUNC_DEF("real_wait", 1, ScriptRealWait),
    JS_CFUNC_DEF("real_wait_or_click", 1, ScriptRealWaitOrClick),
    JS_CFUNC_DEF("wait_for", 1, ScriptWaitFor),
    JS_CFUNC_DEF("check_wait_for", 0, ScriptCheckWaitFor),
    JS_CFUNC_DEF("cancel_wait_for", 0, ScriptCancelWaitFor),
    JS_CFUNC_DEF("print_wait_for", 0, ScriptGetWaitFor),
};

#undef GK_COMMAND
#undef GK_LOCALIZED_COMMAND
#undef GK_TEXT_COMMAND
#undef GK_LOCALIZED_TEXT_COMMAND

} // namespace

JSValue RunConsoleCommand(JSContext *ctx, const char *name, int argc,
                          JSValueConst *argv) {
  return RunCommandImpl(ctx, name, argc, argv);
}

JSValue RunConsoleTextCommand(JSContext *ctx, const char *name, int argc,
                              JSValueConst *argv) {
  return RunCommandImpl(ctx, name, argc, argv, true);
}

JSValue RunLocalizedConsoleCommand(JSContext *ctx, unsigned resource_id,
                                   int argc, JSValueConst *argv) {
  return RunLocalizedCommandImpl(ctx, resource_id, argc, argv);
}

JSValue NewFxNamespace(JSContext *ctx) {
  return NewNamespace(ctx, FxProps, static_cast<int>(std::size(FxProps)));
}
JSValue NewLightNamespace(JSContext *ctx) {
  return NewNamespace(ctx, LightProps, static_cast<int>(std::size(LightProps)));
}
JSValue NewObjectivesNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ObjectiveProps,
                      static_cast<int>(std::size(ObjectiveProps)));
}
JSValue NewMusicNamespace(JSContext *ctx) {
  return NewNamespace(ctx, MusicProps, static_cast<int>(std::size(MusicProps)));
}
JSValue NewScreenNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ScreenProps,
                      static_cast<int>(std::size(ScreenProps)));
}
JSValue NewUnitsNamespace(JSContext *ctx) {
  return NewNamespace(ctx, UnitsProps, static_cast<int>(std::size(UnitsProps)));
}
JSValue NewInventoryNamespace(JSContext *ctx) {
  return NewNamespace(ctx, InventoryProps,
                      static_cast<int>(std::size(InventoryProps)));
}
JSValue NewTracksNamespace(JSContext *ctx) {
  return NewNamespace(ctx, TrackProps, static_cast<int>(std::size(TrackProps)));
}
JSValue NewDemoNamespace(JSContext *ctx) {
  return NewNamespace(ctx, DemoProps, static_cast<int>(std::size(DemoProps)));
}
JSValue NewScriptNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ScriptProps,
                      static_cast<int>(std::size(ScriptProps)));
}

} // namespace gk::js
