---
title: "How to package a mod"
description: "Lay out a mod so the engine finds its files and GkPlus can say who it is."
weight: 40
audience: ["mod-author"]
---

This guide shows a **mod author** how to lay out a mod: where its files go, and what to put in
`metadata/` so it identifies itself.

## Mirror the game's own directory tree

A mod is a directory or a `.zip` (or any other archive PhysicsFS reads), **anywhere on disk**,
whose contents mirror the game's tree:

```
bigger-bugs.zip
  metadata/info.json
  metadata/README.md
  metadata/icon_small.png        optional
  metadata/icon_big.png          optional
  rif/units/bug.rif              replaces <Gunlok>\rif\units\bug.rif
  scripts/defaults.gsh           replaces <Gunlok>\scripts\defaults.gsh
  graphics/bitmaps/level01.rim
  sound/robots.dat
```

That internal layout is not a convention you can vary: the engine chdirs into one of its
configured directories and then opens a relative name, so "where in the game tree" is the only
thing the interception can reconstruct. In a stock install the categories are `scripts`, `fmv`,
`rif`, `graphics`, `sound`, `fonts` and `Screenshots`. Paths are matched
case-insensitively and either slash works.

`metadata/` is the one directory that is **not** game content: the engine has no such
category, so nothing it opens can ever land there.

Where the mod itself sits is up to you. Nothing scans for mods; a boot script names the path.
`<profile>\mods\` is a convention many profiles follow, not a location GkPlus knows.

## Write `metadata/info.json`

```json
{
  "name": "Bigger Bugs",
  "author": "Your Name",
  "website": "https://example.invalid/bigger-bugs",
  "license": "MIT",
  "version": "1.3",
  "script": "bugs.mjs"
}
```

Every field is a string and every field is optional. Two rules are worth following exactly:

- **Quote the version.** An unquoted `1.3` is a number, and a number is reported as a problem
  rather than used, since a value nobody wrote is worse than an empty one.
- **`script` is relative to `metadata/`**, so write `"bugs.mjs"`, never `"metadata/bugs.mjs"`.
  It must stay inside `metadata/`, must be `.mjs` or `.js`, and must name a file the mod
  actually ships. See [How to ship a script with a mod](/how-to/modding/ship-a-script-with-a-mod/).

Add `metadata/README.md` describing what the mod does; it is served to scripts and UIs as
`mod.readme`, with line endings normalised. Icons must be real PNGs; anything else is reported.

## Load it and read the problems

A mod that fails any part of this **still loads and still enables**: `mod.name` falls back to
the name on disk and everything wrong lands in `mod.problems`. So check the list rather than
assuming silence means correctness:

```js
const mod = mods.load("mods/bigger-bugs.zip");
console.log(mod.name, mod.version, mod.author);
for (const problem of mod.problems) console.warn(problem);
```

`load` throws only when there is nothing loadable at the path at all: no such path, nothing
that opens as an archive, or the game directory itself, which is not a mod.

Then enable it: see [How to enable and order mods](/how-to/modding/enable-and-order-mods/).

## Ship it as a zip

Zip the mod's contents, not the folder containing them: `metadata/` must be at the archive
root. Both forms behave identically, including relative imports between a mod's own scripts.

## Confirm the engine is reading your files

From the REPL, after loading a level:

```js
mods.served      // opens answered from a mod
mods.recent      // the paths behind the last few
```

`mods.recent` is the only way to learn the exact name the engine asked for, since it assembles
that name from a directory category and a string inside a `.gls`.

## Next

- [Replace a texture](/how-to/modding/replace-a-texture/)
- [Ship a script with a mod](/how-to/modding/ship-a-script-with-a-mod/)
- `mod_loading_notes.md` records the metadata contract and the decisions behind it.

## Reference and background

- [Mod metadata](/reference/data/mod-metadata/): the full `info.json` field list, the icon
  and README rules, and every problem string.
- [The profile directory](/reference/data/profile-directory/): where a mod path is resolved
  from.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why
  packaging a mod is not the same as installing it.
