---
title: "One settings file, many owners"
description: "Why settings.json is a shared document rather than GkPlus's own file, why nobody calls save(), and why the renderer's knobs are synchronised once a frame instead of written by their setters."
weight: 50
audience: ["mod-author", "developer"]
---

This page is for mod authors who want to know where their mod's configuration lives and why it
survives, and for developers who have wondered why persistence is arranged the way it is. It is
about one file, `<profile>/settings.json`, and three decisions that shaped everything around it.

## The file belongs to everybody

The top level of `settings.json` is one object per owner. GkPlus keeps its own settings under `core`;
a mod takes a key of its own beside it:

```json
{
  "core":  { "render": { "msaa": 4 } },
  "mymod": { "anything": [1, 2, 3] }
}
```

Nothing in GkPlus knows the schema of anything. A write re-serialises the **parsed document**, so a
section belonging to a mod this build has never heard of survives being rewritten by a build that
only understands `core` (`src/Settings.h`).

That requirement is the entire reason `json::Document` exists. The obvious implementation, parsing
into a typed struct and writing it back, deletes every key it does not know about, which for a
shared file means the first GkPlus save after a mod writes its configuration destroys it. The
alternative of giving each mod its own file was available and was not taken; one file means one thing
to copy when moving a profile between machines, and it means a profile is genuinely a complete
description of a launch.

## Scripts see the document, not a copy

In JavaScript, `settings` is an object tree over that document. Reading an object subtree hands out a
node bound to its path, and every write goes straight into the document. Nothing is cached on the
script side.

The object tree is the reason the dotted-path calls that preceded it existed at all.
A plain object parsed once would be a *second truth*, and there are already two writers to the same
keys: the Advanced Graphics page writes `core.render.*` from the front end while a script may be
holding the same object. Nothing cached means nothing to reconcile.

The dotted-path `get`/`set`/`remove` survive on the root anyway, because `set` creates intermediate
objects and `settings.a.b = 1` cannot. Arrays are a leaf, and are handed out **frozen**, since
their elements are not addressable by path and a `push` would otherwise silently vanish.

### The prototype bug, which was live

`json::Document` walks a path using **own-property** lookups only. That looks like a precaution.
It is a fix for a live defect: the nodes are ordinary JavaScript
objects, so every one of them inherits
`Object.prototype`, and a `JS_GetPropertyStr` walk made

- `settings.hasOwnProperty` resolve to a subtree node,
- `set("constructor.x", 1)` write into `Object` itself, and
- `remove("core.toString")` report a deletion it had not made.

JSON has no prototypes, so an inherited key is never part of a document, and the fix is to say so
(`src/Json.cpp`). It is worth recording because it is the kind of defect that reads as absurd in
hindsight and is invisible in review: every one of those three behaviours is *correct* JavaScript.

## Nobody calls save()

There is no save call in the API's expected usage. Two mechanisms cover it:

- `SaveSettled()` runs from the script host's per-frame hook and writes once a change has sat still
  for about a second, with a fifteen-second cap so a script writing every frame is still persisted.
- `SaveIfDirty()` runs first thing in `DllMain(DLL_PROCESS_DETACH)`.

The per-frame half is the load-bearing one, and that ordering is measured, not cautious.
Exiting Gunlok faults (`game_defects_notes.md` §4), which is why the mod filesystem's temp-tree
cleanup on the way out never ran once, and why the detach flush is placed *ahead of the first
destructor* rather than inside one, since a fault in an earlier destructor can stop the rest of
teardown entirely.

The save itself writes a temporary and moves it over the target, because a half-written file would
take every other owner's section with it.

## One table for the renderer's knobs, walked both ways

The renderer settings are the largest single group of stored values, and they are not persisted by
their setters. One table in `src/RenderSettings.cpp` is walked in both directions:

- `ApplyStored()` goes document → knobs at the engine's first intercepted file open, inside
  `WinMain` and therefore ahead of device creation, so a stored value is in place before the renderer
  initialises rather than a frame or a menu later.
- `SyncToSettings()` goes knobs → document once a frame, writing only what differs.

Comparing before writing is load-bearing. Without it the store would be dirty every frame and
the fifteen-second cap would become a file write every fifteen seconds forever.

