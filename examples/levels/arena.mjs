// A level built from script instead of a .gls + .gcs pair.
//
// Register it from main.mjs:
//
//     import { levels } from "gk";
//     import * as arena from "./levels/arena.mjs";
//     levels.add("Test Arena", arena);
//
// This module's namespace - `map` plus the hooks below - is the description
// `levels.add` takes, so an ordinary import is the whole mechanism. Registration
// validates `map` there and then, so anything wrong with it is reported at
// startup rather than halfway through a level load. The level then appears in
// Choose Level, reachable from a "Choose Level" item GkPlus adds to Single
// Player (the game's own one needs -chooselevel).
//
// What is *not* replaced is the .rif. Geometry still comes out of the game's own
// asset files; this module supplies everything the two script files used to.

import { console } from "gk";
import { roles as bugRoles } from "../headers/bug.mjs";

/** @type {import("gk").LevelMap} */
export const map = {
  // The level geometry: an object inside a .rif, exactly as a .gls map section
  // names it. Paths are relative to the RIFs directory.
  rif: "levels\\level01.rif",
  object: "Land",

  bitmap: "bitmaps\\LEVEL01.rim",
  camera_plane: "camhund",
  max_camera_distance: 60,
  max_camera_focus_height: "max focus height",
  min_camera_focus_height: "min focus height",
  shadow_object_rif: "levels\\level01_shadow.rif",
  shadow_object_name: "Land",
  max_vertices_per_section: 250,
};

// The roles, characters and ammo this level's actors need - the `#include` block
// a hand-written .gls opens with. Paths are relative to the game's Scripts
// directory, and they are parsed in a single pass, so a .gsh included twice
// still only defines its roles once.
//
// Drop this entirely and the parser never runs at all: build the definitions in
// JS with `gls` instead and register them from `define` below. The two mix
// freely, which is what makes converting a header at a time practical.
export const includes = [
  "defaults.gsh",
  "pickups.gsh",
  "gunlok.gsh",
  "elint.gsh",
  "archore.gsh",
  "technocrate.gsh",
];

/**
 * Registers the definitions this level brings of its own. Runs once per load,
 * before the map is built - which is where a .gls's `#include` block sits.
 *
 * It has to be per load rather than once at startup: the roles hash is cleared
 * between levels, so a Role converted for the previous load is long gone. The
 * `GlsObject`s themselves are reusable, which is why they live at module scope
 * in headers/ and only `register()` happens here.
 *
 * @param {import("gk").Level} level
 */
export function define(level) {
  for (const makeRole of bugRoles) {
    makeRole();
  }
  console.log(`${level.title}: registered ${bugRoles.length} script-defined roles`);
}

/**
 * Fills the world in. Runs once per load, after the geometry exists and before
 * the camera settles - the same window a .gls's `use ... for ...` clauses spawn
 * their placed objects in.
 *
 * @param {import("gk").Level} level
 */
export function populate(level) {
  // `locators` is the `for "<rif object>"` half of a .gls `use` clause: every
  // object of that name in the level rif, already in world coordinates. The
  // .rif supplies the position and orientation; this decides what goes there.
  for (const spot of level.locators("Goodie A")) {
    // `as` creates a token holding the new actor's id, which is how the engine
    // names actors - the `as "gunlok"` clause of a `use`.
    level.spawn("Rol_GunLok", 1, spot, { as: "gunlok" });
  }
  for (const spot of level.locators("Goodie B")) {
    level.spawn("Rol_Elint", 1, spot, { as: "elint" });
  }

  // Nothing forces you to use locators, though - a bare position works, which a
  // .gls has no way of expressing.
  for (let i = 0; i < 4; ++i) {
    level.spawn("Rol_Archore", 2, { x: 40 + i * 6, y: 4, z: -24 });
  }

  // "bug" is the `identifier` of a role define() registered from headers/bug.mjs
  // - spawning it is no different from spawning one the parser produced.
  level.spawn("bug", 0, { x: 52, y: 4, z: -30 });

  console.log(`${level.title}: populated from ${level.map.rif}`);
}

/**
 * The .gcs half. Runs last, once the world is built and the camera has settled -
 * the exact point LoadLevel would have run the level's console command file, and
 * behind the same gate, so restoring a savegame does not run it again.
 *
 * Everything a .gcs does belongs here: fog, lighting, camera bounds, inventory
 * and the triggers that make the level a mission rather than a diorama. The
 * actors populate() spawned already exist, and the tokens its `as` clauses
 * created are how you name them - exactly the names a .gcs would have used.
 *
 * The lines below are `level01.gcs`, one console.execute per line. Unlike the
 * file, they run *now* rather than one per frame, so ordering is guaranteed.
 *
 * @param {import("gk").Level} level
 */
export function setup(level) {
  for (const command of [
    "fogcolour 0 0 0",
    "fogvalue 0.67",
    "sunangle 140",
    "sunbrightness 1.3 1 0.7",
    "ambient 0.06 0.06 0.06",

    // Camera limits, in the coordinates the map is built in.
    "set upper left bound 37.38 3.73 -18.63",
    "set lower right bound 78.59 3.40 -31.52",
    "set max distance 75",

    // `gunlok` and `elint` are the tokens populate() created with `as`, which is
    // all a .gcs ever had to go on either.
    "actor select gunlok",
    "give and equip gunlok mini_plasma_bolts",
    "give and equip gunlok plasma_pistol",
    "give and equip elint mini_plasma_bolts",
    "give elint repair_arm",

    // A death trigger, armed the way a .gcs arms one: the script it names is
    // still a .gcs on disk - triggers fire console files, not JS.
    "add trigger death crtbaa.gcs gunlok",
  ]) {
    console.execute(command);
  }

  console.log(`${level.title}: setup done`);
}
