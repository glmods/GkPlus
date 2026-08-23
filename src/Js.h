#pragma once

#include <quickjs.h>

namespace gk::vfs {
struct Mod;
}

namespace gk::js {
// Registers the "gk" C module in `ctx`. Call once per context, before evaluating
// any script that imports it; returns false if any registration step failed.
//
// The module exports nine live namespace objects - `camera`, `console`,
// `actors`, `roles`, `tokens`, `triggers`, `levels`, `make`, `gls` - both as
// named exports and as properties of the default export, which are the same
// objects:
//
//   import gk, { actors, camera } from "gk";
//   gk.actors === actors;          // true
//   camera.distance = 900;
//
// `menus` is deliberately not among them - see NewMenusNamespace below.
//
// The host that creates the context, loads main.mjs and pumps the job queue is
// src/Script.cpp.
bool RegisterGkModule(JSContext *ctx);

// Builds the `menus` collection. Unlike the namespaces above this is not a "gk"
// export: the host calls this once and passes the result to the entry module's
// setup_menus, which is the only place a script receives it.
//
// Adding a front-end item is a boot-time act - the game's own items must be in
// place first, because OnMenuItemClicked switches on the item *index* - so the
// object is scoped to the callback that runs at the right moment instead of
// being importable from anywhere. A script that wants it later (menus.current,
// menu.open) keeps the argument.
JSValue NewMenusNamespace(JSContext *ctx);

// One `Mod` object, the same kind the `mods` collection hands out. Also not a
// "gk" export, and for a related reason: the host sets it as `import.meta.mod` on
// every module it loads out of a mod, which is the only way a mod's script can
// know which mod it is - the module has no path on disk to read its name out of,
// and the mods collection cannot be searched for "me". Undefined for a null
// record; the wrapper needs no finalizer, records being interned and never freed.
JSValue NewModValue(JSContext *ctx, const vfs::Mod *mod);

// Writes one script-facing line to the game console and to the debugger. Splits
// on newlines, because the console draws one list entry per line.
void Log(const char *text);

// How loud a `console.*` line is. The five spellings used to be one function
// with no severity at all, so nothing downstream - a panel, a log filter, the
// REPL backchannel - could tell an error from a trace.
//
// It is an ordering, and `js::SetLogLevel` drops anything below the current one.
// `Error` is deliberately last so "quieter" is "larger": a script can silence
// its own tracing without silencing what went wrong.
enum class Severity { Debug, Log, Info, Warn, Error };

// Writes one line at `level`. Below the current threshold it does nothing.
void LogAt(Severity level, const char *text);

// The threshold, default `Severity::Debug` (everything). `console.level` is the
// script-facing name.
void SetLogLevel(Severity level);
/// The threshold currently in force.
Severity LogLevel();

// The lowercase name of a level, and the reverse. The reverse returns false for
// an unknown name rather than guessing, so a typo in `console.level` raises
// instead of silently going quiet.
const char *SeverityName(Severity level);
/// The Severity \p name stands for, written to \p out.
/// \return false, leaving \p out untouched, for an unknown name.
bool SeverityFromName(const char *name, Severity *out);

// Reports (and clears) ctx's pending exception through Log, prefixed with
// `where` - "main.mjs", "draw_gui", a menu callback, and so on. Every seam that
// calls into script has to end here rather than let an exception escape into
// game code.
void ReportException(JSContext *ctx, const char *where);

// Drops every script callback the bindings hold - menu item handlers today - and
// makes the matching native registrations inert. Call before JS_FreeContext, or
// the runtime's leak check trips and the menu hooks call into freed memory.
void ReleaseCallbacks(JSContext *ctx);
} // namespace gk::js
