#pragma once

#include <quickjs.h>

namespace gk {
// The QuickJS host: one runtime, one context, one entry module.
//
// Boot point is a detour on SetupMenus @ 0x004e95e0, which WinMain calls exactly
// once - after LoadResourceStringTable and the console, and before the first
// frame. That ordering is the whole reason for hooking it rather than booting
// from DllMain or the first frame: a script that adds menu items has to run
// *after* the game has filled its own menus, or every index in the game's
// dispatch table shifts by one.
//
// The entry module is GKPLUS_SCRIPT if that environment variable is set, and
// otherwise `gkplus\main.mjs` next to this DLL (i.e. inside Gunlok's directory).
// A missing file is not an error - it logs the path it looked for and the game
// runs unmodified.
//
// Two exports are called if the module provides them:
//
//   export function setup_menus(menus)  // once, at boot
//   export function draw_gui(ImGui)     // every frame the F11 overlay is open
//
// The game bindings are also importable directly:
//
//   import { menus } from "gk";
//
// ImGui is not. Its calls are only valid between NewFrame and Render, so the
// object exists solely as draw_gui's argument - there is no "ImGui" module to
// import it from somewhere it would not work.
//
// Everything runs on the main thread: boot inside SetupMenus, draw_gui inside
// the overlay's ImGui frame, menu callbacks from the front-end input handler,
// and the job queue drained once per presented frame.
class ScriptSystem {
public:
  ScriptSystem();
  ~ScriptSystem();
};

// Creates the runtime, registers the bindings, runs the entry module and calls
// its setup_menus. The SetupMenus detour calls this once; calling it again is a
// no-op. In the game nothing else should - it is public so the host can be
// driven from a test harness (see the harness recipe in CLAUDE.md), which is the
// only way any of this executes outside Gunlok.
void BootScriptHost();
} // namespace gk
