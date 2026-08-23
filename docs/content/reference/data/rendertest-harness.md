---
title: "The rendertest harness"
description: "The seven PowerShell scripts in utils/rendertest: the functions each defines, their parameters, and what they set."
weight: 100
audience: ["developer"]
---

The PowerShell harness that drives Gunlok through the REPL, for developers. `utils/rendertest/`.
Each script is dot-sourced; none is a program with arguments of its own.

```powershell
. .\utils\rendertest\shoot-settled.ps1
```

Dot-sourcing `shoot-settled.ps1` also pulls in `launch-gunlok.ps1` and `shot-gunlok.ps1`.

## `launch-gunlok.ps1`

Launches the game and opens a REPL.

| Function | Parameters | Returns |
|---|---|---|
| `Start-Gunlok` | `-Renderer` (string, default `d3d9`), `-Validation` (switch), `-Fullscreen` (switch), `-Port` (int, default `0`) | the launched process |
| `Focus-Gunlok` | none | none |
| `Repl` | `code` (string), `-Port` (int, default `$global:GunlokReplPort`) | the raw NDJSON reply line |
| `ReplJson` | `code` (string), `-Port` (int, default `$global:GunlokReplPort`) | the parsed reply |

`Start-Gunlok` stops any running `gl.exe` first, sets the environment below, launches
`gl.exe -skipfmv`, answers the modal "Run in a window?" requester (Yes unless `-Fullscreen`),
takes the window foreground, and stores the reported port in `$global:GunlokReplPort`.

| Variable it sets | Value |
|---|---|
| `GKPLUS_REPL_PORT` | `-Port`, `0` by default |
| `GKPLUS_LAUNCHER_HWND` | the handle of a message-only `GkPlusLauncher` window it opens |
| `GKPLUS_RENDERER` | `-Renderer` |
| `GKPLUS_RENDER_UNFOCUSED` | `1` |
| `GKPLUS_VK_VALIDATION` | `1` with `-Validation`, otherwise `0` |

The install path is hardcoded to `C:\Program Files (x86)\Steam\steamapps\common\Gunlok`; there is
no environment override.

## `shot-gunlok.ps1`

| Function | Parameters |
|---|---|
| `Get-GunlokShot` | `Path` (string) |

Captures the game window with `PrintWindow` and flags `PW_CLIENTONLY | PW_RENDERFULLCONTENT` (3).
The script calls `SetProcessDPIAware()` on load, because `gl.exe` is not DPI aware and a client
rect read without it is virtualized.

`PrintWindow` returns a black bitmap under `GKPLUS_RENDERER=d3d8`.

## `shoot-settled.ps1`

| Function | Parameters | Returns |
|---|---|---|
| `Get-Camera` | `-Port` (int, default `$global:GunlokReplPort`) | the camera globals |
| `Dismiss-Briefing` | `-TimeoutSeconds` (int, default 60) | none |
| `Wait-World` | `-TimeoutSeconds` (int, default 90), `-MinDraws` (int, default 100) | none |
| `Wait-CameraRest` | `-TimeoutSeconds` (int, default 150), `-StableReads` (int, default 3) | the settled camera |
| `Shoot-Settled` | `-Renderer`, `-Level`, `-Out` (all mandatory strings), `-Before` (string, default empty), `-NoPause` (switch) | the settled camera |
| `Measure-Frame` | `-Seconds` (int, default 8), `-Healthy` (double, default 100.0) | a frame rate |

`Shoot-Settled` launches, starts `<Level>.gls` / `<Level>.gcs`, dismisses the briefing, waits for
the world pass and then for camera rest, optionally evaluates `-Before`, pauses unless `-NoPause`,
and writes the PNG. A bare `-Out` name lands beside the scripts; an absolute path is used as
given.

`Dismiss-Briefing` presses space until `actors.count` is non-zero.

## `find-draw.ps1`

| Function | Parameters | Returns |
|---|---|---|
| `Get-Pixel` | `Path` (string), `X` (int), `Y` (int) | the pixel colour |
| `Find-Draw` | `-X`, `-Y`, `-Count` (ints), `-Port` (int, default `$global:GunlokReplPort`) | the draw index |

A binary search that hides a *window* of draws rather than truncating a prefix, and waits 900 ms
between setting the range and capturing.

## `harvest-draws.ps1`

Accumulates a per-texture render-state profile inside the game and brings back the totals. Keeps
one socket open rather than reconnecting per call.

| Function | Parameters |
|---|---|
| `Open-Repl` | `Port` (int, default `$global:GunlokReplPort`) |
| `Close-Repl` | none |
| `Rx`, `RxVal` | `code` (string) |
| `RxBlock` | `source` (string) |
| `Seed-Harvest` | none |
| `Poll-Frames` | `Seconds` (double) |
| `Save-Harvest` | `Path` (string) |
| `Camera-Tour` | `Leg` (double, default 2.5) |
| `Dismiss-BriefingHarvesting` | `TimeoutSeconds` (int, default 45) |
| `Wait-Rest` | `TimeoutSeconds` (int, default 45), `Stable` (int, default 3) |
| `Harvest-Level` | `-Level` (mandatory string), `-Leg` (double, default 2.5) |

`Save-Harvest` writes the JSON that `gkpbr observed --from` reads.

## `census-levels.ps1`

| Function | Parameters |
|---|---|
| `Measure-Census` | `-Out` (string, default `census`), `-Levels` (string[], default `$CensusLevels`), `-Port` (int, default 0) |

One launch per level. Writes `<Out>\<level>.txt` per level plus `<Out>\_failures.txt`.
`$CensusLevels` is the twelve numbered campaign levels plus four others; `railway` is deliberately
absent.

## `load-phases.ps1`

Times each level-load phase by reading `ShowLoadingMessage`'s global out of the process with
`ReadProcessMemory`, rather than through the REPL.

| Function | Parameters |
|---|---|
| `Get-LoadPhase` | `Process`, `Slot` |
| `Watch-LoadPhases` | `-Seconds` (int, default 120), `-NoSpace` (switch) |
| `Measure-LoadPhases` | `-Level` (string, default `level01`), `-Seconds` (int, default 120) |

Seven phase messages exist in `glres<lang>.dll`; a given load shows only some of them. The global
is never cleared, so time spent by a phase that sets no message of its own is charged to the
previous one.

## Calls that no longer resolve

The measurement members of the `"gk"` module's `render` namespace moved onto `render.debug`. These
scripts still use the former spelling:

| Script | Call | Current spelling |
|---|---|---|
| `find-draw.ps1` | `render.draw_hide` | `render.debug.draw_hide` |
| `shoot-settled.ps1` | `render.frame_draws()`, `render.vulkan.frames_presented` | `render.debug.frame_draws()`, `render.debug.vulkan.frames_presented` |
| `harvest-draws.ps1` | `render.stats.frames`, `render.frame_draws()` | `render.debug.stats.frames`, `render.debug.frame_draws()` |
| `census-levels.ps1` | `render.normal_census()` | `render.debug.normal_census()` |

Assigning an unknown property to `render` is silently accepted; reading one throws.

The current member list is derivable:

```bash
sed -n '/RenderDebugProps\[\]/,/^};/p' src/JsRender.cpp | grep -oE '"[a-z_]+"'
```

## Related

- [Environment variables](/reference/data/environment-variables/): what `Start-Gunlok` sets.
- [Command-line utilities](/reference/data/cli-utilities/)
- `utils/rendertest/README.md`: the harness's own notes.
- [What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/): how to
  read what this harness produces.
- [Compare two renderers on the same frame](/how-to/development/compare-two-renderers/): the
  procedure it exists for.
