// A throwaway entry module for testing the custom-level path in-game.
//
// It is deliberately self-contained - no relative imports - so it can be run
// without copying anything else. Two ways to use it:
//
//   mkdir C:\gk-leveltest
//   copy examples\leveltest\main.mjs C:\gk-leveltest\
//   set GKPLUS_PROFILE=C:\gk-leveltest
//   "<Gunlok>\gl.exe"
//
// - a profile of its own, which needs no settings.json at all because
// `core.script` defaults to `main.mjs` inside it - or copy this one file to
// <Gunlok>\gkplus\main.mjs (which replaces whatever is there). Launch gl.exe
// directly either way - Steam does not pass the variable through. Add
// GKPLUS_REPL_PORT=9222 and the REPL comes up too, which is the easiest way to
// poke at a loaded level.
//
// A profile of its own also mounts no mods, since it has no boot.mjs - which is
// usually what you want for a test.
//
// It registers THREE levels, because there are two code paths plus a control:
//
//   "Test Level Parsed"  - names `includes`, so its .gsh files become a prelude
//                          SOURCE TEXT that gls::ParseSource feeds to the parser
//                          from memory. Nothing is written to disk. Measured:
//                          140 roles registered by the time define() runs.
//   "Test Level Scripts" - the control, identical but with every include spelled
//                          `scripts\...`. The cwd during LoadGLS is already the
//                          Scripts directory, so this one is EXPECTED to register
//                          nothing; it exists to prove which directory that is.
//   "Test Level Native"  - names no `includes` at all, so LoadGLS never runs and
//                          the roles come from make.role. Currently wedges the
//                          game - see the warning above it.
//
// Roles are spawned by their GLS `identifier` (`gunlok`, `elint`, `archore`),
// which is what the roles hash is keyed on - NOT the section symbol `Rol_GunLok`.
// The lookup is __mbsicmp, so case does not matter.
//
// Both are reached from Single Player -> Choose Level -> <title> -> Normal.
// Geometry is level01.rif in both cases; only the script halves differ.
//
// What to look for, in the console (` to open it) and in the F11 overlay:
//   * at boot, two "registered" lines whose script_file is `gkplus\...` - a
//     virtual name, and machine-independent, which is the point of it;
//   * on load, the define/populate/setup lines for whichever level you picked;
//   * about five seconds in, a "tick" message arriving through the engine's own
//     script queue, which exercises the JSON envelope end to end.

import { actors, console, game, levels, make, roles, tokens, triggers, world } from "gk";

/**
 * Records what the roles hash looks like at a given point of a load, both to the
 * console and into tokens - which survive for the REPL to read back even after
 * the console has scrolled.
 *
 * @param {string} tag
 * @param {import("gk").Level} level
 */
function diag(tag, level) {
  const names = [...roles].map((r) => r.name).filter(Boolean);
  tokens["diag_" + tag] = roles.count;
  console.log(
    `[diag ${tag}] ${level.title}: roles.count=${roles.count}` +
      `, gunlok=${roles["gunlok"] ? "yes" : "no"}` +
      `, first names: ${names.slice(0, 6).join(", ") || "(none)"}`
  );
}

