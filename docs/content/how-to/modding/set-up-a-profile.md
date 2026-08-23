---
title: "How to set up a profile"
description: "Create the directory GkPlus reads its settings, scripts and mod list from, and point the game at it."
weight: 10
audience: ["player", "mod-author"]
---

This guide shows a **player or mod author** how to give GkPlus a profile: the one directory
that holds `settings.json` and the boot and entry scripts it names, and the anchor a relative
mod path resolves against. Everything else in this section assumes you have one.

It assumes `d3d8.dll` is already deployed next to `gl.exe`.

## Use the default profile, or name one

With no environment variable set, the profile is `gkplus` beside `d3d8.dll`, i.e.
`<Gunlok>\gkplus\`. Create it if it is not there:

```
mkdir "<Gunlok>\gkplus"
```

To keep several setups side by side instead, put each in its own directory and select it with
`GKPLUS_PROFILE`:

```
set GKPLUS_PROFILE=D:\gunlok-profiles\modded
```

A relative `GKPLUS_PROFILE` resolves against the **game directory**, not against the shell you
launched from (`src/Profile.cpp` records why). Use an absolute path if there is any doubt.

## Put the three files in it

None of them is required (a profile with nothing in it runs the unmodified game), but this is
what the rest of these guides write into:

```
<profile>\settings.json    persisted settings, shared between GkPlus and every mod
<profile>\boot.mjs         runs before the engine reads its first asset; enables mods
<profile>\main.mjs         the entry module; menus, overlay panels, script-defined levels
```

`settings.json` names the two scripts, and both default to the names above, so you only need
the file if you want to change something:

```json
{
  "core": { "boot": "boot.mjs", "script": "main.mjs" }
}
```

Set either key to `""` to turn that phase off. Point both at one path and it is evaluated once.
Any other top-level key belongs to whoever wrote it; see
[How to persist your own settings](/how-to/modding/persist-your-own-settings/).

For working starting points, copy `examples/boot.mjs` and `examples/main.mjs` out of the
repository. **`examples/main.mjs` imports `levels/` which imports `headers/`**, so copy all three
directories or none, because a module the host cannot find takes the whole entry module with it
and registers nothing at all. `examples/leveltest/main.mjs` is self-contained if you want one
file to start from.

## Launch the game so the profile is seen

Launch `gl.exe` directly with the game directory as the working directory. **Steam does not pass
`GKPLUS_*` environment variables through**, so a game started from the Steam client will ignore
`GKPLUS_PROFILE` and every other switch these guides use:

```
set GKPLUS_PROFILE=D:\gunlok-profiles\modded
"<Gunlok>\gl.exe" -skipfmv
```

`-skipfmv` skips the intro movie. A modal *Run in a window?* requester may appear first; answer
it before expecting anything else to respond.

## Confirm it loaded

- The bottom-left of the main menu reads `GkPlus - <renderer>` where stock Gunlok reads
  `v1.3 DX8`. That is the cheapest proof the DLL loaded and its hooks committed.
- Press **F11** for the ImGui overlay.
- `console.log` from a script reaches the in-game console (backtick to open) **and** the
  debugger. There is no GkPlus log file; host diagnostics go to `OutputDebugString`, so run
  DebugView (or any debugger) to see them. The `d3d8.log` in the game directory belongs to the
  d3d8to9 dependency, not to GkPlus.
- Output from `boot.mjs` reaches the debugger only: at that point the game has no console yet.

If nothing at all happens and the version stamp is missing, the profile is not the problem and
the DLL is not loading. If the stamp is there but your script is not running, check that
`<profile>\main.mjs` exists and is non-empty: an empty entry module loads, evaluates, exports
nothing, and is indistinguishable from success.

## Next

- [Enable and order mods](/how-to/modding/enable-and-order-mods/)
- [Draw an ImGui panel](/how-to/modding/draw-an-imgui-panel/)
- Background on the two boot points is in `script_host_notes.md`.

## Reference and background

- [The profile directory](/reference/data/profile-directory/): every path resolved against
  a profile, and what is read out of it.
- [settings.json](/reference/data/settings-json/): the `core.boot` and `core.script` keys
  used above, and when the file is written.
- [Environment variables](/reference/data/environment-variables/): `GKPLUS_PROFILE` and
  everything else settable at launch.
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why
  `boot.mjs` and `main.mjs` are two files and not one.
