---
title: "Modding"
description: "For players and mod authors: profiles, mods, assets, renderer features, scripts and levels."
weight: 10
---

These assume a working GkPlus install, with `d3d8.dll` sitting beside `gl.exe`. If you do not
have one yet, start with [Installing GkPlus](/tutorials/installing-gkplus/).

## Setting up

- **[How to set up a profile](/how-to/modding/set-up-a-profile/)**: create the directory
  GkPlus reads its settings, scripts and mod list from, and point the game at it.
- **[How to enable and order mods](/how-to/modding/enable-and-order-mods/)**: name the
  mods a profile loads, decide which one wins a file conflict, and switch one off.
- **[How to turn on renderer features](/how-to/modding/turn-on-renderer-features/)**: switch to
  the Vulkan renderer and enable HDR, bloom, ambient occlusion and soft shadows.

## Building a mod

- **[How to package a mod](/how-to/modding/package-a-mod/)**: lay a mod out so the engine
  finds its files and GkPlus can say who it is.
- **[How to ship a script with a mod](/how-to/modding/ship-a-script-with-a-mod/)**: give a
  mod code of its own instead of only replacing assets.

## Assets

- **[How to replace a texture](/how-to/modding/replace-a-texture/)**: get a `.RIM` out to
  PNG, edit it, and serve the result back through a mod.
- **[How to get true-colour textures into the game](/how-to/modding/true-colour-textures/)**: the
  uncompressed-DDS route, the only one that reaches 24-bit colour.
- **[How to generate PBR maps for a texture](/how-to/modding/generate-pbr-maps/)**: run
  `gkpbr` over the textures the shipped geometry uses, and check one on screen.
- **[How to generate a lighting map for a texture](/how-to/modding/generate-a-lighting-map/)**:
  turn one `.RIM` into the companion `lighting.dds` the Vulkan renderer looks for.
- **[How to import and export .rif geometry with Blender](/how-to/modding/edit-geometry-in-blender/)**: install `io_scene_rif`, bring a model or
  level in, and export one the game will load.

## Scripting

- **[How to draw an ImGui panel](/how-to/modding/draw-an-imgui-panel/)**: add your own
  window to the F11 overlay from a profile script or a mod script.
- **[How to persist your own settings](/how-to/modding/persist-your-own-settings/)**: keep
  state in `settings.json` without disturbing anybody else's section.
- **[How to author a script-defined level](/how-to/modding/author-a-script-defined-level/)**:
  register a level that has no `.gls` and no `.gcs`, and put it in Choose Level.
- **[How to start a level without the menus](/how-to/modding/start-a-level-without-the-menus/)**:
  load a level straight from a script or the REPL, with no briefing.
- **[How to drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/)**: open
  the loopback JavaScript console into a running Gunlok and check what actually
  happened.

Developer tasks (building the DLL, adding bindings, profiling) are in
[development](/how-to/development/).
