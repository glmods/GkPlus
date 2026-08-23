---
title: "settings.json"
description: "The shared settings document: its location, its ownership rule, the core keys, and when it is written."
weight: 20
audience: ["player", "mod-author"]
---

The persistent configuration file, for players and mod authors. Implemented by `src/Settings.cpp`
over `json::Document` (`src/Json.cpp`).

## Location

`<profile>/settings.json`, where `<profile>` is the directory
[`GKPLUS_PROFILE`](/reference/data/environment-variables/) names, by default `gkplus` beside
`d3d8.dll`. `settings.path` in the `"gk"` module reports the resolved path.

The file need not exist. Every key has a fallback, and a launch that changes nothing writes
nothing.

## Ownership

The top level is one object per owner. GkPlus keeps its settings under `core`; a mod takes a
top-level key of its own.

```json
{
  "core":  { "render": { "msaa": 4 } },
  "mymod": { "window": { "x": 40 } }
}
```

A write re-serialises the parsed document, so a top-level section belonging to a mod the running
build has never heard of survives a rewrite unchanged.

`core` is reserved for GkPlus. No other top-level key is reserved, and nothing validates one.

## Paths

Keys are addressed by a dot-separated path (`core.render.ao.radius`) both from C++ and from the
`get`/`set`/`remove` calls on the `settings` object in the `"gk"` module. A key whose own name
contains a dot cannot be addressed by path.

Lookups walk **own properties only**. An inherited name such as `toString` or `constructor` is
never a document member.

A value of the wrong type reads as absent rather than being coerced: a `true` where a number
belongs falls back to the default.

An array is a leaf. Its elements are not addressable by path, and scripts receive it frozen.

## Core keys

| Key | Type | Default | Effect |
|---|---|---|---|
| `core.boot` | string | `boot.mjs` | The boot module, resolved against the profile directory and evaluated at the engine's first intercepted file open. `""` turns the phase off. The default applies only when the key is absent. `src/Script.cpp:84` |
| `core.script` | string | `main.mjs` | The entry module, resolved against the profile directory and evaluated from a detour on `SetupMenus`. `""` turns the phase off. `src/Script.cpp:88` |
| `core.render.*` | bool, number, string | per knob | The renderer knobs. See [Renderer setting keys](/reference/data/render-settings-keys/). |

Pointing `core.boot` and `core.script` at one file evaluates that file once.

No other `core.*` key is read by any build in this tree.

## Keys that are conventions, not contract

`boot.mods` is read by the example boot module (`examples/boot.mjs:46`) as
`settings.boot?.mods ?? []` and passed to `mods.enable`. Nothing in `src/` knows this key. A boot
module may use any key it likes, or none.

## When the file is written

Nothing calls `save()` on a script's behalf.

| Trigger | Condition | Where |
|---|---|---|
| `SaveSettled()` | Something has been written, and either nothing has been written for one second or a run of writes has been going on for fifteen. Called once a frame from the script host's frame hook. | `src/Settings.h` |
| `SaveIfDirty()` | Something has been written since the last save or load. Called first thing in `DllMain(DLL_PROCESS_DETACH)`, ahead of every subsystem destructor. | `src/entry.cpp` |
| `settings.save()` | Called explicitly from a script. | `src/JsSettings.cpp` |

A save writes a temporary file and moves it over the target. It creates the profile directory if
it is not there.

Loading is lazy and happens once, on first access.

## Precedence

An environment variable outranks the file. A renderer knob that carries a companion `GKPLUS_*`
variable is skipped in both directions while that variable is set to anything at all, `0`
included: it is neither restored from the file at startup nor written back to it. The knobs that
carry one are listed under [Renderer setting keys](/reference/data/render-settings-keys/).

`GKPLUS_PROFILE` decides which file is read, so nothing in the file can override it.

## Related

- [Environment variables](/reference/data/environment-variables/)
- [Renderer setting keys](/reference/data/render-settings-keys/)
- [The profile directory](/reference/data/profile-directory/)
- [One settings file, many owners](/explanation/one-settings-file-many-owners/): why this
  is a shared document rather than GkPlus's own file, why `json::Document` exists, and why
  nobody calls `save()`.
- [How to persist your own settings](/how-to/modding/persist-your-own-settings/): writing a
  section of it from a script.
