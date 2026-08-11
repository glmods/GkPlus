---
name: git-bug
description: Track issues for GkPlus with git-bug, the bug tracker embedded in the repo's own git objects. Use this whenever work needs to be recorded rather than done now — the user says "file an issue", "open a bug", "track this", "add a TODO", "what's still open", "close that one", "triage the backlog", or asks what is known to be broken — and also when you notice a real defect while doing something else and want it captured instead of forgotten. Use it too for any git-bug command, any `refs/bugs` question, and before writing a bare "TODO" into source or notes, since a tracked issue is what this repo uses instead.
---

# Issue tracking with git-bug

Issues live in the repo, in `refs/bugs/*` and `refs/identities/*`, as ordinary git objects. Nothing
is written to the working tree or the index — measured: `git status --porcelain` stays empty across
create, comment, label and close — so filing an issue mid-build never dirties the tree and never
collides with an edit in flight. That is the property that makes it usable here at all: this repo's
work is long single-threaded sessions against a game binary, and stopping to open a browser tab
costs more than the note is worth.

Everything below was measured against **git-bug v0.10.1** on this machine, in throwaway repos. The
traps section is not upstream documentation — several items contradict what the docs imply, and each
one has the failing case that produced it.

The binary is `git-bug`; `git bug` works too, as git finds it on PATH. Use `git-bug` in scripts, it
is unambiguous.

## Before anything: this repo's Windows trap

**Symptom.** Every write fails, reads are fine:

```
Error: rename ...\.git\objects\pack\tmp_obj_120055519 ...\.git\objects\e6\9de29bb2d1d6434b8b29ae775ad8c2e48c5391: Access is denied.
```

**Cause.** `e69de29…` is the SHA-1 of the *empty blob*. git-bug writes it as part of its own trees.
Canonical git writes loose objects read-only (mode 0444), and go-git — which git-bug uses instead of
shelling out — renames its temp file over the destination, which Windows refuses when the
destination is read-only. On Linux the rename would just succeed, which is why this is not in the
upstream docs. Reproduced from scratch: `git add` an empty file, then any `git-bug` write fails, and
it fails identically every retry.

**Fix**, either one, both verified:

```bash
chmod u+w .git/objects/e6/9de29bb2d1d6434b8b29ae775ad8c2e48c5391
```

```bash
git gc --prune=now
```

`git gc` works because packing removes the loose object, and git-bug then writes its own writable
copy. As of this writing GkPlus has no loose `e6/9de29…`, so writes work — but any `git add` of an
empty file recreates it, so recognise the error rather than re-diagnosing it. It is not a
permissions problem with the repo and not a corrupted index.

## Setup, once per clone

An identity is required before **any** operation that writes, and — this is the part that surprises
— before `git-bug pull`, which merges and therefore writes. A fresh clone with no identity fails
with `merge error: No identity is set`.

**First machine / no identities exist yet:**

```bash
git-bug user new --non-interactive -n "Francesco Bertolaccini" -e "francesco.bertolaccini@trailofbits.com"
```

**Any later clone — do not run `user new` again.** It creates a *second* identity with the same name
and email (measured: `69aaedc` and `a3e3614`, both "Francesco Bertolaccini"), and the local one wins,
so that machine's issues are attributed to a different person and the history splits in two. The
identity has to be fetched and adopted before the first pull, which is awkward because pull is what
would normally fetch it:

```bash
git fetch origin 'refs/identities/*:refs/identities/*'
git-bug user                       # read the id of the existing identity
git-bug user adopt <full-identity-id>
git-bug pull origin
```

That sequence leaves exactly one identity and correct attribution — verified end to end.

## The commands

Always pass `--non-interactive` on anything that accepts it. With `-t` and `-m` supplied git-bug does
not prompt, but the flag is the guarantee: without it, a missing field falls back to `$EDITOR`, and
an editor waiting on a terminal that will never answer hangs the tool call until it times out.

```bash
# file one
git-bug bug new --non-interactive -t "Title" -m "Body"

# multi-paragraph body — see the -F trap below, the title comes from the first line
printf 'Title here\n\nFirst paragraph.\n\nSecond paragraph.\n' | git-bug bug new --non-interactive -F -

# read
git-bug bug                                   # id, status, title — one line each
git-bug bug 'status:open' 'sort:edit-desc'
git-bug bug -l area-vulkan --status open      # flags, for values with spaces
git-bug bug "residual"                        # full-text search over titles and bodies
git-bug bug show 88298e4
git-bug bug show --field status 88298e4       # single field, no parsing
git-bug bug -f json                           # everything, machine-readable

# write
git-bug bug comment new 88298e4 --non-interactive -m "Cause found: …"
git-bug bug label new 88298e4 area-vulkan kind-bug
git-bug bug label rm 88298e4 needs-measurement
git-bug bug title edit 88298e4 --non-interactive -t "Better title"
git-bug bug status close 88298e4
git-bug bug status open 88298e4
```

Progress chatter (`Building cache...`) goes to **stderr**; stdout is clean, so `-f json` and
`-f plain` pipe straight into a parser with no filtering. `comment new`, `status close` and
`label rm` print nothing on success — silence is the success signal, check the exit code, not the
output.

## Traps

