#include "CustomLevel.h"

#include "Console.h"
#include "Core.h"
#include "CustomMenu.h"
#include "GLS.h"
#include "Map.h"
#include "Memory.h"
#include "ScriptQueue.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member
// pointers, and merely declaring them inside namespace gk hides the global
// templates that handle a plain function pointer.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace gk {
// Not in the anonymous namespace: the header hands these out by pointer, so this
// has to be the same gk::CustomLevel those declarations name.
struct CustomLevel {
  std::string title;
  // This level's ScriptFileName: a virtual name that is never a file on disk -
  // see VirtualScriptName.
  std::string script_file;
  // The .gls source this level's `includes` become, parsed straight out of memory
  // by HookedLoadGLS. Empty means the level builds its definitions with `make` or
  // `gls` and never touches the parser at all.
  std::string prelude_source;
  CustomLevelMap map;
  gls::ParsedThing *parsed_map; // built once, re-converted on every load
  CustomLevelDefine define;
  CustomLevelPopulate populate;
  CustomLevelSetup setup;
  CustomLevelMessage message;
  void *user;
  // Whether this level has reached the game's LevelList yet. See
  // ReconcileLevelList: registration and listing are deliberately separate, so
  // the campaign gets to be first.
  bool listed = false;
};

namespace {

// Both are called by LoadLevel around the level script, and by nothing else that
// matters here (LoadGLDirs converts the startup gldirs.gls before any level
// exists). Resolved into trampolines by DetourAttach, so calling through these
// runs the original - unlike gk::gls::ConvertParsedObjects, which holds the raw
// address and would re-enter the hook.
FastCall<void, gls::ParsedObjectList *> ConvertParsedObjects;
FastCall<void, gls::ParsedObjectList *> FreeParsedObjectList;
FastCall<gls::ParsedObjectList *, const char *, int> LoadGLS;

// The drain that makes the level .gcs take effect. LoadLevel queues the file at
// step 7 (ExecuteCommandFile, which only appends to CommandsToExecute) and runs
// the queue here at step 11 - and this is the *only* call site in the binary
// @ 0x004e1e00, guarded by the same `freshStart` byte the ExecuteCommandFile
// call is. So hooking it needs no "is a level loading?" test of its own: reaching
// it already means a fresh level start is at the point its .gcs would run.
StdCall<void> ExecuteAllCommands;

// The front-end entry point, hooked for one reason: it is where the game's own
// fifteen campaign missions reach LevelList, and it only puts them there **if
// the list is empty**. See ReconcileLevelList.
StdCall<void> EnterMainMenuScreen;

// unique_ptr, not by value: AddLevel hands the game a copy of every string, but
// `parsed_map` is handed to ToMap and `this` reaches a populate callback, so a
// vector reallocation must not move the registration.
std::vector<std::unique_ptr<CustomLevel>> Levels;

// Set for the duration of one custom level's conversion. Also the re-entrancy
// guard: a populate callback that itself parses a .gls would otherwise come back
// through the ConvertParsedObjects hook and build the map a second time.
CustomLevel *Building = nullptr;

// --- the game's ScriptFileName -----------------------------------------------

const char *ScriptFileName() {
  char **p;
  GetObjectAtOffset(p, 0x007b6dcc);
  return *p;
}

// Which custom level - if any - the game is loading. Keyed on ScriptFileName
// rather than on a flag we set at menu time, because every path that starts a
// level goes through that global: Choose Level, ADD MISSION, a savegame restore
// and a multiplayer client all write it before LoadLevel runs.
CustomLevel *LevelForCurrentScript() {
  const char *script = ScriptFileName();
  if (!script) {
    return nullptr;
  }
  for (const std::unique_ptr<CustomLevel> &level : Levels) {
    if (level->script_file == script) {
      return level.get();
    }
  }
  return nullptr;
}

// --- the level's identity -----------------------------------------------------

// This level's ScriptFileName. Nothing ever opens it - the prelude is parsed from
// memory (gls::ParseSource) and there is no .gcs - so it is an identity rather
// than a path, and the shape it has is what the engine's own derivations need:
//
//   * a stem plus a THREE-letter extension, because ToMap builds
//     "<ScriptFileName minus 3 chars>cut" in place and LoadOrBuildSectionAdjacency
//     does the same for ".map";
//   * more than four characters, which is the length LoadOrBuildSectionAdjacency
//     guards that overwrite with;
//   * no double quote, since PushFileToParserStack sprintf's the name into a
//     '# line 1 "<name>"' directive that pass 2 re-lexes;
//   * MACHINE-INDEPENDENT, which is the whole reason this is not the absolute
//     %TEMP% path it used to be. SaveGame serialises ScriptFileName verbatim and
//     ApplyUpdateMessage strdup's it out of a network payload on a joining client,
//     so an absolute path under one user's profile made a custom-level save
//     unportable and a multiplayer join silently fail to match any registration.
//
// The two derived cache names stay legal and land next to main.mjs in
// <Gunlok>\gkplus (ToMap restores the cwd to the game root before deriving them).
// Both are optional: the .cut read is an OPEN_EXISTING that has an explicit
// rebuild path, and the .map one is skipped the same way.
std::string VirtualScriptName(const char *title) {
  std::string stem;
  for (const char *c = title; *c; ++c) {
    stem += (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                    (*c >= '0' && *c <= '9')
                ? *c
                : '_';
  }
  return "gkplus\\" + stem + ".gls";
}

// The .gls a level's `includes` amount to, as a string. It is handed to the parser
// straight out of memory, which is why this is the whole of what used to be a file
// in %TEMP%: the parser's input is a source object, not a path (see GLS.h).
//
// One script rather than one LoadGLS call per include, because the multiple-
// inclusion guards only hold within a single call - ClearParseSymbolTables runs per
// call - so N separate parses would re-register every shared .gsh.
std::string PreludeSource(const CustomLevel &level,
                          const std::vector<std::string> &includes) {
  std::string source = "// Generated by GkPlus for the custom level \"" +
                       level.title + "\".\n\n";
  for (const std::string &include : includes) {
    source += "#include \"" + include + "\"\n";
  }
  // A script that defines nothing leaves ParsedObjList null, which makes LoadGLS
  // report "confused by earlier errors" and hand LoadLevel a null list. The
  // hooks below tolerate that, but the console line is noise, so the prelude
  // always defines one object. A shape needs exactly the two strings the map
  // section already requires, and resolves to the very rif object ToMap loads a
  // moment later - so this costs one hit on the rif cache and nothing else.
  source += "\n// Keeps the parsed-object list non-empty; see CustomLevel.cpp.\n"
            "shape GkPlusLevelGeometry\n{\n\tname \"" +
            level.map.object_name + "\"\n\tfile \"" + level.map.rif_file +
            "\"\n}\n";
  return source;
}

// --- the parsed map section ---------------------------------------------------

void Fail(const char *title, const char *why) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "custom level '%s': %s", title, why);
  Print(buf);
  DebugWrite(buf);
}

