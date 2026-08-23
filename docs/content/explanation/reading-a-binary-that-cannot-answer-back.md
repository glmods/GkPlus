---
title: "Reading a binary that cannot answer back"
description: "How findings move between Ghidra, the C++ mirror and the notes, and why a conclusion that something is absent is held to a much higher standard than one that something is there."
weight: 80
audience: ["developer"]
---

This page is for developers doing reverse engineering on `gl.exe`. It is about the loop the project
runs (decompiler, source mirror, prose) and about one asymmetry inside it that has caused more
wrong entries in these notes than any other single cause.

## Three places, one analysis

A finding about Gunlok lands in three surfaces, and the convention is that it lands in all three or
in none:

**The Ghidra database.** `FUN_004a1b30` becomes a name; parameters and locals get names and types;
structs and enums get defined and applied to the variables that use them; a non-obvious function gets
a plate comment. The rule is to leave the database in a better state than you found it, because the
alternative is that the analysis lives in a chat log and evaporates.

**The `src/` mirror.** Decompiled structures are re-expressed as ordinary C++ with `static_assert`s
on `sizeof` and `offsetof`. This is the part that makes the loop unusual: a claim about a struct
layout, written this way, is checked by the compiler on every build. A document saying "field at
`0x50` is the nav agent" is a note; a mirror saying it is a claim that fails loudly when it stops
being true.

**The notes.** The `*_notes.md` files at the repository root are the prose record: what a subsystem
does, which measurements pin it, and what was tried and abandoned. They are where a finding that has
no code shape survives at all.

A rename happens in all three at once, plus any getter, setter, constructor and destructor that
reaches the thing. The example the conventions give is the object at `MobileActor+0x200`, modelled as
an `AIController` until it turned out to be the actor's nav-mesh movement agent: renamed to
`NavAgent` across the database, the mirror and the slot table, with a one-line breadcrumb of the old
name where external write-ups would still use it. A misleading name is renamed, never annotated
around.

## The asymmetry

A **positive** finding is cheap to check. If you claim a function has this signature, you can call it
and see. If you claim a struct is `0xc0` bytes with a pointer at `0x68`, the `static_assert` says so
at build time and the game says so at run time.

A **negative** finding is a claim about absence, and absence in a disassembly is almost always a
statement about the *disassembly* rather than about the binary.

This is why `ghidra_analysis_notes.md` opens by telling you to read it before concluding anything
negative (that a field has no writer, a function no caller, a signature no arguments), and adds
that nearly every entry in it is such a conclusion that turned out to be wrong.

## The cases that make the point

**The orange alert state.** The shipped manual describes an enemy state between "searching" and
"engaged". The notes recorded it as unreachable, on the grounds that a sweep of `.text` found no
write of `alert_state = 1` anywhere. The write is at `0x00453e3f`. It sits inside `AiThink_Bot`,
whose 8,400 bytes were at the time undefined bytes behind four unresolved jump tables, so the sweep
was correct about the disassembly and wrong about the game.

**A function with no callers.** `IsWithinElevationLimit` was recorded as having no cross-references
at all and possibly dead. It has two call sites, both in the same undefined region.

**Flares.** Recorded as dead code. The *player* flare is fully reachable and functional through its
key binding; what is dead is only the executor-side auto-flare mechanism, and even that is a
redundant route to a projectile the ammo table already supplies. The correction is in the notes with
the split spelled out, because a flat "flares are dead" was worse than nothing.

All three share one cause: a negative conclusion drawn from a view that could not see what it needed
to see.

## The traps behind those traps

A handful of Ghidra-specific behaviours produce the same shape of error, and they are worth knowing
because none of them announces itself:

- A `CreateThread` reference is an **entry-point reference, not a call edge**. Leave it in a caller
  closure and every executor-only function falsely appears reachable from the main thread.
- "No cross-references" often means **the referencing data was never defined**: a vtable sitting as
  raw bytes references nothing.
- **The decompiler does not consult `COMPUTED_JUMP` references.** Creating correct ones makes the
  listing right and the decompiler still re-derives the jump table itself; when it cannot, it treats
  the indirect jump as a call and starves every downstream switch in the same function. So
  `getJumpTables()` returning zero is not evidence the references are wrong.
