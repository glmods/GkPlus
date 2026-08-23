---
title: "Debug a crash from its WER dump"
description: "Turn a Gunlok crash into a symbolized stack with cdb and llvm-symbolizer, without attaching a debugger."
weight: 70
audience: ["developer"]
---

This guide shows a **developer** how to get a stack out of a Gunlok crash.

**Do not attach a live debugger.** It was tried and does not work here: `cdb -pv` (non-invasive)
reports "Could not fetch any stack frames" for the faulting thread, and an invasive `-p` attach
breaks in on its own thread, reports "Unable to get initial context information" for `k`, and killed
the target twice out of two. Attaching anything also makes the game crawl. Windows Error Reporting
already writes a full dump with no configuration, and `cdb -z` on it beats every live attach.

## 1. Confirm it actually crashed

"Not responding" is not evidence of a fault, and neither is high CPU: Gunlok burns a full core at
the front end normally. One recorded fault presented as a hang with the main thread parked in
`ZwWaitForMultipleObjects`, and that wait was WER's own error reporting.

Read the Application Error entry in the Windows event log first, and check the exit code. A clean
exit rewrites `<Gunlok>\scripts\GLkeys.cfg`, so its timestamp separates "quit" from "crashed".

If you were driving the game over the REPL, note that **a crash looks like a socket timeout**, and the
snippet that stopped answering is the one that did it.

## 2. Find the dump

```
%LOCALAPPDATA%\CrashDumps\gl.exe.<pid>.dmp
```

Around 6 MB, written automatically.

## 3. Get `cdb`

`cdb` ships inside the WinDbg MSIX package and **cannot be executed in place**, so copy it out first.
`game_defects_notes.md`, "Debugging Gunlok: what actually works", records the path it was copied
from on this machine. `WinDbgX.exe`'s command-line launch does not work in this environment (it
fails on `notepad.exe` too), and VS Code's `vsdbg` enforces a host licence check, so it is usable by
pressing F5 and not scriptable.

## 4. Get the stack

```
cdb -z %LOCALAPPDATA%\CrashDumps\gl.exe.<pid>.dmp -c ".ecxr; k 40; q"
```

`.ecxr` switches to the exception context; without it you get the reporting thread.

## 5. Symbolize our own frames

cdb will not load the clang-built PDB at all: `bm d3d8!*Foo*` reports "No matching code symbols
found" even with `-y` and `.reload /f`. Take the RVAs the stack prints as `d3d8+0x…` and resolve
them offline against **the same configuration you deployed**:

```
llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address 0x3c25d
```

That turns "the game stopped responding" into
`MakeRoleJs -> MakeRole (MakeRole.cpp:447) -> HierarchyResolveNamedPointPos -> ___ascii_stricmp`
in one command. RVAs of our own DLL move every build, so record **names**, never offsets.

For frames inside `gl.exe`, install the symbol map; see
[Profile a frame](/how-to/development/profile-a-frame/), which uses the same file.

## Three parsing traps

- **`bp d3d8+0x…` silently resolves wrong.** cdb parses `d3d8` as the hex literal `0xD3D8`, because
  all four characters are hex digits. Use `module!symbol` form, or breakpoint the caller in
  `gl.exe`, where `Gl` parses fine as a module name.
- **cdb echoes its entire `-c` list on startup**, so any `.echo MARKER` appears twice. Anchor greps
  with `^MARKER$` or you will parse the echo and conclude a fault occurred on every clean run.
- **Do not breakpoint `Gl+0x6e498`** (`RunGameFrame`'s pump call) during play. It is gated on
  `NumCommandsToExecute != 0`, which stays non-zero for the whole duration of a pending `WAIT`, so
  it fires every frame and makes the game unplayable.

## If there is no dump at all

- **A `/GS` fast-fail (`0xc0000409`) under a debugger writes nothing.** The known instance is
  `PolygonAdjacencyTest` @ `0x0048ecf0` overflowing a three-element buffer during
  `LoadOrBuildSectionAdjacency` on level geometry with degenerate-after-weld triangles
  (`game_defects_notes.md` §5). It is not an access violation, and a debugger suppresses the dump
  entirely, so run without one.
- **`gl.exe` exited `-1` having written nothing anywhere**: no dump, no WER event, no `d3d8.log`
  line, no `-skipfmv` dialog. That is session 0, not a broken build. See
  `utils/rendertest/README.md`, "From an SSH session".

## Before filing it as a GkPlus bug

Rename `d3d8.dll` aside and reproduce with no GkPlus in the process. `game_defects_notes.md`
requires that before anything is filed there, and the version stamp is the check that you actually
got stock: bottom left reads `v1.3 DX8` with GkPlus absent. Put the DLL back the moment you are
done: a rename left in place makes every later run in the session silently stock, and the symptom
is that a fix you just built "does nothing".

## For a crash-detection soak

Run **without** a debugger and read the Application Error log plus the exit code. Undebugged, the
game loads a level and exits in about 22 seconds; under cdb with the parse-warning redirect on it
could not finish the parse in 40.

## Reference and background

- [Design records index](/reference/data/notes-index/): `game_defects_notes.md` is the list
  of crashes that reproduce without GkPlus; check it before blaming a hook.
- [Environment variables](/reference/data/environment-variables/): the `raw` switches that
  turn each behavioural hook off, which is the fastest way to rule one out.
- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): why a fault in
  an early destructor can stop the rest from running at all.
