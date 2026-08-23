---
title: "Your first GkPlus script"
description: "Write a main.mjs that adds rows to Gunlok's own menus and draws a live panel over the running game."
weight: 20
audience: ["mod-author"]
---

# Your first GkPlus script

This tutorial is for a **mod author**: someone who wants to make Gunlok do something it
does not do, in JavaScript, without touching a compiler.

In this tutorial we write one file, `main.mjs`, into the GkPlus profile. By the end of it
Gunlok's main menu carries a row we added, its Options menu carries a switch we added, and
a panel of our own is drawn over the running game showing live numbers out of it.

We write about thirty lines of JavaScript. We build nothing and install no tools.

## Before we start

We need two things.

1. **GkPlus installed and working in Gunlok**, with the profile directory present. That is
   exactly what [Installing GkPlus](/tutorials/installing-gkplus/) produces: a
   `gkplus` folder next to `gl.exe`, containing `settings.json`.
2. **A text editor.** Notepad will do; anything with JavaScript syntax highlighting is
   nicer.

Open Explorer on the game folder (in the Steam library, right-click **Gunlok**,
**Manage**, **Browse local files**) and open the `gkplus` folder inside it. This is where
we work. Its path is usually:

```
C:\Program Files (x86)\Steam\steamapps\common\Gunlok\gkplus
```

## Step 1: create the entry module

In that `gkplus` folder, create a new file called exactly:

```
main.mjs
```

Watch the extension. If you use Notepad, choose **Save as type: All Files**, or
Windows saves the file as `main.mjs.txt` and GkPlus will not find it. In Explorer, turn on
**View → File name extensions** and check that the file really is `main.mjs`.

Put this in it, and save:

```js
// My first GkPlus entry module.

import { console } from "gk";

console.log("my first script loaded");

let clicks = 0;
let extras = false;

/** @type {import("gk").SetupMenus} */
export function setup_menus(menus) {
  menus.Main.add_item("Hello from my script", () => {
    clicks += 1;
    console.log(`clicked ${clicks} time(s)`);
  });

  menus.Options.add_toggle("Show extra info", extras, (item) => {
    extras = item.value === true;
  });
}
```

Three things to notice, because they are the shape of every GkPlus script.

- Everything comes from one module, `"gk"`. There is no global `console`: the one we
  imported writes to Gunlok's own console and to a debugger.
- `setup_menus` is called once, at startup, after the game has filled its own menus. The
  `menus` object arrives as its argument, and is not something we import.
- `menus.Main` and `menus.Options` are Gunlok's own menus, named. `add_item` appends a
  plain row; `add_toggle` appends an ON / OFF row which flips itself before our callback
  runs.

## Step 2: see the rows on the menus

Start Gunlok from Steam and let it reach the main menu.

Look down the main menu, below the game's own items. There is a new row:

```
Hello from my script
```

Only six rows are visible at once, so if the game's own items fill the menu, move the
selection down past them until the new row scrolls into view.

Select it and click. A menu sound plays and nothing else happens on screen yet. That is
expected; our callback only counts the click for now. **Click it twice**, so the counter
is at 2 for later.

Now go into **Options** and look at the bottom of that menu:

```
Show extra info                OFF
```

Click it. It flips:

```
Show extra info                ON
```

That is our second result, and it is worth pausing on: the game drew that row, the game
flipped it, and our callback ran. Leave it **ON**.

