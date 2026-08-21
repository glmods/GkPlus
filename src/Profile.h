#pragma once

#include <string>

// The **profile**: one directory holding everything a launch is configured by -
// `settings.json` and the boot and entry scripts it names - and the anchor a
// relative mod path is resolved against.
//
//     <profile>/settings.json   the shared settings repository (src/Settings.h)
//     <profile>/boot.mjs        `core.boot`  - runs inside WinMain, mounts mods
//     <profile>/main.mjs        `core.script` - the entry module, at SetupMenus
//     <profile>/mods/           only a convention - see below
//
// `GKPLUS_PROFILE` selects it; with the variable unset it is `gkplus` beside
// this DLL, which is where all four of those already lived, so a stock install
// is unchanged. Two profiles are therefore two directories, and switching
// between them is one environment variable rather than a file swap.
//
// This is the **only** launch-time path knob. There is no separate override for
// the settings file or for the entry script: `GKPLUS_SETTINGS` and
// `GKPLUS_SCRIPT` were exactly the two halves of "point GkPlus somewhere else",
// and splitting them let a run take its settings from one place and its script
// from another - which is not a configuration anybody wanted, and it left the
// mods directory behind in the install either way.
//
// **The profile does not own a mods directory.** `src/Vfs` has no notion of where
// a mod lives; a boot script names paths, and a *relative* one is resolved against
// this directory - which is what makes a mod list in `settings.json` follow
// GKPLUS_PROFILE instead of hard-coding a location. `<profile>/mods/` above is
// therefore whatever layout a profile's own config chose, not a path anything here
// knows.
//
// Nothing here touches game memory or reads a file, so it is safe from DllMain
// and harness-testable.
namespace gk::profile {

// The profile directory: `GKPLUS_PROFILE` if set, otherwise `<the directory
// holding d3d8.dll>/gkplus`. Forward slashes, no trailing separator, and "" if
// the module path could not be resolved. Resolved once, on first use.
//
// A **relative** `GKPLUS_PROFILE` is relative to the game directory, not to the
// launching shell's: this is first read from inside a file open the engine makes
// having already chdir'd into a GLDir, so the process's current directory at that
// moment is an asset category rather than anything the user chose.
const std::string &Dir();

// `relative` joined onto Dir(), with forward slashes. An absolute `relative` (a
// drive letter, or a leading separator) is returned normalized but unjoined, so
// a settings key may name a file outside the profile. "" when `relative` is
// empty, or when it is relative and Dir() is.
std::string Resolve(const char *relative);

} // namespace gk::profile
