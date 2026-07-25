#pragma once

#include <quickjs.h>

namespace gk::js {
// Registers the "gk" C module in `ctx`. Call once per context, before evaluating
// any script that imports it; returns false if any registration step failed.
//
// The module exports seven live namespace objects - `camera`, `console`,
// `actors`, `roles`, `tokens`, `triggers`, `menus` - both as named exports and
// as properties of the default export, which are the same objects:
//
//   import gk, { actors, camera } from "gk";
//   gk.actors === actors;          // true
//   camera.distance = 900;
//
// The host that creates the context, loads main.mjs and pumps the job queue is
// src/Script.cpp.
bool RegisterGkModule(JSContext *ctx);

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
