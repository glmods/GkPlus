---
title: "How to turn on renderer features"
description: "Switch to the Vulkan renderer and enable HDR, bloom, ambient occlusion, soft shadows and the rest, from the menu, a script, or the launch environment."
weight: 30
audience: ["player", "mod-author"]
---

This guide shows a **player or mod author** how to select a renderer and turn its features on,
and where each setting is remembered.

## Select the renderer

The renderer is chosen at launch with `GKPLUS_RENDERER`, and only there. There is no setting for
it and no menu row:

```
set GKPLUS_RENDERER=vulkan
"<Gunlok>\gl.exe" -skipfmv
```

Three values: `vulkan` (the replacement renderer, and the only one with the features below),
`d3d9` (the d3d8to9 translation layer, and the default), and `d3d8` (Windows' own runtime, still
shipped in SysWOW64, and the fidelity reference).

Launch `gl.exe` directly. Steam does not pass `GKPLUS_*` through.

Confirm it took by reading the bottom-left of the main menu: it names the **resolved** renderer,
so a request that could not be honoured shows what you actually got. `vulkan-1.dll` is
delay-loaded, so a machine with no Vulkan starts normally and keeps running on d3d8to9.

## From the front end

Under Vulkan, **Options ▸ Advanced Graphics** carries the main switches: Tessellation, Dynamic
Shadows, Sun Shadows, Map Shadows, Ambient Occlusion, Per-Pixel Lighting, Lighting Maps, HDR,
Bloom, plus Antialiasing and Tone Mapping as cycle rows.

The page hides itself entirely under any other renderer, and the Tessellation row is absent on a
device with no tessellation shader, since a setting that could only ever read back off is dropped
rather than shown lying. A click takes effect immediately; there is no apply step, and nothing
on the page needs saving.

## From a script or the REPL

Every knob is a property on `render`, and a write from anywhere is picked up and persisted the
same way:

```js
render.hdr.enabled = true;          // the float target and the tonemap pass
render.hdr.tonemap = "agx";         // clamp | rolloff | reinhard | aces | filmic | agx
render.bloom.enabled = true;        // needs HDR; inert without it
render.bloom.layer(0, { threshold: 1.0, intensity: 0.6 });
render.ao.enabled = true;
render.ao.radius = 3;
render.sun_shadow.enabled = true;
render.sun_shadow.softness = 0.01;  // the tangent of the sun's angular radius, not a width
render.lighting_map.enabled = true;
render.msaa = 4;
```

The families are `tess`, `hdr`, `bloom`, `ao`, `sun_shadow`, `map_shadow`, `map_light`,
`local_light`, `dynamic_shadow`, `lighting_map` and `material`; each family's own switch is
`<family>.enabled`. `types/gk.d.ts` documents every knob, its range and the measurement behind
its default. To list exactly what persists, derive it rather than trusting a written count:

```
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep '\.name = '
```

`render.debug.*` is a separate surface for measurement (probes, censuses, draw hiding), and it
**persists nothing**.

For a ready-made overlay panel covering all of it, copy `examples/render-panel.mjs` into your
profile and call `draw_render_panel(ImGui)` from `draw_gui`. Its one rule is to write only when
a widget reports `changed`: `draw_gui` runs every frame, and setting
`render.lighting_map.enabled` re-reads every lighting map while `render.map_shadow.rate`
re-bakes the shadow atlas.

## At launch, for the four knobs that have a switch

Some knobs carry a companion environment variable, which is what you reach for when the setting
you need to change is the one keeping the game from starting:

```
set GKPLUS_VK_HDR=1
set GKPLUS_VK_BLOOM=1
set GKPLUS_VK_MSAA=4
set GKPLUS_VK_PER_PIXEL_LIGHTING=1
```

**An environment-set knob is skipped in both directions**: it is neither restored from
`settings.json` nor written back to it. So a knob that ignores the file, or a preference that
refuses to stick, usually means one of these is still set in the environment. Re-derive the
current list with:

```
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep '\.env = ' | grep -v nullptr
```

## Where the settings live

Anything you set from the menu, a script or the REPL is written to `<profile>\settings.json`
under `core.render.*`, using the same spelling as the JS property: `render.ao.radius` is
`core.render.ao.radius`. The write happens on its own, about a second after the last change
settles, and again when the game exits. Nothing needs saving.

Two knobs behave differently on purpose, and both would otherwise let one launch on a weaker
machine erase a preference:

- `render.msaa` reads back the **effective** count, clamped to what the device supports, and a
  new count is adopted at the top of the next frame. A UI control has to hold its own pending
  value rather than reading the knob straight back.
- `render.tess.enabled` is restored from the file but never written back, because it reads back
  false on a device with no tessellation shader however it was set.

## Check a feature is doing something

- **Bloom does nothing without HDR.** Turn `render.hdr.enabled` on first;
  `render.debug.bloom_report` says in words when the pass is inert.
- **Lighting maps are silent when the file is not found**, because a texture with no companion
  map is the normal case. `render.debug.lighting_map_report` lists every name probed and what
  came of it.
- Measure a frame time only with `GKPLUS_VK_PRESENT_MODE=immediate`; under the default FIFO the
  frame time is quantized to the refresh interval, so you would be measuring the monitor.

Reading any of those back means talking to the running game: see
[How to drive the game from the REPL](/how-to/modding/drive-the-game-from-the-repl/).

## Next

- [Generate a lighting map for a texture](/how-to/modding/generate-a-lighting-map/): content for
  `render.lighting_map`.
- `vulkan_renderer_plan.md` is the status and testing record; `vulkan_renderer_notes.md` has the
  measurement behind every default.

## Reference and background

- [Renderer setting keys](/reference/data/render-settings-keys/): every `core.render.*` key
  that persists, and the two that behave differently.
- [Environment variables](/reference/data/environment-variables/): `GKPLUS_RENDERER` and
  the four knob companions, which outrank the file in both directions.
- [settings.json](/reference/data/settings-json/): where a knob set from a script or from
  the menu ends up.
- [`render`](/api/js/variables/gk.gk.render.html): the whole namespace, in the generated
  JavaScript reference.
- [Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/): what
  "the Vulkan renderer" actually replaces.
- [What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/): before
  trying to measure whether a feature improved anything.
