---
title: "How to ship a script with a mod"
description: "Give a mod code of its own (a menu item, an overlay panel, its own settings) instead of only replacing assets."
weight: 50
audience: ["mod-author"]
---

This guide shows a **mod author** how to make a mod run code when it is enabled, rather than
only replacing files.

You need a mod with a `metadata/` directory; see
[How to package a mod](/how-to/modding/package-a-mod/).

## Name the module

In `metadata/info.json`:

```json
{ "name": "Hello Mod", "version": "1.0", "script": "hello.mjs" }
```

The path is relative to `metadata/`, with no `metadata/` prefix, and must name a `.mjs` or `.js`
file the mod ships. A mod that names a file it does not ship reports that in `problems` and
enables anyway.

## Write the module

`metadata/hello.mjs`:

```js
import { console, settings } from "gk";
import { greeting } from "./lib/greeting.mjs";

const self = import.meta.mod;          // this mod's own record

/** @type {import("gk").SetupMenus} */
export function setup_menus(menus) {
  console.log(`${greeting(self.name)} from ${self.path}`);
  menus.Main.add_item(self.name, () => console.log(`${self.name} ${self.version}`));
}

/** @type {import("gk").DrawGui} */
export function draw_gui(ImGui) {
  if (ImGui.Begin(`${self.name} ${self.version}`)) {
    ImGui.Text(`load order ${self.order}`);
  }
  ImGui.End();
}
```

Four things this depends on:

- **The module is evaluated when the mod is enabled.** From a boot script that is inside
  `WinMain`, before the engine has read an asset: there is no console, no resource string table
  and no menus yet. Anything needing those goes in `setup_menus`, which the host calls once the
  front end is up (immediately, if the mod was enabled later than that).
- **`import.meta.mod` is how the module knows which mod it is.** The file may be inside a
  `.zip` and has no path of its own to go on. Every module the host loads out of a mod gets it,
  helpers included.
- **`setup_menus` and `draw_gui` are this mod's own slots**, called alongside the profile's and
  every other mod's. Each is disabled on its own if it throws, but they share one ImGui frame,
  so every `Begin` still needs its `End`.
- **Ordinary relative imports work, including inside a `.zip`.** Every `.mjs`/`.js` under
  `metadata/` is read when the mod loads, which is what makes `./lib/greeting.mjs` resolvable
  where neither file has a path on disk. The scan stops at 64 files, 1 MB or four directories
  deep, and reports that it truncated.

## Keep the mod's own state

`settings` is a shared document keyed by owner, so take a section nobody else will use:

```js
settings["hello-mod"] ??= {};
const state = settings["hello-mod"];
state.launches = (state.launches ?? 0) + 1;
```

See [How to persist your own settings](/how-to/modding/persist-your-own-settings/) for the
rules that section follows.

## Enable it

Nothing runs by being present. A profile's `boot.mjs` still has to name the directory:

```js
mods.enable(mods.load("mods/hello-mod"));
```

`examples/mods/hello-mod` is the complete worked version of everything above.

## What a mod script cannot do yet

There is no teardown. Disabling a mod later in a session does not undo what its script
registered: a menu item it added stays, and its `draw_gui` slot is not reclaimed. Treat the
enable set as decided at boot.

## Check it ran

- `setup_menus` output goes to the in-game console (backtick) and the debugger; anything logged
  at module scope from a boot-time enable reaches the debugger only.
- If the mod's panel vanishes mid-session, its `draw_gui` threw and was disabled for the rest of
  the run; the exception is reported to the console.
- To exercise a panel without launching the game, drive the module under Node with a stub `gk`
  package and a recording ImGui object; `harness_testing_notes.md`, "Driving a script module
  under Node", has the recipe. That is what catches `Begin`/`End` imbalance, which a type check
  cannot.

## Reference and background

- [Mod metadata](/reference/data/mod-metadata/): the `script` field, its validity rules,
  and the scan caps on `metadata/`.
- [JavaScript API](/reference/javascript/): the surface a mod script is written against,
  including [`import.meta.mod`](/api/js/interfaces/gk.ImportMeta.html).
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why a
  mod script evaluated from `boot.mjs` has no console, no resource strings and no menus.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why a
  mod's scripts are read at load time rather than when it is enabled.
