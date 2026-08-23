#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The mod filesystem: archives and directories layered over Gunlok's data tree,
// so a mod can add or replace any file the engine loads without a byte of the
// base install changing. PhysicsFS provides the archive readers and the
// search-path semantics; everything here is the translation between what the
// engine asks for and what PhysicsFS understands.
//
// This header is the *lookup* half and touches no game memory at all. The half
// that makes the engine consult it is src/FileHooks.h.
//
// --- The layout ---------------------------------------------------------------
//
// A mod is a `.zip` (or any other archive PhysicsFS reads) or a plain directory,
// **anywhere on disk**, and its contents **mirror the game's own directory
// tree**:
//
//     D:\mods\bigger-bugs.zip
//       metadata/info.json         <- who this mod is (see ModInfo)
//       metadata/README.md
//       metadata/bugs.mjs          <- run when this mod is enabled, if info.json
//                                     names it (see ModFile, and src/Script.h)
//       rif/units/bug.rif          <- replaces <Gunlok>\rif\units\bug.rif
//       scripts/defaults.gsh       <- replaces <Gunlok>\scripts\defaults.gsh
//       sound/robots.dat
//
// That *internal* mapping is not a convention we chose, it is the one the engine
// forces. Every loader does SetCurrentDirectoryToGLDir(<category>) and then opens
// a *relative* name (53 call sites; see file_io_notes.md §2), so the only path a
// hook can reconstruct is "where in the game tree was this". The seven
// categories come from `gldirs.gls` and in a stock install are exactly `scripts`,
// `fmv`, `rif`, `graphics`, `sound`, `fonts` and `Screenshots`.
//
// `metadata` is the one directory that is *not* game content: the game has no
// such category, so nothing an engine open asks for can ever land in it. That is
// also why a mod's scripts live there and are named relative to it - a `.mjs` in
// `scripts/` would be sitting in a directory the engine does chdir into, and
// every mod's copy would collide in the merged view besides.
//
// Where the mod itself sits, on the other hand, is nobody's convention: there is
// no blessed location and no mods directory, and nothing here knows where a mod
// lives - only what path it was handed. A **relative** path is resolved against
// the profile (see AbsolutePath), which is what makes a mod list portable: a
// settings.json naming `mods/bigger-bugs.zip` follows GKPLUS_PROFILE rather than
// hard-coding a location that exists on one machine. The `mods/` in that is
// whoever wrote the config choosing a layout, not this layer knowing one.
//
// --- Two steps, because they answer different questions --------------------------
//
// **Load** reads a mod's metadata and nothing else - it is how a script (or a
// manager UI) learns what a *named* mod is before deciding anything. **Enable**
// declares the active set, in order, and is the only thing that puts a file in
// front of the engine:
//
//     const hi_res = vfs::Load("D:/mods/hi-res.zip", &why);
//     const tweaks = vfs::Load("D:/mods/tweaks", &why);
//     vfs::Enable({tweaks->path, hi_res->path}, &why);   // hi_res wins
//
// Enable **replaces** the set rather than adding to it, so the enabled list and
// its order are stated in one place instead of accumulated by a sequence of
// calls whose order is then the only record of the load order. Re-enabling with a
// different order is how a mod is reordered, and enabling with a shorter list is
// how one is switched off.
//
// **This file enables nothing on its own, and it does not scan for candidates
// either.** Initialize() starts PhysicsFS with an empty search path; every
// enabled mod comes from an Enable() call naming a path, and in a running game
// those come from the profile's boot script (`core.boot`, see src/Script.h),
// which runs at FileHookSystem's first intercepted open - the last instant before
// the engine reads an asset, and therefore the only point from which the decision
// can still be made.
//
// There is deliberately **no directory enumeration and no mods directory**. A mod
// is named by a script or by something a script read out of config, and nothing
// else; a mod sitting next to one that is named does not load. That rules out the
// whole class of "it was enabled because it was in the folder" surprise - a
// renamed-to-disable directory that is still served, a leftover preview mod
// changing what the game draws, a baseline run that is silently still modded -
// none of which a listing can distinguish from an intention. A blessed directory
// would put that one indirection away rather than remove it, so there is not one:
// a mod is a path, and it can be any path.
//
// --- Load order ---------------------------------------------------------------
//
// Enabled() reports the set in **load order, and a later entry wins**: given
// Enable({"tweaks", "hi-res"}) a file both provide comes from `hi-res`. That is
// the direction every mod manager reads in, and it falls out of PHYSFS_mount
// prepending - each mount outranks the one before it, so walking the list forward
// and prepending puts the last one first in the search path.
//
// --- What this deliberately does not do --------------------------------------
//
// Only mod content lives in the VFS; **the base install is never mounted and is
// not a mod**. A lookup miss means "the game reads the real file exactly as it
// always did", which keeps the blast radius of the whole feature to files a mod
// actually ships. It also means Resolve() is the one place that has to be fast,
// and it is a PhysicsFS hash lookup on a miss.
//
// So the install is *underneath* every mod without being part of the load order,
// and Load() refuses it by name (see Load) rather than letting a caller mount the
// whole game directory as a mod - which costs an index walk over every shipped
// asset and buys nothing a miss does not already do.