// The level01 map description, field for field as a .gls map section - shared by
// both levels below, since neither is testing the geometry.
const level01 = {
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

// The atmosphere and camera bounds level01.gcs sets, as one list. A .gcs gets one
// line per frame; these all run inside the same ExecuteAllCommands call.
const level01Setup = [
  "fogcolour 0 0 0",
  "fogvalue 0.67",
  "sunangle 140",
  "sunbrightness 1.3 1 0.7",
  "ambient 0.06 0.06 0.06",
  "set upper left bound 37.38 3.73 -18.63",
  "set lower right bound 78.59 3.40 -31.52",
  "set max distance 75",
];

/**
 * A time trigger carrying a message rather than a .gcs name, so the round trip
 * through QueueScriptExecution -> the envelope -> message_received is visible
 * without having to make anything die first.
 *
 * @param {number} seconds
 * @param {import("gk").ScriptMessage} message
 */
function pingIn(seconds, message) {
  triggers.create({ kind: triggers.kind.time, value: seconds, script: message });
}

/**
 * @param {any} msg
 * @param {import("gk").Level} level
 */
function report(msg, level) {
  console.log(`${level.title}: message ${JSON.stringify(msg)}`);
  if (msg.kind === "tick" && msg.n < 3 && game.simulation_running) {
    // Re-arming from the handler proves the queue keeps working during play, and
    // the authority guard is what stops a joining client sending its own copy.
    pingIn(5, { kind: "tick", n: msg.n + 1 });
  }
}

// --- level 1: roles from .gsh files, parsed from an in-memory prelude ---------

const gshFiles = [
  "defaults.gsh",
  "pickups.gsh",
  "gunlok.gsh",
  "elint.gsh",
  "archore.gsh",
  "technocrate.gsh",
];

// Registered twice, differing ONLY in how the includes are spelled, because that
// is the open question: the parser opens an #include with a bare fopen against the
// current directory, and LoadLevel is supposed to have set that to Scripts (the
// SetCurrentDirectoryToGLDir(0) at 0x004e0e28, immediately before LoadGLS). If the
// bare spelling comes up empty and the scripts\-prefixed one does not, the cwd is
// the game root at that moment and the note is wrong.
levels.add("Test Level Parsed", {
  ...level01,

  // The `#include` block. These become one generated source text - not a file -
  // and are parsed in a single pass, so the multiple-inclusion guards hold.
  includes: gshFiles,

  define(level) {
    diag("parsed_define", level);
  },

  populate(level) {
    diag("parsed_populate", level);
    let spawned = 0;
    for (const spot of level.locators("Goodie A")) {
      level.spawn("gunlok", 1, spot, { as: "gunlok" });
      ++spawned;
    }
    for (const spot of level.locators("Goodie B")) {
      level.spawn("elint", 1, spot, { as: "elint" });
      ++spawned;
    }
    for (let i = 0; i < 4; ++i) {
      level.spawn("archore", 2, { x: 40 + i * 6, y: 4, z: -24 });
      ++spawned;
    }
    console.log(`${level.title}: spawned ${spawned} actors from parsed roles`);
  },

  setup(level) {
    for (const command of level01Setup) {
      console.execute(command);
    }
    console.execute("give and equip gunlok mini_plasma_bolts");
    console.execute("give and equip gunlok plasma_pistol");
    console.execute("give and equip elint mini_plasma_bolts");
    console.execute("actor select gunlok");
    pingIn(5, { kind: "tick", n: 1 });
    console.log(`${level.title}: setup done, ${actors.count} actors`);
  },

  message_received: report,
});

// The control: every include prefixed with `scripts\`. Since the cwd during
// LoadGLS is *already* the Scripts directory, this resolves to scripts\scripts\...
// and registers nothing - which is the measurement that pins down the directory.
levels.add("Test Level Scripts", {
  ...level01,
  includes: gshFiles.map((f) => "scripts\\" + f),

  define(level) {
    diag("scripts_define", level);
  },

  populate(level) {
    diag("scripts_populate", level);
    let spawned = 0;
    for (const spot of level.locators("Goodie A")) {
      level.spawn("gunlok", 1, spot, { as: "gunlok" });
      ++spawned;
    }
    for (const spot of level.locators("Goodie B")) {
      level.spawn("elint", 1, spot, { as: "elint" });
      ++spawned;
    }
    console.log(`${level.title}: spawned ${spawned}`);
    tokens["diag_scripts_spawned"] = spawned;
  },

  setup(level) {
    for (const command of level01Setup) {
      console.execute(command);
    }
    console.log(`${level.title}: setup done, ${actors.count} actors`);
    tokens["diag_scripts_actors"] = actors.count;
  },

  message_received: report,
});

// --- level 3: roles from make, so the parser never runs ----------------------
//
// WARNING: make.role currently BLOCKS - thread 0 sits in ZwWaitForMultipleObjects
// and the game stops responding. Loading this level wedges the process. It is kept
// registered on purpose, as the reproduction.
//
// No `includes` key at all. That is the whole difference: LoadGLS is answered
// with an empty object list and every definition below is built natively.
//
// It is a diorama rather than a mission - there is no player unit, because that
// would mean transcribing gunlok.gsh - so expect to look at it, not play it. One
// creature goes in team 1 so LoadLevel's camera snap has an actor to find.

/** @type {import("gk").CharacterDesc} */
const bugCharacter = {
  walking_speed: 1.5,
  turning_speed: 0.4,
  strength: 10,
  aim: 20,
  aggression: 0.1,
  sight_angle: 70,
  sight_range: 20,
  hearing_range: 25,
};

levels.add("Test Level Native", {
  ...level01,

  define(level) {
    // Registered per load, because the roles hash is cleared between levels.
    make.role({
      identifier: "testbug",
      hierarchy: { rif: "units\\bug.rif", object: "bug", hotspot: "head" },
      character: bugCharacter,
      // Underscored, not the GLS spelling: AITypeFromName is a plain strcmp over
      // the table in src/Roles.cpp, so "background creature" raises a RangeError.
      ai: "background_creature",
      destructibility: { kind: "explode" },
      per_vertex_fogging: false,
      alpha_fogging: true,
      reflective: false,
    });
    console.log(`${level.title}: registered 'testbug' natively`);
  },

  populate(level) {
    let spawned = 0;
    // Team 1 first, and at a locator the level rif is known to have, so the
    // camera lands somewhere sensible.
    for (const spot of level.locators("Goodie A")) {
      level.spawn("testbug", 1, spot, { as: "bug1" });
      ++spawned;
    }
    for (let i = 0; i < 6; ++i) {
      level.spawn("testbug", 2, { x: 44 + i * 4, y: 4, z: -26 });
      ++spawned;
    }
    console.log(`${level.title}: spawned ${spawned} native actors`);
  },

  setup(level) {
    for (const command of level01Setup) {
      console.execute(command);
    }
    pingIn(5, { kind: "tick", n: 1 });
    console.log(`${level.title}: setup done, ${actors.count} actors`);
  },

  message_received: report,
});

// The identity check, and the one line worth reading at boot: `script_file`
// should be `gkplus\Test_Level_Parsed.gls` and `gkplus\Test_Level_Native.gls`.
// Neither file exists, and neither is ever opened.
for (const level of levels) {
  console.log(`registered "${level.title}" as ${level.script_file}`);
}

/** @type {import("gk").DrawGui} */
export function draw_gui(ImGui) {
  if (ImGui.Begin("Custom level test")) {
    ImGui.Text(`${actors.count} actors, fog ${world.fog.available ? "yes" : "no"}`);
    ImGui.Text(`simulation_running: ${game.simulation_running}`);
    // levels.current is null outside a load callback, so this is null in play -
    // which is itself worth seeing rather than hiding.
    ImGui.Text(`levels.current: ${levels.current?.title ?? "null"}`);

    if (ImGui.CollapsingHeader("Registered")) {
      for (const level of levels) {
        ImGui.Text(`${level.title}  ${level.script_file}`);
      }
    }
    if (ImGui.Button("Ping in 2s")) {
      pingIn(2, { kind: "tick", n: 99 });
    }
  }
  ImGui.End();
}
