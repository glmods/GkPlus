---
title: "How to enable and order mods"
description: "Name the mods a profile loads, decide which one wins a file conflict, and switch one off."
weight: 20
audience: ["player", "mod-author"]
---

This guide shows a **player or mod author** how to put mods in front of the engine, in an order
you control, from a profile's boot script.

**Nothing is enabled by being present.** There is no mods directory and no directory scan: a mod
loads because a script named its path, and a mod sitting next to one that is named does not load.
A profile with no boot module runs the unmodified game.

You need a profile; see [How to set up a profile](/how-to/modding/set-up-a-profile/).

## Enable a mod

Put this in `<profile>\boot.mjs`:

```js
import { mods } from "gk";

mods.enable(mods.load("mods/hello-mod"));
```

`load` reads a mod's `metadata` directory and nothing else; `enable` declares the active set and
is the only call that puts a file in front of the engine. A path is absolute, or **relative to
the profile directory**, which is what keeps a list portable between machines. `mods/` above is
just this profile's own layout; a mod can live anywhere.

Do this from `boot.mjs` and not from `main.mjs`. The boot module runs at the engine's first file
open, which is the last moment the decision still applies to every asset the game will load; a
call from later is only seen by files opened after it.

## Set the load order

`enable` takes the set in load order and **the last one wins** a file both mods provide:

```js
const hiRes  = mods.load("mods/hi-res.zip");
const tweaks = mods.load("mods/tweaks");

mods.enable(tweaks, hiRes);   // hiRes wins
mods.enable(hiRes, tweaks);   // reordered: tweaks wins now
```

## Switch one off, or go back to stock

`enable` **replaces** the set rather than adding to it, so there is nothing to un-enable:

```js
mods.enable(tweaks);   // hiRes switched off
mods.enable();         // the unmodified game, the honest baseline for any A/B
```

**Renaming or moving a mod directory is not how you disable one.** A mod named in the boot
script and still present is still served, whatever it is called; a `.disabled` suffix on the
directory has produced "baseline" comparisons that were quietly still modded.

## Drive the list from settings.json

Keep the list out of the script so it can be edited without touching code. `enable` accepts a
`Mod`, a path, or an array of either, which is exactly the shape a config list has:

```js
import { console, mods, settings } from "gk";

const wanted = settings.boot?.mods ?? [];
mods.enable(wanted);
```

```json
{
  "core": { "boot": "boot.mjs", "script": "main.mjs" },
  "boot": { "mods": ["mods/10-tweaks", "mods/20-hi-res.zip"] }
}
```

`boot` is that script's own settings section; GkPlus reads nothing out of it, and any other key
would do. To survive one bad entry, load each path in its own `try`, as `examples/boot.mjs` does.

## Report what loaded

Every mod loads even if its metadata is incomplete, so print what you got rather than assuming:

```js
for (const mod of mods) {
  console.log(`${mod.order}: ${mod.name} ${mod.version}` +
              (mod.problems.length ? `  [${mod.problems.join(", ")}]` : ""));
}
```

The collection **is** the enabled set, weakest first. `mods.loaded` is the other half: everything
`load` has been given, enabled or not, which is the list a manager UI puts checkboxes against.

Those lines go to the debugger only, since the in-game console does not exist that early.

## Confirm a file is actually being served

Enabled is not the same as being read, and a replaced asset looks identical from outside the
game. From the REPL (see [How to drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/)):

```js
mods.served            // how many opens were answered from a mod
mods.recent            // the VFS paths behind the last few, newest first
mods.resolve(mods.game_dir + "Graphics\Ground\gunlok rust.RIM")
```

`mods.served` reads 0 until something actually loads assets, so check it after a level load
rather than at the main menu.

Music and FMV are the one gap: Bink opens those inside its own DLL, out of reach of the
interception.

## Next

- [Package a mod](/how-to/modding/package-a-mod/): the layout and `metadata/` contract a mod
  needs before any of this will find it.
- `mod_loading_notes.md` has the mechanism and the decisions behind it.

## Reference and background

- [Mod metadata](/reference/data/mod-metadata/): what `mods.load` reads, and every problem
  string it can report.
- [The profile directory](/reference/data/profile-directory/): what a relative mod path
  resolves against.
- [`mods`](/api/js/variables/gk.gk.mods.html): `load`, `enable`, `served` and `recent`, in
  the generated JavaScript reference.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why
  there is no mods folder, why `enable` replaces rather than adds, and the two
  incidents that shaped it.
