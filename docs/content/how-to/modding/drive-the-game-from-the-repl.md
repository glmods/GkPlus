---
title: "How to drive the game from the REPL"
description: "Open the loopback JavaScript console into a running Gunlok, and use it to check what a mod or script actually did."
weight: 150
audience: ["mod-author"]
---

This guide shows a **mod author** how to evaluate JavaScript inside the running game. It is how
you find out whether a mod is being served, whether a knob took, or what a level actually
spawned - none of which is visible from the screen.

The channel is off unless you ask for it, and binds `127.0.0.1` only: it executes arbitrary code
in the game process.

## Open it

```
set GKPLUS_REPL_PORT=9222
"<Gunlok>\gl.exe" -skipfmv
```

Launch `gl.exe` directly - Steam will not pass the variable through. The listener logs
`repl: listening on 127.0.0.1:9222` to the game console, and `repl.port` reports the same number
from inside.

## Talk to it

One line of NDJSON each way, UTF-8. Anything that speaks TCP will do:

```
printf 'actors.count\n' | nc 127.0.0.1 9222
```

```powershell
$c = [Net.Sockets.TcpClient]::new("127.0.0.1", 9222)
$s = $c.GetStream()
$w = [IO.StreamWriter]::new($s); $w.AutoFlush = $true
$r = [IO.StreamReader]::new($s)
$w.WriteLine("actors.count"); $r.ReadLine()
```

**A line that is not a JSON object with a string `code` is treated as source**, so one-liners
need no quoting ceremony. Replies:

```
<- {"ok": true,  "value": "37", "id": 7}
<- {"ok": false, "error": "TypeError: ...", "stack": "..."}
```

A reply always carries `ok`. A line without one is an unsolicited notification - see the
backchannel below.

Multi-line source rides in the object form, because a newline is the frame delimiter:

```
{"code": "for (const a of actors) console.print(a.id);\nactors.count", "id": 3}
```

Everything the `"gk"` module exports is already a global, plus the default export as `gk`, so
there is nothing to import. Evaluation is global scope: `var` and `function` persist between
lines, and `import` is a syntax error there.

## What to ask it

```js
mods.served                     // opens answered from a mod - 0 until assets load
mods.recent                     // the paths behind the last few
mods.enable()                   // the honest un-modded baseline, no files moved

levels.start({script: "level02.gls", console: "level02.gcs"});
game.state
actors.count
[...actors].filter(a => a.alive).length

render.hdr.enabled = true;
render.debug.lighting_map_report
render.debug.frame_draws([0, 20])

console.print("hello")          // to the game's own console
settings.path
```

## Push events out instead of polling

Something that happens between two polls is invisible to polling. `repl.notify` pushes an
unsolicited line to every connected client:

```js
repl.notify("spawned", { id: actor.id });
```

```
<- {"event": "spawned", "data": {"id": 12}}
```

## Let a launcher pick the port

Do not choose a number: anything can take it between the check and the game's bind, an
ephemeral-range number can hit a reserved block and fail with nothing listening, and a fixed port
can stay unbindable after a crash.

```
set GKPLUS_REPL_PORT=0
set GKPLUS_LAUNCHER_HWND=<hwnd>
```

`0` binds an ephemeral port, and once the listener is *accepting* the number is **posted** to the
message-only window named by `GKPLUS_LAUNCHER_HWND` - which must be of class `GkPlusLauncher` -
as `RegisterWindowMessage("GkPlusReplPort")`, with the **pid in `wParam` and the port in
`lParam`**. Arrival is the readiness signal as well as the number.

Three receiver-side rules, two of which fail silently:

- **Pump messages.** A posted message reaches a window procedure only through `DispatchMessage`.
- **Call `ChangeWindowMessageFilterEx`** for that message id if the launcher runs at a higher
  integrity level than the game, or UIPI drops it with no diagnostic.
- **Check the pid** against the process you spawned, and time out yourself: a post is confirmed
  queued, not delivered.

The port is logged either way. A window message cannot cross a session or a desktop.

`utils/rendertest/launch-gunlok.ps1` is a worked receiver, and the scripts beside it dismiss the
briefing, wait for the camera to come to rest and capture frames.

## When it stops answering

- **A socket timeout usually means the snippet crashed the game.** Check the process rather than
  trusting the timeout; the snippet that stopped answering is the one that did it.
- A modal *Run in a window?* requester at launch blocks everything until it is answered. A
  successful connect is not a readiness signal - wait for a completed round trip.
- A level load sticks unless the window has been focused at least once.
- `levels.start` lands on the briefing screen, which renders plausibly. Press space until
  `actors.count` is non-zero.
- Use `level02` for anything scripted; `level01` ends in a cutscene. See
  [How to start a level without the menus](/how-to/modding/start-a-level-without-the-menus/).
- From an SSH shell you are in session 0, where the game cannot run at all and exits having
  written nothing anywhere. Run the whole procedure in the interactive session with
  `schtasks ... /it`; `utils/rendertest/README.md` has the recipe. The REPL itself is fine across
  that boundary, being localhost TCP.

## Next

- `script_host_notes.md` documents the protocol and its limits in full.
- `utils/rendertest/README.md` is the list of things that otherwise waste a run.

## Reference and background

- [Environment variables](/reference/data/environment-variables/): `GKPLUS_REPL_PORT`,
  `GKPLUS_LAUNCHER_HWND`, and the rest of what a launch can set.
- [`repl`](/api/js/variables/gk.gk.repl.html): the port and the notification backchannel,
  from the script side.
- [The rendertest harness](/reference/data/rendertest-harness/): a worked client, function
  by function.
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why the
  REPL only opens once a script host exists.
