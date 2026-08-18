---
name: ghidra-recon
description: Delegate all Ghidra and binary reverse-engineering work on Gunlok's gl.exe to subagents instead of running it in the main context, and run it as explore-then-consolidate. Use this whenever a task needs the decompiler, the listing, the DataTypeManager, memory reads, vtable or struct-layout recovery, a function address or signature, a call-graph question, or any use of the mcp__ghidra__* tools — including when reverse engineering is only one step of a larger implementation task, and including small "just check one function" lookups, which are exactly the ones that quietly fill the context with decompiler output.
---

# Ghidra work goes in a subagent, in two phases

Decompiled C is enormous and almost all of it is scaffolding for one or two facts. A single
mid-sized function can cost more context than the entire change it informs, and a session that
decompiles inline runs out of room to do the actual work — writing the C++ mirror, updating the
notes, building and testing.

So: **the main context never calls `mcp__ghidra__*`.** It poses questions, subagents answer them
against the binary, and only the answers come back. The decompiler output, the dead ends and the
listing dumps stay in the subagent.

A `PreToolUse` hook enforces this — `.claude/hooks/gate-ghidra-access.sh`, wired up in
`.claude/settings.json`. It denies `mcp__ghidra__*` outright in the main context, and denies
`create_context` (the read-write one) to a `ghidra-explorer`. It fails open on input it does not
recognise, so it is a backstop for this skill rather than a replacement for it.

The explorer's read-only status is belted and braced: its `tools:` allowlist does not include
`create_context` either, so the read-write context is not even offered to it as a deferred name — both
layers are verified against a live agent. What neither layer covers is the main context's own
discipline about *when* to delegate, which is the rest of this file.

This holds even when the lookup looks trivial. "Just get me the signature of one function" still
means opening the decompiler, and it is the case where inlining feels cheapest and is most often
wrong — the function turns out to be a stub, or a tail-jump, or its epilogue contradicts its label,
and three more decompilations follow.

## The two agents

The Ghidra MCP server has two kinds of context, and the split between them is the whole design:

| | `ghidra-explorer` | `ghidra-consolidator` |
|---|---|---|
| Ghidra context | `create_readonly_context` — an immutable shared snapshot | `create_context` — read-write |
| Writes | **nothing**, neither DB nor repo | the DB, `src/`, the notes |
| Concurrency | **any number in parallel** | **exactly one, alone** |
| Output | findings + evidence + proposed edits | what it applied, verified, declined |

`create_readonly_context` is what makes wide fan-out safe: each explorer gets its own interpreter,
none can disturb the program or each other, and every one of them sees the *same* stable snapshot,
so two explorers cannot disagree merely because the program moved under one of them. That removes
the constraint the old single-agent arrangement was built around.

The consolidator is the opposite. The database is shared mutable state with no merge: concurrent
renames collide, one agent retypes a global another is reading through, and `createLabel` deleting a
`Symbol` handle another holds is a race rather than a documented gotcha. **Never run two, and never
run one alongside an explorer.**

## The workflow

Every reverse-engineering task, from a one-function lookup to a whole-subsystem recovery, runs the
same shape. Small tasks just collapse the middle.

**1. Check the repo first.** `CLAUDE.md`, the `*_notes.md` files, `src/*.h` and the AvP source drop
at `D:\Documenti\GitHub\aliens-vs-predator\source\AvP_vc\3dc` are cheap to grep and are often
enough. A delegation that rediscovers what `role_system_notes.md` already records is wasted.
Delegate when the repo is silent, ambiguous, or suspected wrong — and note which of the three, since
"suspected wrong" changes the brief.

**2. Fan out explorers.** Split the question into areas that can be answered independently and send
one `Agent` call per area, **all in a single message** so they run concurrently. Each brief must
stand alone (see below). Do not pre-serialise out of caution: explorers cannot collide.

**3. Read the reports and find the seams.** This is the main context's real job, and it is the step
that is easy to skip. You are the only party that sees every report, so you are the only one who can
notice:

- **A contradiction** — two explorers give different sizes for the same struct, different owners for
  the same vtable slot, different arities for the same function.
- **A boundary** — an explorer's "Uncertain / not established" section names something another
  area's answer would settle, or says outright that it stopped at a boundary.
- **A collision with the repo** — a finding contradicts a `*_notes.md` claim or a `src/`
  declaration. That is a result, not a problem, but it changes what the consolidator has to touch.
- **An answer that is thinner than the question** — the report answers what was asked and leaves the
  area still incoherent.

**4. Fan out again to clarify.** Aim narrow briefs at exactly the seams from step 3, and say what the
disagreement is: an explorer told "report A says 0x230 and report B says 0x238; settle it from the
constructor's `malloc` and slot 35" costs a fraction of one told "check the size again". Repeat 2–4
as many rounds as it takes. **Stop when the remaining uncertainty does not affect what you are about
to write** — not when everything is known.

Use `SendMessage` to an explorer that is still around when the follow-up builds directly on what it
just found, so it keeps the context it built rather than paying to rebuild it. Use a fresh `Agent`
when the follow-up is a different area, or when you want an independent look at a contested claim —
asking the agent that made a claim to re-check it is the weaker test.

