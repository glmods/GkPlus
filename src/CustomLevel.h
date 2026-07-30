#pragma once

#include "GLS.h"
#include "Math.h"
#include "Menu.h"

#include <string>
#include <vector>

namespace gk {
// Opaque to callers; defined in CustomLevel.cpp.
struct CustomLevel;

// Fills the live world in once TheMap exists. Runs inside LoadLevel, in the same
// window ToMap spawns a .gls script's placed objects in: after the level's own
// parsed objects have been converted (so every role the level includes is
// registered) and before the camera snaps to the first team-1 actor. `user` is
// whatever was handed to AddCustomLevel.
using CustomLevelPopulate = void (*)(CustomLevel *level, void *user);

// Registers the level's roles, characters and ammo, for a level that builds them
// through `gk::gls` instead of naming .gsh files in `includes`. Runs earlier than
// populate - before the map is even converted - which is where a .gls's
// `#include` block effectively sits.
//
// It has to run per *load*, not once at registration, because DestroyRoles clears
// the roles hash between levels; the parsed objects a script holds are reusable,
// the converted Roles are not (see gls::ResetConversionCache).
using CustomLevelDefine = void (*)(CustomLevel *level, void *user);

// Stands in for the level's .gcs - the console command file that sets up fog,
// camera bounds, the sun and the level's triggers. Runs at exactly the point
// LoadLevel would have run one (step 11: after the world is built, the camera
// has snapped to the first team-1 actor and the clocks have resynced), and only
// when LoadLevel's `freshStart` flag is set, which is the same gate the .gcs
// itself is behind - so a savegame restore does not re-run it, exactly as the
// save already holds whatever the script set up.
//
// Later than populate, therefore, and with the whole world already in place:
// this is where a script names the actors populate spawned, arms triggers and
// issues console commands.
using CustomLevelSetup = void (*)(CustomLevel *level, void *user);

// The level's inbox. `json` is a complete JSON document that is not a string -
// see ScriptQueue.h for why a string is a .gcs name instead - and is valid only
// for the duration of the call.
//
// This is the other half of what a trigger does. Where a .gls trigger names a
// file and every machine runs its own copy, a trigger built from script can
// carry a message, and this is where it arrives: on the host from
// RunQueuedScript, on every joiner from update 0x67, once each, on the main
// thread. Unlike the three load hooks it fires during play, not during a load,
// so the level it belongs to is the one the game is *in* rather than the one it
// is building.
//
// Returns nothing - a payload nobody wants is dropped with a note to the
// debugger.
using CustomLevelMessage = void (*)(CustomLevel *level, const char *json,
                                    void *user);

// The `map` section a custom level replaces its .gls with, field for field - see
// the "map" table in gls_system_notes.md for the ranges. An empty string means
// "not assigned", which for the four optional fields is the same as leaving the
// line out of a .gls.
//
// The two required halves are `rif_file` (GLS `file`) and `object_name` (GLS
// `name`): together they name the object *inside* the .rif that becomes the
// level geometry. Everything a custom level needs from disk is that one .rif -
// the .gls and .gcs are what the module replaces.
struct CustomLevelMap {
  std::string rif_file;    // GLS `file`, e.g. "levels\\level01.rif" - required
  std::string object_name; // GLS `name`, e.g. "Land"                - required
  std::string bitmap;      // loading/HUD bitmap; empty = `none`
  std::string camera_plane;             // .loc locator name; empty = `none`
  double max_camera_distance = 60.0;    // 10..500
  std::string max_camera_focus_height;  // .loc locator name
  std::string min_camera_focus_height;  // .loc locator name
  std::string shadow_object_rif;
  std::string shadow_object_name;
  int max_vertices_per_section = 200; // 10..10000
};

// One placed object read out of the level .rif, in the coordinates ToMap would
// have spawned it at: MapToWorld(TheMap, rif, integer locator position).
struct CustomLevelLocator {
  Vec3 position;
  Vec4 orientation;
};

// Registers a level that is built by native code instead of by a .gls + .gcs
// pair, and appends it to Choose Level. Returns null - having logged why - if
// the map description is incomplete or the game rejects one of its values.
//
// The description is turned into a parsed `map` section immediately, through the
// game's own section constructor and CheckValue, so a bad field is reported at
// registration time rather than halfway through a level load. That object is
// then kept and re-converted on every load of this level.
//
// `includes` are .gls/.gsh files - the role, character and ammo definitions the
// level's actors need. They become an in-memory prelude script, handed to the
// parser through gls::ParseSource, so the parser resolves them in one pass with
// its multiple-inclusion guards intact, exactly as a hand-written level's
// `#include` block would. Paths are relative to the game's Scripts directory.
// A level that builds its definitions through `gk::gls` needs none of them and
// passes an empty list, which is the "no GLS parsing at all" case.
//
// Nothing here is written to disk: the level's ScriptFileName is a virtual name
// that no code path opens. Two titles that differ only in punctuation collapse to
// the same one, and the second registration is refused.
//
// Registrations are never freed and never move.
CustomLevel *AddCustomLevel(const char *title, const CustomLevelMap &map,
                            const std::vector<std::string> &includes,
                            CustomLevelDefine define,
                            CustomLevelPopulate populate,
                            CustomLevelSetup setup,
                            CustomLevelMessage message, void *user);

const char *CustomLevelTitle(const CustomLevel *level);
// This level's ScriptFileName - a virtual name, not a file that exists.
const char *CustomLevelScriptFile(const CustomLevel *level);

// A registered level with this title, matched case-insensitively, or null.
//
// Registering and appearing in the game's LevelList are separate acts here -
// listing waits for the first EnterMainMenuScreen so the campaign gets the
// empty list it insists on - so this finds a level from the moment
// AddCustomLevel returns, which the game's own list does not.
CustomLevel *CustomLevelByTitle(const char *title);
const CustomLevelMap &CustomLevelDescription(const CustomLevel *level);

// The custom level LoadLevel is building right now, or null when the game is
// loading one of its own - or nothing at all. Only meaningful from inside the
// define, populate and setup callbacks and the hooks that drive them. It doubles
// as the re-entrancy guard for those hooks, so a callback that itself parses a
// .gls cannot make the level build twice.
CustomLevel *CurrentCustomLevel();

// Hands a script-queue message to the custom level the game is currently in -
// the one whose generated script is ScriptFileName, which is set for as long as
// the level is loaded rather than only while it builds. Returns false when no
// custom level is loaded or it has no message hook, which is what makes the
// payload get reported as undelivered.
//
// This is ScriptQueueSystem's handler, installed by CustomLevelSystem. It is
// exported so a harness can drive it without constructing either.
bool DispatchCustomLevelMessage(const char *json);

// Every object in the loaded level .rif whose name matches, as ToMap reads them:
// integer locator coordinates scaled by the world unit and offset by the map
// origin, plus the object's quaternion. This is the `for "<rif object>"` half of
// a .gls `use` clause, without the spawning. Empty outside a loaded level.
std::vector<CustomLevelLocator> LevelRifLocators(const char *object_name);

// Makes every registration inert without unregistering it - the levels stay in
// Choose Level and still load their geometry, they just stop calling back. This
// is what a script host calls before it drops the callbacks those `user`
// pointers refer to.
void ClearCustomLevelActions();

// Hooks the two ends of the parsed-object pipeline - ConvertParsedObjects (where
// a custom level's map is converted and its populate callback runs) and
// FreeParsedObjectList (which shares the same null guard) - plus
// ExecuteAllCommands, the one place LoadLevel runs the level .gcs, which is
// where the setup callback goes, and EnterMainMenuScreen, which is where these
// levels reach the game's LevelList (it seeds the campaign only into an *empty*
// list, so registering must not put anything there first). Also installs
// DispatchCustomLevelMessage as the script queue's message handler, which needs
// no hook of its own - ScriptQueueSystem owns those. RAII, like every other
// *System - construct/destroy inside a Detours transaction.
class CustomLevelSystem {
public:
  CustomLevelSystem();
  ~CustomLevelSystem();
};
} // namespace gk
