---
title: "Installing GkPlus"
description: "Put GkPlus into a stock Steam copy of Gunlok, confirm on screen that the game is running under it, and change a setting in the profile it writes."
weight: 10
audience: ["player"]
---

# Installing GkPlus

This tutorial is for a **player**: someone who owns Gunlok on Steam and wants to play it
with GkPlus.

In this tutorial we take a stock Steam copy of Gunlok, install GkPlus into it, read the
version stamp on the main menu to confirm the game is running under GkPlus, find the
profile directory GkPlus writes into the game folder, and change one setting in it and
watch the change survive a restart.

Everything here is done with Windows Explorer, Steam, and a plain text editor. We write no
code and build nothing.

## Before we start

We need three things.

1. **Gunlok, installed through Steam.** Any state will do; freshly installed is fine.
2. **A copy of `d3d8.dll` built from GkPlus.** There are no downloadable releases of
   GkPlus: the only way to get this file is to build it. If you do not have one, work
   through [Building GkPlus](/tutorials/building-gkplus/) first, which ends with the file
   at `build\Debug\d3d8.dll`, and then come back here.
3. **A text editor** that can open a `.json` file. Notepad is enough.

Put the `d3d8.dll` somewhere you can find it, and keep this page open.

## Step 1: find the game folder

In the Steam library, right-click **Gunlok**, choose **Manage**, then **Browse local
files**. Explorer opens the game folder.

Look at what is in it. You will see `gl.exe`, and folders called `Graphics`, `RIF`,
`Sound` and `scripts`. The path is usually:

```
C:\Program Files (x86)\Steam\steamapps\common\Gunlok
```

Notice two things about this folder, because both change during the tutorial:

- there is **no** `d3d8.dll` in it;
- there is **no** `gkplus` folder in it.

Leave this Explorer window open. We come back to it three times.

## Step 2: put GkPlus in the game folder

Copy your `d3d8.dll` into that folder, right next to `gl.exe`.

That is the whole installation. There is no installer, no registry entry, and nothing to
configure yet.

Windows may ask for administrator permission, because the Steam folder is under
`Program Files (x86)`. Allow it.

The folder now contains `gl.exe` and `d3d8.dll` side by side.

## Step 3: start the game and read the version stamp

Start Gunlok from Steam the way you normally would: select it in the library and press
**Play**.

The intro movie plays first. Wait for it, and let the game settle on the main menu.

Now look at the **bottom-left corner of the screen**, below the menu. A stock Gunlok
prints its own version there:

```
v1.3 DX8
```

Under GkPlus that line reads:

```
GkPlus - d3d9 (Debug)
```

That is our first result: the game is running with GkPlus loaded. The word after the dash
is the renderer GkPlus resolved, and `(Debug)` is there because the `d3d8.dll` we built
came out of the default build configuration. If your line says `GkPlus - d3d9` with no
`(Debug)` after it, that is fine too, and means somebody built the DLL in a different
configuration.

If the line still reads `v1.3 DX8`, the DLL is not next to `gl.exe`. Quit, check Step 2,
and start again.

## Step 4: play far enough to reach a level

From the main menu, choose **Single Player**, then **New Game**, then the **Medium**
difficulty.

The level takes a while to load, up to about a minute. You then land on a mission
briefing screen. Press **Space** to leave it. The mission opens with a scripted camera
sequence; let it run.

Stay in the level for a few seconds. That is all we need: GkPlus keeps its settings up to
date while a level is running, and this is what makes the profile directory appear in the
next step.

## Step 5: quit the game and find the profile

Quit Gunlok using the game's own menus. Let the game close itself, and do not end it from
Task Manager, because GkPlus writes its settings file on the way out.

Go back to the Explorer window from Step 1 and press **F5** to refresh it.

A folder called `gkplus` has appeared next to `gl.exe`. Open it. Inside is one file:

```
settings.json
```

That folder is the **profile**: it is where GkPlus keeps everything it remembers about
this installation, and the game wrote it for us on that first run.

## Step 6: look inside the settings file

Open `gkplus\settings.json` in your text editor.

It is a JSON file, and everything GkPlus itself owns lives under a top-level key called
`core`. It starts like this:

```json
{
  "core": {
    "render": {
      "specular": true,
```

Scroll down until you find the block named `"ao"`. It looks like this, and the block carries
a few more numbers after the ones shown here:

```json
      "ao": {
        "enabled": false,
        "map_only": true,
        "taps": 64,
        "radius": 3,
```

Every one of those values was written by the game itself, from the settings it was
actually running with.

## Step 7: change a setting

In that `"ao"` block, change `"taps": 64` to `"taps": 16`:

```json
      "ao": {
        "enabled": false,
        "map_only": true,
        "taps": 16,
        "radius": 3,
```

Save the file and close the editor. Keep the change exactly as shown: a stray comma or a
missing quote makes the file unreadable, and GkPlus would then start from scratch and
write a fresh one.

## Step 8: confirm the game keeps it

Start Gunlok from Steam again, and repeat Step 4: **Single Player**, **New Game**,
**Medium**, **Space** at the briefing, wait for the level.

Then quit the game from its menus, as in Step 5.

Now open `gkplus\settings.json` again and find the `"ao"` block:

```json
      "ao": {
        "enabled": false,
        "map_only": true,
        "taps": 16,
```

It still says `16`. The game read our edit at startup, ran with it, and wrote the same
value back when it closed. Had it ignored the file, this number would have gone back to
`64`.

## What we have done

We installed GkPlus into a stock Steam copy of Gunlok by copying a single file, confirmed
it was running by reading `GkPlus - d3d9` in place of the game's own version stamp, let
the game create its profile directory, and made a change in `settings.json` that survived
a restart.

The profile is now yours. It is the same directory that holds mods you enable and scripts
you write, and `settings.json` is the same file the in-game **Advanced Graphics** page
writes to.

## Where to go next

The [how-to guides for players and mod authors](/how-to/modding/) are the next step, and
these three are the ones a profile is for:

- [How to set up a profile](/how-to/modding/set-up-a-profile/): a second profile, kept
  somewhere else, so a modded setup and a clean one are two directories rather than a file
  swap.
- [How to enable and order mods](/how-to/modding/enable-and-order-mods/): nothing you drop
  in a folder loads on its own; this is how a mod actually gets in front of the engine.
- [How to turn on renderer features](/how-to/modding/turn-on-renderer-features/): the
  Vulkan renderer, and the HDR, bloom, ambient-occlusion and soft-shadow settings that only
  do anything under it. None of the knobs you just edited has a visible effect on the
  default renderer.

Then:

- [Your first GkPlus script](/tutorials/your-first-script/): add an item to Gunlok's own
  menus and draw a panel over the game, in the profile you now have.
- Reference: [settings.json](/reference/data/settings-json/) for the file you just edited,
  [Renderer setting keys](/reference/data/render-settings-keys/) for every key in it, and
  [Environment variables](/reference/data/environment-variables/) for the switches that
  outrank it.
- Explanation: [Why GkPlus is a d3d8.dll](/explanation/why-gkplus-is-a-d3d8-dll/): why
  copying one file into the game folder is all the installation there is; and
  [One settings file, many owners](/explanation/one-settings-file-many-owners/): why the
  game rewrote `settings.json` on exit without losing your edit.