**5. Partition the findings into independent areas.** Two areas are independent only if applying one
touches none of the same *artifacts* as the other:

- no shared Ghidra type, function, global or namespace,
- no shared `src/` file,
- no shared `*_notes.md` file — and in practice no shared *section*, since the consolidator is
  editing prose.

If two would overlap, **merge them into one brief** rather than running two consolidators over the
same file. Merging costs a longer brief; overlapping costs a lost edit or a mangled section.

**6. Run consolidators one at a time.** One `Agent` call, `run_in_background: false`, wait for the
report, audit it, then the next. Never two in one message — that is the one arrangement this
workflow forbids outright. A single area is the normal case; several is what the partition is for.

**7. Audit each report before the next one starts.** See "When the report comes back".

## Briefing an explorer

Call the Agent tool with `subagent_type: "ghidra-explorer"`. The subagent starts with no knowledge
of your session, and the single biggest determinant of a useful report is how sharply the question is
posed. Include:

- **The question**, as specifically as you can put it. "Is `ApplyDamage`'s third parameter the
  attacker's team id?" beats "look at `ApplyDamage`".
- **The area's boundary** — what is yours and what belongs to a sibling explorer this round. This is
  what keeps two reports from spending the same tokens on the same function.
- **What is already known** — the addresses, names and struct offsets you have, and where they came
  from (`CLAUDE.md`, a notes file, a mirror header). This is what stops the agent re-deriving your
  starting point. Include names applied in an earlier round, since the read-only snapshot may predate
  them.
- **Why you are asking** — the change you are about to make. An agent that knows the answer feeds a
  `static_assert` will check the size; one that does not will report the field and stop.
- **What "done" looks like** — the specific fields, slots, or signatures the report needs.
- **How deep to go.** The default is "as deep as the area needs". Say so when you want the whole
  region explained, and say the opposite when you genuinely want one epilogue read and nothing else.

### Example explorer brief

```
Question: What is the exact signature and behaviour of MakeCameraTrack's callee at 0x005aa920,
and does it read ECX/EDX before writing them?

Area: that function and its callees only. A sibling explorer has ToCameraTrack and the GLS
parser side — do not follow the call chain up into the parser.

Known: src/MakeRole.cpp binds it as FastCall<void, void*, void*, const char*, Vec3>.
console_command_notes.md §6.5 says this bind is wrong on all four arguments but does not give
the correct one. ToCameraTrack @ (see gls_system_notes.md) is the game's own caller.

Why: I am fixing the bind in src/MakeRole.cpp. A wrong arity drifts ESP per call, so I need the
epilogue's RET form, not just a plausible parameter list.

Needed: the calling convention as evidenced by the epilogue and the register reads; each
parameter's type and where it is passed; whether the Vec3 is by value or by pointer; and what
the game's own call site puts in each slot.
```

## Briefing a consolidator

Call the Agent tool with `subagent_type: "ghidra-consolidator"`, one at a time. It does not
re-open the investigation, so its brief is the *findings*, not the question. Include:

- **The findings themselves, verbatim enough to act on** — the explorer's "Findings", "Suggested
  database edits" and "Suggested repo edits" sections, with their CONFIRMED/PROPOSED marks intact.
  Do not paraphrase a signature or an offset; a lossy retelling is how a wrong arity gets applied.
- **The area's boundary as a list of artifacts** — the functions, types, globals, `src/` files and
  notes files it may touch. Everything else is out of scope and should come back as prose.
- **What is contested and how to treat it** — if two reports disagreed and you decided to have it
  measured rather than guessed, say that explicitly. It will otherwise pick one.
- **What is already known from other areas** that it needs but must not edit.
- **Whether a build is expected.** If the area touches a `src/` struct header, say so — the
  `static_assert`s are the check and it should run it.

## When the report comes back

The report is now your only record of that work — the tool output is gone.

- **Audit the DB changes a consolidator lists.** What you are looking for is scope: a rename outside
  the brief's area is worth reverting before it propagates into notes written later, and a name that
  turns out to describe the function wrongly is worse in the DB than a `FUN_` was, because it reads
  as settled.
- **Check the "Not applied" section**, which is where the real work left over lives. A suggestion
  declined for want of a measurement is next round's explorer brief.
- **Do not assume an explorer wrote nothing.** Its context is read-only and the hook denies it a
  write context, so this is now enforced rather than advisory — but if an explorer's report claims to
  have changed something, believe the report and check, rather than the mechanism.
- **Treat a finding marked uncertain as uncertain.** If the next edit depends on it, send another
  explorer for the measurement rather than building on the inference — the failure mode is a wrong
  name or arity that compiles cleanly and faults somewhere unrelated much later.
- **Durable facts belong in the repo.** That is the consolidator's job, and if a round ends without
  one having run, the findings exist only in this conversation and die with it. Either run a
  consolidator or file what you learned with `git-bug`; do not leave a measured fact in the
  transcript alone.
