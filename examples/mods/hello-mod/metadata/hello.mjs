// A mod's own script: what `metadata/info.json`'s `script` field names.
//
// The host evaluates this once, when the mod is **enabled** - so under a profile
// whose `boot.mjs` enables it, this top level runs inside `WinMain` before the
// engine has read an asset, and none of the game exists yet: no console, no
// resource strings, no menus. Everything that needs those goes in `setup_menus`,
// which the host calls at the point the front end is up (immediately, if the mod
// was enabled later than that).
//
// The two exports are the same ones a profile's `main.mjs` uses, and a mod's are
// called **alongside** the profile's rather than instead of them.

import { console, settings } from "gk";
import { greeting } from "./lib/greeting.mjs";

// `import.meta.mod` is this mod's own record - the only way a script here can
// know which mod it belongs to, since the file may be inside an archive and has
// no name of its own to go on. Every module the host loads out of a mod gets it,
// helpers included.
const self = import.meta.mod;

// A section of `<profile>\settings.json` keyed by something nobody else will use.
// The store is shared, and a rewrite keeps sections this build has never heard
// of - so a mod may keep its own state in there beside `core`.
settings["hello-mod"] ??= {};
const state = settings["hello-mod"];
state.launches = (state.launches ?? 0) + 1;

// Deferred deliberately: `console` is not usable this early (the game's console
// does not exist yet), and a script that logs at module scope silently drops the
// line. Say it when the front end is up instead.
/** @type {import("gk").SetupMenus} */
export function setup_menus(menus) {
  console.log(`${greeting(self.name)} - launch #${state.launches}`);
  menus.Main.add_item("Hello Mod", () => {
    console.log(`${self.name} ${self.version} by ${self.author}`);
  });
}

// One mod's panel. Each mod's `draw_gui` is called separately and disabled on its
// own if it throws, so this cannot take another mod's overlay down with it - but
// it does share one ImGui frame, so every Begin needs its End.
/** @type {import("gk").DrawGui} */
export function draw_gui(ImGui) {
  if (ImGui.Begin(`${self.name} ${self.version}`)) {
    ImGui.Text(`enabled at load order ${self.order}`);
    ImGui.Text(`served from ${self.path}`);
    ImGui.Text(`launch #${state.launches}`);
  }
  ImGui.End();
}
