---
title: "Why nothing here writes down a count"
description: "Four rosters in one file drifted at once, one of them contradicting the same file two hundred lines later. The fix was not four repairs."
weight: 90
audience: ["developer"]
---

This page is for developers who have noticed that the project's own documentation keeps replacing
numbers with the shell command that produces them, and wondered whether that is fussiness. It is
about one small habit, why it was adopted, and the larger principle it turns out to be an instance
of.

## What happened

`CLAUDE.md` is the project's own overview: architecture, conventions, a table of every source file.
It carries rosters (the hook-installing subsystems, the JavaScript namespaces, the persisted
renderer knobs), and it carried counts of them, written down.

Four of them were wrong at the same time, and the failure was discovered as a group rather than one
by one:

- The subsystem roster listed an `image::ImageCodecSystem` that exists in no translation unit,
  while the same file's table of source files correctly described that codec as a *registration*
  made from the file hooks' first intercepted open. One file, two accounts, two hundred lines apart.
- The same roster omitted `ActorArgSystem`, which `entry.cpp` does construct.
- The renderer settings section claimed two knobs carry `sync = false` when only one does. The
  *reason* given was right and the *mechanism* was not: the second knob reaches the same behaviour by
  the environment-variable rule, which the same section documents separately.
- A tool that prints its own namespace count was recorded as covering 25 namespaces. It covers 36.

Every one of those is the same failure. A fact was stated in prose beside the code that determines
it, the code moved, and the prose did not.

## Why the fix was not four repairs

Repairing four numbers produces four correct numbers and the same mechanism that made them wrong.
What went in instead was a section of the file that says how to *derive* each one: eight one-line
commands, each with its expected value, each run before being written down:

```
sed -n '/Namespaces\[\]/,/^};/p' src/JsGk.cpp | grep -oE '"\w+"\s*,' | wc -l
grep -c '^GK_ACTOR_CLASS' src/ActorClasses.inc.h
sed -n '/struct Subsystems/,/};/p' src/entry.cpp
```

The commit that did this is titled "A count that is written down drifts, so the file now says how to
derive each one", which is the argument in one line.

There is a nice miniature of the problem inside the fix. The first derivation attempted was itself
wrong: `grep -c` counts *lines*, and the namespace table puts several entries on some of them. A
command that is right by inspection is not right, either. The discipline is to run it and read the
answer, and that is the discipline the numbers it replaces had lacked.

## The two rules that came out of it

**Prefer a derivable count to a written one.** If the number has to appear, the command that produces
it appears with it, and the number is what that command actually printed at the time of writing.

**Where a list only ever grows, do not number it at all.** The Blender addon's design record declines
to number its design list for precisely this reason, having watched a count go stale. A numbered list
makes a promise about its own length that nothing enforces; an unnumbered one makes no promise and
loses nothing.

A third rule has more teeth: **a name in the documentation that greps to nothing in `src/` is a
defect in the documentation.** That makes prose partially checkable, which is unusual
and worth noticing. `ImageCodecSystem` had survived in two files attached to an anchor that had been
superseded; after the repair, it greps to nothing anywhere, and that fact is itself the test.

## The same instinct, elsewhere in the project

Once you see it as "if two places state the same fact, make one derive it or make disagreement fail
loudly", it turns up all over this codebase, in forms that have nothing to do with documentation:

**Shader ABI.** A set of structs is declared twice, once in C++ and once in Slang. The roster is
the `PAIRS`
list in `src/gen-shader-abi.py`, and it is longer than the last prose description of it said.
That generator parses the Slang declarations and emits an `offsetof` per field and a `sizeof` per
struct into the build tree, so adding or reordering a
field on one side only is a compile error naming the field. Hand-written asserts cannot substitute: a
*permutation* preserves `sizeof` and every assert pinning a field after the disturbance. Not having
this cost two sections of the renderer notes: three knobs silently stuck on, and a lost device.

**The scripting surface.** The hand-written TypeScript declarations describe the `"gk"` module, and a
type-checker proves they are self-consistent and that the shipped scripts compile against them. It
cannot prove they match the *bindings*, because the bindings are C++. A separate checker compares
every binding table against its interface in both directions, and both failure directions have
actually occurred: a method stayed declared after the C++ entry was deleted, and one namespace
accumulated fifty undeclared members behind an index signature.

**Generated headers are not committed.** The shader SPIR-V and the ABI asserts are build products in
the binary tree. A committed generated file can be stale, can be reviewed as though it were source,
and can conflict on a merge. A *believed*-stale one produced a session of byte-identical
screenshots. The price, paid deliberately and stated as a hard error at configure time, is that a
shader compiler and Python are now build requirements.

**Struct layouts.** The `static_assert`s in the decompiled mirrors are the same move applied to the
game binary: the layout is stated once in a form the compiler checks, rather than twice in a form
nobody does.

In every case the shape is identical. Two statements of one fact, and a mechanism that makes their
disagreement loud instead of quiet.

## The cost, stated fairly

A derivation command is worse than a number for a reader who just wants to know. It assumes a
checkout, a shell, and a willingness to run something; it is longer; and it is opaque to anyone
reading on a phone. A count in prose is instantly legible and occasionally wrong; a command is always
right and never immediate.

This project pays that cost because its documentation is read overwhelmingly by people who have the
checkout open and are about to change the thing being counted, the audience for whom the stale
number is a live hazard rather than a curiosity. For a user-facing manual the trade would probably
run the other way.

There is also a limit worth admitting: this only works where a fact *has* a derivation. Most of what
is interesting in this repository (why the renderer seam is where it is, what a residual means,
which alternatives were rejected) has no command that produces it, and stays as prose that can rot
like any other. The rosters were the easy case, which is why they were fixed first.

## What this means for these pages

Every number in this documentation set is either derived at writing time with the command shown
beside it, or absent. Where a figure would have been useful and is not derivable (how many functions
in the Ghidra database are named, how many chunk types the RIF format has), the page names the file
that holds the current figure instead of copying it.

This project has already paid for that lesson twice: the copy is where the drift lives.

## Where to go next

- [One settings file, many owners](/explanation/one-settings-file-many-owners/): where the knob
  count above is derived, and what it counts.
- [Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/): the same discipline applied to
  claims about the game binary.
- [Design records index](/reference/data/notes-index/): the files this habit was learned
  in, and what each of them answers.
