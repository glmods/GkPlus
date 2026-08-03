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
| `shot-gunlok.ps1` | `Get-GunlokShot`. `PrintWindow` with `PW_RENDERFULLCONTENT`, and `SetProcessDPIAware()` in the capturing process |
| `shoot-settled.ps1` | `Dismiss-Briefing`, `Wait-CameraRest`, `Shoot-Settled`. The whole procedure |
| `find-draw.ps1` | `Find-Draw -X -Y -Count`: binary-searches `render.draw_hide` for the draw that painted a pixel |

Four things they encode, each of which produced a wrong answer first:

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
- **`PrintWindow` needs `PW_RENDERFULLCONTENT`,** or a swapchain window prints solid black — which
  looks exactly like a renderer that is not drawing. And the capturing process must call
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

Kill `WerFault.exe` as well as `gl.exe` before rebuilding — WER holds the crashed process's handle
to `d3d8.dll`, so `--target copy` fails with "Permission denied" long after the game is gone.
