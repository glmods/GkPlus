---
title: "How to start a level without the menus"
description: "Load a level straight from a script or the REPL, with no menu navigation and no briefing."
weight: 140
audience: ["mod-author"]
---

This guide shows a **mod author** how to start a level outright - from an entry module, a menu
item of their own, or the REPL - rather than clicking through the front end.

## Start one

```js
levels.start("Test Arena");                                  // a title, case-insensitive
levels.start({ script: "level02.gls", console: "level02.gcs" });   // a .gls never registered
levels.start(myLevel, { difficulty: "hard" });               // a Level from levels.add
```

`levels.startable` lists everything `start` will take by name: the campaign missions, anything a
script added, and every script-defined level. `difficulty` defaults to `"medium"`.

`levels.quit()` ends the session and returns to the main menu.

Single player only - the sequence forces the game mode, so it throws while a multiplayer session
is live.

## Expect it to return before the level exists

**The load happens at the next turn of the message loop, not in the call.** `start` is therefore
callable from anywhere - the REPL, `draw_gui`, a `message_received` - and returns as soon as the
request is accepted. Anything wrong with the *request* still throws immediately.

Watch for the result rather than assuming it:

```js
levels.start_pending    // true between the request and the load running
game.state
actors.count            // 0 until the world is built
```

A second request while one is pending throws rather than replacing it.

## Two things that will waste a run

- **The briefing screen renders plausibly.** A start lands there, and `actors.count` and
  `game.state` read the same as they do in the level, so neither says which you are looking at.
  Press space until `actors.count` is non-zero.
- **A load sticks unless the game window has been focused at least once.** Bring it to the
  foreground; `GKPLUS_RENDER_UNFOCUSED=1` keeps it rendering afterwards.

## Pick level02 for anything scripted

`level01.gcs` ends in `PLAY CUTSCENE first contact`, so a scripted run lands in a camera sequence
whose progress depends on how fast the machine got there. `level02` plays no cutscene and is the
cheapest in-level state to assert against:

```js
levels.start({ script: "level02.gls", console: "level02.gcs" });
```

Of the campaign levels, `prison`, `level02`, `level03`, `level04` and `level06` issue no
cutscene. Four multiplayer maps - `mplay_bombsite`, `mplay_canyon`, `mplay_dockyard` and
`mplay_tf_oilrig01` - crash the game on load, because they include unit headers whose `.rif` was
never shipped; that is Gunlok's own defect and reproduces without GkPlus.

## Add a level of your own to the list

```js
levels.add_file("mylevel.gls", "mylevel.gcs");    // a .gls on disk
levels.add("Test Arena", arena);                  // a script-defined level
```

Both end up in `startable` and in Choose Level. See
[How to author a script-defined level](/how-to/modding/author-a-script-defined-level/).

## Next

- [Drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/), which is where
  most of these calls get typed.

## Reference and background

- [`levels`](/api/js/variables/gk.gk.levels.html): `start`, `add` and the shipped level
  list, in the generated JavaScript reference.
- [JavaScript API](/reference/javascript/): the rest of the surface a level start reaches.