- **Four-byte alignment is not sufficient to call a dword a pointer.** A 16-bit array whose values are
  large enough reads as a plausible `.text` pointer; the worked case turned out to be a slice of the
  GLS parser's yacc tables, where a repeated parser state number reads as `0x00630063`.

The general form is that the tooling presents a *model* of the binary, and the model's silences and
the binary's silences are different things.

## The organisational answer

Ghidra work here is **explore, then consolidate**, and the split is enforced structurally and not
by instruction.

Any number of explorer agents read the database in parallel through read-only contexts. Such a
context refuses to open a transaction, and the explorers' tool allowlist does not offer the
write-capable context or the save operation at all, so read-only is a property of the arrangement
rather than a promise. An explorer also has no ability to write files or spawn further agents, so it
cannot touch the repository either.

A single consolidator afterwards applies what was found to the database, the `src/` mirror and the
notes together, in the one pass that keeps the three surfaces agreeing.

**Two consolidators at once remains unsafe.** The database is shared mutable state with no merge, so
concurrent writers are a data-loss problem and not a coordination inconvenience. Where parallel
consolidation is needed, the areas have to share no type, function, source file or notes section.

This arrangement used to be advisory, with a subagent told not to write and doing it anyway. That
history is why it is now enforced in the agent definitions, and worth noting: the enforcement lives
entirely there, so a brief that hands an explorer the wrong tool has no backstop.

## Two ground truths that are not the binary

**The shipped scripts are commented primary sources.** `<Gunlok>\scripts\*.gsh` and `*.gcs` are the
developers' own headers and console scripts, and they carry comments. `defaults.gsh` names the
perception fields and tells you `sight angle 45` is a half-angle; `mine.gsh` shows that a mine is an
ordinary character with `ai mine`. Reading them before decompiling anything data-shaped is usually
faster than the decompiler, and the field values across all the headers are a free cross-check on any
recovered unit or enum. They are authoring intent rather than measurement (two shipped roles have a
field in the wrong units), so they say what to look for, not what is true.

**The upstream Aliens versus Predator source is verified ground truth for exactly one layer.**
Gunlok's asset layer is Rebellion's `3dc` chunk library, and the published AvP Gold source is
available locally. This is measured, not assumed: AvP's Huffman decompressor handles 563 of 563
shipped `.rif` files, and 88 of Gunlok's 105 registered chunk ids appear in that source.

The boundary is sharp and worth respecting. The chunk and RIF library is shared, along with the list
and hash-table templates that `src/List.h` and `src/HashTable.h` mirror. The **entire game layer is
not**: roles, actors, GLS scripts, triggers, menus and the save format have no AvP counterpart, and
mapping AvP's strategy blocks onto Gunlok's structures would produce something confident and wrong.

## What the manual is for

The shipped manual is not reverse engineering, but it is the best inventory of what the gameplay
layer is *supposed* to do, and the project treats it as claims to verify in both directions.

Some claims fail: formations have no geometry in the code. Some claims that were recorded here as
failing turned out to be true, and both of those were the negative-conclusion error above. That
two-way traffic is the reason the manual is in the reference list at all instead of being dismissed
as marketing.

## Limits

The symbol coverage of the database is partial. The last recorded export names a bit under two
thirds of the functions, and the current figure lives in `utils/symdump/README.md` instead of being
copied here, for the reasons in
[Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/).

More importantly: everything above describes how to be *less* wrong, not how to be right. The notes
are a record of conclusions, several of which have been reversed, and the honest reading of
`ghidra_analysis_notes.md` is that it is a list of about forty ways this project has already been
confidently mistaken.

## Where to go next

- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): what the recovered
  addresses are used for.
- [What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/): the same
  bar, applied to a different instrument.
- `ghidra_analysis_notes.md` and `address_map.md` at the repository root are the working references.
- [Reverse-engineer a function in Ghidra](/how-to/development/reverse-engineer-in-ghidra/)
  and [Mirror a game struct and prove its layout](/how-to/development/mirror-a-game-struct/): this
  discipline as two procedures.
