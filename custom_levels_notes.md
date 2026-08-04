# Script-defined levels, and starting one without the menus

A level with no `.gls` and no `.gcs`, and the programmatic level start that goes with it.
`level_loading_notes.md` sections 6.5 and 7 are the measurements behind both.

### Script-defined levels (`src/CustomLevel.h/cpp`)

A level with **no `.gls` and no `.gcs`** — only the `.rif` still comes off disk.
`import * as arena from "./levels/arena.mjs"; levels.add("Test Arena", arena)` registers one;
the module exports `map` (the GLS map section, field for field), `includes` (the `.gsh` files
its roles come from, or nothing at all if they are built with `gls` instead), `define(level)`,
`populate(level)`, `setup(level)` and `message_received(msg, level)`. The whole thing lands in
Choose Level.

**`add` takes an object, never a path.** A module namespace already *is* a description object
with the map under `map`, so the host has no reason to load the file itself — the script's own
`import` does it, statically, with the editor and the module cache both in play. An inline
description is the same object with the map fields flat instead of nested, which is the only
thing `ResolveDescription` has to tell apart, and it can: no `CustomLevelMap` field is called
`map`.

The three load hooks split the way the two script files split: `define` is the `.gls`'s `#include`
block — it registers roles, and runs per *load* before the map is converted, because the roles
hash is cleared between levels — `populate` is its `use ... for ...` clauses, and `setup` is
the whole `.gcs`.

`message_received` is the fourth slot and the odd one out: it fires **during play**, off the script
queue (see the section above), so `DispatchCustomLevelMessage` keys it on `LevelForCurrentScript()`
rather than on `CurrentCustomLevel()` — the latter is null outside a load. That is also why the
level arrives as the hook's **second** argument: with `levels.current` null while it runs, a
module-level function would otherwise have no way to reach its own `Level`, and putting the message
first keeps the documented signature `message_received(msg)`.

```js
export const map = { rif: "levels\\level01.rif", object: "Land", camera_plane: "camhund" };
export const includes = ["defaults.gsh", "gunlok.gsh"];
export function populate(level) {
  for (const spot of level.locators("Goodie A"))   // the `for "<rif object>"` clause
    level.spawn("Rol_GunLok", 1, spot, { as: "gunlok" });   // ... and the `as` clause
}
export function setup(level) {
  console.execute("sunangle 140");                                  // the .gcs
  triggers.create({ kind: triggers.kind.death, targets: ["elint"],  // data, not a file
                    script: { kind: "unit_lost" } });
}
export function message_received(msg, level) {                      // ... arrives here
  if (msg.kind === "unit_lost") level.send({ kind: "mission_failed" });
}
```

Six facts pin the design; the full reasoning is `level_loading_notes.md` §6.5 and §7.

- **`ConvertParsedObjects` @ 0x004747b0 is the main hook**, not `LoadGLS`: it lets the game
  build and free its own list, with GkPlus only calling the map's `to_game_object` slot
  afterwards. `FreeParsedObjectList` is hooked for the null guard both need — `LoadGLS`
  returns null for a script that defined nothing and the game dereferences it — and
  `LoadGLS` itself only for the no-`includes` case below. The fourth hook,
  `EnterMainMenuScreen`, has nothing to do with loading: it is where these levels reach
  `LevelList`, and it has to be *after* the campaign seed — see "getting into the game's
  LevelList" below.
- **The map section is built by `gls::Create(SectionType::Map)` at registration time**, not
  at load time, so a bad field is reported at boot through the game's own `CheckValue`
  rather than halfway through a level load. The object is kept and re-converted on every
  load; `to_game_object` neither takes nor drops a reference.
