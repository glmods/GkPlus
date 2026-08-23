---
title: "Build and deploy an optimized DLL"
description: "Produce a RelWithDebInfo d3d8.dll, deploy it into the Gunlok install, and confirm the game is running the build you think it is."
weight: 10
audience: ["developer"]
---

This guide shows a **developer** how to produce and deploy an optimized `d3d8.dll`, and how to
confirm the game picked it up.

Reach for this whenever a number is going to come out of the run: a frame time, a profile, a load
duration, a stall count. The generator is Ninja Multi-Config and its default configuration is
Debug (`/Od /Ob0 /RTC1 -MDd`), which is the right build for hook and correctness work and the wrong
one for any measurement: `CLAUDE.md` records level02 at 22.5 ms/frame under Debug against 9.2 ms
for the same code optimized, with the ranking inside a profile changing too, not just the scale.

## Build and deploy

Configure once per checkout:

```powershell
cmake --preset builtin-vcpkg
```

Then build and deploy, passing `--config` to **both** commands:

```powershell
cmake --build build --config RelWithDebInfo
cmake --build build --target copy --config RelWithDebInfo
```

Omitting `--config` on the copy deploys the Debug DLL over your optimized one, silently. The
`copy` target resolves the install through `cmake/FindSteam.cmake` and writes to
`<Steam>/steamapps/common/Gunlok/`.

If Steam is somewhere that module cannot find, configure with `-DGKPLUS_COPY_DLL=OFF` and copy
`build/RelWithDebInfo/d3d8.dll` into the game directory yourself.

## Clear the file lock first

`--target copy` fails while anything holds the DLL:

- **`gl.exe` running.** Quit the game. A clean exit rewrites `<Gunlok>\scripts\GLkeys.cfg`, which
  is a cheap "did it quit or crash?" signal.
- **`WerFault.exe` running.** Windows Error Reporting keeps the crashed process's handle to
  `d3d8.dll` long after `gl.exe` is gone, and the copy then fails with "Permission denied" for no
  visible reason. Kill both processes.

## Confirm what is actually deployed

Two checks, and both have caught a wrong conclusion in this repo:

1. **Read the timestamp of the deployed file, not the build's.** A copy that failed leaves the
   previous build in place and the next run measures it. `vulkan_renderer_plan.md` records this
   invalidating a whole bisect.
2. **Read the version stamp in the game.** The bottom left of the main menu reads
   `GkPlus - <renderer>` when GkPlus is loaded and its hooks committed, against stock Gunlok's
   `v1.3 DX8`. It is in every screenshot already, so confirm it before believing a negative result.

If a change is supposed to move pixels and the screenshots come back **byte-identical**, suspect
the deploy rather than the change, since a real no-effect change almost never produces that.

## Keep your own diagnostics visible

A translation unit that includes `src/Actors.h` emits around thirty pre-existing, benign
`-Winvalid-offsetof` warnings, which bury everything else. Filter them:

```powershell
cmake --build build 2>&1 | Select-String ': (warning|error):' | Select-String -NotMatch 'invalid-offsetof'
```

## Shader edits

`cmake --build` compiles `src/shaders/*.slang` and derives the shader-ABI asserts; both headers are
generated into the build tree rather than checked in, so **`slangc` and Python 3 are hard build
requirements** and CMake fails with a `FATAL_ERROR` naming the missing one. To ask whether the
generated header is stale without a shader compiler:

```bash
python3 src/gen-shaders.py --check
```

## Troubleshooting

**RelWithDebInfo fails to link with `/failifmismatch` on `_ITERATOR_DEBUG_LEVEL`.** vcpkg ships a
debug and a release build of `detours` and `d3d8to9` that disagree, and a single `find_library`
pins whichever it reaches first for every configuration. `cmake/Finddetours.cmake` and
`cmake/Findd3d8to9.cmake` are config-aware for exactly this reason, so do not simplify them.

**The DLL loads and nothing hooks: no version stamp, no REPL listener, no file hooks.** The
Detours transaction was begun and never committed. Never put a call inside `assert`: `NDEBUG` is
defined in every optimized configuration and discards the argument expression, call and all. The
commit goes through `Commit()` in `src/entry.cpp`, which reports a non-zero result via
`DebugWrite`.

**You cannot find a log file.** There is no GkPlus log. `DebugWrite` is one `OutputDebugString`
call (`src/Core.cpp`), so use DebugView or a debugger's output window. The `d3d8.log` in the game
directory belongs to the vendored d3d8to9, not to us.

**Nothing runs at all from a Windows SSH shell.** `sshd` is a service, so the game launches into
session 0 and exits `-1` having written nothing anywhere. See
`utils/rendertest/README.md`, "From an SSH session".

## Next

- [Profile a frame](/how-to/development/profile-a-frame/): what to do with the optimized build.
- [Compare two renderers on the same frame](/how-to/development/compare-two-renderers/).

## Reference and background

- [Environment variables](/reference/data/environment-variables/): every `GKPLUS_*` switch,
  including the ones that decide which renderer and which profile the deployed DLL uses.
- [C++ API](/reference/cpp/): the generated reference, and the compilation database this
  build already emits for it.
- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): why an
  optimized build that installs no hooks looks exactly like a stock game with a DLL loaded.
