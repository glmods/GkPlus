---
title: "Why mods are named, never discovered"
description: "There is no mods folder and no scan: a mod loads because a script named its path. What that rules out, and what it costs."
weight: 40
audience: ["mod-author"]
---

This page is for mod authors. Every other modding framework you have used probably had a `mods/`
folder, scanned it, and showed you a list with checkboxes. GkPlus does not, and the omission is
deliberate. This explains the reasoning, the incidents behind it, and the price the design pays.

## What actually happens

`vfs::Initialize` starts PhysicsFS with an **empty search path**. Nothing is mounted. Nothing is
enumerated. There is no blessed directory, and the layer that resolves a file has no idea where mods
live, only what path it was handed (`src/Vfs.h`).

A mod becomes active in exactly one way: a script calls `mods.enable` with its path. In a running
game that script is the profile's boot module, which runs at the last instant before the engine reads
its first asset (see [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/)).

A profile with no boot module runs the completely unmodified game.

## The two steps answer different questions

`mods.load(path)` reads a mod's `metadata/` directory and nothing else. It is how a script (or a
manager UI that does not exist yet) learns what a *named* mod is before deciding anything about it.

`mods.enable(a, b, ...)` declares the active set, in load order, and is the only thing that puts a
file in front of the engine.

`enable` **replaces** the set instead of adding to it, which is the decision most worth
understanding, because a surprising amount follows from it:

- switching a mod off is enabling the rest;
- reordering is enabling the same list in a different order;
- `mods.enable()` with no arguments is an honest un-modded baseline, and it needs no files moved,
  renamed or deleted.

The alternative, an additive `enable`, would mean the active set and its order were never stated
anywhere, only accumulated by a sequence of calls whose ordering became the sole record of the load
order. Replacing puts the whole answer in one place.

Later entries win. That direction falls out of `PHYSFS_mount` prepending: each mount outranks the one
before, so walking the list forward and prepending puts the last-named mod at the front of the search
path. It is also the direction every mod manager displays.

## Why not scan a folder

Because **a listing cannot distinguish a listing from an intention**, and this repository has been
bitten by exactly that twice, both recorded:

- A directory renamed to `cutscene-test.disabled` was still mounted and still served. A rendering
  baseline comparison ran for several rounds against itself before anyone noticed.
- A leftover `gkpbr-preview` mod, written by the texture-generation tooling and never removed,
  quietly replaced an asset for a whole session. The game looked fine, which is the problem.

Both are the same failure: the system inferred an intention from the presence of a file. Once you
accept that inference, every question of the form "was this run actually clean?" becomes
unanswerable from inside the running game, and answerable from outside only by remembering what is in
a folder you have not looked at for a week.

A blessed `mods/` directory that still required explicit enabling would move that indirection one
step away without removing it. You would then have two truths, the folder and the list, and the
interesting bugs live in the gap between them.

The honest trade is this: **discoverability is genuinely lost**. Nothing in GkPlus can present you a
list of available mods, because it does not know that any exist. A tool that writes a mod tree has to
tell its operator to enable it, and both `pbr/` and `lightmap/` do exactly that in their output. If
you want a checkbox list, it has to be built on top: a script that reads a directory and hands the
result to `mods.enable` is perfectly legal, and it is then *your* configuration doing the scanning,
where you can see it.

## The base install is not a mod

Only mod content is mounted. A lookup that misses is what makes the engine read the shipped file
exactly as it always did. The miss *is* the fall-through; there is no fallback bolted on beside it.

An earlier revision presented the install as a `Mod` at the bottom of the load order, which meant
mounting the whole game directory: an index walk over every shipped asset, for behaviour a miss
already provided. `vfs::Load` now refuses the game directory **by name**, which is unusually blunt
and is there because the mistake was made once.

## Where a mod lives is nobody's business

A path handed to `enable` is absolute, or relative to the **profile** directory. That anchor is what
makes a mod list portable: a `settings.json` naming `mods/hi-res.zip` follows `GKPLUS_PROFILE` rather
than hard-coding one machine's layout.

It is specifically *not* the process's current directory, and that is not pedantry. At the moment the
boot module runs, the engine has already `chdir`'d into one of its asset categories, so the working
directory is something like `<Gunlok>\rif`, a value nobody chose and nothing should depend on.

So `<profile>/mods/` is a layout a configuration may adopt. It is not a path GkPlus knows.

## The internal layout comes from the engine

Inside a mod, files mirror the game's own tree: `rif/units/bug.rif` replaces
`<Gunlok>\rif\units\bug.rif`. That mapping was not chosen either. Every loader in the engine calls
`SetCurrentDirectoryToGLDir(<category>)` and then opens a *relative* name across dozens of call
sites, so the only thing a hook can reconstruct is "where in the game tree was this".

`metadata/` is the exception, and it is the one directory that is *not* game content. The engine has
no such category, so nothing an engine open asks for can ever land there. That is why a mod's scripts,
icons and `info.json` live in it, and why every mod may name its script whatever it likes without
colliding with any other mod's in the merged view.

## Tolerance, on purpose

A mod is *expected* to carry `metadata/info.json` with `name`, `author`, `website`, `license`,
`version` and `script`, all strings and all optional, plus a `README.md` and optionally two icons.

A mod that fails any of that **still loads and still enables**. `mod.name` falls back to the entry
name on disk and `mod.problems` lists what was wrong. Being strict would have stopped every mod
predating the metadata contract, including the ones this repository's own tooling writes, instead of
reporting them as incomplete.

Two details there are measurements rather than taste. `version` is a string because an unquoted
`1.3` in JSON reads back as a float and prints as `1.2999999999999998`. And a mod's scripts are all
read at **load** time (every `.mjs` and `.js` under `metadata/`, within bounds), because that is the
only moment a mod can be inspected on its own: `PHYSFS_mount` silently succeeds *without mounting*
when the archive is already in the search path, so an enabled mod cannot be re-opened for inspection
later, and the `PHYSFS_unmount` that would follow such an inspection takes the real mount with it.
Caching the whole set at load is also what makes an ordinary relative `import` work inside a `.zip`,
where neither the importing file nor the imported one has a path on disk.

## What this costs, plainly

The single most likely reason a previously-working install stops picking up its mods is that its
profile has no boot module, or that the boot module's list no longer names the mod. Nothing on
screen says so. There is no "0 mods loaded" warning, because from GkPlus's point of view zero mods is
the normal state of a fresh install.

The instruments that exist against this are diagnostic rather than preventive: `mods.served` counts
opens answered from a mod instead of from disk, and `mods.recent` names the last few, which is the
only way to tell "the mod is mounted" from "the mod is actually being read", since a replaced asset
usually looks identical from outside the game.

Whether that trade is right depends on who you are. For someone maintaining a renderer against a
pixel-exact baseline, "it was enabled because it was in the folder" is a category of bug worth
paying real ergonomics to eliminate. For someone who just wants three texture packs on at once, the
boot script is a chore. The design record in `mod_loading_notes.md` at the repository root makes the
first case and does not much argue the second.

## Where to go next

- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why the mount
  decision happens where it does, and what else runs there.
- [One settings file, many owners](/explanation/one-settings-file-many-owners/): where a mod list
  is usually kept, and how a mod stores settings of its own.
- [How to enable and order mods](/how-to/modding/enable-and-order-mods/): actually enabling
  one, which is where this design meets a keyboard.
- [Mod metadata](/reference/data/mod-metadata/): the contract `mods.load` reads.