// Optional string fields take `none` when the description leaves them empty,
// which is what a .gls omitting the line resolves to.
bool SetOptionalString(gls::ParsedThing &thing, gls::FieldId id,
                       const std::string &value) {
  return value.empty() ? gls::SetNone(thing, id)
                       : gls::Set(thing, id, value.c_str());
}

// Builds the ParsedMap the game would have parsed out of a `map { ... }` block.
// Every assignment goes through the section's own CheckValue, so ranges, the
// none-allowed rules and the required-field bookkeeping are the game's.
gls::ParsedThing *BuildParsedMap(const char *title, const CustomLevelMap &map) {
  if (map.rif_file.empty() || map.object_name.empty()) {
    Fail(title, "needs both a rif file and an object name");
    return nullptr;
  }

  gls::ParsedThing *thing = gls::Create(gls::SectionType::Map);
  if (!thing) {
    Fail(title, "could not create a map section");
    return nullptr;
  }

  bool ok = gls::Set(*thing, gls::FieldId::Name, map.object_name.c_str()) &&
            gls::Set(*thing, gls::FieldId::File, map.rif_file.c_str()) &&
            SetOptionalString(*thing, gls::FieldId::Bitmap, map.bitmap) &&
            SetOptionalString(*thing, gls::FieldId::CameraPlane,
                              map.camera_plane) &&
            gls::Set(*thing, gls::FieldId::MaxCameraDistance,
                     map.max_camera_distance) &&
            SetOptionalString(*thing, gls::FieldId::MaxCameraFocusHeight,
                              map.max_camera_focus_height) &&
            SetOptionalString(*thing, gls::FieldId::MinCameraFocusHeight,
                              map.min_camera_focus_height) &&
            SetOptionalString(*thing, gls::FieldId::ShadowObjectRif,
                              map.shadow_object_rif) &&
            SetOptionalString(*thing, gls::FieldId::ShadowObjectName,
                              map.shadow_object_name) &&
            gls::Set(*thing, gls::FieldId::MaxVerticesPerSection,
                     map.max_vertices_per_section);
  // A rejected assignment has already named itself on the console (CheckValue
  // prints the field and the range); this says which level it belonged to.
  if (!ok) {
    Fail(title, "the game rejected one of the map values");
    gls::Release(thing);
    return nullptr;
  }
  if (!gls::IsValidDeep(*thing)) {
    Fail(title, "the map section is incomplete");
    gls::Release(thing);
    return nullptr;
  }
  return thing;
}

