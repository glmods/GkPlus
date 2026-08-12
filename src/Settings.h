#pragma once

#include <string>

// `<Gunlok>\gkplus\settings.json` - one JSON file for anything that has to
// outlive a launch.
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
// when the variable is absent (src/RenderMenu.cpp).
//
// Loading is lazy and happens once, on the first access, because there is nothing
// to read at DllMain: the file is found relative to this module and read with our
// own CRT, but doing file I/O and a JSON parse under the loader lock buys nothing
// when every caller runs later anyway.
//
// Paths are dot-separated and go straight to `json::Document`, so the same rules
// apply - a key containing a dot cannot be addressed.
namespace gk::settings {

// `<the directory holding d3d8.dll>\gkplus\settings.json`, with forward slashes,
// or "" if the module path could not be resolved. `GKPLUS_SETTINGS` overrides it
// whole, which is what lets a test run against a file of its own.
const std::string &Path();

// The value at `path` as JSON text, "" when it is not there.
std::string GetJson(const char *path);
// `json` must be one complete JSON document. Does **not** save - a caller that
// wants the change on disk calls Save(), which is one write for any number of
// changes.
bool SetJson(const char *path, const char *json);
bool Remove(const char *path);

// Typed convenience over the two above. A value of the wrong type reads as
// absent rather than being coerced: a `true` where a number belongs is a mistake
// in the file, and silently making it 1 would hide it.
bool GetBool(const char *path, bool fallback);
double GetNumber(const char *path, double fallback);
std::string GetString(const char *path, const char *fallback);
bool SetBool(const char *path, bool value);
bool SetNumber(const char *path, double value);
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
// Re-reads from disk, discarding anything set since the last save.
bool Reload();

} // namespace gk::settings
