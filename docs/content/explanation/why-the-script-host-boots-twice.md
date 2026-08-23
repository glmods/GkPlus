---
title: "Why the script host boots twice"
description: "Two moments in the game's startup are each the only correct time to run a script, and they are not the same moment."
weight: 30
audience: ["mod-author", "developer"]
---

This page is for mod authors who have noticed that a profile has two scripts rather than one, and
for developers wondering why the host is not booted once at a sensible point instead. The short
answer is that there is no single sensible point: two different jobs each have exactly one correct
moment, and those moments are far apart.

## Two deadlines, pulling opposite ways

**Menu registration has a floor.** A script that adds an item to the front end must run *after* the
game has populated its own menus. Gunlok's `OnMenuItemClicked` switches on the item's index, so an
item inserted before the game's own shifts every index in that dispatch by one. The natural anchor is
`SetupMenus` @ `0x004e95e0`, which `WinMain` calls exactly once, after the resource string table and
the console are up and before the first frame.

**Mount selection has a ceiling.** The mod filesystem mounts nothing on its own (see
[Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/)), so
*something* has to name the mods, and it has to do so before the engine reads its first asset.
Once the first texture has been loaded from disk, a mount that would have replaced it is too late in
a way nothing reports.

`SetupMenus` is well past that ceiling. The engine has been opening files since early in `WinMain`.
So the two jobs cannot share an anchor, and the host runs twice:

| Phase | Setting | When |
|---|---|---|
| Boot module | `core.boot` | `FileHookSystem`'s first intercepted file open, inside `WinMain` |
| Entry module | `core.script` | a detour on `SetupMenus`, before the first frame |

Both are named by the profile's `settings.json`, both are resolved against the profile directory,
both default to a filename (`boot.mjs`, `main.mjs`), and either may be set to `""` to turn its phase
off entirely (`src/Script.h`).

## What the early phase costs

The boot module runs at the earliest point from which the mount decision is still meaningful, and
almost nothing of the game exists there. No resource string table, no console registry, no menus.
A boot module that reaches for them faults rather than degrading: `script_host_notes.md` records an
early `gk::Print` faulting on a null `Font *` before the console had been initialised.

That constraint defines the phase. A boot module is meant to
mount, configure, and hand everything that needs a running game to the entry module. Diagnostics
from it reach the debugger through `OutputDebugString` and not the in-game console, because the
in-game console does not exist yet.

There is a matching subtlety for mods. A mod's own script is evaluated *where the mod was enabled*:
from a boot module that means inside `WinMain` under the same constraints, and from anything later
it means immediately. A mod script therefore cannot assume at module scope that the front end is up;
`setup_menus` is called at the `SetupMenus` point for a mod enabled before it and straight away for
one enabled after, so "the game is ready" is something to be told, never something to infer.

## One runtime, one context

Both phases share a single `JSRuntime` and a single `JSContext`. Two consequences are deliberate:
state left by the boot module is visible to the entry module, and pointing both settings keys at the
same file evaluates it once.

There is also a cost that is easy to miss. The runtime is only created early *when a boot module
actually exists*. A profile with no `boot.mjs` leaves the host exactly where it always was, so the
price of this feature on a stock install is one `GetFileAttributesA`.

## Why not one phase

**Boot everything from `DllMain`.** Impossible for either job: the loader lock forbids the work, the
game's CRT is not initialised, and the menus do not exist.

**Boot everything from the first frame.** Too late for both. Assets have been read and the menus have
been populated and drawn.

**Boot everything at `SetupMenus`.** This is what the host did originally, and it is still where the
entry module runs. It simply cannot decide mounts, because the decision has already been made for it
by the engine's own reads.

**Boot everything at the first file open.** Early enough for mounts, useless for menus, and hostile to
almost every other thing a script wants to do.

So each phase is the only point at which its job is possible. What is a convenience, and worth
stating as such, is that they were made *nameable and separately disableable*, which turns a
profile into a complete description of a launch instead of a settings file next to a hardcoded
script.

## Why the boot module shares an anchor

The boot module does not have a detour of its own. It runs from `EnsureFirstOpen` in
`src/FileHooks.cpp`, alongside the DDS codec registration and the restore of stored renderer
settings, because two subsystems detouring one target inside a single Detours transaction do not
chain, and one of them silently stops working (see
[How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/)). Three unrelated things
share one anchor because the mechanism permits only one hook there, not because they belong together.

## Where the callbacks live

Each module may export two functions:

- `setup_menus(menus)`: once, at boot
- `draw_gui(imgui)`: every frame the overlay is open

ImGui is deliberately not importable. Its calls are only valid between `NewFrame` and `Render`, so
the object exists solely as `draw_gui`'s argument; there is no module from which to import it into a
place it would not work. The same reasoning keeps `menus` out of the `"gk"` module's exports: adding
a front-end item is a boot-time act, so the object is scoped to the callback that runs at the right
moment.

Each enabled mod gets its *own* pair of slots instead of sharing the profile's. There is one
profile, so "whichever module was loaded last wins" is a coherent rule for it; there are many mods,
and a mod silently replacing another mod's overlay panel by loading second is not a rule at all.
Each slot is disabled independently if it throws, which matters because an exception inside
`draw_gui`, or an unbalanced ImGui `Begin`/`End`, takes that panel out for the rest of the session.

## Limits and open questions

**Failure is quiet in one specific way.** A missing script logs the path it looked for. An *empty*
script loads, evaluates, exports nothing, and is indistinguishable from a working host that has
nothing to do. `script_host_notes.md` puts this plainly: check the file is non-empty before debugging
the bindings.

**A missing import takes the whole module with it.** An entry module that imports a file the host
cannot find registers nothing at all, not one menu item, instead of losing only the feature that
import served. The shipped `examples/main.mjs` imports three other files, which is why it must be
installed with its directories rather than copied alone.

**There is no teardown for a mod's script.** Disabling a mod stops serving its files but leaves
whatever its script registered (menu items, callbacks) in place. This is recorded as an open gap in
the repository's issue tracker and not solved, and it is the least satisfying part of the mod
script contract as it stands.

**The two-phase split is a source of confusion by construction**, and it is fair to say so: a mod
author reading only the entry-module documentation has no reason to expect that the thing which
enables mods runs somewhere else entirely, under rules that forbid most of the API. The design record
is `script_host_notes.md`, and the two boot points are the first thing it discusses.

## Where to go next

- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): what
  the boot module exists to do.
- [One settings file, many owners](/explanation/one-settings-file-many-owners/): where `core.boot`
  and `core.script` are read from, and who else writes to that file.
- [Your first GkPlus script](/tutorials/your-first-script/): the entry module, written out
  in full; and [How to set up a profile](/how-to/modding/set-up-a-profile/) for the boot
  module beside it.
