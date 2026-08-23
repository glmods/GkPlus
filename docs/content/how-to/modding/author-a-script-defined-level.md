---
title: "How to author a script-defined level"
description: "Register a level that has no .gls and no .gcs, and put it in Choose Level."
weight: 130
audience: ["mod-author"]
---

This guide shows a **mod author** how to build a level from a script module instead of a
`.gls` + `.gcs` pair. The geometry still comes out of a `.rif`; everything the two script files
used to say is in the module, and nothing is written to disk.

You need a profile with an entry module - see
[How to set up a profile](/how-to/modding/set-up-a-profile/).

## Write the level module

A level module's namespace *is* the description `levels.add` takes, so an ordinary `import` is
the whole mechanism. `<profile>\levels\arena.mjs`:

```js
import { console } from "gk";

/** @type {import("gk").LevelMap} */
export const map = {
  rif: "levels\\level01.rif",     // relative to the RIFs directory
  object: "Land",                 // the object inside it
  bitmap: "bitmaps\\LEVEL01.rim",
  camera_plane: "camhund",
  max_camera_distance: 60,
  shadow_object_rif: "levels\\level01_shadow.rif",
  shadow_object_name: "Land",
};

// The `#include` block a hand-written .gls opens with. Paths are relative to the
// game's Scripts directory; a header included twice still defines its roles once.
export const includes = ["defaults.gsh", "pickups.gsh", "gunlok.gsh"];

/** Definitions this level brings of its own. Runs once per load, before the map
 *  is built. Optional - drop it and the parser is all you use. */
export function define(level) {}

/** Fills the world in: the `use ... for ...` clauses of a .gls. */
export function populate(level) {
  for (const spot of level.locators("Goodie A")) {
    level.spawn("gunlok", 1, spot, { as: "gunlok" });
  }
  level.spawn("archore", 2, { x: 40, y: 4, z: -24 });   // a bare position works too
}

/** The .gcs half: fog, lighting, camera bounds, inventory, triggers. Runs last,
 *  once the world is built and the camera has settled. */
export function setup(level) {
  for (const command of [
    "fogcolour 0 0 0",
    "sunangle 140",
    "actor select gunlok",
    "give and equip gunlok plasma_pistol",
  ]) {
    console.execute(command);
  }
}
```

`rif` and `object` are the only required map fields. `level.locators(name)` is the
`for "<rif object>"` half of a `use` clause - every object of that name in the level rif, already
in world coordinates - and `{as: "gunlok"}` creates the token a later command names the actor by,
exactly as a `.gls` does.

The commands in `setup` run immediately, one after another, rather than one per frame as a `.gcs`
would, so ordering is guaranteed.

## Register it

In `<profile>\main.mjs`:

```js
import { levels } from "gk";
import * as arena from "./levels/arena.mjs";

levels.add("Test Arena", arena);
```

The map is validated **there and then**, through the game's own field checks, so a bad value
throws at startup rather than halfway through a load. The level then appears in Choose Level,
reachable from a "Choose Level" item GkPlus adds to Single Player - the game's own one needs
`-chooselevel`.

Registration order matters only for the list order.

## Define roles without a header file

`includes` uses the game's own parser on files in `Scripts`. To build a definition in JavaScript
instead, use the `make` and `gls` namespaces from `define`, which runs per load because the roles
hash is cleared between levels:

```js
import { roles as bugRoles } from "../headers/bug.mjs";

export function define(level) {
  for (const makeRole of bugRoles) makeRole();
}
```

`examples/headers/bug.mjs` re-implements a shipped `.gsh` this way and is the worked example; the
two approaches mix freely, which is what makes converting one header at a time practical.
Descriptions are in `.gls` units - degrees, seconds, metres - and `make_role_notes.md` lists the
conversions that carry real risk.

## Receive messages during play

Export `message_received` to handle whatever a trigger or another machine put on the script queue
that was not a file name:

```js
export function message_received(msg, level) {
  if (msg.kind === "unit_lost") console.log(`${level.title}: lost ${msg.who}`);
}
```

Arm such a trigger with an object rather than a file name in its `script` field:

```js
triggers.create({ kind: triggers.kind.death, targets: [actors["elint"]],
                  script: { kind: "unit_lost", who: "elint" } });
```

A string there still means "run this `.gcs`", so both forms keep working.

## Test it

Start it without going through the menus - see
[How to start a level without the menus](/how-to/modding/start-a-level-without-the-menus/):

```js
levels.start("Test Arena");
```

If the entry module stops registering anything at all, suspect an import: a module the host
cannot find takes the whole entry module with it, so the symptom is that nothing happens rather
than that one level is missing.

## Next

- `custom_levels_notes.md` records the design; `level_loading_notes.md` §6.5 and §7 are the
  measurements behind it.
- `examples/levels/arena.mjs` is the complete version of the module above.

## Reference and background

- [JavaScript API](/reference/javascript/): the level contracts:
  [`LevelMap`](/api/js/interfaces/gk.gk.LevelMap.html),
  [`LevelBody`](/api/js/interfaces/gk.gk.LevelBody.html) and
  [`Level`](/api/js/interfaces/gk.gk.Level.html).
- [`levels`](/api/js/variables/gk.gk.levels.html) and
  [`make`](/api/js/variables/gk.gk.make.html): registration, and the native constructors
  the `define` hook uses. `make` takes `.gls` units.
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why
  registration belongs in `setup_menus`.
