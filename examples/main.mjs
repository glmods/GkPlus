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

import { actors, camera, console, levels, tokens } from "gk";
// A level module is an ordinary module, imported the ordinary way. Its namespace
// - `map` plus the hooks - is exactly the description `levels.add` wants, so
// there is nothing else to unpack. Drop this line (and the add() below) if you
// have not copied levels/ alongside.
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

    if (ImGui.Button("Heal everything")) {
      for (const actor of actors) {
        if (actor.alive) {
          actor.health = 100;
        }
      }
    }

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
