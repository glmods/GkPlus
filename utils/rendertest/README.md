# Renderer comparison harness

PowerShell for driving Gunlok through the REPL and capturing frames, so the Vulkan renderer can be
compared against the original D3D8. `vulkan_renderer_plan.md` is what to do with the results;
this is the mechanics.

These lived in a session scratchpad and were rebuilt from the notes twice before being checked in.
Each one exists because something about driving this game wastes a run otherwise.

```powershell
. .\utils\rendertest\shoot-settled.ps1        # dot-source; it pulls in the other two
Shoot-Settled -Renderer d3d8   -Level level02 -Out ref.png
Shoot-Settled -Renderer vulkan -Level level02 -Out vk.png
```

| script | what it is for |
|---|---|
| `launch-gunlok.ps1` | `Start-Gunlok`, `Focus-Gunlok`, `Repl`. Answers the modal `-skipfmv` dialog that blocks *before* the REPL listener opens, waits for the port, and takes the window foreground — a level load sticks at `game.state 18` otherwise |
| `shot-gunlok.ps1` | `Get-GunlokShot`. `PrintWindow` with flag **3**, and `SetProcessDPIAware()` in the capturing process |
| `shoot-settled.ps1` | `Dismiss-Briefing`, `Wait-World`, `Wait-CameraRest`, `Shoot-Settled`. The whole procedure |
| `find-draw.ps1` | `Find-Draw -X -Y -Count`: binary-searches `render.draw_hide` for the draw that painted a pixel |
| `harvest-draws.ps1` | `Seed-Harvest`, `Harvest-Level`, `Save-Harvest`. Accumulates `render.frame_draws()` across a whole session into a per-texture render-state profile, inside the game, over one kept-open socket. Consumed by `pbr` (`gkpbr.cli observed`); its own header is the list of things that waste a run |

Five things they encode, each of which produced a wrong answer first:

- **`actors.count` says the level started; the draw count says the world is on screen.** They are
  not the same test. An overlay screen *replaces* the world submit rather than drawing over it
  (`rendering_notes.md` §5), so the HUD, the objectives text and the pause indicator render over a
  **black frame** — with 178 actors alive and the renderer at a healthy 16.6 ms/frame. Every A/B
  taken in that state reads zero differing pixels, which looks exactly like the knob under test
  being inert; it cost a session concluding that about a build where it was not
  (`vulkan_renderer_notes.md` §4.67). `Wait-World` polls until this-frame draws are in the hundreds
  — 16 against a 273 peak is the tell. It reads that count from **`render.frame_draws`**, which is
  mirror-side: `render.draws` is the *Vulkan* renderer's own counter and reads 0 under `-Renderer
  d3d8`, so polling it made every reference capture impossible (§4.70).

- **Wait for the camera to stop, never a fixed delay.** The renderers run at different frame
  rates, so the same wall-clock delay lands at a different point in an intro sequence. junkyard at
  20 s gave a close-up under Vulkan and a wide shot under d3d8, with the camera globals reading
  *identically* a minute later because both settle to the same place. Camera-rest is
  renderer-independent. Check `actors.count` matches too — a level that ran further has different
  world state, which reads exactly like a renderer defect.
- **`levels.start` lands on the briefing screen and it renders plausibly** — a character portrait
  over rock. A run that misses it looks like a broken renderer rather than a game waiting for a
  keypress (`render.draws` sits at ~4 draws a frame). `Dismiss-Briefing` presses space *until
  `actors.count` is non-zero*, because when the briefing appears depends on how long the load took
  and a single press at a fixed delay lands before it exists about half the time.
- **`PrintWindow` needs flag 3, and both bits of it.** Without `PW_RENDERFULLCONTENT` (2) a
  swapchain window prints solid black, which looks exactly like a renderer that is not drawing.
  Without `PW_CLIENTONLY` (1) the whole window is rendered — title bar and border — into a bitmap
  sized from `GetClientRect`, so the picture is pushed down and right and the bottom and right
  edges of the frame fall off it. The script passed 2 alone until §4.47 and nobody noticed,
  because the result still looks like a screenshot of a game in a window.
  And the capturing process must call
  `SetProcessDPIAware()`: gl.exe is not DPI aware, so `GetClientRect` reports 418x312 against a
  real 628x468 swapchain and the bitmap silently keeps the top-left two thirds. The HUD is in the
  upper right and went missing from an entire session's screenshots.
- **Bisect by hiding a window, never by truncating a prefix.** A prefix truncates the depth and
  stencil buffers along with the draw list, so a draw that merely becomes unoccluded reads as the
  one that painted the pixel. `find-draw.ps1` also waits 900 ms between setting the range and
  capturing — at 300 ms the shot lags one step behind and the search converges neatly on the wrong
  draw.

`level02` is the level to shoot: no cutscene, and all three renderers settle to bit-identical
camera values and the same 178 actors. Use `level01` only to reproduce a level01 number.

**Looking at a generated texture** is a second use of `Shoot-Settled` and its recipe lives with
the generator: `gkpbr.cli preview` packs a PBR map as a `.RIM` into a throwaway mod so the engine
loads it in place of the sheet it came from, and `Shoot-Settled -Renderer d3d9 -Level level02`
photographs the result. See `pbr/README.md`, "Putting a map on screen" — including why
`render.material_override` cannot do it, and the one thing that wastes a run here: **a sheet's
draw count is not its screen area.** The most-drawn ground texture in level02 covered 234 pixels
at the settled camera; the one that covered a quarter of the frame was fourth on the list.

Kill `WerFault.exe` as well as `gl.exe` before rebuilding — WER holds the crashed process's handle
to `d3d8.dll`, so `--target copy` fails with "Permission denied" long after the game is gone.

## Reaching a screen that is not the world

Some things worth comparing are behind a key press rather than behind the REPL, and the upgrade
screen — the one §4.47 is about — is the awkward case. There is no `screen.*` command for it, so
it is `keybd_event` into the focused window, and three things have to be right:

- **Send the scancode as well as the virtual key.** Input arrives on `WM_KEYDOWN` and is mapped
  VK → DIK (`input_notes.md`), and the bindings are in `<Gunlok>\scripts\GLkeys.cfg` as
  name/scancode/modifier triples — DIK values, so 22 is `U` and 2 is `1`.
- **Select a character first.** `U` alone does nothing; the screen is per unit. `1` is
  "Select Gunlok" (scancode 2), then `U` is "Upgrade Screen" (scancode 22).
- **Do not pause.** `screen.toggle_pause()` blocks the screen from opening at all, so this is the
  one comparison that cannot use the pin-the-frame procedure. Take consecutive shots inside one
  session and use a run-time toggle for the A/B instead; two shots of that screen with nothing
  changed differ by 0.043 MAD, which is a good enough floor.

`Escape` (VK 0x1B, scancode 1) opening the in-game menu is the cheap check that key input is
reaching the game at all, before concluding that a particular binding is wrong.