The reason for per-frame synchronisation instead of a write inside each of roughly seventy setters
is that it catches a write from **any** source (a script, the Advanced Graphics page, the
REPL), where seventy setters would each have to remember. And the failure it replaced was real:
the front-end
page used to persist the eleven knobs it happened to expose, so clicking HDR on that page survived a
restart while setting the same knob from a script did not. Two writers, two rules, and no way to tell
from JavaScript which one you had. The page now has no persistence of its own; a click sets the knob
and the next frame's sync picks it up.

The keys follow the JavaScript spelling, dots and all: `render.ao.radius` is stored as
`core.render.ao.radius`. Each family's own switch is `<group>.enabled`, which is what lets a family
and its switch share a name.

## What deliberately does not persist

The measurement surface, `render.debug.*`, stores nothing, and that split is the same one the
bindings draw. A stored `draw_hide` would hide a draw on the next launch with nothing on screen to
say why, and `render.debug.stock` reads back *derived* rather than as a mode flag, so storing it
would be meaningless.

The count of knobs that *do* persist is derivable instead of written down, for the reasons in
[Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/). At the
time of writing:

```
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep -c '\.name = '
```

reports 79.

## Two ways a value can lie about itself

A knob whose *effective* value differs from what was asked for cannot be stored as read, or one
launch on a weaker machine erases the preference for every machine that can honour it. Two knobs have
this problem and reach the fix by different routes, which is worth noticing because the difference is
easy to mistake for inconsistency.

`tess.enabled` is flagged `sync = false`: restored from the file, never written back. It reads back
false on a device with no tessellation shader however it was set.

`msaa` has the same problem, since the effective count is clamped to what the device offers, but is
handled by the **environment-variable rule** instead, because it carries `env = "GKPLUS_VK_MSAA"`.

## An environment variable outranks the file, in both directions

The `GKPLUS_*` overrides are launch-time instruments: the switch you reach for when the setting you
need to change is the one stopping the game from starting. A stored value must therefore never
quietly beat one.

So a knob whose companion variable is set is skipped **both** ways: neither restored from the file
nor written back to it. Doing that in one place is what keeps the rule from depending on which
setter happened to latch its own environment-read flag. Four of the knobs carry such a variable
(`GKPLUS_VK_MSAA`, `GKPLUS_VK_PER_PIXEL_LIGHTING`, `GKPLUS_VK_HDR`, `GKPLUS_VK_BLOOM`), derivable
with:

```
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep '\.env = ' | grep -v nullptr
```

`GKPLUS_PROFILE` is the exception in shape, not in rule. It decides *which* file is read, so
there is nothing inside the file for it to lose to.

## The file also says what runs

`core.boot` and `core.script` name the two script modules, each resolved against the profile
directory and each settable to `""` to turn that phase off. This is what makes a profile a complete
description of a launch (settings, scripts and, through whatever the boot script reads, the mod
list) instead of a bag of settings sitting beside a hardcoded script path.

It is also why there is only one launch-time path knob. `GKPLUS_SETTINGS` and `GKPLUS_SCRIPT` were
the two halves of "point GkPlus somewhere else", and splitting them permitted a run that took its
settings from one place and its script from another: not a configuration anybody wanted, and it left
the mods directory behind in the install either way.

## Limits

**Loading is lazy and happens once**, on first access, so a file edited while the game runs is not
picked up unless something reloads it.

**A key containing a dot cannot be addressed** through the dotted-path calls, since paths go straight
to `json::Document` under the same rules.

**Nothing validates a section.** A mod writing nonsense into its own key gets nonsense back on the
next launch, and GkPlus has no opinion about it, which is the price of not knowing anybody's schema.

## Where to go next

- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): how `core.boot`
  and `core.script` are used.
- [Why nothing here writes down a count](/explanation/why-nothing-here-writes-down-a-count/): the
  habit behind the derivation commands above.
- [settings.json](/reference/data/settings-json/),
  [Renderer setting keys](/reference/data/render-settings-keys/) and
  [Environment variables](/reference/data/environment-variables/): the keys themselves.
- [How to persist your own settings](/how-to/modding/persist-your-own-settings/): writing a
  section of the document from a script.