// --- conversion ----------------------------------------------------------------

// Converts the map and hands over to the script. ToMap is the map section's
// toGameObject slot; calling it directly is what ConvertParsedObjects does, and
// it neither takes nor drops a reference, so `parsed_map` survives for the next
// load of this level.
void BuildLevel(CustomLevel *level) {
  Building = level;
  // Definitions first, for the same reason a .gls opens with its #include block:
  // the roles have to be in the hash before anything can spawn one. It also runs
  // before ToMap so a script can build definitions off the map description.
  if (level->define) {
    level->define(level, level->user);
  }
  level->parsed_map->vtbl->to_game_object(level->parsed_map);
  if (!GetCurrentMap()) {
    Fail(level->title.c_str(), "the map failed to load; the level will be empty");
  } else if (level->populate) {
    level->populate(level, level->user);
  }
  Building = nullptr;
}

void __fastcall HookedConvertParsedObjects(gls::ParsedObjectList *list) {
  // Null is reachable and the game dereferences it without checking: LoadGLS
  // hands back null for a script that defined nothing (it prints "confused by
  // earlier errors" and returns the never-created ParsedObjList). The prelude
  // source always defines its filler shape so this should not happen, but an
  // #include that fails to parse would land exactly here, and a crash inside
  // LoadLevel is a much worse diagnostic than an empty level.
  if (list) {
    ConvertParsedObjects(list);
  }
  if (Building) {
    return; // re-entered from a populate callback that parsed a script itself
  }
  if (CustomLevel *level = LevelForCurrentScript()) {
    BuildLevel(level);
  }
}

void __fastcall HookedFreeParsedObjectList(gls::ParsedObjectList *list) {
  if (list) {
    FreeParsedObjectList(list);
  }
}

// The .gcs slot. Runs *before* the drain rather than after, because the drain
// loops until the queue is empty (0x004d62d0): a setup callback that queues
// commands - ExecuteCommandFile only appends, it does not run - gets them
// executed by this very call, which is what the level's own .gcs lines would
// have got. Queueing after it would instead leave them to the once-per-frame
// PumpQueuedConsoleCommand.
//
// `Building` is set for the callback so locators(), spawn() and levels.current
// work here as they do in populate; it is also what stops a setup callback that
// parses a .gls from re-entering the ConvertParsedObjects hook.
void __stdcall HookedExecuteAllCommands() {
  if (!Building) {
    if (CustomLevel *level = LevelForCurrentScript(); level && level->setup) {
      Building = level;
      level->setup(level, level->user);
      Building = nullptr;
    }
  }
  ExecuteAllCommands();
}

// An empty ParsedObjectList the game will destroy for us, laid out exactly as
// ParseGSH lays out its own (0x00478f6c): a pool_alloc'd 0x10 header whose
// sentinel is a pool_alloc'd 0xc List_Member_Base carrying vtable 0x00663064,
// linked to itself.
//
// ConvertParsedObjects walks it and finds nothing; FreeParsedObjectList destroys
// the sentinel through that vtable and pool-frees the header. Both are what the
// game would have done with a real list, so nothing leaks and nothing special
// has to be remembered.
gls::ParsedObjectList *NewEmptyParsedList() {
  auto *list =
      static_cast<gls::ParsedObjectList *>(pool_alloc(sizeof(gls::ParsedObjectList)));
  if (!list) {
    return nullptr;
  }
  void *sentinel = pool_alloc(0xc);
  if (!sentinel) {
    pool_free(list);
    return nullptr;
  }
  void *sentinel_vtbl;
  GetObjectAtOffset(sentinel_vtbl, 0x00663064);
  auto *words = static_cast<void **>(sentinel);
  words[0] = sentinel_vtbl;
  words[1] = sentinel; // prev
  words[2] = sentinel; // next
  list->sentinel = static_cast<List_Member_Base<gls::ParsedThing *> *>(sentinel);
  list->n_entries = 0;
  list->entry_pointers = nullptr;
  list->calculated_indices = false;
  return list;
}

