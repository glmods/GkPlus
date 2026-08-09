---
name: ghidra-recon
description: Delegate all Ghidra and binary reverse-engineering work on Gunlok's gl.exe to the ghidra-analyst subagent instead of running it in the main context. Use this whenever a task needs the decompiler, the listing, the DataTypeManager, memory reads, vtable or struct-layout recovery, a function address or signature, a call-graph question, or any use of the mcp__ghidra__* tools — including when reverse engineering is only one step of a larger implementation task, and including small "just check one function" lookups, which are exactly the ones that quietly fill the context with decompiler output.
---

# Ghidra work goes in a subagent

Decompiled C is enormous and almost all of it is scaffolding for one or two facts. A single
mid-sized function can cost more context than the entire change it informs, and a session that
decompiles inline runs out of room to do the actual work — writing the C++ mirror, updating the
notes, building and testing.

So: **the main context never calls `mcp__ghidra__*`.** It asks a question, a `ghidra-analyst`
subagent answers it against the binary — and, when it is the only agent running, writes what it
learned back into the DB itself (see "Who applies the fix"). Only the answer comes back. The
decompiler output, the dead ends and the listing dumps stay in the subagent.

A `PreToolUse` hook enforces this — `.claude/hooks/deny-ghidra-in-main-context.sh`, wired up in
`.claude/settings.json`. It denies `mcp__ghidra__*` unless the call comes from inside a subagent,
which it tells from the transcript path. It fails open on input it does not recognise, so it is a
backstop for this skill rather than a replacement for it.

This holds even when the lookup looks trivial. "Just get me the signature of one function"
still means opening the decompiler, and it is the case where inlining feels cheapest and is
most often wrong — the function turns out to be a stub, or a tail-jump, or its epilogue
contradicts its label, and three more decompilations follow.

## When this applies

Any task where a step is answered by the binary rather than by the repo:

- a function's address, calling convention, arity, or what it does
- a struct or vtable layout, a field's meaning, a slot's owner
- what writes a global, what a constant is, which callers reach something
- confirming a `*_notes.md` claim or a `src/` mirror against the binary
- the `RET`-form / ECX-read sweeps, enum recovery, table extraction

It applies just as much when RE is one step inside a larger job ("add a binding for X" that
needs X's signature first) as when the whole request is an investigation.

**It does not apply** to questions the repo already answers. `CLAUDE.md`, the `*_notes.md`
files, `src/*.h` and the AvP source drop at `D:\Documenti\GitHub\aliens-vs-predator\source\
AvP_vc\3dc` are cheap to grep and are often enough. Check them first — a delegation that
rediscovers what `role_system_notes.md` already records is wasted. Delegate when the repo is
silent, ambiguous, or suspected wrong.

## How to delegate

Call the Agent tool with `subagent_type: "ghidra-analyst"`. Run it with
`run_in_background: false` when you need the answer to continue — which is the usual case,
since the next edit depends on it.

The subagent starts with no knowledge of your session. Its brief must stand alone, and the
single biggest determinant of a useful report is how sharply the question is posed. Include:

- **The question**, as specifically as you can put it. "Is `ApplyDamage`'s third parameter the
  attacker's team id?" beats "look at `ApplyDamage`".
- **What is already known** — the addresses, names and struct offsets you have, and where they
  came from (`CLAUDE.md`, a notes file, a mirror header). This is what stops the agent
  re-deriving your starting point.
- **Why you are asking** — the change you are about to make. An agent that knows the answer
  feeds a `static_assert` will check the size; one that does not will report the field and
  stop.
- **What "done" looks like** — the specific fields, slots, or signatures the report needs.

Fan out when the questions are independent: several `Agent` calls in one message run
concurrently. Use `SendMessage` to the same agent for a follow-up that depends on what it just
found, so it keeps the context it built rather than paying to rebuild it.

## Who applies the fix

**One agent at a time: let it write.** If you are running a single `ghidra-analyst` and no other
agent is touching the DB, it should normally be free to rename, retype, comment and otherwise
improve what it passes through, as the Ghidra hygiene rules in `CLAUDE.md` ask. Say so in the
brief.

That is not a relaxation of the read-only habit, it is the cheaper arrangement. The agent already
has the decompiler open and the surrounding analysis loaded; making it *describe* an edit so the
main context can replay it is a lossy round trip of exactly the detail that is expensive to
rebuild — and the main context cannot call `mcp__ghidra__*` at all, so every such edit would need
a second delegation to apply what the first one already knew.

Scope the authority rather than granting it open-endedly. A brief that says which functions,
globals and types are in play, and that anything else found should be reported in prose and left
alone, gets the hygiene without the drift.

**Fanning out: read-only.** The DB is shared mutable state with no merge. Concurrent renames
collide, one agent retypes a global another is reading through, and `createLabel` deleting a
`Symbol` handle another holds is a race rather than the documented single-agent gotcha. When
several agents run at once, they report and the main context applies — sequentially, afterwards,
in one pass.

The same rule decides `SendMessage` versus a fresh `Agent`: a follow-up to the agent that just
did the work keeps its write authority and its context, where a new one starts blind and may
contradict what the first one wrote.

## Example brief

```
Question: What is the exact signature and behaviour of MakeCameraTrack's callee at
0x005aa920, and does it read ECX/EDX before writing them?

Known: src/MakeRole.cpp binds it as FastCall<void, void*, void*, const char*, Vec3>.
console_command_notes.md §6.5 says this bind is wrong on all four arguments but does not
give the correct one. ToCameraTrack @ (see gls_system_notes.md) is the game's own caller.

Why: I am fixing the bind in src/MakeRole.cpp. A wrong arity drifts ESP per call, so I need
the epilogue's RET form, not just a plausible parameter list.

Needed: the calling convention as evidenced by the epilogue and the register reads; each
parameter's type and where it is passed; whether the Vec3 is by value or by pointer; and
what the game's own call site puts in each slot.
```

## When the report comes back

The report is now your only record of that work — the tool output is gone. Two things follow:

- **Durable facts belong in the repo, not just in the conversation.** If a finding changes a
  struct mirror, a signature, or contradicts a `*_notes.md` claim, write it into `src/` and the
  notes in the same session. That is main-context work; the subagent's job ended at the report.
  A correction in particular should land everywhere at once — the mirror, the notes, and the
  Ghidra DB — per the renaming convention in `CLAUDE.md`.
- **Audit the DB changes the report lists**, whether or not you authorised them. Read-only is not
  enforced on delegated work — a subagent told not to write has renamed things anyway — and an
  agent that *was* given write authority will have used it. The report's "Database changes"
  section is what you check against. What you are looking for is scope: a rename outside the task
  is worth reverting before it propagates into notes written later, and a name that turns out to
  describe the function wrongly is worse in the DB than a `FUN_` was, because it reads as settled.

  A DB rename is also only one of the three surfaces. `CLAUDE.md`'s renaming convention wants the
  `src/` mirror and every `*_notes.md` moved in the same pass, and the subagent cannot see whether
  the old name appears there — so grep for it yourself once the report is in.

Treat a finding marked uncertain as uncertain. If the next edit depends on it, send a follow-up
asking for the measurement rather than building on the inference — the failure mode here is a
wrong name or arity that compiles cleanly and faults somewhere unrelated much later.
