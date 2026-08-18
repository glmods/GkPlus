#pragma once

#include <cstdint>
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
// A mod is a `.zip` (or any other archive PhysicsFS reads) or a plain directory
// under `<profile>\mods` (src/Profile.h), and its contents **mirror the game's
// own directory tree**:
//
//     <profile>/mods/bigger-bugs.zip
//       rif/units/bug.rif          <- replaces <Gunlok>\rif\units\bug.rif
//       scripts/defaults.gsh       <- replaces <Gunlok>\scripts\defaults.gsh
//       sound/robots.dat
//
// That mapping is not a convention we chose, it is the one the engine forces.
// Every loader does SetCurrentDirectoryToGLDir(<category>) and then opens a
// *relative* name (53 call sites; see file_io_notes.md §2), so the only path a
// hook can reconstruct is "where in the game tree was this". The seven
// categories come from `gldirs.gls` and in a stock install are exactly `scripts`,
// `fmv`, `rif`, `graphics`, `sound`, `fonts` and `Screenshots`.
//
// --- Who decides what is mounted -----------------------------------------------
//
// **This file mounts nothing on its own.** Initialize() starts PhysicsFS with an
// empty search path; every mount comes from a Mount() or MountAll() call, and in
// a running game those come from the profile's boot script (`core.boot`, see
// src/Script.h), which runs at FileHookSystem's first intercepted open - the last
// instant before the engine reads an asset, and therefore the only point from
// which the decision can still be made.
//
// So a profile with no boot module runs the base game however many archives are
// sitting in its `mods` directory, and `mods.mount_all()` in a boot script is
// what asks for the scan-everything behaviour. That is deliberate: which mods a
// launch gets is the most consequential thing about it, and having it follow
// from a directory listing meant it could not be varied without moving files
// about.
//
// --- Load order ---------------------------------------------------------------
//
// MountAll() mounts in ascending name order, and **a later name wins**: given
// `10-base.zip` and `20-tweaks.zip` that both carry `rif/units/bug.rif`, the one
// from `20-tweaks.zip` is what the game gets. That falls out of Mount()
// prepending - each mount outranks the one before it - so a Mount() after a
// MountAll() beats everything in it, and a script that wants a different order
// just calls Mount() in the order it wants, weakest first.
//
// --- What this deliberately does not do --------------------------------------
//
// Only mod content lives in the VFS; the base install is never mounted. A lookup
// miss therefore means "the game reads the real file exactly as it always did",
// which keeps the blast radius of the whole feature to files a mod actually
// ships. It also means Resolve() is the one place that has to be fast, and it is
// a PhysicsFS hash lookup on a miss.

namespace gk::vfs {

// One mod. Mods() reports the mounted ones in search-path order (index 0 is
// consulted first, i.e. it is the highest-priority mod); Discover() reports
// candidates that are not mounted at all.
struct Mod {
  std::string name; // the entry name inside <profile>\mods, e.g. "20-tweaks.zip"
  std::string path; // absolute, exactly what PHYSFS_mount was given
  bool archive;     // false for a plain directory
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
bool IsInitialized();
// Unmounts everything and removes the materialization directory. Nothing may
// call into this namespace afterwards except Initialize().
void Shutdown();

// In search-path order. Empty until the first Initialize().
const std::vector<Mod> &Mods();

// Where gl.exe lives, with a trailing backslash. Every VFS path is relative to
// this, and a file resolved outside it is never virtualized.
const std::string &GameDir();
// <profile>\mods\, with a trailing backslash. Need not exist.
const std::string &ModsDir();

// What is sitting in `dir` (ModsDir() when null or empty), ascending by name and
// case-insensitively, whether or not any of it is mounted or is even something
// PhysicsFS can read. Nothing is opened, so this is the list a script filters
// before deciding what to Mount().
std::vector<Mod> Discover(const char *dir);

// Mounts one archive or directory at the highest priority - above everything
// mounted so far. `error` is filled with PhysicsFS's message on failure and may
// be null.
bool Mount(const char *path, std::string *error);

// Discover(dir) then Mount() each, ascending, so a later name wins. Returns how
// many mounted, or -1 if the filesystem is unavailable; an entry PhysicsFS
// cannot read is logged and skipped rather than failing the call.
int MountAll(const char *dir, std::string *error);

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
// at mount time hides the difference - see g_index in Vfs.cpp for why it has to.
// This matters because the casing the engine asks for is undiscoverable: the
// directory comes from `gldirs.gls` (`rif`) and the file name from a .gls or an
// exe string literal (`bitmaps\water.rim`, `User Interface/Main Menu.RIF`).

bool Exists(const char *vpath);
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