**`-F` silently discards `-t`.** `bug new -t "Second bug" -F -` fed `line one\n\nline two` produced a
bug titled *"line one"* — the `-t` was dropped without a warning. The file form is
`title\n\nbody`, always. So either use `-t`/`-m` together, or use `-F` alone and put the title on its
first line. Mixing them mis-titles the issue and nothing tells you.

**Quoted values in the query language never match.** `label:"needs repro"` returns nothing, while
`-l "needs repro"` returns the bug. Isolated: it is not the colon, a plain space is enough —
`label:area-renderer` matches, `label:"needs repro"` does not. Two consequences: keep labels
**hyphenated and space-free** so the query language can address them, and when a value does contain a
space, use the flag form (`-l`, `-t`, `-a`) rather than the query form.

**`select` is sticky, per-clone, and invisible.** `git-bug bug select <id>` stores state in
`.git/git-bug/select`, and every later `bug show` / `comment new` / `status close` with no id targets
it — across sessions, for as long as it is set. A selection made an hour ago silently retargets a
close. Pass the id explicitly every time; never rely on selection, and do not set one.

**Plain `git push` carries no issues.** Measured: after `git push origin main` the remote had only
`refs/heads/main`. Bugs move only via `git-bug push` / `git-bug pull`, which sync `refs/bugs/*` and
`refs/identities/*`.

**Never launch `termui` or `webui`.** Both are interactive full-screen programs that do not exit on
their own; they are for the user, not for a tool call.

**Ids are prefixes, like commits.** `88298e4` is the 7-char human id of a 64-char id. Prefixes work
wherever an id is accepted. Report the short form to the user, and take the full one from
`-f json`'s `id` field when something needs to be unambiguous.

**Comments have their own ids.** `bug comment edit` takes a COMMENT_ID, not a bug id; get it from
`bug show -f json`, whose `comments[].human_id` differs from the bug's.

## Conventions for this repo

**Labels are hyphenated and space-free**, because that is the only form the query language can
filter on. Use the areas the codebase already has rather than inventing a taxonomy:

- `area-vulkan`, `area-script`, `area-vfs`, `area-profiler`, `area-render`, `area-gls`,
  `area-blender`, `area-pbr`, `area-lightmap`, `area-build`, `area-re`
- `kind-bug`, `kind-feature`, `kind-task`, `kind-question`
- `game-defect` — a defect in **Gunlok itself**, reproducing without GkPlus. This repo already draws
  that line hard (`game_defects_notes.md` exists so nobody re-blames our hooks), and the label keeps
  it drawn in the tracker.
- `needs-measurement` — the issue rests on an inference nobody has confirmed against the binary or a
  running game. Worth its own label here, since the standing rule is that a claim is only as good as
  the measurement behind it.

**Titles state the defect, not the area.** "Shadow atlas re-bakes every frame when map_shadow_rate is
0" beats "shadow bug" — the list view shows only id, status and title, so the title is the whole
index.

**Bodies carry the evidence, because a bug here is usually a measurement.** What was run, what
happened, what was expected, and the file:line or address. An issue that says "PN triangles look
wrong" costs a full re-derivation later; one that names the pass, the setting and the residual does
not. Where a fact is already written down, cite the notes file and section rather than restating it.

### What belongs in git-bug, and what does not

This repo keeps its durable knowledge in `*_notes.md` and `CLAUDE.md`, and that is not changing.
git-bug holds **work items** — things that are open, that someone might close.

- **git-bug**: a defect to fix, a feature to build, a measurement to take, a suspicion to confirm,
  a cleanup deferred out of a change that was getting too big.
- **`*_notes.md` / `CLAUDE.md`**: how something works, what was measured, why a design is the way it
  is, a trap that will bite the next person. These are true whether or not anyone acts on them, so
  they do not belong in a tracker that sorts by open/closed.

The two meet when an issue is resolved: **the fix goes in the code, what was learned goes in the
notes, and the issue closes with a comment pointing at both.** Closing without writing the finding
down is how a measurement gets paid for twice.

Prefer an issue to a bare `TODO` in source. A `TODO` is invisible to `git-bug bug` and to anyone not
reading that file; if a comment marking the spot is genuinely useful, leave it *and* file the issue,
and put the short id in the comment.

## Sync

GkPlus has two remotes — `origin` (github.com:frabert/GkPlus.git) and `minipc`. `git-bug push` and
`git-bug pull` take a remote argument and it is worth passing explicitly:

```bash
git-bug pull origin        # before triaging, so the view is current
git-bug push origin        # after filing or closing
```

Bug refs merge rather than conflict — the data model is an append-only DAG per bug, so two machines
editing the same issue both land and nothing needs resolving. Do not try to fix a bug ref with
`git push --force` or by rewriting refs by hand; there is no rebase story here and force-pushing a
bug ref discards whatever the other side appended.

Sync is not automatic. Nothing pushes bug refs as a side effect of a normal `git push`, so an issue
filed and never pushed exists only on this machine.

## When the user asks for the state of things

Read before answering — `git-bug bug 'status:open'` is cheap, and answering the backlog from memory
is how a closed issue gets reported as open. For a summary across many issues use `-f json` and
aggregate, rather than paging through `bug show` one at a time.
