---
title: "Install a detour safely"
description: "Pick an anchor nothing else hooks, attach and detach it correctly, and confirm it actually ran."
weight: 50
audience: ["developer"]
---

This guide shows a **developer** how to run code at a point inside Gunlok. It assumes you already
have a target address and its recovered signature.

The failure mode here is silence: a detour that never runs produces a game that looks normal, and a
wrong signature faults somewhere unrelated long after the call. Every step below exists because one
of those happened.

## 1. Check nothing already hooks the target

**Two subsystems must never detour the same address.** `Subsystems` is constructed inside a single
Detours transaction, and two `DetourAttach` calls against one target do not chain there, and one hook
silently stops running. This was measured: adding a second `SetupMenus` detour beside
`ScriptSystem`'s killed the script host with no diagnostic at all, and the game otherwise looked
fine.

Before writing anything, grep for the address and for the existing hook set:

```bash
grep -rn '0x004xxxxx' src/
grep -rn 'DetourAttach' src/*.cpp
```

## 2. If the target is taken, use an anchor instead

Two options, in order of preference:

- **Have the existing hook call you.** This is what the codebase does for anything that must run at
  a fixed moment.
- **Hang off `EnsureFirstOpen`** in `src/FileHooks.cpp` if what you need is "before the engine reads
  its first byte". Three things already do: the DDS codec registration, `ApplyStoredRenderSettings`,
  and the profile's boot module. All four file hooks that reach `vfs::Resolve` call it, so whichever
  the engine gets to first is the anchor, and it is provably both late enough (gl.exe's CRT heap
  exists by `WinMain`) and early enough (a file is always opened before its bytes are needed).

Anything you add there must be idempotent, and the guard flag is set before the callees run so a
call back into a hook cannot recurse.

## 3. Confirm the signature before you wrap it

A wrong calling convention or arity drifts ESP on every call. The ground truth is the epilogue's
`RET` form, not the decompiler's parameter list:

- For a declared arity, `__fastcall` puts the first two **integral** arguments in registers (never
  floats) and `__thiscall` only `this`, so the expected operand is `4 * stack_args`.
- The `RET` test cannot see a register argument. Pair it with "does the target read ECX or EDX
  before writing it?", which costs one pass over the first few instructions. Calling a function with
  no argument that in fact reads ECX pops exactly as many bytes as calling it correctly.

`ghidra_analysis_notes.md` has the full rules; `console_command_notes.md` §6.5 has three worked cases
where getting this wrong faulted somewhere else entirely.

## 4. Attach in the constructor, detach in the destructor

Resolve the original with `GetObjectAtOffset`, attach, and record that you did:

```cpp
ThingSystem::ThingSystem() {
  ReadThingMode();                 // the GKPLUS_THING=raw kill switch
  if (!ThingEnabled) { return; }
  GetObjectAtOffset(OriginalThing, 0x0055fb20);
  ::DetourAttach(reinterpret_cast<void **>(&OriginalThing),
                 reinterpret_cast<void *>(HookedThing));
  ThingHooked = true;
}

ThingSystem::~ThingSystem() {
  if (!ThingHooked) { return; }
  ::DetourDetach(reinterpret_cast<void **>(&OriginalThing),
                 reinterpret_cast<void *>(HookedThing));
  ThingHooked = false;
}
```

`src/HudFix.cpp` is the full worked example. The guard flag matters: without it, a subsystem that
declined to hook still tries to detach.

**For a `__thiscall` hook on a game object**, use the `gk::DetourAttach` / `gk::DetourDetach`
templates in `src/DetourUtils.h`, which handle the member-function-pointer cast. For a plain
function pointer, call `::DetourAttach` with the leading `::` and **do not** include
`src/DetourUtils.h`: merely declaring those overloads inside `namespace gk` hides the global
templates that handle a plain pointer.

Then add the member to `Subsystems`; see [Add a subsystem](/how-to/development/add-a-subsystem/).

## 5. Respect the detach path

`DllMain` runs under the loader lock, and on detach a failure to unpatch leaves gl.exe with detours
into a DLL that is about to be unmapped. So:

- Nothing in a destructor may throw or fault. A fault in an earlier one can stop the rest of the
  teardown (`game_defects_notes.md` §4), so nothing later may depend on running.
- **Never put a call inside `assert`.** `NDEBUG` is defined in every optimized configuration and
  discards the argument expression, call and all, which is how RelWithDebInfo and Release once
  began a Detours transaction and never committed it, installing not one hook and reporting nothing.
  The commit goes through `Commit()` in `src/entry.cpp`, which reports a non-zero result.

## 6. Confirm it ran

Nothing tells you a hook was installed. Pick one:

- `DebugWrite` from inside the hook, read with DebugView. There is no log file
  ([why](/how-to/development/build-an-optimized-dll/)).
- The version stamp reading `GkPlus - <renderer>` proves the transaction committed at all.
- Drive the observable effect from the REPL: launch with `GKPLUS_REPL_PORT=9222` and query it. That
  is how anything in a running game gets checked here; `script_host_notes.md` has the protocol and
  `utils/rendertest/launch-gunlok.ps1` is a worked client.

Note the failure mode when you do: **a crash looks like a socket timeout**, so check the process
afterwards rather than trusting the timeout. If it did crash, go to
[Debug a crash from its WER dump](/how-to/development/debug-a-crash/).

## Choosing not to hook something

`DebugSystem` deliberately leaves `PrintParseWarning` @ `0x00477050` unhooked, and re-enabling it is
the fastest way to make in-game testing impossible: the GLS parser emits one warning per unset field
per section, 13,000+ per level load, and redirecting those to `OutputDebugString` makes the game
unplayable under any debugger. Before hooking anything on a parse or per-draw path, count how often
it fires.

## Reference and background

- [C++ API](/reference/cpp/) and the [namespace map](/reference/cpp/namespaces/): the
  existing `*System` classes, which are the worked examples.
- [Environment variables](/reference/data/environment-variables/): the `raw` escape hatches
  every behavioural hook in this codebase ships with.
- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): why two
  subsystems must never detour one target, and why the failure is silent.
