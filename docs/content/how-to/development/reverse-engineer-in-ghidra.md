---
title: "Reverse-engineer a function in Ghidra"
description: "Run a round of binary analysis as explore-then-consolidate, and land the result in the database, the src/ mirror and the notes together."
weight: 100
audience: ["developer"]
---

This guide shows a **developer** how to answer a question about `gl.exe` and leave the record better
than it was.

All Ghidra work in this repo is delegated to subagents through the `ghidra-jython-mcp` plugin:
`ghidra-recon` is the skill, `ghidra-explorer` reads, `ghidra-consolidator` writes. Decompiled C is
enormous and almost all of it is scaffolding for one or two facts, so the main context poses
questions and only the answers come back.

## 1. Check the repo's own record first

A delegation that rediscovers what the notes already say is wasted. Grep, in this order:

- `address_map.md`: segment layout, named globals and functions, the Actor hierarchy and subclass
  sizes, `Role`/`Map` offsets.
- The subsystem's `*_notes.md`, and `actor_vtable_notes.md` for a slot.
- The `src/` mirror, whose `static_assert`s are a layout claim someone already compiled.

Delegate when the record is **silent**, **ambiguous**, or **suspected wrong**, and note which of
the three, because it changes the brief.

Before you conclude anything **negative** (that a field has no writer, a function no caller, a
signature no arguments), read `ghidra_analysis_notes.md`. Nearly every entry in it is such a
conclusion that turned out to be wrong: a sweep over `.text` is a statement about the
*disassembly*, and two entries this project once recorded as dead code are live, in both cases
because the code sat in bytes that were undefined at the time.

## 2. Fan out explorers

Invoke the `ghidra-recon` skill, split the question into areas that can be answered independently,
and send one `Agent` call per area **in a single message** so they run concurrently. Explorers use
`create_readonly_context`, which refuses a transaction, and their tool allowlist withholds
`create_context` and `save_program`, so any number are safe at once and all see the same stable
snapshot.

Each brief must stand alone. Include:

- **The question**, as specifically as you can put it. "Is `ApplyDamage`'s third parameter the
  attacker's team id?" beats "look at `ApplyDamage`".
- **The area's boundary**: what belongs to a sibling explorer this round.
- **What is already known**: addresses, names, struct offsets, and where they came from. Include
  names applied in an earlier round, and say so explicitly if that round's consolidator did **not**
  save, since the snapshot will still show the old `FUN_`/`DAT_` names and an unwarned explorer
  reports
  them back as fact.
- **Why you are asking.** An agent that knows the answer feeds a `static_assert` will check the
  size; one that does not will report the field and stop.
- **What "done" looks like**: the specific fields, slots or signatures the report needs.

Ask for evidence, not just a verdict. For a signature that means the epilogue's `RET` form and
whether the target reads ECX/EDX before writing them, because a wrong arity drifts ESP per call and
faults somewhere unrelated.

## 3. Read the reports and find the seams

This is the step that is easy to skip, and you are the only party that sees every report. Look for:

- **A contradiction**: two sizes for one struct, two arities for one function.
- **A boundary**: an "uncertain" section naming something another area would settle.
- **A collision with the record**: a finding that contradicts a notes file or a header. That is a
  result, and it changes what the consolidator has to touch.
- **An answer thinner than the question.**

Fan out again at exactly those seams, saying what the disagreement is. Repeat until the remaining
uncertainty does not affect what you are about to write, rather than until everything is known.

Use `SendMessage` to an explorer that is still around when the follow-up builds on what it just
found; use a fresh agent for a different area, or for an independent look at a contested claim.

## 4. Partition, then consolidate one at a time

Two areas are independent only if applying one touches none of the same **artifacts**: no shared
Ghidra type, function, global or namespace, no shared source file, and no shared notes *section*. If
two would overlap, merge them into one brief.

Then run consolidators **one at a time**, `run_in_background: false`, auditing each report before
the next starts. Two at once, or one alongside an explorer, is the arrangement this workflow
forbids outright: the database is shared mutable state with no merge.

A consolidator's brief is the *findings*, not the question. Pass the explorer's "Findings",
"Suggested database edits" and "Suggested repo edits" verbatim enough to act on, with their
CONFIRMED/PROPOSED marks intact, since a paraphrased signature is how a wrong arity gets applied.
Give it
the artifact list it may touch, say what is contested, and **say so if the area touches a mirrored
struct header**, because the `static_assert`s are the check and it should build.

## 5. What lands, and where

Leave the database in a better state than you found it. That is the consolidator's job and the
reason it exists:

- Rename `FUN_00xxxxxx` and `DAT_00xxxxxx` to descriptive names as soon as the purpose is clear.
- Rename locals and parameters off `uVar1`/`param_1`, and set their types.
- Define and apply structs and enums rather than leaving raw offset arithmetic.
- Add a plate comment on anything non-obvious, naming the calling convention.

And in the repo: the `src/` mirror
([how](/how-to/development/mirror-a-game-struct/)), `address_map.md` for addresses and offsets, and
the subsystem's `*_notes.md` for behaviour. A finding that ends a round with no consolidator run
exists only in that conversation and dies with it.

**A misleading name gets renamed, not annotated around**, in one pass across all three surfaces,
then grep the old name to confirm nothing dangles. Leave a one-line breadcrumb of the prior name
where external write-ups will still use it.

## 6. Audit the report

- **Check the scope of the DB changes.** A rename outside the brief's area is worth reverting before
  it propagates into notes written later, and a name that describes the function wrongly is worse
  than a `FUN_` was, because it reads as settled.
- **Read the "Not applied" section.** That is where the real work left over lives, and it is next
  round's explorer brief.
- **Read the "Saved" section.** A save reporting *nothing to write* after edits were applied means
  they may not have landed; a suppressed or failed save leaves names that exist in the database but
  in no snapshot, and those go into the next brief verbatim.
- **Treat a finding marked uncertain as uncertain.** If the next edit depends on it, send another
  explorer for the measurement rather than building on the inference.

## Before you use what you found

Two upstream sources save whole rounds:

- **The published AvP Gold source** is ground truth for the chunk/RIF layer, `List<T>` and
  `HashTable<T>`, and that is verified rather than assumed. It is *not* ground truth for the game
  layer: roles,
  actors, GLS, triggers, menus and saves have no AvP counterpart. `rif_chunk_format.md` has the
  id → class/file map.
- **`<Gunlok>\scripts\*.gsh` and `*.gcs` are the developers' own commented source.** Read them
  before decompiling anything data-shaped; they name fields and units. Treat comments as authoring
  intent rather than measurement, and confirm against the consumer.

Then wrap it: [Mirror a game struct](/how-to/development/mirror-a-game-struct/),
[Add a subsystem](/how-to/development/add-a-subsystem/),
[Install a detour safely](/how-to/development/install-a-detour/).

## Reference and background

- [Design records index](/reference/data/notes-index/): which notes file a finding is
  written back into.
- [Namespace map](/reference/cpp/namespaces/): which header the mirror half of a finding
  belongs in.
- [Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/): why the workflow is
  explore-then-consolidate, why only one agent writes, and why a
  negative conclusion needs more evidence than a positive one.
