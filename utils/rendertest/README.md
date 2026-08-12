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
| `launch-gunlok.ps1` | `Start-Gunlok`, `Focus-Gunlok`, `Repl`, `GKPort`. Answers the modal `-skipfmv` dialog that blocks *before* the REPL listener opens, takes the port the game reports back (see below), and takes the window foreground — a level load sticks at `game.state 18` otherwise |
| `shot-gunlok.ps1` | `Get-GunlokShot`. `PrintWindow` with flag **3**, and `SetProcessDPIAware()` in the capturing process |
| `shoot-settled.ps1` | `Dismiss-Briefing`, `Wait-World`, `Wait-CameraRest`, `Shoot-Settled`. The whole procedure |
| `find-draw.ps1` | `Find-Draw -X -Y -Count`: binary-searches `render.draw_hide` for the draw that painted a pixel |
| `harvest-draws.ps1` | `Seed-Harvest`, `Harvest-Level`, `Save-Harvest`. Accumulates `render.frame_draws()` across a whole session into a per-texture render-state profile, inside the game, over one kept-open socket. Consumed by `pbr` (`gkpbr.cli observed`); its own header is the list of things that waste a run |

Six things they encode, each of which produced a wrong answer first:

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
- **The harness does not choose the REPL port, and it has to pump messages to hear the one it
  gets.** `Start-Gunlok` sets `GKPLUS_REPL_PORT=0` and hands the game the `HWND` of `GKPort`'s
  message-only window; the game binds an ephemeral port and posts it back — pid in `wParam`, port
  in `lParam` (`script_host_notes.md`, "Launching without choosing a port"). Picking a number here
  was a race — anything could take the port between the check and the game's bind — and it is what
  forced the old "kill every `gl.exe` and hope" preamble, since two runs could not share one
  hardcoded 9222. The message is *posted*, so nothing on the game's side waits on this script; but
  a posted message reaches a window procedure only through `DispatchMessage`, so the wait loop
  calls `[GKPort]::Pump()` rather than only sleeping. A loop that just sleeps never sees the port,
  which will be sitting in the queue the whole time. Timing out is this side's call — the game
  cannot report that nothing is coming. `$global:GunlokReplPort` is where the answer lands, and it
  is what every `-Port` in these scripts now defaults to. `Start-Gunlok -Port 9222` still pins one
  if you want a fixed target to attach `nc` to.

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

## Comparing against **stock** Gunlok, with no GkPlus in the process

The A/B between `d3d8` and `vulkan` tells you whether a defect is our renderer's. It does not tell
you whether it is ours at all — `GKPLUS_RENDERER=d3d8` still has every hook installed. The test for
"is this the game's own bug", which `game_defects_notes.md` requires before anything is filed
there, is to rename `d3d8.dll` aside and reach a level with no REPL at all:

- **The `-skipfmv` dialog still has to be answered**, exactly as `launch-gunlok.ps1` does it
  (`FindDialog` for class `#32770`, `PostMessage WM_COMMAND` with IDYES for windowed). That part of
  the harness works unchanged; only `Repl` and everything built on it are gone.
- **Navigate with the mouse, not the keyboard.** The front end is mouse-driven and the cursor runs
  on Raw Input, so `SetCursorPos` + `mouse_event` reaches it; synthesising clicks at client
  coordinates read off a `Get-GunlokShot` capture works first time. `ClientToScreen` on the main
  window converts, and the clicking process needs `SetProcessDPIAware()` for the same reason the
  capture does.
- **Single Player → Training Level → Area 1 is the cheapest level to reach**, at three clicks and
  ~60 s. "New Game" costs the `first contact` cutscene on top. The training areas have one
  character rather than four, which is enough for anything about the HUD, a unit, or a shader.
- **The version stamp is the check that you actually got stock.** Bottom left reads `v1.3 DX8`
  with GkPlus absent and `GkPlus - <renderer>` with it present, so it is in every screenshot
  already — worth confirming before believing a negative result.
- Put `d3d8.dll` back the moment you are done. A rename left in place makes every later run in the
  session silently stock, and the symptom is that a fix you just built "does nothing".

## From an SSH session: none of this works in session 0

`sshd` is a Windows service, so **every process launched from an SSH shell is in session 0** — its
own named-object namespace, and no desktop. Two things follow, and both look like a broken build
rather than a wrong shell:

- `gl.exe` **exits -1 having touched no file at all**. No crash dump, no WER event, nothing added
  to `d3d8.log`, and the `-skipfmv` dialog never appears, so every script here fails at "the
  dialog never appeared". The cause is `SteamAPI_Init()` unable to see a Steam client in session 1,
  and the message goes only to **stdout**, which `Start-Process` discards unless asked:
  `-RedirectStandardOutput` prints *"Steam must be running to play this game"*. Rule the DLL out in
  one step by renaming `d3d8.dll` aside — if it still exits -1, GkPlus is not involved.
- `PrintWindow` cannot reach a session-1 window from session 0, so **screenshots are impossible**
  however the game was started. The REPL is unaffected: localhost TCP is not session-bound.

The fix is `schtasks /IT`, which runs in the logged-on user's interactive session with no stored
password. Put the *whole* procedure in the script — launch, drive, capture — because the PNG has
to be written by a process inside session 1; then read the file from the SSH side.

```powershell
schtasks /create /tn X /tr "powershell -NoProfile -ExecutionPolicy Bypass -File `"$dir\go.ps1`"" /sc once /st 00:00 /it /f
schtasks /run /tn X          # verified: session 1, WinSta0, the console user
schtasks /delete /tn X /f
```

`/ST` in the past warns and runs anyway under `/run`. A task's console goes nowhere, so
`Start-Transcript` at the top of the script or you get no diagnosis when it fails.

**Do not launch `steam.exe` from session 0.** It starts a *second* client there which takes over
`HKCU\Software\Valve\Steam\ActiveProcess` (leaving `ActiveUser: 0`), displaces the user's
logged-in one, and then swallows every later launch — a session-1 Steam hands off to it and exits.
`steam.exe -shutdown` closes it gracefully in ~16 s; no force-kill needed. Start Steam through a
task instead and it logs itself in when `AutoLoginUser`/`RememberPassword` are set;
`ActiveUser != 0` with `(Get-Process steam).SessionId -eq 1` is the check that it is usable.

## Photographing an ImGui panel

A collapsed `CollapsingHeader` or `TreeNode` photographs as one line, and clicking through a panel
by synthesising mouse events at computed coordinates is not worth the trouble. Wrap the ImGui
object instead, from a **test-only** module that leaves the panel's own code untouched:

```js
const shim = new Proxy(ImGui, {
  get(target, key) {
    const value = target[key];
    if (key === "CollapsingHeader" || key === "TreeNode") {
      return (...args) => { target.SetNextItemOpen(true); return value.apply(target, args); };
    }
    return typeof value === "function" ? value.bind(target) : value;
  },
});
```

Then `ImGui.SetNextWindowSize` before the entry module's `Begin` so the whole thing has room, and
crop the shot afterwards — the overlay renders at the swapchain's scale, so a panel is a few
hundred pixels wide in a 3060x1716 capture and unreadable until cropped and scaled up
(`System.Drawing`, `InterpolationMode = NearestNeighbor`). Remember to take the scaffolding back
out of the installed `main.mjs`.

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
