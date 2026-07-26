// Recovers the integer behind every GLS enum keyword, by asking the parser.
//
// Install as <Gunlok>\gkplus\main.mjs (or import it from yours) and start the
// game. It runs at boot, from the menu - NOT during a level load, because the GLS
// parser uses destructive global state.
//
// Output goes to the in-game console (` to open) and to the debugger, so
// DebugView or a debugger's output window captures it without touching the game.
//
// Why this exists: these keywords are compiled into the lexer's flex DFA rather
// than stored as strings, so they cannot be found in gl.exe and are not in any
// shipped header - glresrc.h only has GL_* resource ids, on unrelated numbering.
// But `gls.probe` hands the parser a one-field section per keyword and reads the
// stored integer straight back, which is exact and needs no debugger.

import { console, gls } from "gk";

/**
 * Keywords harvested per (section, field) from the shipped scripts, with block
 * comments stripped - a first pass that missed those fed the parser `enemy plasma
 * strong pulsox` out of a commented-out block in plasma.gsh, which is not a real
 * keyword and killed the run.
 *
 * ORDER MATTERS. A failed parse poisons every later one, and `gls.probe` stops at
 * the first refusal, so anything doubtful goes last: everything before it is
 * still good data.
 *
 * @type {{section: import("gk").GlsSection, field: string, names: string[]}[]}
 */
