#pragma once

#include <quickjs.h>

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

// Writes one script-facing line to the game console and to the debugger. Splits
// on newlines, because the console draws one list entry per line.
void Log(const char *text);

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