- **Nothing a custom level needs is ever written to disk, and its `ScriptFileName` names no
  file.** That name is a virtual `gkplus\<slug>.gls` (the title with every non-alphanumeric
  character folded to `_`), and a level that names `includes` gets the `#include` list as a
  **source text**, parsed from memory through `gls::ParseSource` — the parser takes its input
  through a source object, so a null `FILE *` plus a text buffer is a complete parse. One
  source rather than one `LoadGLS` per include, because the multiple-inclusion guards only
  hold within a single call (`ClearParseSymbolTables` runs per call). The `#include` lines
  still resolve, because the parser opens one with a bare `fopen` against the *current*
  directory, which `LoadLevel` has set to Scripts. It always ends with a filler `shape` —
  its `name` + `file` are the two strings the map already requires — because an empty script
  is the null-list case above.

  With no `includes` there is nothing to parse at all, so `LoadGLS` hands back an empty
  `ParsedObjectList` built the way `ParseGSH` builds its own. **Letting the parse fail
  instead is not an option**: a failed parse poisons the parser for the rest of the process,
  so the next *game* level to load would fail too.

  **The name's shape is dictated by four consumers, not chosen.** A three-letter extension
  and more than four characters, because `ToMap` and `LoadOrBuildSectionAdjacency` overwrite
  the last three in place for `.cut`/`.map` (both optional caches, and they land in
  `<Gunlok>\gkplus`); no double quote, because `PushFileToParserStack` puts the name in a
  `# line` directive that pass 2 re-lexes; and **machine-independent**, which the absolute
  `%TEMP%` path it replaced was not — `SaveGame` serialises it verbatim and
  `ApplyUpdateMessage` `strdup`s it off a network payload on a joining client, so a path
  under one user's profile made a custom-level save unportable and a multiplayer join match
  no registration at all.
- **`setup` hooks `ExecuteAllCommands` @ 0x004d62c0, which has exactly one call site** —
  `LoadLevel` @ 0x004e1e00, step 11, behind the very `freshStart` byte that gates the
  `ExecuteCommandFile(ConsoleFileName)` queueing the `.gcs` at step 7. So reaching the hook
  already means "a fresh level start is at the point its `.gcs` would take effect", and a
  savegame restore skips it for free — no flag of our own, and none of `LoadLevel`'s own
  callers to disambiguate. It runs the callback **before** the original, because the original
  loops until the queue is empty, so anything `setup` queues drains in that same call rather
  than trickling out one per frame through `PumpQueuedConsoleCommand`.
- **The loading level is identified by `ScriptFileName`, not remembered from the menu**, so
  Choose Level, `ADD MISSION`, a savegame restore and a multiplayer client all work alike.
- **Menu 5 needs its own way in.** The game's "Choose Level" item exists only when
  `FlagChooseLevel` @ 0x006b0173 is set, `WinMain` sets it from the `-chooselevel` switch,
  and `SetupMenus` reads it once — long before any script runs. So the first registered
  level appends a "Choose Level" item to Single Player through `CustomMenu`.

**Both paths are verified in a running game** (`level_loading_notes.md` §6.5.1): 140 roles from an
in-memory prelude of six `.gsh` files with nothing on disk, 6 actors from `populate`, fog and sun
from `setup`, and 7 actors on the no-`includes` path where `LoadGLS` never runs. Two naming traps
came out of that run and cost a load each: **a script spawns a role by its GLS `identifier`, not by
the section symbol** (`"gunlok"`, never `"Rol_GunLok"` - `examples/levels/arena.mjs` had it wrong,
which is how it went unnoticed), and **`actor.name` is a reverse token lookup that loses to any
numeric token sharing the id's value** (actor 0 reads as `RES`, not `gunlok`).

Placed objects go through `gk::MapSpawn` after `ToMap` rather than the binding hash at
`ParsedMap+0x1b60` (which would need a forged field-9 `ParsedField`), so a script can spawn
at arbitrary coordinates as well as at rif locators. `gk::LevelRifLocators` supplies the
locator half and is only valid while a load callback is running — `populate` being the window
`ToMap` spawns a `.gls`'s placed objects in, after the roles are registered and before the
camera settles. `CurrentCustomLevel()` marks all three windows, and doubles as the
re-entrancy guard for both hooks, so a callback that parses a `.gls` itself cannot make the
level build twice.

