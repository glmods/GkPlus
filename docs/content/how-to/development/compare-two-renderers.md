---
title: "Compare two renderers on the same frame"
description: "Capture aligned frames with utils/rendertest, difference them, and read the number correctly for a fidelity fix or for a new feature."
weight: 90
audience: ["developer"]
---

This guide shows a **developer** how to take two comparable frames out of Gunlok and turn the
difference into an answer.

The harness is `utils/rendertest`: PowerShell that launches the game, drives it over the REPL, and
captures the window. Its README is the list of things that waste a run otherwise; this is the
procedure.

## 0. Patch the harness before you use it

The checked-in scripts predate the split of the measurement surface onto `render.debug`, and still
call the pre-split names:

| file | stale call |
|---|---|
| `shoot-settled.ps1` | `render.frame_draws`, `render.draws`, `render.stats`, `render.vulkan` |
| `find-draw.ps1` | `render.draw_hide` |
| `harvest-draws.ps1` | `render.frame_draws`, `render.stats` |

All of those are on `RenderDebugProps` in `src/JsRender.cpp` now; `RenderProps` carries four
members and nothing else. Those names are simply not on `render` any more, so the reads fail and the
writes go nowhere: `Wait-World` never sees a draw count and the run times out looking like a
renderer that is not drawing. Prefix them with `debug.` before the first run.

## 1. Choose the reference

| `GKPLUS_RENDERER` | what it is |
|---|---|
| `d3d8` | **the ground truth**: Windows' own 32-bit runtime in SysWOW64, with the capture layer and the whole REPL harness still in place. No ImGui overlay, and `PrintWindow` comes back black, so it cannot be photographed |
| `d3d9` | d3d8to9, the default and the second opinion: it says whether a difference is in the translation layer or in the game |
| `vulkan` | the replacement renderer |

Use `d3d9` for anything that needs a picture. Three-way is one extra launch of the same script.

To compare against **stock Gunlok**, with no GkPlus in the process at all, rename `d3d8.dll` aside
and reach a level by hand; Single Player → Training Level → Area 1 is three clicks. The version
stamp is the check that you got stock: bottom left reads `v1.3 DX8` rather than
`GkPlus - <renderer>`. Put the DLL back immediately; a rename left in place makes every later run in
the session silently stock.

## 2. Shoot both frames

```powershell
. .\utils\rendertest\shoot-settled.ps1        # dot-source; it pulls in the other two
Shoot-Settled -Renderer d3d8   -Level level02 -Out ref.png
Shoot-Settled -Renderer vulkan -Level level02 -Out vk.png
```

Use **level02**. It plays no cutscene, and all three renderers settle to the same camera and the
same actor count, which is what makes the comparison mean anything. Use level01 only to reproduce a
level01 number.

`Shoot-Settled` encodes the four things that otherwise produce a wrong answer, and it is worth
knowing which check is which when one of them fails:

- **`actors.count` says the level started; the draw count says the world is on screen.** An overlay
  screen *replaces* the world submit, so the HUD renders over a black frame with the actors alive
  and the renderer healthy, and every A/B taken in that state reads zero differing pixels, which
  looks exactly like the knob under test being inert. `Wait-World` polls `frame_draws` until
  this-frame draws are in the hundreds.
- **`levels.start` lands on the briefing screen and it renders plausibly.** `Dismiss-Briefing`
  presses space *until* `actors.count` is non-zero, because a single press at a fixed delay lands
  before the briefing exists about half the time.
- **Wait for the camera to stop; never a fixed delay.** The renderers run at different frame rates,
  so the same wall-clock delay lands at a different point in an intro sequence. Check the actor
  counts match too: a level that ran further has different world state, which reads exactly like a
  renderer defect.
- **`PrintWindow` needs flag 3, both bits, and the capturing process must call
  `SetProcessDPIAware()`.** With flag 1 alone a swapchain window prints solid black; without
  `SetProcessDPIAware` the bitmap silently keeps the top-left two thirds and the HUD is in the part
  that falls off.

If the run needs the window focused and it is not, a level load sticks at `game.state 18`;
`Focus-Gunlok` does the foreground dance, and `GKPLUS_RENDER_UNFOCUSED=1` keeps it rendering
afterwards.

## 3. Difference them

```bash
python3 -c "from PIL import Image, ImageChops; a=Image.open('ref.png').convert('RGB'); b=Image.open('vk.png').convert('RGB'); d=ImageChops.difference(a,b); px=list(d.getdata()); print(sum(sum(p) for p in px)/(3*len(px)))"
```