If neither row appeared, the script did not load. Press the backtick key (`` ` ``) to open
Gunlok's console, where GkPlus reports the problem, usually a missing file or a syntax
error. The most common cause is the file being called `main.mjs.txt`.

Quit the game from its menus.

## Step 3: add a panel

Open `main.mjs` again. Change the import line at the top, and add a second exported
function at the bottom. The whole file now reads:

```js
// My first GkPlus entry module.

import { actors, console, game } from "gk";

console.log("my first script loaded");

let clicks = 0;
let extras = false;

/** @type {import("gk").SetupMenus} */
export function setup_menus(menus) {
  menus.Main.add_item("Hello from my script", () => {
    clicks += 1;
    console.log(`clicked ${clicks} time(s)`);
  });

  menus.Options.add_toggle("Show extra info", extras, (item) => {
    extras = item.value === true;
  });
}

/** @type {import("gk").DrawGui} */
export function draw_gui(ImGui) {
  if (ImGui.Begin("My first panel")) {
    ImGui.Text(`${actors.count} actors are in this level`);
    ImGui.Text(`the menu row was clicked ${clicks} times`);

    if (extras) {
      ImGui.Text(`difficulty: ${game.difficulty}`);
      const god = ImGui.Checkbox("God mode", game.god_mode);
      game.god_mode = god.value;
    }
  }
  // End is unconditional: ImGui requires it even when Begin returns false.
  ImGui.End();
}
```

Save the file.

`draw_gui` is called once per frame while the overlay is open, and the `ImGui` object it
is handed is the only way to draw. Widgets return their new state rather than writing
through a pointer, which is why the checkbox reads
`const god = ImGui.Checkbox(...)` and then assigns `god.value`.

Keep `ImGui.End()` where it is, outside the `if`, and keep every `Begin` paired with an
`End`.

## Step 4: see the panel over the running game

Start Gunlok again.

At the main menu, click **Hello from my script** twice, as before. Go into **Options** and
click **Show extra info** so it reads **ON**. Then go back and start a game:
**Single Player**, **New Game**, **Medium**.

The level takes up to about a minute to load, and drops you on a mission briefing screen.
Press **Space** to leave it. The mission opens with a scripted camera sequence; let it
run. Wait until the level itself is on screen, which is the point at which our panel can
be drawn.

Now press **F11**.

A small window appears over the game, titled **My first panel**, with three lines and a
checkbox in it:

```
My first panel
  158 actors are in this level
  the menu row was clicked 2 times
  difficulty: medium
  [ ] God mode
```

The actor count will be a number of its own. It counts what is alive in this level right
now, so it changes as you play. The click count is the 2 we clicked at the menu, and the
last two lines are there because we left the Options switch **ON**.

Press **F11** again and the panel disappears. Press it once more and it comes back.

## Step 5: make the panel do something

Click the **God mode** checkbox in the panel.

The tick stays on, because the panel reads the value back out of the game every frame and
the game accepted it. Walk into a firefight and stay there.

Untick it before you go on. Then quit the game from its menus.

## What we have done

We wrote one file into the profile and got three things out of it: a row on Gunlok's main
menu, an ON / OFF switch on its Options menu, and a panel drawn over the running game that
reads live state and writes back into it.

That file is a complete GkPlus entry module. Everything larger is the same two exports
with more in them: `setup_menus` for anything that has to be registered at startup, and
`draw_gui` for anything drawn over the frame.

## Where to go next

- `examples/main.mjs` in the GkPlus repository is the same two functions, fully worked:
  camera sliders, world fog, effect buttons, a custom level, and the whole renderer panel.
  `examples/render-panel.mjs` beside it is the piece most worth copying.
- Copy `examples/jsconfig.json` and the `types/` folder into the profile next to your
  `main.mjs` and an editor will autocomplete the whole `"gk"` module and check your script
  as you type. `types/README.md` has the layout.
The [modding how-to guides](/how-to/modding/) carry on from here. In rough order of what a
script grows into:

- [How to draw an ImGui panel](/how-to/modding/draw-an-imgui-panel/): the overlay in
  earnest: windows, layout, the write-on-`changed` rule, and what makes a panel disappear
  for the rest of a session.
- [How to persist your own settings](/how-to/modding/persist-your-own-settings/): give
  your script a section of `settings.json` that survives a restart.
- [How to package a mod](/how-to/modding/package-a-mod/) and
  [How to ship a script with a mod](/how-to/modding/ship-a-script-with-a-mod/): turn this
  into something someone else can install.
- [How to author a script-defined level](/how-to/modding/author-a-script-defined-level/): a
  level with no `.gls` and no `.gcs` behind it.
- [How to drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/): type
  JavaScript into a running Gunlok instead of restarting to test a line.

Reference: the [JavaScript API](/reference/javascript/): every namespace, every method,
the `ImGui` interface, and the entry-module contracts you just wrote two of.

Explanation: [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why
there are two script phases, and why `menus` and `ImGui` arrive as arguments instead
of imports.