### Starting a level programmatically (`src/Session.h/cpp`)

`levels.start(target, {difficulty})` puts the game in a level with no menus and no briefing
screen. `target` is a `Level` from `levels.add`, a title from `levels.startable`, or
`{script, console}` for a `.gls` that was never registered; `levels.quit()` is the way back.

```js
levels.start("Test Arena", { difficulty: "hard" });          // by title
levels.start({ script: "level02.gls", console: "level02.gcs" });
arena.start();                                                // on the Level itself
```

This exists because driving the front end is not viable: menu activation runs from the
**window procedure**, the front end does not run a frame at all while the window is
unfocused (see `SetFrameWakeupEnabled` in `src/GUI.h`), and `GoToMenu` leaves
`ChosenMenuItem` at `0x100` = "nothing selected", so synthetic keystrokes depend on focus
*and* on a selection the game deliberately clears. Full sequence and its three hazards are
in `level_loading_notes.md` §7.5; the two that cost a crash each:

- **`LeaveFrontEndScreen` @ 0x004e8dd0 is once per front-end session.** Its first two
  branches are null-guarded and the rest is not — it releases a reference on ~40 menu
  sprites and zeroes each global on the way out, so a second call faults. `SpriteScrollUp`
  @ 0x007b7d0c is the "is the front end up?" predicate.
- **`ClearTeamCarryOverState` @ 0x004da230 is `__thiscall`** and takes the global
  `TeamCarryOverState` in ECX. A missing register argument pops exactly as many bytes as a
  correct call, so the `RET`-form sweep is blind to it — this is what the companion
  ECX/EDX-read check is for.

**The load is deferred by one turn of the message loop, and that is structural.** `LoadLevel`
may not run inside the renderer, and the script host's frame callback is driven from inside
`HookedPresentScene` whenever a level is up. So `Session.cpp` queues the request and drains
it from a private `WM_APP` message handled in `HookedWndProc` — `SetMessageLoopCallback` /
`PostMessageLoopWork` in `src/GUI.h`, which is the seam to reuse for anything else that
reloads the world. Everything wrong with the *request* (unknown level, bad difficulty, a
quote in the path, a multiplayer session, a start already pending) still throws at the call;
only the load itself is asynchronous, so poll `game.state` or `actors.count`.

**`levels.startable` is the game's one `LevelList`**, which holds the campaign, anything
`ADD MISSION` added, and — because `AddCustomLevel` registers through the same `AddLevel` —
every script-defined level. One lookup serves all three.

**Registering a level and listing it are separate acts, and the order is load-bearing.**
`EnterMainMenuScreen` seeds its fifteen campaign missions only when `LevelList` is empty
(`CMP dword [0x007b74e0],0` then `JNZ` past all fifteen `AddLevel` calls *and* the block
that seeds `ScriptFileName`/`ConsoleFileName` from the first entry), and `levels.add` runs
during `SetupMenus`, which is earlier. Calling `AddLevel` at registration time therefore
cost the player the whole campaign — Choose Level held only the script-defined levels, menu
7's "new game" launched the first of *those* (it starts `LevelList.sentinel->next`), and the
default script name was never set.

So `CustomLevel.cpp` holds its levels back and appends them from a detour on
`EnterMainMenuScreen`, after the original has had the empty list it insists on
(`ReconcileLevelList`). The result is campaign 0-14 then script-defined levels from 15,
which is also the Choose Level order, and it is stable: `LevelList` is cleared only by
`ShutdownMenuSystem` at process exit and `EnterMainMenuScreen` never touches `Menus[5]`, so
returning to the menu neither re-seeds nor duplicates. A level registered *after* the first
main-menu entry — from the REPL, say — is appended immediately instead. **Idempotency is a
per-level `listed` flag, not a re-read of the list**, because two levels may legitimately
share a title. `CustomLevelByTitle` covers the window in between, so
`levels.start("My Level")` works from the moment `levels.add` returns.