namespace gk::vfs {

// What `metadata/info.json` says a mod is. Every field is optional and an absent
// one is "" - a mod with no info.json at all still loads, with `name` falling
// back to the entry name and Mod::problems saying what was missing. Being strict
// here would mean every mod predating the metadata contract, including the ones
// this repo's own tooling writes, stopped loading rather than reading as
// incomplete.
struct ModInfo {
  std::string name;
  std::string author;
  std::string website;
  std::string license;
  std::string version;
  // What `script` names, relative to `metadata/` and with forward slashes - the
  // module the script host evaluates when this mod is enabled. "" when info.json
  // names none, **and also when it names one that is not there**: the miss is a
  // Mod::problem and clearing the field is what keeps the host from having to
  // re-check something Load already knows.
  std::string script;
};

// One JavaScript file out of a mod's `metadata` directory, held as source.
//
// The bytes are read at Load() time and kept, which is not an optimization: it
// is the only moment a mod can be read **on its own**. PHYSFS_mount silently
// succeeds without mounting when the archive is already in the search path
// (doMount returns 1 on a strcmp match, whatever mount point it is handed), so
// an enabled mod cannot be re-mounted for inspection later - and the
// PHYSFS_unmount that followed would take the real mount with it. Reading a
// mod's scripts at Load, under the inspection mount that is already open, is
// what avoids that entirely.
struct ModFile {
  // Path relative to `metadata/`, forward slashes, as it was spelled on disk.
  std::string path;
  std::string source;
};

// One mod: a candidate that has been Load()ed, whether or not it is enabled.
//
// Records are **stable and never freed** - Load() interns one per canonical path
// and hands back a pointer good for the life of the process, which is what lets
// the JS layer wrap one without a finalizer and what makes a Mod * an identity
// rather than a snapshot.
struct Mod {
  // The entry name on disk, e.g. "20-tweaks.zip".
  std::string entry;
  // Absolute, backslashed, and exactly the string PHYSFS_mount/PHYSFS_unmount
  // are given - PhysicsFS matches a mount by strcmp on it, so the two calls have
  // to agree character for character.
  std::string path;
  bool archive = false; // false for a plain directory

  ModInfo info;
  // The display name: info.name, or `entry` when info.json did not give one.
  std::string name;
  // metadata/README.md, with CRLF normalised to LF. Empty when there is none.
  std::string readme;
  // metadata/icon_small.png and icon_big.png, verbatim. Empty when absent.
  std::vector<char> icon_small;
  std::vector<char> icon_big;
  // Every `.mjs` / `.js` under `metadata/`, in enumeration order. The one
  // info.name's `script` field points at is the entry point; the rest are here so
  // that an `import "./helper.mjs"` inside it resolves - which nothing else could
  // do for an archive mod, since the file has no path on disk to read.
  std::vector<ModFile> scripts;
  // What is wrong with this mod's metadata, in the order it was found: a missing
  // info.json, a malformed one, a field of the wrong type, an icon that is not a
  // PNG, a `script` naming a file the mod does not ship. Empty means the metadata
  // contract is met.
  std::vector<std::string> problems;