// LoadLevel calls LoadGLS(ScriptFileName) unconditionally, and a custom level's
// ScriptFileName names no file. Letting the open fail is not an option for the
// level that asked for `includes` - it would parse nothing - and pointless for the
// one that did not.
//
// So both cases are served here without a file existing:
//
//   * `includes` become a source text, parsed from memory. gls::SourceTextScope
//     rather than gls::ParseSource, because the original has to be reached through
//     *this hook's* trampoline: gls::LoadGLS holds the raw address and would come
//     straight back in here.
//   * no `includes` means the parser is never involved at all, and an empty list
//     built the way ParseGSH builds its own goes back instead. That also keeps a
//     failed parse from poisoning every later one, which would take the next
//     *game* level down with it.
gls::ParsedObjectList *__fastcall HookedLoadGLS(const char *file, int mode) {
  if (file) {
    for (const std::unique_ptr<CustomLevel> &level : Levels) {
      if (level->script_file != file) {
        continue;
      }
      if (level->prelude_source.empty()) {
        return NewEmptyParsedList();
      }
      gls::SourceTextScope armed{file, level->prelude_source.c_str()};
      return LoadGLS(file, mode);
    }
  }
  return LoadGLS(file, mode);
}

// --- getting to Choose Level ------------------------------------------------------

void OpenChooseLevel(CustomMenuItem *, void *) {
  GoToMenu(MenuIndex::ChooseSinglePlayerLevel, true);
}

// Menu 5 holds the levels but has no reachable entry point on a normal launch:
// the game's "Choose Level" item exists only when FlagChooseLevel was set from
// the `-chooselevel` command line, and SetupMenus decides that long before any
// script has registered a level (see Menu.h). So the first custom level brings
// its own item, appended to Single Player after the game's own four.
void EnsureChooseLevelEntry() {
  static bool added = false;
  if (added || GetChooseLevelEnabled()) {
    return; // -chooselevel was passed; the game's own item is already there
  }
  added = true;
  AddCustomMenuItem(MenuIndex::SinglePlayer, "Choose Level", OpenChooseLevel,
                    nullptr);
}

// --- getting into the game's LevelList, without displacing the campaign -------
//
// **Registering a level and listing it are separate acts, and the order
// matters.** EnterMainMenuScreen @ 0x004e7e50 seeds LevelList with the fifteen
// campaign missions only when the list is *empty* - it opens with
// `CMP dword [0x007b74e0],0` / `JNZ 0x004e81d2`, and that jump skips all fifteen
// AddLevel calls and the block after them that seeds ScriptFileName /
// ConsoleFileName from the first entry. A script registers during SetupMenus,
// which runs earlier, so calling AddLevel there cost the player the whole
// campaign: Choose Level held only the script-defined levels, menu 7's "new
// game" launched the first of them (it starts `LevelList.sentinel->next`), and
// the default script name was never set at all.
//
// So the levels wait, and this appends them after the seed has had its turn.
// Idempotent by the `listed` flag rather than by re-reading the list, because
// two levels may legitimately share a title.
//
// Nothing here rebuilds anything: LevelList is cleared only by
// ShutdownMenuSystem at process exit, and EnterMainMenuScreen does not touch
// Menus[5], so the append happens exactly once per level per process and each
// one keeps the index AddLevel gave it. That index has to stay put - menu 5's
// dispatch walks the list positionally by ChosenMenuItem.
bool MainMenuEntered = false;

void ReconcileLevelList() {
  for (const std::unique_ptr<CustomLevel> &level : Levels) {
    if (level->listed) {
      continue;
    }
    // The game copies all three strings and appends the Choose Level item
    // itself. The .gcs slot is empty on purpose: a custom level's triggers and
    // setup are the setup callback's job, and ExecuteCommandFile on a missing
    // file is a no-op.
    AddLevel(level->title.c_str(), level->script_file.c_str(), "");
    level->listed = true;
  }
}

void __stdcall HookedEnterMainMenuScreen() {
  EnterMainMenuScreen();
  MainMenuEntered = true;
  ReconcileLevelList();
}

} // namespace

const char *CustomLevelTitle(const CustomLevel *level) {
  return level ? level->title.c_str() : "";
}

const char *CustomLevelScriptFile(const CustomLevel *level) {
  return level ? level->script_file.c_str() : "";
}

