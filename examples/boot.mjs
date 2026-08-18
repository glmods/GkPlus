// GkPlus boot module - the `core.boot` half of a profile.
//
// Copy this into a profile directory as `boot.mjs`, beside settings.json and
// main.mjs. A profile is whatever GKPLUS_PROFILE points at; with that variable
// unset it is the `gkplus` folder next to d3d8.dll. `core.boot` in the profile's
// settings.json can name a different file, or "" to skip this phase entirely.
//
// --- When this runs -----------------------------------------------------------
//
// Inside WinMain, at the first file the engine opens - before it has read a
// single asset, and long before main.mjs. That is the point of it: **nothing is
// mounted unless something here mounts it**, and this is the last instant at
// which that decision still applies to every file the game will load.
//
// The price is that almost none of the game is up yet. There is no resource
// string table, no console registry and no menus, so `console.register`,
// `menus`, anything that reads a level, and most of the rest of "gk" have
// nothing to talk to. Mount, read settings, and leave the rest to main.mjs.
//
// A failure here is worse than a failure in main.mjs: this runs inside the
// engine's own file open, so keep it short and keep it in a try/catch if it does
// anything clever.

// There is no global console; this one comes from "gk" like everything else.
import { console, mods } from "gk";

// The whole of what the loader used to do on its own: everything in
// <profile>\mods, ascending by name, so a later name wins a conflict.
const mounted = mods.mount_all();

// Or, for a profile that wants some of the directory rather than all of it -
// mount() prepends, so the *last* one mounted wins:
//
//   const off = new Set(settings.get("mymod.disabled", []));
//   for (const m of mods.discover())
//     if (!off.has(m.name)) mods.mount(m.path);
//
// Or from somewhere else entirely - a shared pool outside the profile:
//
//   mods.mount_all("D:\\gunlok-mods");

// `console.log` reaches the in-game console, which does not exist yet, and the
// debugger, which does - so this line is visible in DebugView but not in game.
console.log(`boot.mjs mounted ${mounted} mod(s) from ${mods.dir}`);

// State left here is still around when main.mjs runs: one runtime, one context,
// two modules. Exporting it is the tidy way to hand it over.
export const mountedCount = mounted;

// Both callbacks main.mjs can export work from here too, if a profile is small
// enough to be one file. Point core.boot and core.script at the same path and it
// is evaluated once, not twice.
