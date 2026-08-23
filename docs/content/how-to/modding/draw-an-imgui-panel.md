---
title: "How to draw an ImGui panel"
description: "Add your own window to the F11 overlay from a profile script or a mod script."
weight: 110
audience: ["mod-author"]
---

This guide shows a **mod author** how to draw an overlay panel of their own.

You need a profile ([How to set up a profile](/how-to/modding/set-up-a-profile/)) or a mod that
ships a script ([How to ship a script with a mod](/how-to/modding/ship-a-script-with-a-mod/)).
The export is the same in both.

## Export `draw_gui`

In `<profile>\main.mjs`, or in a mod's script module:

```js
import { actors, camera } from "gk";

/** @type {import("gk").DrawGui} */
export function draw_gui(ImGui) {
  if (ImGui.Begin("My Panel")) {
    ImGui.Text(`${actors.count} actors`);

    const d = ImGui.SliderFloat("camera distance", camera.distance, 100, 3000);
    if (d.changed) camera.distance = d.value;

    if (ImGui.Button("log the camera")) {
      console.log(camera.position);
    }
  }
  ImGui.End();
}
```

Press **F11** in the game to show the overlay. The callback runs every frame it is open, inside
an active ImGui frame.

Widgets return their new state rather than writing through a pointer: `SliderFloat` and
`Checkbox` give `{changed, value}`, `InputText` gives `{changed, text}`, `Button` and
`CollapsingHeader` give a boolean. `types/imgui.d.ts` is the full list, generated from the
bindings.

## Rules that will cost you a session

- **`ImGui.End()` is unconditional** - outside the `if`, exactly as above. `Begin` returning
  false means the window is collapsed, not that it was not opened.
- **An exception disables `draw_gui` for the rest of the session**, because a half-finished ImGui
  frame is not recoverable. It is reported to the console. The same goes for a stray `TreePop`.
- **Write only when a widget reports `changed`.** This runs every frame, and some writes are
  expensive: setting `render.lighting_map.enabled` re-reads every lighting map, and
  `render.map_shadow.rate` re-bakes an atlas.
- **Query on a cadence, not per frame,** for anything that builds one object per row -
  `prof.zones`, `prof.samples`, `render.debug.frame_draws`. Snapshot into a variable and refresh
  on a timer, and let a collapsed section query nothing at all.
- **ImGui is only reachable as this argument.** There is no module to import it from, because a
  call outside this callback is not in a frame.

## Add a menu item instead, or as well

`setup_menus` is the other half of the same contract, called once after the game has filled its
own menus:

```js
/** @type {import("gk").SetupMenus} */
export function setup_menus(menus) {
  menus.Main.add_item("My Mod", (item) => console.log(item.label, item.index));
  menus.Options.add_toggle("Show my panel", true, (item) => { show = item.value === true; });
}
```

Items are appended after the game's own, so its indices keep working, and they come back if the
game rebuilds that menu.

## Get editor autocomplete and a type check

Copy `types/` and `examples/jsconfig.json` next to your script. Then:

```
npx -y -p typescript tsc -p jsconfig.json
```

TypeScript is deliberately not a dependency of this repository, which is why the invocation
carries `-p typescript`.

## Test the panel without launching the game

A script module imports `"gk"` and nothing else, so Node plus a stub `gk` package and a recording
ImGui object drives it in process. That is what catches `Begin`/`End` imbalance and the query
cadence, which neither a type check nor a screenshot can. `harness_testing_notes.md`, "Driving a
script module under Node", has the recipe - including the rule to break something once and
confirm the harness says so.

## Copy a bigger one

`examples/render-panel.mjs` draws every renderer knob in collapsing sections and
`examples/prof-panel.mjs` draws the CPU profiler. Both take the caller's `ImGui` and draw into
the caller's window, so they drop into a `draw_gui` of your own as one call each.

## Reference and background

- [JavaScript API](/reference/javascript/): the whole scripting surface, and the
  [`ImGui` interface](/api/js/interfaces/imgui.ImGui.html) with every widget and its return
  shape.
- [`DrawGui`](/api/js/types/gk.gk.DrawGui.html): the callback contract itself.
- [Why the script host boots twice](/explanation/why-the-script-host-boots-twice/): why
  `ImGui` arrives as an argument rather than as something a module imports.
