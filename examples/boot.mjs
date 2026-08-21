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
// single asset, and long before main.mjs. That is the point of it: **no mod is in
// front of the engine unless something here enables it**, and this is the last
// instant at which that decision still applies to every file the game will load.
//
// The price is that almost none of the game is up yet. There is no resource
// string table, no console registry and no menus, so `console.register`,
// `menus`, anything that reads a level, and most of the rest of "gk" have
// nothing to talk to. Enable mods, read settings, and leave the rest to main.mjs.
//
// A failure here is worse than a failure in main.mjs: this runs inside the
// engine's own file open, so keep it short and keep it in a try/catch if it does
// anything clever.

// There is no global console; this one comes from "gk" like everything else.
import { console, mods, settings } from "gk";

// --- The mod set is a list somebody wrote down --------------------------------
//
// **Nothing scans for mods, and there is no mods directory.** A mod is named -
// here, or in config this reads - and a mod sitting next to one that is named does
// not load. That is deliberate: "it was enabled because it was in the folder" is
// the whole family of surprises where a renamed-to-disable directory is still
// served, a leftover preview mod quietly changes what the game draws, and a
// baseline run turns out to have been modded all along.
//
// A mod can live anywhere. A path is absolute, or **relative to the profile** -
// which is what makes a list like this portable, since it follows GKPLUS_PROFILE
// instead of naming a place that exists on one machine:
//
//     { "boot": { "mods": ["mods/10-alpha", "D:/gunlok-mods/20-beta.zip"] } }
//
// `boot` is this module's own section - GkPlus reads nothing out of it, because
// the decision belongs to whatever is booting rather than to the host. The
// `mods/` prefix is just this profile's layout; nothing enforces it.

const wanted = settings.boot?.mods ?? [];

// `load()` reads a mod's `metadata` directory and nothing else - it is how you
// find out what something *is*. It throws for a path that is not a mod at all, so
// one bad entry in config should not cost the rest of them.
const loaded = [];
for (const path of wanted) {
  try {
    loaded.push(mods.load(path));
  } catch (e) {
    console.warn(`boot.mjs: cannot load ${path}: ${e}`);
  }
}

// `enable()` declares the active set, in order, and is the only thing that puts a
// file in front of the engine. **The last one wins**: `enable(a, b)` means b
// overrides a where they ship the same file, which is the direction every mod
// manager reads a load order in - and the same direction the collection reports.
const enabled = mods.enable(loaded);

// It **replaces** rather than adds, so:
//
//   mods.enable(a, b)   // b wins
//   mods.enable(b, a)   // reordered: a wins now
//   mods.enable(a)      // b switched off
//   mods.enable()       // the unmodified game - the baseline for any A/B
//
// Nothing accumulates, so there is one place that states the load order rather
// than a sequence of calls whose order is its only record. A path works wherever
// a Mod does, so the config form above is really a one-liner - the loop is only
// there to survive a bad entry:
//
//   mods.enable(wanted);

// --- What a mod says about itself ----------------------------------------------
//
// Every mod is expected to carry a `metadata` directory:
//
//   metadata/info.json          name, author, website, license, version
//   metadata/README.md          what it does, in full
//   metadata/icon_small.png     optional
//   metadata/icon_big.png       optional
//
// A mod missing any of that still loads and still enables - `mod.problems` says
// what was missing, and `mod.name` falls back to the name on disk. So this loop
// reports rather than refuses:

for (const mod of mods) {
  const version = mod.version ? ` ${mod.version}` : "";
  console.log(`  ${mod.order}: ${mod.name}${version}` +
              (mod.author ? ` by ${mod.author}` : "") +
              (mod.problems.length ? `  [${mod.problems.join(", ")}]` : ""));
}

// `console.log` reaches the in-game console, which does not exist yet, and the
// debugger, which does - so these lines are visible in DebugView but not in game.
console.log(`boot.mjs enabled ${enabled} of ${wanted.length} mod(s)`);

// State left here is still around when main.mjs runs: one runtime, one context,
// two modules. Exporting it is the tidy way to hand it over.
export const enabledCount = enabled;

// Both callbacks main.mjs can export work from here too, if a profile is small
// enough to be one file. Point core.boot and core.script at the same path and it
// is evaluated once, not twice.
