---
title: "Mod metadata"
description: "The metadata/ contract a mod is read through: info.json fields, README.md, the icons, the script field, and every problem string."
weight: 50
audience: ["mod-author"]
---

What GkPlus reads out of a mod, for mod authors. Implemented by `src/Vfs.cpp`.

## The shape of a mod

A mod is a plain directory, or an archive PhysicsFS can open; `.zip` is what this tree uses. The
content it serves mirrors the game's own tree: a file at `graphics/bitmaps/water.rim` inside the
mod replaces `<Gunlok>\graphics\bitmaps\water.rim`.

Everything GkPlus itself reads is in one directory:

```
<mod>/
  metadata/
    info.json
    README.md
    icon_small.png
    icon_big.png
    <the module named by `script`, and anything it imports>
  <game content>
```

`metadata/` is the one directory in a mod the engine never opens, so nothing in it can collide
with game content.

Directory and file names inside `metadata/` are matched case-insensitively, with either slash.

The Gunlok install directory itself is refused as a mod.

## `metadata/info.json`

One JSON object. Every field is a string and every field is optional.

| Field | Effect |
|---|---|
| `name` | The display name, `mod.name`. When absent or empty, `mod.name` is the mod's entry name on disk. |
| `author` | `mod.author`. |
| `website` | `mod.website`. |
| `license` | `mod.license`. |
| `version` | `mod.version`. A number rather than a string is refused and reported: an unquoted `1.3` reads back as `1.2999999999999998`. |
| `script` | A JavaScript module inside `metadata/`, evaluated when the mod is enabled. See below. |

Any other member of the object is ignored.

```json
{
  "name": "Hello Mod",
  "author": "you",
  "version": "1.0.0",
  "license": "MIT",
  "script": "hello.mjs"
}
```

## `metadata/README.md`

Served verbatim as `mod.readme`, with CRLF and lone CR normalised to LF. Expected; a mod without
one loads and enables, and reports the omission in `mod.problems`.

## `metadata/icon_small.png`, `metadata/icon_big.png`

Optional. Served verbatim by `mod.icon_small()` / `mod.icon_big()`; `mod.has_icon_small` and
`mod.has_icon_big` say whether one is present. A file whose first eight bytes are not the PNG
signature is discarded and reported.

## The `script` field

The named module is evaluated when the mod is **enabled**, in the same runtime and context as the
profile's own scripts. It may export `setup_menus` and `draw_gui`, which are the mod's own slots
and are called alongside the profile's; either is disabled on its own if it throws.
`import.meta.mod` inside such a module is the `Mod` it belongs to.

The value is a path **relative to `metadata/`**, so a module at `metadata/hello.mjs` is named
`"hello.mjs"`, not `"metadata/hello.mjs"`.

It is refused, with the field cleared and a problem recorded, when it:

| Condition | Problem text |
|---|---|
| is empty | `` `script` is '' - it names nothing `` |
| is absolute, or begins with a drive letter | `it must be relative to metadata/, not an absolute path` |
| is `..`, or contains `../` or `..\` | `it must stay inside metadata/` |
| does not end in `.mjs` or `.js` | `it must name a .mjs or .js file` |
| names a file the mod does not ship | `` `script` names metadata/<path>, which this mod does not ship `` |

A value beginning `metadata/` gets the additional note
`` - `script` is relative to metadata/, so drop that prefix ``.

### The script cache

Every `.mjs` and `.js` under `metadata/` is read at **load** time, not at enable time, so a
relative `import "./lib/util.mjs"` resolves inside an archive. The walk stops at these limits:

| Limit | Value |
|---|---|
| Files | 64 |
| Total bytes | 1 MB |
| Directory depth below `metadata/` | 4 |

Hitting a cap records
`metadata holds more scripts than GkPlus will read (64 files or 1 MB); the rest were skipped`
and stops the walk.

Any single file under `metadata/` larger than 4 MB is refused with `it is larger than 4 MB`.

## Failure is reported, not fatal

A mod that meets none of this contract still loads and still enables. What was wrong lands in
`mod.problems`, in the order it was found; an empty list means the contract is met.

| Problem text | Cause |
|---|---|
| `metadata is not a directory` | Something named `metadata` exists and is not a directory |
| `metadata/info.json is missing` | No `info.json`, or no `metadata/` at all |
| `metadata/README.md is missing` | No `README.md`, or no `metadata/` at all |
| `metadata/info.json is not a JSON object` | The file parsed as something other than one complete JSON object |
| `` metadata/info.json: `<field>` must be a string `` | A field of another JSON type |
| `metadata/info.json: <reason>` | The file could not be read: a filesystem error, `the archive does not report its size`, or `it is larger than 4 MB` |
| `metadata/icon_small.png is not a PNG`, `metadata/icon_big.png is not a PNG` | The signature check failed |
| `metadata/icon_small.png: <reason>`, `metadata/icon_big.png: <reason>` | The file could not be read |
| `metadata/<path>: <reason>` | A script under `metadata/` could not be read |
| `metadata holds more scripts than GkPlus will read (64 files or 1 MB); the rest were skipped` | A script-cache cap was hit |
| the `script` messages above | See The `script` field |

`mods.load` returns nothing at all only when the path cannot be opened as a mod: no such path,
nothing PhysicsFS can open, or the game directory itself.

## Load order

`mods.enable` replaces the enabled set. Position in that list is `mod.order`, counting from 0, and
the **highest** number wins a file conflict. `mod.order` is `-1` while the mod is not enabled.

Enabling nothing is the unmodified game. The base install is not a mod and is not in the load
order; a lookup that no enabled mod satisfies falls through to the shipped file.

A path appearing twice in one `enable` call keeps its last position. Loading the same path twice
returns the same record and re-reads nothing.

## Related

- [The profile directory](/reference/data/profile-directory/): what a relative mod path resolves
  against.
- [settings.json](/reference/data/settings-json/): where a mod keeps its own settings.
- `mod_loading_notes.md`: the design record behind this contract.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why a
  mod that fails this contract still loads, why its scripts are read at load time,
  and why they live under `metadata/`.
- [How to package a mod](/how-to/modding/package-a-mod/) and
  [How to ship a script with a mod](/how-to/modding/ship-a-script-with-a-mod/): writing one.
