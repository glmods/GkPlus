// GkPlus entry module.
//
// Copy this to <Gunlok>\gkplus\main.mjs - i.e. a `gkplus` folder next to
// d3d8.dll - or point the GKPLUS_SCRIPT environment variable at it. It is loaded
// once during the game's menu setup, before the first frame.
//
// Everything here is optional: a module with no exports still loads, and a
// missing file just logs the path it looked for and leaves the game unmodified.
//
// Copy jsconfig.json and ..\types\ alongside it for autocomplete and type
// checking in an editor - the JSDoc annotations below are what drive it.

import { actors, camera, console, fx, game, levels, light, tokens, world } from "gk";
// The Vulkan renderer's own knobs, as ImGui. Its own module because it is longer
// than everything else here put together, and because it is the piece most worth
// copying into an entry module of your own.
import { draw_render_panel } from "./render-panel.mjs";
// The CPU profiler, as ImGui. Also its own module, and the same shape: it draws
// into the caller's window. Needs GKPLUS_PROFILER=1 (or its own Arm button) to
// have anything to show.
import { draw_prof_panel } from "./prof-panel.mjs";
// A level module is an ordinary module, imported the ordinary way. Its namespace
// - `map` plus the hooks - is exactly the description `levels.add` wants, so
// there is nothing else to unpack. Drop this line (and the add() below) if you
// have not copied levels/ alongside - **and headers/, which arena.mjs imports**.
//
// A module this fails to find takes the whole entry module with it: the host
// reports `could not load module '<path>'` on the game's own text layer and then
// registers no hooks at all, so the symptom is that nothing here happens rather
// than that one level is missing.
import * as arena from "./levels/arena.mjs";

// There is no global console - this one comes from "gk", and carries both the
// game's console object (print, the text colours, execute) and log/info/warn/
// error/debug, which write to the in-game console (` to open it) and to the
// debugger.
console.log("main.mjs loaded");

let showActors = false;

// A level built from script rather than from a .gls + .gcs pair. The map is
// validated here, at startup, so a bad field is reported now rather than halfway
// through a load; the level then shows up in Choose Level, which GkPlus adds a
// Single Player item for.
levels.add("Test Arena", arena);

/**
 * Called once at startup, after the game has filled its own menus - which is
 * why this is a hook rather than something you do at module scope. The argument
 * is the only way to reach `menus`: there is no export for it, because adding
 * an item before the game's own would shift every index in its dispatch table.
 * Keep it in a module-level variable if you need it later (menus.current,
 * menu.open).
 *
 * @type {import("gk").SetupMenus}
 */
export function setup_menus(menus) {
  // Menus are keyed by id and by name: menus[0], menus.Main, menus["main"].
  // The item is appended after every item the game put there, so the game's own
  // indices keep working, and it comes back if the game rebuilds that menu.
  menus.Main.add_item("Press F11 for GkPlus", (item) => {
    console.log(`clicked '${item.label}' at index ${item.index}`);
  });

  // A toggle renders ON / OFF like the game's own options and flips itself
  // before the callback runs.
  menus.Options.add_toggle("Show the actor list", showActors, (item) => {
    showActors = item.value === true;
  });

  console.log(
    `${menus.Main.name} has ${menus.Main.count} items: ` +
      menus.Main.items.map((i) => i.label).join(", ")
  );
}

/**
 * Called every frame the F11 overlay is open, inside an active ImGui frame.
 * The argument is the only way to reach ImGui - there is no module to import it
 * from, because an ImGui call outside this callback is not in a frame and does
 * not work.
 *
 * Keep Begin/End balanced: an exception thrown from here disables draw_gui for
 * the rest of the session (and is reported to the console), because a
 * half-finished ImGui frame is not recoverable.
 *
 * @type {import("gk").DrawGui}
 */
export function draw_gui(ImGui) {
  if (ImGui.Begin("GkPlus")) {
    ImGui.Text(`${actors.count} actors, ${tokens.count} tokens`);

    // Widgets return their new state rather than writing through a pointer.
    const slider = ImGui.SliderFloat(
      "camera distance",
      camera.distance,
      100,
      3000
    );
    if (slider.changed) {
      camera.distance = slider.value;
    }

    // The camera's Euler angles, in degrees - what `SET CAMERA ORI` sets.
    // Assigning the whole object costs one matrix rebuild instead of three.
    const yaw = ImGui.SliderFloat("camera yaw", camera.yaw, 0, 360);
    if (yaw.changed) {
      camera.orientation = { yaw: yaw.value };
    }

    // game.difficulty replaces the EASY/MEDIUM/HARD/EXTREME gate commands: they
    // are "re-run this line only on a match", which is an ordinary `if`.
    ImGui.Text(`difficulty: ${game.difficulty}, mode: ${game.mode}`);

    const god = ImGui.Checkbox("God mode", game.god_mode);
    game.god_mode = god.value;

    const hovered = game.actor_under_cursor;
    ImGui.Text(hovered ? `under cursor: #${hovered.id}` : "under cursor: none");

    // The level's atmosphere. `world.fog` reads through a pointer that is null
    // outside a level, so `available` is the guard.
    const sun = ImGui.SliderFloat("sun angle", world.sun_angle, 0, 360);
    if (sun.changed) {
      world.sun_angle = sun.value;
    }
    if (world.fog.available) {
      const fog = ImGui.SliderFloat("fog", world.fog.value, 0, 1);
      if (fog.changed) {
        world.fog.value = fog.value;
      }
    } else {
      ImGui.Text("no level loaded, so no fog");
    }

    // The command-backed namespaces run the game's own handler, so their
    // defaults are the console's: explode() with no argument uses the cursor.
    if (ImGui.Button("Explode at cursor")) {
      fx.explode();
    }
    ImGui.SameLine();
    if (ImGui.Button("Rain")) {
      fx.rain(true);
    }
    ImGui.SameLine();
    if (ImGui.Button("Fade in")) {
      light.fade_in();
    }

    if (ImGui.Button("Heal everything")) {
      for (const actor of actors) {
        if (actor.alive) {
          actor.health = 100;
        }
      }
    }

    // Every `render` knob, in its own set of collapsing headers. It draws into
    // this window rather than opening one of its own, so the order here is the
    // order on screen.
    draw_render_panel(ImGui);

    // Where the frame's CPU time goes. Same rule as the panel above, for a
    // different reason: the queries behind it are far too expensive to run
    // every frame, so it refreshes on a cadence of its own.
    draw_prof_panel(ImGui);

    const check = ImGui.Checkbox("Show the actor list", showActors);
    showActors = check.value;

    if (showActors && ImGui.CollapsingHeader("Actors")) {
      for (const actor of actors) {
        // `kind` is a discriminated union, so this is how you reach a member
        // that only some actors have.
        const orders = actor.kind === "character" && actor.has_pending_orders;
        ImGui.Text(`${actor.id} ${actor.kind} ${actor.name ?? ""}${orders ? " *" : ""}`);
      }
    }
  }
  // End is unconditional: ImGui requires it even when Begin returns false.
  ImGui.End();
}
