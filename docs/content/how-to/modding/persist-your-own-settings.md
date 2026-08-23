---
title: "How to persist your own settings"
description: "Keep a mod's or a script's state in settings.json without disturbing anybody else's section."
weight: 120
audience: ["mod-author"]
---

This guide shows a **mod author** how to store state that outlives a launch.

`<profile>\settings.json` is a **shared** repository, not GkPlus's own file: the top level is one
object per owner. GkPlus keeps its settings under `core`; take a key of your own beside it.

## Read and write a section

`settings` is the document itself, not a copy:

```js
import { settings } from "gk";

settings.mymod ??= {};                 // create the section once
settings.mymod.window = { x: 10, y: 20 };
settings.mymod.window.x = 40;          // straight into the document
const launches = settings.mymod.launches ?? 0;
settings.mymod.launches = launches + 1;
```

Reading an object subtree hands back another live view of it, so there is no snapshot to push
back and nothing to reconcile - the front end's Advanced Graphics page writes `core.render.*`
while your script holds the same object, and both see the same value.

## Do not call `save()`

The file catches up on its own: a change is written about a second after the last one settles,
with a cap so a script writing every frame still gets saved, and again when the game exits.
`save()` is only for a change that must survive a *crash*.

Because a write re-serialises the parsed document, a section belonging to a mod the running build
has never heard of survives being rewritten. Yours survives somebody else's write for the same
reason.

## Use the dotted-path calls when you need a default or an intermediate

```js
settings.get("mymod.window", { x: 0, y: 0 });   // value, or the fallback
settings.set("mymod.window.x", 40);             // creates every intermediate object
settings.remove("mymod.window");
```

`set` is worth reaching for because `settings.a.b = 1` needs `a` to exist, exactly as it would
for any object, while `set("a.b", 1)` creates it.

## Two limits, both of which throw

- **A key cannot contain a dot**, because keys are the path separator. `settings["my.mod"]` would
  mean `mod` inside `my`; such a key reads as absent and refuses to be written. Pick a
  dot-free section name.
- **An array is a value, and the one you get back is frozen.** Its elements are not addressable
  by path, so `list.push(x)` would have nowhere to go. Assign a whole array instead:

  ```js
  settings.mymod.recent = [...(settings.mymod.recent ?? []), name].slice(-10);
  ```

Also: a section whose name collides with one of the document's own members (`get`, `set`, `all`,
`path`, …) shadows it. Keep to a key of your own.

## Let a user edit the file

`settings.path` reports where it is. The file is written atomically - a temporary is moved over
the target - so a half-written file cannot take everybody else's section with it.
`settings.reload()` re-reads from disk and discards anything written since the last save.

## Drive your mod's list from it

Config a boot script reads is the common case:

```json
{
  "core": { "boot": "boot.mjs", "script": "main.mjs" },
  "boot": { "mods": ["mods/10-tweaks", "mods/20-hi-res.zip"] }
}
```

Nothing in GkPlus knows the `boot` key - it belongs to whichever script reads it. See
[How to enable and order mods](/how-to/modding/enable-and-order-mods/).

## Reference and background

- [settings.json](/reference/data/settings-json/): the ownership rule, when the file is
  written, and what outranks it.
- [`settings`](/api/js/variables/gk.gk.settings.html): the live object tree and the
  dotted-path accessors, in the generated JavaScript reference.
- [The profile directory](/reference/data/profile-directory/): which file is being written
  into.
- [One settings file, many owners](/explanation/one-settings-file-many-owners/): why your
  section survives a rewrite by a build that has never heard of your mod, and why nobody
  calls `save()`.
