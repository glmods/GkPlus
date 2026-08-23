#pragma once

#include <string>
#include <vector>

#include "Json.h"

// `<profile>\settings.json` - one JSON file for anything that has to outlive a
// launch, in the directory `GKPLUS_PROFILE` names (src/Profile.h).
//
// It is also what decides **what runs**: `core.boot` and `core.script` name the
// two script modules, so a profile is a settings file plus the scripts it points
// at, and switching profiles switches both together.
//
// **It is a shared repository, not GkPlus's own file.** The top level is one
// object per owner: GkPlus keeps its settings under `core`, and a mod takes a key
// of its own next to it. Nothing here knows the schema of anything, and a write
// re-serialises the *parsed* document, so a section belonging to a mod this build
// has never heard of survives being rewritten by a build that only understands
// `core`. That is the whole reason `json::Document` exists.
//
//     {
//       "core":  { "render": { "msaa": 4, "ao": true } },
//       "mymod": { "anything": [1, 2, 3] }
//     }
//
// **Precedence: an environment variable wins.** The `GKPLUS_*` overrides are
// launch-time instruments - the switch you reach for when the thing you want to
// turn off is the thing stopping the game from starting - so a stored value must
// never quietly beat one. The knobs that have such a companion apply the file only
// when the variable is absent (src/RenderMenu.cpp). `GKPLUS_PROFILE` is the one
// exception in shape rather than in rule: it decides *which* file is read, so
// there is nothing in the file for it to lose to.
//
// **Scripts see this document itself, not a copy** - `settings` in JS is an object
// tree over the calls below, where every read and write goes straight through
// (src/JsSettings.cpp). Nothing here is cached on that side, which is what keeps a
// script and the front-end pages that write `core.render.*` from ever holding two
// different answers. Nor does anything call Save(): SaveSettled() below does it
// once a change settles.
//
// Loading is lazy and happens once, on the first access, because there is nothing
// to read at DllMain: the file is found relative to this module and read with our
// own CRT, but doing file I/O and a JSON parse under the loader lock buys nothing
// when every caller runs later anyway.
//
// Paths are dot-separated and go straight to `json::Document`, so the same rules
// apply - a key containing a dot cannot be addressed.
namespace gk::settings {

// `<profile>/settings.json`, with forward slashes, or "" if the profile
// directory could not be worked out. Pointing `GKPLUS_PROFILE` at a directory of
// its own is what lets a test run against a settings file of its own.
const std::string &Path();

// The value at `path` as JSON text, "" when it is not there.
std::string GetJson(const char *path);
// `json` must be one complete JSON document. Does **not** save - a caller that
// wants the change on disk calls Save(), which is one write for any number of
// changes.
bool SetJson(const char *path, const char *json);
/// Deletes the value at `path` and everything under it. False when there was
/// nothing there. Does not save.
bool Remove(const char *path);

// What shape the value at `path` has, and what is in it if that is an object.
// These are how the script-facing tree walks the document (src/JsSettings.cpp):
// a subtree that is an object gets a live node, anything else is read as a value.
// An empty path is the document itself - the one place that is meaningful, see
// json::Document::KindAt.
json::Kind KindAt(const char *path);
/// The own keys of the object at `path`, in document order. Empty when `path`
/// is absent or is not an object.
std::vector<std::string> Keys(const char *path);

// Typed convenience over the two above. A value of the wrong type reads as
// absent rather than being coerced: a `true` where a number belongs is a mistake
// in the file, and silently making it 1 would hide it.
bool GetBool(const char *path, bool fallback);
/// The number at `path`, or \p fallback when it is absent or not a number.
double GetNumber(const char *path, double fallback);
/// The string at `path`, or \p fallback when it is absent or not a string.
std::string GetString(const char *path, const char *fallback);
/// Writes a boolean at `path`, creating the intermediate objects the path
/// names. Does not save. False means the path could not be written.
bool SetBool(const char *path, bool value);
/// Writes a number at `path`, creating intermediate objects. Does not save.
bool SetNumber(const char *path, double value);
/// Writes a string at `path`, creating intermediate objects. Does not save.
bool SetString(const char *path, const char *value);

// Whether `path` has a value at all - which is how "the file says nothing about
// this, leave the default alone" is told from "the file says false".
bool Has(const char *path);

// The whole document as JSON text.
std::string Text();

// Writes the document out, creating `gkplus\` if it is not there. The write goes
// to a temporary and is then moved over the target, because a half-written file
// would take every *other* owner's section with it.
bool Save();
// Save() only if something has been written since the last save or load, so that
// calling it costs nothing for a launch that changed nothing. True means either
// that there was nothing to do or that the write succeeded.
bool SaveIfDirty();
// The same, once a change has *settled*: dirty, and either nothing written for a
// second or a run of writes going on for fifteen. Cheap enough to call every
// frame, which is what the script host does (src/Script.cpp).
//
// **This, and not the flush at DLL detach, is what makes a change durable**, and
// that ordering is measured rather than cautious: exiting Gunlok faults
// (game_defects_notes.md 4), so DLL_PROCESS_DETACH is best-effort - `vfs`'s
// temp-tree cleanup was written to run there and never ran once. The repo's rule
// out of that is to prefer doing the work while the process is healthy, so a
// settled write reaches the disk with the game still running and the flush on the
// way out only covers a change made in the last few frames.
//
// The delay is what keeps a per-frame assignment - a window position dragged
// across the screen - from being one file write per frame; the fifteen-second cap
// is what keeps a script that writes *every* frame from deferring the save
// forever.
void SaveSettled();
// Re-reads from disk, discarding anything set since the last save.
bool Reload();

} // namespace gk::settings
