---
title: "How a hook reaches the game"
description: "Why every game address is resolved on the call that needs it, why all the hooks go in through one transaction, and what that arrangement makes impossible."
weight: 20
audience: ["developer"]
---

This page is for developers working on `d3d8.dll` itself. It is about the layer underneath every
subsystem: how code in GkPlus names a function that has no symbol, how a detour gets installed, and
which of those decisions are constraints rather than preferences.

## Everything is an address

`gl.exe` ships no symbols and exports nothing useful. A game function is a number: `Print` is at
`0x004d4b50` and `LoadLevel` at `0x004e0980`, recovered from Ghidra and recorded in
`address_map.md` at the repository root.

Those numbers are not usable directly, because the image can load anywhere. GkPlus computes the load
base once, by subtracting a constant from the host executable's actual entry point
(`src/Core.cpp`):

```cpp
constexpr uintptr_t EntryPointOffset = 0x005e50c8;
// base = actual entry point - EntryPointOffset
```

`GetObjectAtOffset` then adds the base to an offset and reinterprets the result as a typed function
pointer, using one of four aliases that spell the calling convention into the type: `CDecl`,
`StdCall`, `FastCall`, `ThisCall`.

This is the whole naming layer. It fits in one small header, and what it does *not* contain
matters: no symbol table, no lazy-loaded map, no cache of resolved pointers.

## Why resolution happens on every call

A native-API wrapper resolves its offset each time it is called. `GetBaseAddress()` caches the base
in a function-local static, so the cost of a resolution is a load, an add and a cast, which is cheaper
than the branch that would guard a lazily-filled pointer.

The property that matters here is the absence of state. There is no module-owned pointer
to initialise, nothing to keep alive, nothing that can be stale, and no ordering question about when
resolution happens relative to anything else. A subsystem that is nothing but struct mirrors and
native-API wrappers therefore needs no lifetime object at all. It is a header and a `.cpp`, and
`Subsystems` in `src/entry.cpp` contains only the subsystems that install a detour and so genuinely
have a lifetime.

The alternative, resolving a table of pointers at load, would put that work under the loader lock,
would pay for every address whether or not the run touches it, and would introduce exactly the kind
of initialisation ordering that the rest of this design spends effort avoiding.

The corresponding risk sits in the *type*, not the address. A wrong calling convention or a wrong
argument count drifts the stack pointer and faults somewhere unrelated, long after the call that
caused it. This is why `ghidra_analysis_notes.md` treats the `RET` form as ground truth for both
convention and arity, and why that is the one thing to check before wrapping a new function.

## Three ways in

**Call or read directly.** A typed pointer at base + offset, invoked. This covers the great majority
of the native API: `gk::GetActorById`, `gk::MapToWorld`, `gk::GoToMenu`.

**Detour.** Microsoft Detours rewrites the target's prologue to jump to our function and hands back a
trampoline. The convention throughout is RAII: resolve the original, attach in the constructor,
detach in the destructor, so a subsystem's hooks live exactly as long as the object.

**Patch the import table.** Two subsystems resolve no offsets at all. `FileHookSystem` writes nine
pointers into `gl.exe`'s own IAT, and `WindowPlacementSystem` writes one more. Every file call in
`gl.exe` reads its slot at run time, so a single pointer write catches every call site, including
the ones that cache the pointer in a register, and catches only `gl.exe`, because GkPlus's own
calls resolve through this DLL's imports. Detouring kernel32 instead would intercept the whole
process, our own I/O included, and the interception would be recursive (`src/FileHooks.h`).

## One transaction, and the rule that comes out of it

`DllMain(DLL_PROCESS_ATTACH)` opens a single Detours transaction, constructs `Subsystems`, with every
member attaching its detours in its constructor, and commits. Detach reverses it.

Two consequences follow, and both have cost this project real time.

**Two subsystems must never detour the same target.** Inside one transaction, two `DetourAttach`
calls against one address do not chain; one hook silently stops running. This was measured: adding a
second `SetupMenus` detour alongside the script host's killed the script host outright, with no
diagnostic at all. The REPL listener simply never opened, and the game otherwise looked normal.

