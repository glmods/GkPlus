---
name: git-bug
description: Track issues and future work for GkPlus with git-bug, the tracker embedded in the repo's own git objects. Use this whenever something should outlive the current session — the user says "file an issue", "open a bug", "track this", "remember to", "add a TODO", "we should do X later", "what's still open", "close that one", "triage the backlog" — and also, without being asked, when a real defect or deferred task surfaces while doing something else, since anything left only in conversation is lost when the context ends. Use it too for any git-bug command, any `refs/bugs` question, and before writing a bare "TODO" into source or notes.
---

# Issue tracking with git-bug

Issues live in the repo, in `refs/bugs/*` and `refs/identities/*`, as ordinary git objects. Nothing
is written to the working tree or the index — measured: `git status --porcelain` stays empty across
create, comment, label and close — so filing something mid-build never dirties the tree and never
collides with an edit in flight.

Everything here was measured against **git-bug v0.10.1** on this machine. Several items contradict
what the upstream docs imply; each one has the failing case that produced it.

## This is the durable memory for future work

A session's context ends. Anything agreed in conversation — "we should revisit the shadow atlas
rate", "that residual needs re-measuring after the vertex change" — is gone with it, and the next
session starts blind. **git-bug is where that survives.** It is not only for defects: a task, an
idea, a deferred cleanup, a suspicion worth confirming later all belong in it, because it is the one
store that is durable, greppable, travels with the repo through `push`/`pull`, and is visible to
both the user and to whatever session comes next.

So the trigger to file is broader than "found a bug". File when:

- work is **deferred** — the change was getting too big, so the rest got scoped out. That leftover
  is invisible tomorrow unless it is written down.
- a defect is **noticed in passing** while doing something else. Fixing it now derails the task;
  mentioning it in prose loses it.
- a measurement is **owed** — a claim currently resting on inference, which the standing rule here
  says must eventually be confirmed against the binary or a running game.
- the user says "later", "eventually", "remind me", or "we should".

Prefer this to a bare `TODO` in source: a `TODO` is invisible to `git-bug bug`, unattributed, and
undated. If a marker at the exact line genuinely helps, leave it *and* file the issue, and put the
short id in the comment. Prefer it also to holding the item in the reply — a sentence in a response
the user scrolls past is not storage.

What does **not** go here is durable knowledge. `*_notes.md` and `CLAUDE.md` keep how things work,
what was measured, and why a design is as it is. Those are true whether or not anyone acts on them,
so they do not belong in a store sorted by open/closed. The two meet when an issue is resolved:
**the fix goes in the code, what was learned goes in the notes, and the issue closes with a comment
pointing at both.**

## Identities: there are two, and only one is active

| Who | Short id | Full id |
|---|---|---|
| Francesco Bertolaccini | `6d37c90` | `6d37c9018323d9e1d2bba84adbf8a81d81e2b31f0e31c8ad452121c7ac006f76` |
| Claude | `fd8b9fb` | `fd8b9fbee3ca9e1e8bc93ac1e52482b167d597d9903cceb294967fa0f6114e08` |

The active author is a single value in `.git/config` — `git-bug.identity` — which is **per clone,
not per caller**. There is no per-command override: `git -c git-bug.identity=… ` and the
`GIT_CONFIG_COUNT`/`GIT_CONFIG_KEY_0` env form were both tested and neither changed the recorded
author. The config file is the only mechanism.

**The default is Francesco's, and it must be left that way**, because the user's own CLI and webui
writes take whatever is configured. To author as Claude, switch and restore in one shell invocation
with a trap, so an error mid-command cannot leave the repo authoring as the wrong person:

```bash
CLAUDE=fd8b9fbee3ca9e1e8bc93ac1e52482b167d597d9903cceb294967fa0f6114e08
PREV=$(git config --local git-bug.identity)
trap 'git config --local git-bug.identity "$PREV"' EXIT
git config --local git-bug.identity "$CLAUDE"

git-bug bug new --non-interactive -t "…" -m "…"
```

Verified: the bug came out authored by Claude and the trap restored the default even when a later
command in the chain failed. Keep the switch to the narrowest possible span — one invocation — and
never leave it set across tool calls, since the user may write from the webui at any moment.

Attribution is worth the trouble: it is what later says whether a claim came from the user's own
observation of the game or from an agent's inference, which changes how much it should be trusted.

## Before anything: two things that block every command