CustomLevel *CustomLevelByTitle(const char *title) {
  if (!title) {
    return nullptr;
  }
  for (const std::unique_ptr<CustomLevel> &level : Levels) {
    if (_stricmp(level->title.c_str(), title) == 0) {
      return level.get();
    }
  }
  return nullptr;
}

const CustomLevelMap &CustomLevelDescription(const CustomLevel *level) {
  static const CustomLevelMap Empty{};
  return level ? level->map : Empty;
}

CustomLevel *CurrentCustomLevel() { return Building; }

bool DispatchCustomLevelMessage(const char *json) {
  if (!json) {
    return false;
  }
  // LevelForCurrentScript, not Building: a message arrives during play, long
  // after the load callbacks have run and cleared that. ScriptFileName is what
  // stays set for as long as the level is loaded.
  CustomLevel *level = LevelForCurrentScript();
  if (!level || !level->message) {
    return false;
  }
  level->message(level, json, level->user);
  return true;
}

std::vector<CustomLevelLocator> LevelRifLocators(const char *object_name) {
  std::vector<CustomLevelLocator> out;
  Map *map = GetCurrentMap();
  CustomLevel *level = Building;
  if (!map || !level || !object_name) {
    return out;
  }

  // The same pair ToMap's warm path uses. AcquireLevelRifForLocators probes the
  // .loc sidecar next to the rif and returns the loaded rif either way.
  //
  // **This is a full disk LOAD, not a lookup**, which an earlier revision of this
  // comment had backwards in both halves. `LoadLevel` calls RifCache_Clear @
  // 0x004aead0 immediately after ConvertParsedObjects (@ 0x004e0e70), so by the
  // time anything here runs the cache is empty and the level's rif is freed; and
  // the .loc branch passes flag 0, which flushes the cache and re-reads even on a
  // hit. So each call re-reads the file and frees whatever rif was held before -
  // which is also why nothing may retain the pointer this returns.
  FastCall<void *, const char *> acquire_rif;
  GetObjectAtOffset(acquire_rif, 0x00483da0);
  void *rif = acquire_rif(level->map.rif_file.c_str());
  if (!rif) {
    return out;
  }

  // RifFilterObjectsByName appends to a caller-built list and takes no ownership
  // of it. Its call sites in ToMap all inline the same List ctor: a pool_alloc'd
  // 0xc-byte sentinel carrying List_Member_Base's vtable @ 0x00663c2c, linked to
  // itself. List__Dtor @ 0x00483420 is the matching teardown - it empties the
  // list and destroys the sentinel, but leaves the header, which is why this one
  // can live on the stack.
  void *block = pool_alloc(0xc);
  if (!block) {
    return out;
  }
  // Laid out by hand rather than by a constructor: List_Member_Base is a mirror
  // of game memory with a pure virtual destructor, so there is nothing here to
  // construct - the vptr has to be the game's, because List__Dtor calls through
  // it to free this block.
  void *sentinel_vtbl;
  GetObjectAtOffset(sentinel_vtbl, 0x00663c2c);
  auto *words = static_cast<void **>(block);
  words[0] = sentinel_vtbl;
  words[1] = block; // prev
  words[2] = block; // next

  List<void *> matches{static_cast<List_Member_Base<void *> *>(block), 0,
                       nullptr, false};
  // __fastcall, not __thiscall: it takes the out list in ECX **and** the rif in
  // EDX (it reads [EDX+0xc] at 0x005aaac8), with only `name` on the stack. A
  // ThisCall pointer would have put the rif on the stack and left EDX holding
  // whatever the caller last used, and cleaned 8 bytes where the callee cleans 4.
  // See the trap in CLAUDE.md: args in ECX *and* EDX means __fastcall.
  FastCall<void, List<void *> *, void *, const char *> filter;
  GetObjectAtOffset(filter, 0x005aaac0);
  filter(&matches, rif, object_name);

  for (void *object : matches) {
    auto *bytes = static_cast<const unsigned char *>(object);
    auto at = [bytes](size_t off) { return bytes + off; };
    // Locator coordinates are signed int32 in rif units - FILD, not a float
    // load - and become world space the way ToMap does it.
    Vec3 rif_pos{
        static_cast<float>(*reinterpret_cast<const int32_t *>(at(0x44))),
        static_cast<float>(*reinterpret_cast<const int32_t *>(at(0x48))),
        static_cast<float>(*reinterpret_cast<const int32_t *>(at(0x4c)))};
    CustomLevelLocator loc{};
    loc.position = MapToWorld(map, rif, rif_pos);
    loc.orientation = *reinterpret_cast<const Vec4 *>(at(0x50));
    out.push_back(loc);
  }

  FastCall<void, List<void *> *> list_dtor;
  GetObjectAtOffset(list_dtor, 0x00483420);
  list_dtor(&matches);
  return out;
}

