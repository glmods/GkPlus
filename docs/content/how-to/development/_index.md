---
title: "Development"
description: "For working on d3d8.dll itself: builds, tests, struct mirrors, hooks, bindings, crashes, profiling and Ghidra."
weight: 20
---

These assume you can already configure and build the project. If you cannot, start with
[Building GkPlus](/tutorials/building-gkplus/).

## Build and check

- **[Build and deploy an optimized DLL](/how-to/development/build-an-optimized-dll/)**: produce
  a RelWithDebInfo `d3d8.dll`, deploy it, and confirm the game is running the build
  you think it is.
- **[Run the test suites](/how-to/development/run-the-test-suites/)**: every harness in the
  repo, and how to read each result correctly. Two of them report green whatever fails if
  you run them the wrong way.

## Extending the framework

- **[Mirror a game struct and prove its layout](/how-to/development/mirror-a-game-struct/)**:
  model a decompiled struct so a wrong offset is a compile error rather than a fault
  somewhere unrelated.
- **[Add a subsystem](/how-to/development/add-a-subsystem/)**: a new header/source pair,
  wired into the build, and whether it needs a lifetime object.
- **[Install a detour safely](/how-to/development/install-a-detour/)**: pick an anchor
  nothing else hooks, attach and detach it correctly, and confirm it ran.
- **[Add a binding to the gk module](/how-to/development/add-a-gk-binding/)**: expose
  something to scripts and keep `types/` in step with the C++ behind it.

## Investigating

- **[Debug a crash from its WER dump](/how-to/development/debug-a-crash/)**: a symbolized
  stack out of `cdb` and `llvm-symbolizer`, without attaching a debugger.
- **[Profile a frame](/how-to/development/profile-a-frame/)**: where the CPU time goes,
  and how to catch a stutter you cannot reproduce on demand.
- **[Compare two renderers on the same frame](/how-to/development/compare-two-renderers/)**:
  capture aligned frames, difference them, and read the number correctly.
- **[Reverse-engineer a function in Ghidra](/how-to/development/reverse-engineer-in-ghidra/)**:
  explore then consolidate, and land the result in the database, the mirror and the notes
  together.

Player and mod-author tasks are in [modding](/how-to/modding/).