**A running `webui` or `termui` holds an exclusive lock.** git-bug takes a per-repo lock in
`.git/git-bug/lock` containing the owning PID; while `git-bug webui` is up, every CLI command blocks
**indefinitely** — no error, no timeout, just a hang until the tool call is killed. Measured here:
a `webui` started at 22:34 silently ate a `user new` and a plain `user`. Diagnose before retrying:

```bash
tasklist //FI "IMAGENAME eq git-bug.exe"     # is another git-bug alive?
cat .git/git-bug/lock                        # which PID holds it
```

If the PID is alive it is almost certainly the user's own UI — **ask them to close it, do not kill
it.** If the PID is *not* alive, a previous run was killed and left the lock stale; remove it:

```bash
rm -f .git/git-bug/lock
```

Because of this, put a `timeout` on git-bug calls (`timeout 60 git-bug …`) so a lock hang costs
seconds instead of the whole tool budget.

**The Windows read-only empty blob.** Every write fails, reads are fine:

```
Error: rename ...\.git\objects\pack\tmp_obj_120055519 ...\.git\objects\e6\9de29bb2d1d6434b8b29ae775ad8c2e48c5391: Access is denied.
```

`e69de29…` is the SHA-1 of the *empty blob*, which git-bug writes as part of its own trees.
Canonical git writes loose objects read-only (0444); git-bug uses go-git, which renames its temp
file over the destination, and Windows refuses that when the destination is read-only. On Linux the
rename just succeeds, which is why this is not upstream. Reproduced from scratch: `git add` an empty
file, and every git-bug write fails identically. Either fix works, both verified:

```bash
chmod u+w .git/objects/e6/9de29bb2d1d6434b8b29ae775ad8c2e48c5391
```

```bash
git gc --prune=now
```

GkPlus has no loose `e6/9de29…` today, so writes work — but any `git add` of an empty file rearms
it. Recognise the error rather than re-diagnosing it: it is not a permissions problem with the repo.

## The commands

**`git bug X` works only when `X` is a top-level git-bug command.** `git` execs external
subcommands by stripping the first word, so `git bug X` becomes `git-bug X`. That is fine for
`user`, `label`, `push`, `pull`, `version`, `webui`, `termui`, `bridge` and `wipe`, which sit at the
top level — the user's own `git bug webui` and `git bug push` are correct. It fails for everything
under the `bug` namespace, because `git bug new` becomes `git-bug new`:

```
git bug user   -> List identities                          # works
git bug new    -> Error: unknown command "new" for "git-bug"   # needs `git bug bug new`
```

Measured as failing this way: `new`, `show`, `comment`, `select`, `status`, `title`, `rm`, and a
bare query. git-bug's **own help examples are stale on exactly this point** — `git bug select 2f15`
and `git bug status:open sort:edit-desc` are printed by `--help` and all of them error verbatim;
they date from before v0.10 moved those commands under `bug`.

So write `git-bug bug new`. It is the one form that works for every command, and it avoids having
to remember which half of the tree a command is in.

Always pass `--non-interactive` on anything that accepts it. With `-t` and `-m` supplied git-bug
does not prompt, but the flag is the guarantee: without it a missing field falls back to `$EDITOR`,
and an editor waiting on a terminal that will never answer hangs the call.

```bash
# file one
git-bug bug new --non-interactive -t "Title" -m "Body"

# multi-paragraph body — see the -F trap; the title comes from the first line
printf 'Title here\n\nFirst paragraph.\n\nSecond paragraph.\n' | git-bug bug new --non-interactive -F -

# read
git-bug bug                                   # id, status, title — one line each
git-bug bug 'status:open' 'sort:edit-desc'
git-bug bug -l area-vulkan --status open      # flags, for values containing spaces
git-bug bug "residual"                        # full-text search over titles and bodies
git-bug bug show 61f24a5
git-bug bug show --field status 61f24a5       # single field, no parsing
git-bug bug -f json                           # everything, machine-readable

# write
git-bug bug comment new 61f24a5 --non-interactive -m "Cause found: …"
git-bug bug label new 61f24a5 area-vulkan kind-bug
git-bug bug label rm 61f24a5 needs-measurement
git-bug bug title edit 61f24a5 --non-interactive -t "Better title"
git-bug bug status close 61f24a5
git-bug bug status open 61f24a5
```

Progress chatter (`Building cache...`) goes to **stderr**; stdout is clean, so `-f json` and
`-f plain` pipe straight into a parser. `comment new`, `status close` and `label rm` print nothing
on success — silence is the success signal, so check the exit code, not the output.

## Traps