The rule is why three unrelated needs share one anchor rather than each taking a hook. The DDS codec
registration, the restore of stored renderer settings, and the profile's boot module all run from
`EnsureFirstOpen` in `src/FileHooks.cpp`, on the first file the engine opens. They have nothing to do
with each other; they are together because they all need a point after `gl.exe`'s CRT is up and
before the engine reads an asset, and only one of them may own the hook that provides it.

**Construction order is part of the design, and it is documented in place.** `FileHookSystem` is
first because assets read during `WinMain` must already pass through it. `D3D8CaptureSystem` is
second and precedes `GUISystem`, which reads the device global expecting whatever `CreateDevice`
returned. Most other members carry a comment saying explicitly that their position does *not* matter
and why, which is more useful than an ordered list, because it says which constraints are real.

Destruction is reverse order, and it is not fully trustworthy: `game_defects_notes.md` §4 records
that a fault in an earlier destructor can prevent the rest of teardown from running at all. That is
why `settings::SaveIfDirty()` is called at the very top of the detach branch, ahead of the first
destructor, rather than from a destructor of its own.

## The commit that was an assert

The single most instructive defect in this codebase was one line. The transaction used to be
committed as:

```cpp
assert(DetourTransactionCommit() == NO_ERROR);
```

`NDEBUG` is defined in every optimized configuration, and `assert` then discards its argument
*expression*, the call included. So `RelWithDebInfo` and `Release` began a Detours transaction and
never committed it. Every `DetourAttach` queued normally, `Subsystems` constructed without
complaint, and not one hook was installed.

The symptom was stock Gunlok with `d3d8.dll` sitting in the module list: no
version stamp, no REPL listener, no file hooks, no D3D8 capture. That reads as "the optimized build
is broken", which is why only a Debug DLL was ever deployed. The commit is now a function that
reports a non-zero result, and there are no `assert`s anywhere in the codebase.

The wider point is that a hook layer's failures are *silent by
construction*: a hook that did not install produces the original software, which looks like working
software. Anything in this layer that can fail needs a channel to say so, because nothing downstream
will notice.

## Knowing what you are looking at

Reaching a function is half the problem; the other half is agreeing with the game about the shape of
its data. The mirrors in `src/` are ordinary C++ structs with `static_assert`s on `sizeof` and
`offsetof` for the fields that pin the layout, which turns a claim about a decompiled structure into
something the compiler checks on every build.

Two conventions there are worth the space they cost. A struct with a vtable gets **declaration-ordered
pure virtuals**, never an explicit `void *vtbl` member. The implicit vptr occupies offset `0x00`, so
the first data member starts at `0x04`; an explicit field would double-count those four bytes and
hide the slots besides. A second vptr in the middle of a struct means multiple inheritance and is
modelled with real C++ bases, because MSVC lays base subobjects out in declaration order and a
correctly-sized first base puts the second vptr exactly where it belongs.

The price is that such a struct is not standard-layout, so `offsetof` on it is technically undefined
and clang says so, at around thirty warnings per translation unit that includes `src/Actors.h`.
That is
accepted deliberately, and the mitigation is a grep filter rather than a change of approach. It is a
genuine trade, and a reader is entitled to think it the wrong one; the argument for it is that the
warnings are noise while the asserts are the only mechanical check on layout that exists.

## What this arrangement makes impossible

Because every address derives from the host executable's entry point, **nothing built on this layer
runs outside Gunlok**. There is no mock, no stub base address, no offline mode. A subsystem that
touches the native API can only be exercised by loading it into the game, which is why the REPL
exists at all and why `harness_testing_notes.md` is careful to scope what a throwaway harness can and
cannot cover.

The files that escape this are the ones that touch no game memory: the DDS and RIF parsers, the VFS
lookup half, the profile, the settings store, the JSON document, the renderer settings table, the
profiler, the vertex-format converter. Every one of them is testable precisely to the extent that it
avoided this layer, which is a useful thing to notice when deciding where a new piece of logic should
live.

## Where to go next

- [Why GkPlus is a d3d8.dll](/explanation/why-gkplus-is-a-d3d8-dll/): how the DLL gets loaded in
  the first place.
- [Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/): where the addresses in
  `address_map.md` come from, and how far to trust them.
- [Add a subsystem](/how-to/development/add-a-subsystem/) and
  [Install a detour safely](/how-to/development/install-a-detour/): the same material as a
  procedure, for someone about to add one.
