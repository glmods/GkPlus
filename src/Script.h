#pragma once

#include <quickjs.h>

namespace gk {
// The QuickJS host: one runtime, up to two modules of the profile's choosing.
// Usually one context too, but the REPL channel (src/Repl.h) adds a second on
// the same runtime when GKPLUS_REPL_PORT is set.
//
// **Both modules are named by the profile's settings.json** (src/Profile.h),
// which is what makes a profile a complete description of a launch rather than
// a bag of settings beside a hardcoded script:
//
//     "core": { "boot": "boot.mjs", "script": "main.mjs" }
//
// Each is resolved against the profile directory, defaults to the name above,
// and may be set to "" to turn that phase off. A missing file is not an error.
//
// --- The two phases ------------------------------------------------------------
//
// `core.script` is the **entry module**, and it boots from a detour on
// SetupMenus @ 0x004e95e0, which WinMain calls exactly once - after
// LoadResourceStringTable and the console, and before the first frame. That
// ordering is the whole reason for hooking it rather than booting from DllMain
// or the first frame: a script that adds menu items has to run *after* the game
// has filled its own menus, or every index in the game's dispatch table shifts
// by one.
//
// `core.boot` is the **boot module**, and it runs far earlier - at
// FileHookSystem's first intercepted open, inside WinMain, before the engine has
// read a single byte of a single asset. That is the only point from which a
// script can decide **which mods are mounted**, because the mod filesystem is
// consulted on that very open (src/Vfs.h mounts nothing on its own). The price
// is that almost nothing of the game is up yet: no resource string table, no
// console registry, no menus. A boot module that reaches for those faults; it
// should mount, configure and hand everything else to the entry module.
//
// Both share one runtime and one context, so a boot module can leave state the
// entry module reads, and pointing both keys at one file evaluates it once.
//
// Two exports are called if either module provides them:
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
// Everything runs on the main thread: both boots inside WinMain, draw_gui inside
// the overlay's ImGui frame, menu callbacks from the front-end input handler,
// and the job queue drained once per presented frame.
class ScriptSystem {
public:
  ScriptSystem();
  ~ScriptSystem();
};

// Phase one: runs `core.boot`, creating the runtime and registering the bindings
// if that key names a file that exists. A profile with no boot module leaves the
// host exactly where it was, so the cost of this on a stock install is one
// GetFileAttributesA. Called from FileHookSystem's first intercepted open -
// nothing else should, and calling it after the host has booted is a no-op.
void BootScriptProfile();

// Phase two: runs `core.script` and calls its setup_menus, creating the runtime
// first if phase one did not. The SetupMenus detour calls this once; calling it
// again is a no-op. In the game nothing else should - it is public so the host
// can be driven from a test harness (see the harness recipe in CLAUDE.md), which
// is the only way any of this executes outside Gunlok.
void BootScriptHost();
} // namespace gk
