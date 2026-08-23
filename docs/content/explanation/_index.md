---
title: "Explanation"
description: "Why GkPlus is shaped the way it is. Background, readable away from the keyboard."
weight: 40
---

Background, not instructions. Nothing here tells you to do anything; each page takes one
design decision, says what forced it, and names what was tried and abandoned, because in
this project the rejected alternatives are usually the argument. Where a claim is inference
rather than record, it says so.

Read them in any order. If you want a specific job done, that is a
[how-to guide](/how-to/); if you want an exact spelling, [reference](/reference/).

## The shape of the thing

- **[Why GkPlus is a d3d8.dll](/explanation/why-gkplus-is-a-d3d8-dll/)**: how a modding
  framework for a closed 2000 game ends up shaped like a graphics driver, and what that
  costs and buys. Start here.
- **[How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/)**: why every
  game address is resolved on the call that needs it, why all the hooks go in through one
  Detours transaction, and what that arrangement makes impossible.

## Scripts, mods and settings

- **[Why the script host boots twice](/explanation/why-the-script-host-boots-twice/)**: two
  moments in startup are each the only correct time to run a script, and they are not the
  same moment.
- **[Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/)**:
  there is no mods folder and no scan: a mod loads because a script named its path. What
  that rules out, and what it costs.
- **[One settings file, many owners](/explanation/one-settings-file-many-owners/)**: why
  `settings.json` is a shared document rather than GkPlus's own file, why nobody calls
  `save()`, and why renderer knobs are synchronised once a frame.

## The renderer

- **[Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/)**: a
  second renderer has to intercept somewhere total. The engine's own render queue is
  not, and the measurement that settled it.
- **[What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/)**: the
  same number is a merit figure for a fidelity fix and meaningless for a new feature.
  Why this project got that backwards, and what it does instead.

## Working on a binary

- **[Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/)**: how findings move between
  Ghidra, the C++ mirror and the notes, and why "there is no
  caller" is held to a much higher standard than "here is one".
- **[Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/)**:
  four rosters in one file drifted at once. The fix was not four repairs.

## Not here

Gunlok's own internals (the AI, combat, navigation, the save format, the wire protocol,
the asset formats) are explanation-shaped too, but they document the *game* rather than
GkPlus, and they live in the `*_notes.md` files at the repository root. The
[design records index](/reference/data/notes-index/) is the map.
