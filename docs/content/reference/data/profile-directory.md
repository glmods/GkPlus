---
title: "The profile directory"
description: "What GKPLUS_PROFILE names, which files GkPlus reads out of it, and which paths resolve against it."
weight: 40
audience: ["player", "mod-author"]
---

The directory that holds a GkPlus configuration, for players and mod authors. Resolved by
`src/Profile.cpp`.

## Location

| Case | Directory |
|---|---|
| `GKPLUS_PROFILE` unset | `gkplus`, in the directory holding `d3d8.dll` |
| `GKPLUS_PROFILE` absolute | that path |
| `GKPLUS_PROFILE` relative | that path, joined onto the directory holding `d3d8.dll` |

A relative value is **not** resolved against the process's current directory. At the moment the
profile is first needed, the engine has already changed directory into one of its asset
categories.

Paths are held with forward slashes, and trailing slashes are stripped.

`d3d8.dll` sits beside `gl.exe`, so the default profile is `<Gunlok>\gkplus`.

## Contents GkPlus reads

| Path | Read by | Required |
|---|---|---|
| `settings.json` | `src/Settings.cpp` | no; every key has a fallback |
| the file named by `core.boot`, default `boot.mjs` | `src/Script.cpp` | no; an absent file is logged and the phase is skipped |
| the file named by `core.script`, default `main.mjs` | `src/Script.cpp` | no |

`core.boot` and `core.script` are each resolved against this directory, so a value of
`scripts/main.mjs` names `<profile>/scripts/main.mjs`. An absolute value is used as given. A value
of `""` turns that phase off; the default applies only when the key is absent altogether.

Nothing else in the profile directory is read. There is no directory scan of any kind.

## What resolves against the profile

A mod path passed to `mods.load` or `mods.enable` may be absolute, or relative to this directory.
That anchor is what makes a mod list in `settings.json` portable across machines.

`<profile>/mods` is a convention some configurations follow. It is not a path GkPlus knows, it is
not scanned, and a mod placed there does nothing until a script names it.

## Paths that do not resolve against the profile

| Path | Anchor |
|---|---|
| `<gl.exe directory>\gkplus\symbols\<module>.sym` | the host executable's directory, from `GetModuleFileNameA(nullptr, …)`. `prof.symbol_dir` reports it. `src/Profiler.cpp:1666` |
| the mod materialization cache | a per-process directory under `%TEMP%`, removed by `vfs::Shutdown()`. `src/Vfs.h` |
| every path a mod serves to the engine | the Gunlok install directory, `vfs::GameDir()` |

With `GKPLUS_PROFILE` unset the profile and the symbol directory are the same `gkplus` folder;
with it set they are not.

## Related

- [Environment variables](/reference/data/environment-variables/)
- [settings.json](/reference/data/settings-json/)
- [Mod metadata](/reference/data/mod-metadata/)
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why the
  profile names two scripts rather than one.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): why a
  profile has no mods directory, and why a relative mod path still resolves against
  it.
- [How to set up a profile](/how-to/modding/set-up-a-profile/): creating one.
