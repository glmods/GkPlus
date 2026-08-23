---
title: "Add a subsystem"
description: "Add a new header/source pair under src/, wire it into the build, and decide whether it needs a lifetime object in the Subsystems aggregate."
weight: 40
audience: ["developer"]
---

This guide shows a **developer** how to add a subsystem to GkPlus: a header of decompiled structs
and native declarations plus the `.cpp` that implements them, and any behavioural hook that goes
with it.

## Decide what kind you are writing

Most subsystems are **struct + native API only**: a header of mirrors and free-function
declarations, and a `.cpp` that resolves each offset lazily per call. They install nothing, hold
nothing, and need no lifetime object. `src/GLS.h` / `src/GLS.cpp` is the model the rest follow;
`src/Actors`, `src/Roles`, `src/Map`, `src/Menu`, `src/Console` and the others are the same shape.

Write a `*System` RAII class **only if the subsystem installs a detour that must live for the
process lifetime**. If you are unsure, start without one. Adding it later is a few lines, and a
member in the aggregate that does nothing is an ordering constraint nobody can reason about.

To see the current roster in construction order:

```bash
sed -n '/struct Subsystems/,/};/p' src/entry.cpp
```

## Write the pair

`src/Thing.h` holds everything in `namespace gk`: structs with `static_assert`s (see
[Mirror a game struct](/how-to/development/mirror-a-game-struct/)), then the free declarations.
Carry the mechanism and the measurements behind it in a preamble comment at the top of the header;
that is where this codebase keeps its rationale, and `src/Vfs.h` is the canonical example.

`src/Thing.cpp` holds the bodies. Resolve each game address per call rather than caching a
module-owned pointer:

```cpp
GetObjectAtOffset(funcPtr, 0x004d4b50);
```

`GetBaseAddress()` caches, so this is cheap. Before wrapping any game function, confirm its calling
convention and arity from the epilogue's `RET` form. A wrong one drifts ESP and faults somewhere
unrelated, long after the call. `ghidra_analysis_notes.md` has the rules and the traps.

## Wire it into the build

Add the `.cpp` to the `add_library(GkPlus MODULE …)` source list in `CMakeLists.txt`. There is no
glob; a file not listed there simply is not compiled.

## If it installs a detour

Add one member to `Subsystems` in `src/entry.cpp`, and give it a comment saying what its ordering
constraint is, or that it has none. Every member there carries one, and that comment is what stops
the next person reordering the aggregate by accident. Two positions are load-bearing today:
`FileHookSystem` is first because it patches gl.exe's file imports before `WinMain` loads anything,
and `d3d8::D3D8CaptureSystem` is before `GUISystem` because GUI reads the device global that
`CreateDevice` returned.

Destruction is reverse order, and per `game_defects_notes.md` §4 a fault in an earlier destructor
can prevent the rest of the teardown entirely, so nothing in your destructor may depend on running.

The rules for the hook itself are in
[Install a detour safely](/how-to/development/install-a-detour/); read that before choosing an
anchor, because **two subsystems must never detour the same target**.

## Follow two conventions that pay off later

**Expose the hook body as an ordinary function.** `*System`'s constructor should be the detour and
nothing else, with the work in a free function the way `ScriptSystem` exposes `BootScriptHost` and
`CustomMenuSystem` exposes `ReconcileCustomMenu` / `DispatchCustomMenuClick`. A harness cannot
construct a `*System`, whose constructor hands fake-base offsets to Detours, but it can call those
(`harness_testing_notes.md`).

**Give a behavioural change an off switch.** The convention is a `GKPLUS_*` environment variable
read once in the constructor, where `raw` restores the game's own behaviour;
`GKPLUS_HUD_FIX=raw`, `GKPLUS_WINDOW_PLACEMENT=raw` and `GKPLUS_VERSION_TEXT=raw` all follow it.
`src/HudFix.cpp`'s `ReadHudFixMode()` is the pattern, including warning on an unrecognised value
rather than silently disabling.

## Build and record it

```powershell
cmake --build build 2>&1 | Select-String ': (warning|error):' | Select-String -NotMatch 'invalid-offsetof'
```

Then put the durable part where it will be found: a row in `CLAUDE.md`'s source-file table, and a
`*_notes.md` section if the subsystem has a design worth a page. A name in `CLAUDE.md` that greps to
nothing in `src/` is a defect in that file.

## Related

- [Install a detour safely](/how-to/development/install-a-detour/)
- [Add a binding to the `gk` module](/how-to/development/add-a-gk-binding/): exposing the new API
  to scripts.

## Reference and background

- [Namespace map](/reference/cpp/namespaces/): where a new subsystem's namespace fits.
- [C++ API](/reference/cpp/): the generated reference the new header will appear in, and
  the comment conventions that decide how well.
- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): why
  `Subsystems` is constructed inside one transaction, why its order is load-bearing, and
  what that makes impossible.