Amplify and look at the difference image (`d.point(lambda v: min(255, v*4))`) before theorising
about it. Where a textured surface looks wrong, count distinct channel values before differencing
anything: sixteen values all divisible by 17 is a 4-bit channel replicated to 8 with no filtering,
and 256 is a resampled image.

## 4. Pin the frame if you need a number above the noise

A whole-frame cross-launch difference on level02 has a floor of order 1, because two units
idle-animate and nothing pins their phase. The *same binary* against itself differs by 0.67 when
one shot went through an extra sleep and the other did not. Two ways out, and use both:

- **Pause and set the camera explicitly.** On level02, paused with `screen.toggle_pause()` and the
  camera set from the REPL, d3d9 against d3d9 across two launches is 0.094 whole-frame and 0.00 on
  every HUD region. Read the camera values back out of the session you are comparing against.
- **Restrict to regions with no animating geometry**, where the same comparison reads zero.

Mask out the blinking `ACTIVE PAUSE` text at the bottom left. It has accounted for *every*
difference in a comparison, including between two frames that should have been identical.

Where a feature can be toggled at run time, sharper still: pause, shoot the same frame twice with
one write between them, at a 0.000 floor. That is the only comparison that can honestly return
zero.

To switch every departure off at once, write `render.debug.stock = true` rather than one knob at a
time, since each write costs a frame of drift on anything animating, and there are many of them.
`GKPLUS_VK_STOCK=1` does it from launch. It reads back derived, so it also answers "did I leave
something on?" after a long REPL session.

## 5. Read the number for what it is

**For a fidelity fix, the residual against `d3d8` is the merit figure** and smaller is better with
no judgement involved.

**For a new feature, it is not an answer at all.** The whole purpose of a departure is to make the
game look different, so the residual measures *reach*; a change that made the game uglier would
score the same or higher. Four things it still settles, all worth keeping:

- **`off -> on -> off` is bit-identical.** The proof that the feature is gated.
- **The floor.** Two shots at the same setting, or there is no reading at all.
- **Coverage, and specifically the difference image.** *Where* it landed is checkable even when
  *whether it is good* is not.
- **Blast radius**, the one place a big number is a defect signal: a departure that spreads further
  than its description says is wrong regardless of how it looks.

Then look at the frame, and play it. `vulkan_renderer_plan.md`, "What a residual can and cannot
say", is the rule.

## Find the draw behind a pixel

```powershell
Find-Draw -X 412 -Y 260 -Count 273
```

It binary-searches `render.debug.draw_hide`. Two things in it are load-bearing: it hides a
**window**, never a prefix (a prefix truncates depth and stencil too, so a draw that merely becomes
unoccluded reads as the one that painted the pixel), and it waits 900 ms between setting the range
and capturing, because at 300 ms the shot lags one step and the search converges neatly on the wrong
draw. A draw index does not carry between runs or renderer modes; find the draw by its signature in
`render.debug.frame_draws()` in the mode you are actually in.

## Troubleshooting

**A relaunch differs hugely against everything.** Check the draw count: a
camera that landed somewhere empty reads as tens of draws a frame.

**A paused frame drifted.** It stays pinned for a while, not forever. Re-shoot the baseline before
believing a difference; after a long REPL session a pause had drifted the camera out of the scene
entirely.

**A paused frame cannot find a per-frame-data defect.** Pausing is what makes the comparison
reproducible and also what hides anything needing the allocation pattern to change between frames.
`fx.snow(true)` is the cheap dynamic generator: judge the user-pointer path with it on, then pause
to measure.

**`--target copy` fails with "Permission denied" and no game is running.** Kill `WerFault.exe`; it
holds the crashed process's handle to `d3d8.dll`.

**Nothing works over SSH.** Screenshots cannot cross a session boundary. Put the whole procedure
(launch,
drive, capture) in one script and run it with `schtasks /it`, then read the PNG from the
SSH side. `utils/rendertest/README.md` has the recipe, including why you must never launch
`steam.exe` from session 0.

## Reference and background

- [The rendertest harness](/reference/data/rendertest-harness/): every script, function and
  parameter, including the call sites that have gone stale against the current bindings.
- [Renderer setting keys](/reference/data/render-settings-keys/): the knobs being switched,
  and which of them persist between the two runs being compared.
- [Environment variables](/reference/data/environment-variables/): `GKPLUS_RENDERER` and
  the present-mode and validation switches.
- [What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/): read
  this before quoting a number. The same figure means opposite things for a fidelity
  fix and for a new feature.
- [Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/): why
  `GKPLUS_RENDERER=d3d8` is the reference rather than another option.