const VOCABULARY = [
  {
    section: "ammo_info",
    field: "ammo_type",
    names: [
      "autolock bolts", "battery basic", "battery plus", "energy cells",
      "flames", "flares", "grenade basic", "grenade EMP", "grenade plus",
      "missile basic", "missile EMP", "missile plus", "nanotech dismantler",
      "napalm", "needles", "none needed", "plasma bolts", "plasma shells",
      "plasmaxi bolts",
    ],
  },
  {
    // All 31 that shipped `ammo` sections use, minus the commented-out one.
    //
    // `none` is NOT in this list even though the field defaults to 33: `none` is
    // a value form for String and Custom fields, and on an integer enum it is a
    // *syntax error*. Including it is what poisoned the previous run - and since
    // a poisoned parser never recovers, one unspellable name at the end of one
    // list cost every field after it.
    section: "ammo",
    field: "weapon_type",
    names: [
      "binary laser", "enemy epulsar obliteron", "enemy epulsar obliteron deadly",
      "enemy grenade launcher basic", "enemy grenade launcher plus",
      "enemy laser adversor", "enemy laser medium", "enemy laser strong",
      "enemy laser weak", "enemy missile launcher basic",
      "enemy missile launcher basic slow reload", "enemy missile launcher plus",
      "enemy plasma medium", "enemy plasma mini pulsox", "enemy plasma pulsax",
      "enemy plasma pulsox", "enemy plasma strong", "enemy plasma weak",
      "epulsar", "flamethrower", "grenade launcher", "interface arm", "laser",
      "maxim laser", "missile launcher", "nanofrag", "plasma pistol",
      "plasma pistol training", "plasmagnum", "plasmatrix", "repair arm",
    ],
  },
  { section: "destructibility", field: "type", names: ["explode", "splatter"] },
  {
    // The control: all 21 names appear in shipped roles and the answers are
    // already known from AITypeName. If this row disagrees, distrust the rest.
    section: "role",
    field: "ai",
    names: [
      "bot", "scavenger", "mine", "minebot", "pickup", "blocker", "track object",
      "tumbleweed", "background creature", "flying background creature", "node",
      "node waiting", "president", "popup", "centibody", "centipede", "turret",
      "pathfinder", "swarm", "waiting", "reserved",
    ],
  },
  { section: "role", field: "action_on_death", names: ["must drop", "must not drop"] },
  {
    section: "role",
    field: "resistance",
    names: ["resists laser", "resists small arms", "resists explosives", "resists epulsar"],
  },
  {
    // `corona` and `laser trail` are the interesting ones: GetParticleIDFromName
    // (the console's table, where src/Roles.h ParticleType came from) leaves ids
    // 7, 8 and 10 unnamed, and the GLS lexer knows exactly two more names.
    // Only these nine appear in shipped scripts.
    section: "pgenerator",
    field: "type",
    names: [
      "smoke", "steam", "fire", "shot", "explosion", "big explosion", "sparks",
      "corona", "laser trail",
    ],
  },
  {
    // LAST of the confident group, because it is the one that has failed twice:
    // `weapon laser` syntax-errored even though `laser` is weapon type 4 and
    // shipped characters do write `weapon epulsar`. The suspicion is the filler,
    // not the keyword - optional integer fields like `secondary weapon 0` are now
    // omitted, since no shipped script writes an enum as a bare number.
    //
    // 62 distinct values in shipped `character` sections - far past the 34 an
    // `ammo` weapon type allows, and it includes ammo, mines and gadgets
    // (`audio cloak`, `lock decoder`, `terrain scanner`). So this may be a wider
    // inventory-item enum that happens to share field id 0x18.
    section: "character",
    field: "weapon",
    names: [
      "laser", "maxim laser", "plasma pistol", "plasma pistol training",
      "plasmagnum", "plasmatrix", "epulsar", "grenade launcher",
      "missile launcher", "flamethrower", "nanofrag", "repair arm",
      "interface arm", "binary laser", "enemy laser", "enemy laser weak",
      "enemy laser medium", "enemy laser strong", "enemy laser adversor",
      "enemy plasma weak", "enemy plasma medium", "enemy plasma strong",
      "enemy plasma pulsax", "enemy plasma pulsox", "enemy plasma mini pulsox",
      "enemy grenade launcher basic", "enemy grenade launcher plus",
      "enemy missile launcher basic", "enemy missile launcher basic slow reload",
      "enemy missile launcher plus", "enemy epulsar obliteron",
      "enemy epulsar obliteron deadly", "autolock bolts", "battery basic",
      "battery plus", "energy cells", "flames", "flares", "grenade basic",
      "grenade EMP", "grenade plus", "missile basic", "missile EMP",
      "missile plus", "nanotech dismantler", "napalm", "plasma bolts",
      "plasma shells", "plasmaxi bolts", "audio cloak", "beacon tracker",
      "decoy mine", "EMP mine", "hologram generator", "lock decoder",
      "remote mine", "sight pickup", "standard mine", "target imager",
      "terrain scanner", "timed mine",
    ],
  },
  {
    section: "character",
    field: "secondary_weapon",
    names: [
      "enemy grenade launcher basic", "enemy laser strong",
      "enemy missile launcher basic", "enemy missile launcher plus",
      "enemy plasma strong",
    ],
  },
  // --- doubtful, so probed last: a refusal here costs nothing above ------------
  {
    // `stuff.gsh` has `action on death binary laser`, which looks like a typo in
    // a scratch file rather than a real value. Probed alone, at the end.
    section: "role",
    field: "action_on_death",
    names: ["binary laser"],
  },
  {
    // Particle types with no shipped use - GetParticleIDFromName knows them, so
    // the GLS lexer probably does too, but nothing proves it.
    section: "pgenerator",
    field: "type",
    names: ["snow", "trail", "rain"],
  },
];

// The game's own parser chatters while this runs ("default value assumed for
// ...", "abstract definition not declared ..."). Those lines come from the game
// and have no [gkplus] prefix, so filtering the console for [gkplus] gives just
// the results.
function report() {
  console.log("=== GLS keyword probe: filter for [gkplus] to drop parser chatter ===");
  for (const { section, field, names } of VOCABULARY) {
    console.log(`--- ${section}.${field} ---`);
    /** @type {Record<string, number | null>} */
    let values;
    try {
      values = gls.probe(section, field, names);
    } catch (e) {
      console.log(`  probe failed: ${e instanceof Error ? e.message : String(e)}`);
      continue;
    }
    // Sorted by value so the enum reads in order; rejected keywords last.
    /** @type {[string, number | null][]} */
    const rows = names.map((n) => [n, values[n] ?? null]);
    rows.sort((a, b) => {
      if (a[1] === null) return 1;
      if (b[1] === null) return -1;
      return a[1] - b[1];
    });
    for (const [name, value] of rows) {
      console.log(`  ${value === null ? "rejected" : String(value).padStart(4)}  ${name}`);
    }
  }
  console.log("--- done ---");
}

report();
