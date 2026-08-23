---
title: "Why GkPlus is a d3d8.dll"
description: "How a modding framework for a closed 2000 game ends up shaped like a graphics driver, and what that shape costs and buys."
weight: 10
audience: ["player", "mod-author", "developer"]
---

This page is for anyone (player, mod author or developer) who has noticed that the whole of
GkPlus arrives as a single file called `d3d8.dll` and wondered why a modding framework is named
after a graphics API. It explains the choice and the consequences that follow from it. Nothing here
is needed to *use* GkPlus; it is here so that the rest of the system stops looking arbitrary.

## The problem the shape solves

Gunlok shipped in 2000 as a retail Windows binary. There is no source, no plugin interface, no
scripting hook, no documented extension point of any kind. Everything GkPlus does (replacing an
asset, adding a menu item, running JavaScript, drawing the game with Vulkan) requires being *inside*
`gl.exe`'s process, with the ability to read and write its memory and to intercept its calls.

So the first design question was about the route in: how does our code get into that process, and
how early?

## The mechanism

`gl.exe` imports `d3d8.dll`, because it is a Direct3D 8 game. When Windows loads an executable it
resolves each imported DLL by name, and for a name that is not one of the system's protected
`KnownDLLs` it searches the application's own directory before the system directory. `d3d8.dll` is
not a `KnownDLL`. A file of that name sitting next to `gl.exe` is therefore the one the loader binds,
and its `DllMain` runs before `WinMain` does.

GkPlus is that file. Its entire linker export list is one symbol
(`src/exports.def`):

```
LIBRARY d3d8
EXPORTS
Direct3DCreate8
```

Everything the game asks of Direct3D goes through that one function, and GkPlus forwards it: by
default to the vendored `d3d8to9` translation layer, and with `GKPLUS_RENDERER=d3d8` to Windows'
own 32-bit runtime, which is still shipped in SysWOW64. Loading the original has to name it by full
system path, because a bare `LoadLibraryA("d3d8.dll")` from inside a module *called* `d3d8.dll`
resolves to the module already loaded (itself), and `Direct3DCreate8` would recurse into its own
hook (`src/D3D8Capture.cpp`, `LoadSystemDirect3DCreate8`).

A proxy DLL is an old technique and there is nothing clever in it. What is
worth attention is how much of the rest of the design is downstream of it.

## What falls out of the choice

**Installation is a file copy, and uninstallation is deleting it.** No launcher, no service, no
registry keys, no patched executable, and not one byte of the shipped install
changes. That matters more than convenience for a game distributed through Steam, where a modified
`gl.exe` is one file-verification pass away from being reverted, and where a mod that edits assets
in place is indistinguishable from corruption.

**GkPlus runs before the game does.** `DllMain` is called during process initialisation, so the
subsystems are constructed and their hooks installed before `WinMain` executes a single instruction.
This is the property that makes the mod filesystem possible at all: `FileHookSystem` patches
`gl.exe`'s import table first in construction order, precisely so that the assets loaded during
`WinMain`, before any other hook could fire, already go through it (`src/entry.cpp`).

**Every load-time dependency GkPlus adds becomes a requirement for launching the game.** This is
the sharpest cost of the shape, and it is why `vulkan-1.dll` is linked with `/DELAYLOAD` and probed
with a `LoadLibrary` before use: if it were a normal import, a machine with no Vulkan runtime could
not start Gunlok at all, in order to support a rendering path most runs never take. A machine
without Vulkan is not an error condition here; the game simply keeps drawing through `d3d8to9`.

**`DllMain` runs under the loader lock**, which forbids `LoadLibrary` and makes file I/O a bad idea,
and `gl.exe`'s statically linked CRT has not been initialised yet. Several things that would
naturally live at load therefore cannot: the DDS image codec registration, the restore of stored
renderer settings, and the profile's boot script all hang off the first file the engine opens
instead (`src/FileHooks.cpp`). That anchor exists because of where `DllMain` sits, not by preference.

## What is actually inside

Very little of GkPlus is about Direct3D. The DLL carries a QuickJS host running the profile's
JavaScript, a PhysicsFS-backed mod filesystem, a decompiled mirror of the game's own structures, a
CPU profiler, a loopback REPL, and a bindless Vulkan renderer. The `d3d8` name is the door into
the process,
and no more than that.

Two things do genuinely follow the name. The version stamp in the corner of the main menu reads
`GkPlus - <renderer>` and reports the *resolved* mode, so a `GKPLUS_RENDERER=d3d8` that could not
load Windows' runtime says `d3d9` rather than what was asked for. And the seam the Vulkan renderer
sits behind is that same single export. That is a measured result, discussed in
[Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/).

## The alternatives, and how confident this account is

The repository records the *mechanics* of the proxy thoroughly and does not record a comparison
against other injection strategies. What follows is therefore reconstruction rather than history,
and should be read as such.

A **launcher that injects a DLL** after starting the process is the usual alternative. It would have
to win a race against the game's own startup to be as early as an import-table entry can be, and
"as early as possible" turns out to be load-bearing here, since the mount decision must precede the
engine's first asset read. It also means shipping and running a second executable.

**Patching `gl.exe`** would mean distributing a modified copy of a commercial binary, and would
invalidate every address in the reverse-engineering notes, which are all stated as offsets against
the shipped image. `GetBaseAddress()` computes the load base by subtracting a constant from the
host's entry point (`src/Core.cpp`); that arithmetic assumes the shipped executable and nothing else.

An **ASI loader or generic plugin host** is a proxy DLL with a plugin protocol on top. For a project
with exactly one plugin, that is the same mechanism plus indirection.

## What the shape rules out

Because GkPlus resolves every game address from the host executable's entry point, **nothing in
`src/` can run outside Gunlok**: a standalone process has a different entry point and every call
faults. The exceptions are the handful of files that touch no game memory at all: the DDS and RIF
parsers, the VFS lookup half, the profile, the settings store, the JSON document, the renderer
settings table, the profiler and the vertex-format converter. That short list is why the only test
suite in this repository that exercises a `src/` file drives the RIF decoder from a separate
command-line tool.

The licence is also not what a reader might assume from the shape. Parts derive from Rebellion's
published Aliens versus Predator source, and the `LICENSE` file carries those terms: non-commercial
use, credit required.

## Where to go next

- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): what happens after
  `DllMain` gets control, and why an address is resolved on every call.
- [Why mods are named, never discovered](/explanation/why-mods-are-named-never-discovered/): the
  first thing the proxy's early start is spent on.
- [Installing GkPlus](/tutorials/installing-gkplus/): the whole of this argument, as a
  single file copied into a folder.