CustomLevel *AddCustomLevel(const char *title, const CustomLevelMap &map,
                            const std::vector<std::string> &includes,
                            CustomLevelDefine define,
                            CustomLevelPopulate populate,
                            CustomLevelSetup setup,
                            CustomLevelMessage message, void *user) {
  if (!title || !*title) {
    return nullptr;
  }
  for (const std::unique_ptr<CustomLevel> &existing : Levels) {
    if (existing->title == title) {
      Fail(title, "a level with that name is already registered");
      return nullptr;
    }
  }

  // Built before anything is registered, so a bad description costs a console
  // line at startup instead of a half-loaded level later.
  gls::ParsedThing *parsed = BuildParsedMap(title, map);
  if (!parsed) {
    return nullptr;
  }

  auto level = std::make_unique<CustomLevel>();
  level->title = title;
  level->script_file = VirtualScriptName(title);
  level->map = map;
  level->parsed_map = parsed;
  level->define = define;
  level->populate = populate;
  level->setup = setup;
  level->message = message;
  level->user = user;

  // Only levels that name `includes` need a script at all - those are the ones
  // whose roles come from .gsh files. A level built entirely with `make` or `gls`
  // leaves this empty and never reaches the parser.
  if (!includes.empty()) {
    level->prelude_source = PreludeSource(*level, includes);
  }

  // script_file is the identity every load path is matched on, so two levels
  // holding the same one would make the second unreachable. Titles differing only
  // in punctuation collide, since that is what VirtualScriptName folds away.
  for (const std::unique_ptr<CustomLevel> &existing : Levels) {
    if (existing->script_file == level->script_file) {
      Fail(title, "another level already uses that script name - the titles "
                  "differ only in punctuation");
      gls::Release(parsed);
      return nullptr;
    }
  }

  CustomLevel *raw = level.get();
  Levels.push_back(std::move(level));

  // Listing is deferred to the first EnterMainMenuScreen so the campaign gets
  // the empty list it insists on - see ReconcileLevelList. Once that has
  // happened the order is settled and a later registration (from the REPL, say,
  // or a module that runs after boot) can be appended at once.
  if (MainMenuEntered) {
    ReconcileLevelList();
  }
  EnsureChooseLevelEntry();
  return raw;
}

void ClearCustomLevelActions() {
  for (const std::unique_ptr<CustomLevel> &level : Levels) {
    level->define = nullptr;
    level->populate = nullptr;
    level->setup = nullptr;
    level->message = nullptr;
    level->user = nullptr;
  }
}

CustomLevelSystem::CustomLevelSystem() {
  GetObjectAtOffset(ConvertParsedObjects, 0x004747b0);
  GetObjectAtOffset(FreeParsedObjectList, 0x00474870);
  GetObjectAtOffset(LoadGLS, 0x00474540);
  GetObjectAtOffset(ExecuteAllCommands, 0x004d62c0);
  GetObjectAtOffset(EnterMainMenuScreen, 0x004e7e50);

  DetourAttach(&ConvertParsedObjects, HookedConvertParsedObjects);
  DetourAttach(&FreeParsedObjectList, HookedFreeParsedObjectList);
  DetourAttach(&LoadGLS, HookedLoadGLS);
  DetourAttach(&ExecuteAllCommands, HookedExecuteAllCommands);
  DetourAttach(&EnterMainMenuScreen, HookedEnterMainMenuScreen);

  // No hook of its own - ScriptQueueSystem owns those, and this only says where
  // a message payload goes once it has one. Order between the two subsystems
  // does not matter: both sides are file statics.
  SetScriptMessageHandler(DispatchCustomLevelMessage);
}

CustomLevelSystem::~CustomLevelSystem() {
  SetScriptMessageHandler(nullptr);
  DetourDetach(&ConvertParsedObjects, HookedConvertParsedObjects);
  DetourDetach(&FreeParsedObjectList, HookedFreeParsedObjectList);
  DetourDetach(&LoadGLS, HookedLoadGLS);
  DetourDetach(&ExecuteAllCommands, HookedExecuteAllCommands);
  DetourDetach(&EnterMainMenuScreen, HookedEnterMainMenuScreen);
}
} // namespace gk