  // Position in Enabled(), where the **highest number wins** a conflict. -1 when
  // this mod is not enabled.
  int order = -1;
  bool enabled() const { return order >= 0; }
};

// Starts PhysicsFS with an empty search path. Called lazily by every function
// below, so nothing has to sequence it against the game's startup - the first
// file the engine opens triggers it, which is inside WinMain and therefore long
// after DllMain's loader lock.
//
// A failure is remembered: the whole layer then reports "no mod provides this"
// forever and the game runs unmodified. That is also what happens when there is
// no mods directory, which is the common case.
bool Initialize();
/// Whether Initialize() has run and succeeded. False both before the first
/// intercepted open and after a failure the layer has remembered.
bool IsInitialized();
// Unmounts everything and removes the materialization directory. Nothing may
// call into this namespace afterwards except Initialize().
void Shutdown();

// Where gl.exe lives, with a trailing backslash. Every VFS path is relative to
// this, and a file resolved outside it is never virtualized. It is *not* what a
// relative path given to Load() resolves against - that is the profile.
const std::string &GameDir();

// --- Loading -------------------------------------------------------------------

// Interns `path` as a mod and reads its `metadata` directory. `path` may be
// absolute, or relative to the profile directory (see AbsolutePath in the .cpp),
// and may point anywhere. Returns null when there is nothing loadable there - no such
// path, nothing PhysicsFS can open as an archive, or the game directory itself -
// with `error` (which may be null) saying which. Absent or malformed metadata is
// *not* a failure: it lands in Mod::problems.
//
// **The install is refused by name.** It is already what every lookup miss falls
// through to, so mounting it as a mod would add an index walk over every shipped
// asset and change nothing about what the engine reads.
//
// Loading the same path twice returns the same record and re-reads nothing, so a
// Mod * is a stable identity and the archive is opened once. That also keeps the
// inspection mount this uses from ever colliding with a real one: a path already
// in the search path is by construction already interned.
const Mod *Load(const char *path, std::string *error);

// Every Load()ed mod, in the order they were first loaded, enabled or not.
std::vector<const Mod *> Loaded();

// The source text of one of `mod`'s cached `metadata` scripts, by its path
// relative to `metadata/` - case-insensitively, and with either slash - or null.
//
// This layer **stores and serves those bytes and decides nothing about them**.
// What `info.json`'s `script` names is Mod::info.script, when it is run is
// src/Script.h's (the mod becoming enabled), and the module names they are
// evaluated under are the host's too. The pointer is good for the life of the
// process, records being interned and never freed.
const std::string *ModScript(const Mod *mod, const char *path);

// --- Enabling ------------------------------------------------------------------

// Replaces the enabled set with `paths`, in load order, so the **last one wins**
// a file conflict. Each is Load()ed if it has not been already, and a path
// appearing twice keeps its last position.
//
// Returns how many mods are enabled, or -1 if the filesystem is unavailable. An
// entry that cannot be loaded or mounted is logged, appended to `error` and
// skipped, rather than failing the whole call - so this does not equal
// `paths.size()`.
//
// Everything previously enabled is unmounted first, which makes reordering and
// switching one off the same operation as enabling. An empty `paths` is therefore
// how a run gets the unmodified game.
int Enable(const std::vector<std::string> &paths, std::string *error);

// The load order, weakest first: the **last** entry wins a conflict.
std::vector<const Mod *> Enabled();

// --- The lookup the hooks use --------------------------------------------------

// Turns whatever the engine handed an open call into a VFS path, or nothing if
// no mod provides that file.
//
// `engine_path` is usually relative to the process's *current* directory, which
// is whichever GLDir the caller just chdir'd to - so this resolves it against
// the CWD, requires the result to sit under GameDir(), and hands back the
// remainder with forward slashes. Absolute paths work too, as long as they land
// inside the game tree; a save under the user's profile does not and is passed
// through untouched.
//
// Only a regular file counts: a directory that happens to share the name is a
// miss, because the engine cannot open one.
std::optional<std::string> Resolve(const char *engine_path);

// --- Reading -------------------------------------------------------------------
//
// `vpath` is a VFS path: forward slashes, relative to the game root, as returned
// by Resolve(). Backslashes are accepted too.
//
// All of these are **case-insensitive**, and that is GkPlus's doing, not
// PhysicsFS's: PhysicsFS is case-*sensitive* inside an archive (its zip archiver
// inits its directory tree with `case_sensitive = 1`), while a mounted plain
// directory goes to the Windows filesystem and is not. A case-folded index built
// when the enabled set changes hides the difference - see g_index in Vfs.cpp for
// why it has to. This matters because the casing the engine asks for is
// undiscoverable: the directory comes from `gldirs.gls` (`rif`) and the file name
// from a .gls or an exe string literal (`bitmaps\water.rim`,
// `User Interface/Main Menu.RIF`).

bool Exists(const char *vpath);
/// Reads the whole of `vpath` into \p out, replacing whatever was there.
///
/// \return false when no enabled mod provides `vpath`, or the read failed; the
///         contents of \p out are then unspecified. A miss is the ordinary
///         case and is what makes the engine fall through to the real file;
///         this never consults the base install.
bool Read(const char *vpath, std::vector<char> *out);
// `modtime` is Unix seconds, or -1 when the archive does not record one.
bool Stat(const char *vpath, uint64_t *size, int64_t *modtime);
// Every regular file at or below `dir` (empty or null for the whole VFS), as
// VFS paths, in enumeration order.
std::vector<std::string> Files(const char *dir);

// Writes `vpath` out to a real file and hands back its absolute path, for the
// consumers that cannot be given a virtual handle: the game's own static-CRT
// fopen (see FileHooks.cpp) is the one caller today. Results are cached per
// vpath and live in a per-process directory under %TEMP% that Shutdown()
// removes, so the same header materializes once per session.
bool Materialize(const char *vpath, std::string *out);

} // namespace gk::vfs