**`-F` silently discards `-t`.** `bug new -t "Second bug" -F -` fed `line one\n\nline two` produced
a bug titled *"line one"*; the `-t` was dropped with no warning. The file form is `title\n\nbody`,
always. Use `-t`/`-m` together, or `-F` alone with the title on its first line — mixing them
mis-titles the issue and nothing tells you.

**Quoted values in the query language never match.** `label:"needs repro"` returns nothing while
`-l "needs repro"` returns the bug. Isolated: not the colon, a plain space is enough —
`label:area-renderer` matches, `label:"needs repro"` does not. Hence: keep labels **hyphenated and
space-free**, and when a value does contain a space use the flag form (`-l`, `-t`, `-a`).

**`select` is sticky, per-clone, and invisible.** `git-bug bug select <id>` stores state in
`.git/git-bug/select`, and every later `bug show` / `comment new` / `status close` with no id
targets it, across sessions, for as long as it is set. A selection made an hour ago silently
retargets a close. Pass ids explicitly; never set a selection.

**Plain `git push` carries no issues.** Measured: after `git push origin main` the remote had only
`refs/heads/main`. Bugs move only via `git-bug push` / `git-bug pull`.

**`user new` creates a duplicate rather than failing.** It sets `git-bug.identity` only when none is
set, so on this clone it is safe — but on a *fresh* clone it both creates a second identity with the
same name and email and makes it active, splitting attribution in two. See the clone recipe below.

**Ids are prefixes, like commits.** `61f24a5` is the 7-char human id of a 64-char id; prefixes work
wherever an id is accepted. Report the short form; take the full one from `-f json`'s `id` when
something must be unambiguous. Comments have *their own* ids — `bug comment edit` takes a
COMMENT_ID from `bug show -f json`'s `comments[].human_id`, not a bug id.

## Setting up another clone

An identity is required before any write **and** before `git-bug pull`, which merges and therefore
writes; a fresh clone with no identity fails with `merge error: No identity is set`. Since
`user new` would create a duplicate, fetch and adopt the existing one first:

```bash
git fetch origin 'refs/identities/*:refs/identities/*'
git-bug user                                    # read the id you want
git-bug user adopt 6d37c9018323d9e1d2bba84adbf8a81d81e2b31f0e31c8ad452121c7ac006f76
git-bug pull origin
```

Verified end to end: exactly one identity, correct attribution.

## Conventions for this repo

**Labels are hyphenated and space-free**, because that is the only form the query language can
filter on. Use areas the codebase already has rather than inventing a taxonomy:

- `area-vulkan`, `area-script`, `area-vfs`, `area-profiler`, `area-render`, `area-gls`,
  `area-blender`, `area-pbr`, `area-lightmap`, `area-build`, `area-re`
- `kind-bug`, `kind-feature`, `kind-task`, `kind-question`
- `game-defect` — a defect in **Gunlok itself**, reproducing without GkPlus. The repo already draws
  that line hard (`game_defects_notes.md` exists so nobody re-blames our hooks); the label keeps it
  drawn in the tracker.
- `needs-measurement` — the issue rests on an inference nobody has confirmed against the binary or a
  running game.

**Titles state the defect or the task, not the area.** "Shadow atlas re-bakes every frame when
map_shadow_rate is 0" beats "shadow bug" — the list view shows only id, status and title, so the
title is the whole index.

**Bodies carry the evidence, because an issue here is usually a measurement.** What was run, what
happened, what was expected, and the file:line or address. "PN triangles look wrong" costs a full
re-derivation later; naming the pass, the setting and the residual does not. Where a fact is already
written down, cite the notes file and section instead of restating it. For a *deferred task* rather
than a defect, the equivalent is: what should be done, why it was not done now, and what it depends
on — a task with no "why not now" reads later as an oversight.

## Sync

GkPlus has two remotes — `origin` (github.com:frabert/GkPlus.git) and `minipc`. Pass one explicitly:

```bash
git-bug pull origin        # before triaging, so the view is current
git-bug push origin        # after filing or closing
```

Sync is not automatic; nothing pushes bug refs as a side effect of a normal `git push`, so an issue
filed and never pushed exists only on this machine. Bug refs merge rather than conflict — the data
model is an append-only DAG per bug, so two machines editing the same issue both land. Never try to
fix a bug ref with `git push --force` or by rewriting refs by hand; there is no rebase story here
and a force-push discards whatever the other side appended.

## When the user asks for the state of things

Read before answering — `git-bug bug 'status:open'` is cheap, and answering the backlog from memory
is how a closed issue gets reported as open. For a summary across many issues use `-f json` and
aggregate rather than paging through `bug show`.
