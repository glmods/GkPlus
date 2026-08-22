// Exercises every pattern types/gk.d.ts and types/imgui.d.ts claim to support.
//
//     npx tsc -p types/tsconfig.json
//
// Anything that must NOT compile is marked with @ts-expect-error, which is
// itself an error when the line turns out to be fine - so this fails both when
// something legal stops compiling and when something illegal starts. Nothing
// here runs; it is a compile-time assertion file.

import gk, {
  actors,
  camera,
  console as gameConsole,
  game,
  gls,
  levels,
  make,
  roles,
  tokens,
  demo,
  fx,
  inventory,
  light,
  music,
  objectives,
  screen,
  script,
  tracks,
  triggers,
  units,
  mods,
  render,
  settings,
  world,
} from "gk";
// @ts-expect-error - `menus` is not an export: it is only setup_menus'
// argument, because the game's own items must be in place before a script adds
// one.
import { menus as notAnExport } from "gk";
import type {
  Actor,
  CameraOrientation,
  Color,
  ConsoleCommand,
  DifficultyName,
  GameAsset,
  GlsField,
  GlsSection,
  Level,
  LevelLocator,
  LevelModule,
  Menus,
  MenuItem,
  BloomBlend,
  BloomState,
  BloomStatus,
  MaterialOverridesState,
  Mod,
  Render,
  RenderBoolKey,
  RenderNumberKey,
  RenderStringKey,
  Role,
  SetupMenus,
  TurretActor,
  Vec3,
} from "gk";
// @ts-expect-error - ImGui is deliberately not a module: it is only reachable
// as draw_gui's argument, because that is the only place its calls are valid.
import * as NotAModule from "ImGui";

// --- the module ---------------------------------------------------------------

const sameObject: boolean = gk.actors === actors;
// @ts-expect-error - and it is not on the default export either
const notThereEither: Menus = gk.menus;

// --- camera -------------------------------------------------------------------

const distance: number = camera.distance;
camera.distance = 900;
camera.max_distance = 4000;
const eye: Vec3 = camera.position;
camera.position = { x: 1, y: 2, z: 3 };
camera.position = { z: 100 }; // partial assignment keeps the other components
// @ts-expect-error - reading gives a whole Vec3, so this is not optional
const bad: number = camera.position.q;

const yaw: number = camera.yaw;
camera.yaw = 90;
camera.roll = 0;
camera.pitch = -30;
const ori: CameraOrientation = camera.orientation;
const oriYaw: number = ori.yaw;
camera.orientation = { yaw: 45, roll: 0, pitch: -20 };
camera.orientation = { pitch: -20 }; // partial, like position
// @ts-expect-error - reading gives all three, so none of them is optional
const partialRead: undefined = camera.orientation.roll;

const focus: Vec3 | null = camera.focus;
camera.focus = { x: 0, y: 0, z: 0 };
camera.focus = { z: 5 };
camera.focus = null; // FREE CAMERA FOCUS
const isTracking: boolean = camera.tracking;
camera.stop_tracking();
// @ts-expect-error - starting a track is a broadcast, so there is no setter
camera.tracking = true;

// --- console ------------------------------------------------------------------

gameConsole.print("hello");
gameConsole.text_color = 0xff00ff00;
gameConsole.log("host logging", 1, true);
gameConsole.warn("also here");
gameConsole.execute("SPAWN gunlok");
gameConsole.execute_file("level01.gcs"); // throws if the file will not open

// @ts-expect-error - there is no global console: the host installs none, and
// log/info/warn/error/debug live on the one exported by "gk".
console.log("nowhere to go");

// --- actors: collection -------------------------------------------------------

const howMany: number = actors.count;
const byId: Actor | undefined = actors[12];
const byName: Actor | undefined = actors["hark"];
const everything: Actor[] = [...actors];
const ids: string[] = Object.keys(actors);
for (const a of actors) {
  const id: number = a.id;
}
// @ts-expect-error - the collection is read-only
actors[1] = actors[2]!;

// --- actors: the discriminated union ------------------------------------------

const someone = actors[1];
if (someone) {
  const health: number = someone.health;
  someone.health = 50;
  const team: number = someone.team;
  someone.set_team(2);
  // @ts-expect-error - read-only: a team change is a list move plus two
  // broadcasts, so it goes through set_team rather than looking like a field
  someone.team = 2;
  const alive: boolean = someone.alive;
  // @ts-expect-error - goto is a MobileActor member, not on every actor
  someone.goto({ x: 0, y: 0, z: 0 });

  if (someone.kind === "turret") {
    const turret: TurretActor = someone;
    turret.turret_enabled = false;
    turret.attack_position({ x: 1 }); // inherited from CharacterActor
    turret.goto({ x: 1 }); // inherited from MobileActor
  }

  if (someone.kind === "pickup") {
    someone.set_required_item("keycard");
    // @ts-expect-error - a pickup cannot move
    someone.goto({ x: 0 });
  }

  if (someone.kind === "character") {
    someone.attack_target(actors[2]!, 0, 0, 0);
    someone.stop_attacking();
    someone.set_ammo_type(3);
  }
}

// --- actors: instanceof -------------------------------------------------------

const maybe = actors[3];
if (maybe && maybe instanceof actors.classes.MobileActor) {
  maybe.goto({ x: 10, z: 10 }, 1.0);
  const code: number = maybe.goto({ x: 0 });
}
if (maybe && maybe instanceof actors.classes.TurretActor) {
  maybe.turret_enabled = true;
}
// @ts-expect-error - scripts cannot create actors
new actors.classes.PickupActor();

// --- roles --------------------------------------------------------------------

const role: Role | undefined = roles["gunlok"];
if (role) {
  const spawned: Actor | null = role.spawn(0, { x: 0, y: 0, z: 0 });
  role.armor = 10;
  role.ai = "turret";
  // @ts-expect-error - not one of the 21 AI types
  role.ai = "wizard";
  const speed: number | undefined = role.character?.walking_speed;
  const severed: string[] = role.sever_points;
  const aiValue: number = roles.ai_types.pathfinder;
}
for (const r of roles) {
  const name: string | null = r.name;
}

// --- tokens -------------------------------------------------------------------

const score: number | undefined = tokens["score"];
tokens["score"] = 0; // an upsert, unlike the other collections
const rolled: number | undefined = tokens["rand(6)"];
for (const [name, value] of Object.entries(tokens)) {
  const text: string = `${name}=${value}`;
}

// --- triggers -----------------------------------------------------------------

triggers.create({
  kind: triggers.kind.location,
  coords: [{ x: 0, y: 0, z: 0 }],
  value: 500,
  targets: [actors["hark"]!, actors["gunlok"]!],
  script: "ambush",
});
// The same field takes a message instead of a file name.
triggers.create({
  kind: triggers.kind.death,
  targets: [actors["hark"]!],
  script: { kind: "boss_down", bonus: 500 },
});
// @ts-expect-error - `kind` is required
triggers.create({ script: "x" });
// @ts-expect-error - not a trigger kind
const noSuchKind = triggers.kind.explode;
// @ts-expect-error - a symbol is not a message. A function is not one either,
// but it *does* type-check: ScriptMessage is `object`, which Function satisfies,
// so that one is caught at runtime instead - see the note on the type.
triggers.create({ kind: triggers.kind.time, script: Symbol("x") });

// --- game -----------------------------------------------------------------------

const amAuthority: boolean = game.simulation_running;
const modeName: string = game.mode;
// @ts-expect-error - read-only: it is the engine's flag, not ours
game.simulation_running = true;

const gameState: number = game.state;
const isPaused: boolean = game.paused;
// @ts-expect-error - read-only: toggling it is a clock handshake in multiplayer
game.paused = true;

const diff: DifficultyName = game.difficulty;
game.difficulty = "extreme";
game.difficulty = 3; // the raw 0..3 is accepted too
// @ts-expect-error - not one of the four names
game.difficulty = "brutal";
// @ts-expect-error - reading narrows to the four names, never a bare string
const looseDiff: "easy" = game.difficulty;

game.god_mode = true;
game.infinite_ammo = true;
game.friendly_fire = false;
game.controls_enabled = false;
game.epw = true;
game.chrome = true;
game.wireframe = true;
game.low_detail = false;
game.vision_cones = true;
game.battle_number = 4;
game.training_area = 3;

const selected: Actor | null = game.selected_actor;
game.selected_actor = actors[12]!; // the object, matching the getter
game.selected_actor = null;
// @ts-expect-error - a wrapper is a fresh object each lookup; ids are the currency
game.selected_actor = actors[12];
const hovered: Actor | null = game.actor_under_cursor;

game.spawn();
game.spawn(3);
game.spawn(3, 2);

// --- console command table ------------------------------------------------------

const commands: ConsoleCommand[] = gameConsole.commands;
for (const cmd of gameConsole.commands) {
  const cmdName: string = cmd.name;
  const cmdHelp: string | null = cmd.help;
  const cmdCondition: number = cmd.condition;
  const cmdAvailable: boolean = cmd.available;
}
// EXIT and QUIT are localized, so find them rather than spelling them.
const quit = gameConsole.commands.find((c) => /^(QUIT|EXIT)$/i.test(c.name));
// @ts-expect-error - rebuilt on every read; there is nothing to assign to
gameConsole.commands = [];

// --- script-queue messages ------------------------------------------------------

const anyActor = actors[1];
if (anyActor) {
  anyActor.associate("pickup.gcs", true);
  anyActor.associate({ kind: "picked_up", item: "keycard" });
}
// The fourth script-name field: a destructibility that runs something on death.
make.role({
  identifier: "crate",
  destructibility: { kind: "replace", script: { kind: "crate_broken" } },
});
make.role({
  identifier: "crate2",
  destructibility: { kind: "replace", script: "crate.gcs", replace: true },
});
// @ts-expect-error - it is `script` now, not GLS's misleading `name`
make.role({ identifier: "x", destructibility: { kind: "replace", name: "a.gcs" } });

// --- menus --------------------------------------------------------------------

// The whole surface is reachable only through the argument.
export const setup_menus: SetupMenus = (menus) => {
  const main = menus.Main;
  const byIndex: Menus[number] = menus[0];
  const byLowercase = menus["main"];
  const total: number = menus.count;
  const title: string = main.title;
  const items: string[] = main.items.map((i) => i.label ?? "");
  for (const m of menus) {
    const label: string = `${m.id} ${m.name}`;
  }

  const added: MenuItem = main.add_item("Open console", (item) => {
    const where: number = item.index;
    const owner = item.menu;
    gameConsole.log(`clicked ${item.label} on ${owner.name}`);
  });
  const toggle = menus.Options.add_toggle("Cheats", false, (item) => {
    const on: boolean | undefined = item.value;
  });
  toggle.value = true;
  menus.Options.open(true);

  // @ts-expect-error - the menu list is fixed
  menus[0] = main;
  // @ts-expect-error - the callback takes the item, not a string
  main.add_item("x", (item: string) => {});
};

// --- make ---------------------------------------------------------------------

const hcy: GameAsset = make.hierarchy("units\\bug.rif", "bug");
// Every owned sub-object is described inline and built inside make.role, so a
// script can never hand the same Character to two roles.
const Rol_Bug: Role = make.role({
  identifier: "bug",
  hierarchy: { rif: "units\\bug.rif", object: "bug", hotspot: "head" },
  character: {
    walking_speed: 1.5,
    turning_speed: 0.4,
    strength: 1,
    aim: 20,
    sight_angle: 70,
    aggression: 0.1,
    weapon: "enemy laser weak", // a keyword, resolved through the probed table
  },
  ai: "background creature",
  action_on_death: "must not drop",
  resistance: "resists laser",
  destructibility: { kind: "explode" },
  reflective: false,
});
const bugId: number = Rol_Bug.id;
// A reusable handle is fine too - the rif cache owns it.
make.role({ identifier: "bug2", hierarchy: hcy, ai: "bot" });
make.ammo({
  ammo_type: "plasma bolts",
  weapon_type: "plasma pistol",
  role: Rol_Bug,
});
make.ammo_info({ ammo_type: "flares", shape: hcy, max_per_slot: 50 });
make.camera_track({ name: "first contact", file: "levels\\level01.rif" });
// @ts-expect-error - `file` is required: it reaches a strlen with no null check
make.camera_track({ name: "first contact" });

// @ts-expect-error - the destructibility variants are discriminated on `kind`
make.role({ identifier: "x", destructibility: { kind: "shatter" } });
// @ts-expect-error - a Role comes back, not a builder to configure further
make.role({ identifier: "x" }).set("ai", "bot");

// --- gls: what only the parser can answer ---------------------------------------

const sections: GlsSection[] = gls.sections;
const schema: GlsField[] = gls.schema("character");
const required: string[] = schema.filter((f) => f.required).map((f) => f.name);
const aiValues: Record<string, number | null> = gls.probe("role", "ai", ["bot", "turret"]);
const parsed: number = gls.try_parse("destructibility D_X\n{\n\ttype explode\n}\n");

// @ts-expect-error - "creature" is not one of the fifteen section keywords
gls.schema("creature");
// @ts-expect-error - building objects moved to `make`
gls.role({ identifier: "bug" });

// --- levels -------------------------------------------------------------------

const arena: Level = levels.add("Test Arena", {
  rif: "levels\\level01.rif",
  object: "Land",
  bitmap: "bitmaps\\LEVEL01.rim",
  camera_plane: "camhund",
  max_camera_distance: 60,
  includes: ["gunlok.gsh", "archore.gsh"],
  populate(level) {
    const spots: LevelLocator[] = level.locators("Goodie A");
    for (const spot of spots) {
      const id: number = level.spawn("Rol_GunLok", 1, spot, { as: "gunlok" });
    }
    // A bare position works too - a locator is just {position, orientation}.
    level.spawn("Rol_Archore", 2, { x: 10, y: 0, z: 10 });
  },
  setup(level) {
    // The .gcs slot: runs last, with the world already built.
    gameConsole.execute("sunangle 140");
    // A trigger can carry data rather than a file name now.
    triggers.create({
      kind: triggers.kind.death,
      targets: [actors["gunlok"]!],
      script: { kind: "hero_down" },
    });
  },
  message_received(msg, level) {
    // The payload is whatever the sender put there - `any`, like JSON.parse.
    const kind: string = msg.kind;
    // The level arrives second precisely so this works from a module.
    level.send({ kind: "ack", of: kind });
    level.send("wave2.gcs"); // a string still means "run this file"
  },
});
const arenaTitle: string = arena.title;
const arenaRif: string = arena.map.rif;
const levelCount: number = levels.count;
const loadingNow: Level | null = levels.current;
const byTitle = levels["Test Arena"];
for (const level of levels) {
  const busy: boolean = level.loading;
}
// A single include needs no brackets.
levels.add("Minimal", { rif: "levels\\level02.rif", object: "Land", includes: "defaults.gsh" });
// The module form: `import * as m from "./levels/arena.mjs"` produces exactly
// this shape - the map nested under `map`, the hooks beside it - so a namespace
// is passed straight in.
const asModule: LevelModule = {
  map: { rif: "levels\\level01.rif", object: "Land" },
  includes: ["gunlok.gsh"],
  define(level) {},
  populate(level) {},
  setup(level) {},
  message_received(msg, level) {},
};
const fromModule: Level = levels.add("From A Module", asModule);

// --- starting a level, with no menus in the way ----------------------------------
levels.start(arena);
levels.start(arena, { difficulty: "hard" });
levels.start("level01", { difficulty: 2 });
levels.start({ script: "level01.gls", console: "level01.gcs" });
arena.start({ difficulty: "easy" });
const pending: boolean = levels.start_pending;
levels.quit();
for (const entry of levels.startable) {
  const where: number = entry.index;
  const what: string = entry.title + entry.script + entry.console;
}

// @ts-expect-error - difficulty is a name or a number, not a boolean
levels.start(arena, { difficulty: true });
// @ts-expect-error - {console} alone names no level
levels.start({ console: "level01.gcs" });
// @ts-expect-error - the startable inventory is read-only
levels.startable[0].title = "x";

// @ts-expect-error - `object` is required; a rif alone does not name a map
levels.add("Broken", { rif: "levels\\level01.rif" });
// @ts-expect-error - a module *path* is no longer accepted; import it yourself
levels.add("By Path", "./levels/arena.mjs");
// @ts-expect-error - the registered levels are read-only
levels[0] = arena;
// @ts-expect-error - locators() gives whole locators, not bare positions
const notAVec: number = arena.locators("x")[0].y;

// --- ImGui --------------------------------------------------------------------

// @ts-expect-error - and it is a type, not a global value, so there is nothing
// to call from outside the callback either.
ImGui.Text("outside a frame");

export function draw_gui(imgui: ImGui): void {
  if (imgui.Begin("GkPlus")) {
    imgui.Text(`${actors.count} actors`);

    const slider = imgui.SliderFloat("distance", camera.distance, 100, 3000);
    if (slider.changed) {
      camera.distance = slider.value;
    }

    const check = imgui.Checkbox("on", true);
    const on: boolean = check.value;

    if (imgui.Button("Go", { size: { x: 80, y: 20 } })) {
      imgui.SetClipboardText("go");
    }

    const combo = imgui.Combo("pick", 0, ["a", "b", "c"]);
    const picked: number = combo.current_item;

    const pos = imgui.GetCursorScreenPos();
    const x: number = pos.x;

    imgui.PushStyleColor(imgui.Col.Text, { x: 1, y: 0, z: 0, w: 1 });
    imgui.PopStyleColor();

    // @ts-expect-error - Text takes a string, and only a string
    imgui.Text(42);
    // @ts-expect-error - no such flag
    imgui.Begin("x", { flags: imgui.WindowFlags.NoSuchThing });
  }
  imgui.End();
}
gameConsole.echo = false;
const echoing: boolean = gameConsole.echo;

// --- world ----------------------------------------------------------------------

world.sun_angle = 140;
const sunAngle: number = world.sun_angle;
world.sun_angle2 = 30;
world.set_sun_brightness({ r: 1, g: 0.9, b: 0.8, a: 1 });
world.set_sun_brightness({ r: 0.5 }); // partial, like camera.position
const sunDir: Vec3 = world.sun_direction;
world.set_ambient({ r: 0, g: 0, b: 0 });
// @ts-expect-error - not a colour
world.set_ambient(0.5);
// @ts-expect-error - read-only: writing it would desync the shadow rebuild
world.sun_direction = { x: 0, y: 1, z: 0 };

if (world.fog.available) {
  world.fog.enabled = true;
  world.fog.value = 0.5;
  world.fog.update_rate = 4;
  world.fog.transition = 12;
  const fogColor: Color = world.fog.color;
  world.fog.color = { r: 0.2, g: 0.2, b: 0.3, a: 1 };
  world.fog.color = { a: 0.5 };
  const fogMode: number = world.fog.mode;
}
// @ts-expect-error - `mode` is what the engine picked; set `enabled` instead
world.fog.mode = 3;
// @ts-expect-error - the fog object is the level's, not something to replace
world.fog = world.fog;

// --- the broadcast-free command namespaces ----------------------------------------

// camera: the interpolated moves, beside the native properties above.
camera.move_to({ x: 10, y: 0, z: 10 }, 2);
camera.turn_to(90, 0, -30, 1.5);
camera.zoom_to(400, 2);
camera.move_and_zoom_to({ x: 0, z: 0 }, 300, 2);
camera.jerky_zoom_to(250);
camera.rotate(45, 2);
camera.elevate(-10, 1);
camera.nudge(0.25, 0);
camera.center_on(actors[12]!);

// console administration - six of these have localized names.
gameConsole.hide();
gameConsole.write_log("checkpoint reached");
gameConsole.print_version();
gameConsole.set_lines(12);
gameConsole.set_appear(true);
gameConsole.clear_history();
gameConsole.history_size();
gameConsole.queue_size(64);

// fx
fx.water(10, { x: 0, y: 0, z: 0 }, { x: 100, y: 0, z: 100 });
fx.lava();
fx.raise_water({ x: 5, z: 5 }, 2);
fx.set_water_direction({ x: 5, z: 5 }, { x: 1, z: 0 });
fx.electricity({ x: 0 }, { x: 10 }, 3);
fx.laser_fence({ x: 0 }, { x: 10 });
fx.pulse_rings({ x: 0 }, { x: 10 }, 2, 1, 0.5);
fx.ring({ x: 0 }, 8, 1, 4);
fx.explode();
fx.explode({ x: 1, y: 2, z: 3 });
fx.rain(true);
fx.snow(false);
fx.particle_tester(3, { x: 0 });
fx.particle_rate(20);
// @ts-expect-error - `rain` is ON/OFF, not a count
fx.rain(3);

// light
light.dark();
light.fade_to_black(3, 1);
light.ray_color(1, 0, 0);
light.light_on(actors["door_a"]!, "dummy", 1, 1, 1, 0.5);
light.remove_cylinders();

// objectives
objectives.complete(1);
objectives.fail(2);
objectives.print(3);
objectives.training_text(4);
objectives.repeat_text(4);

// music
music.play_sound(0x57);
music.cd_play(true, [2, 4, 17]);
music.cd_tracks("BATTLE", [3, 5]);
music.cd_fade(0, 2);
music.cd_stop();
// @ts-expect-error - not one of the five categories
music.cd_tracks("ROCK", [1]);

// screen
screen.borders(60, 2);
screen.borders_off();
screen.status_window("WATCH");
screen.end_game_fmv("win");
game.set_speed(0);
// @ts-expect-error - the speed is required: there is no getter to pair with
game.set_speed();
levels.quit(); // ends the session; screen.main_menu() was the duplicate
game.quit();
// @ts-expect-error - only GENERAL, WATCH or OFF
screen.status_window("BIG");
// @ts-expect-error - only win or lose
screen.end_game_fmv("draw");

// units - actors are named by token
units.set_ai(actors["hark"]!, "turret");
units.set_activity("PATROL");
units.turn_vision_cone(true, actors["hark"]!);
units.turret_los(false, actors["turret_a"]!);
units.set_scale(2);
units.set_vulnerability(actors["hark"]!, "gunlok", 5, "boom.gcs");
units.watch(actors["elint"]!);
// @ts-expect-error - not one of the three activities
units.set_activity("hark", "DANCE");

// inventory
inventory.give(actors["gunlok"]!, "plasmagnum");
inventory.give_and_equip_role_team("Rol_GunLok", 2, "plasmatrix");
inventory.heap(actors["garbage_pile_a"]!, "plasmagnum", "plasmatrix");
inventory.if_carrying("keycard", "OPEN DOOR 1");

// tracks
tracks.run("lift_a");
tracks.pause("lift_a");
tracks.declare_door({ x: 1, y: 2, z: 3 }, 1);
tracks.attach(actors["lift_a"]!);

// demo
demo.record();
demo.save("run1");

// script pacing - the console queue's, not the script's
script.wait(2);
script.wait_for("gunlok reaches the door");
script.cancel_wait_for();

// A string with whitespace throws at runtime: the console splits on it and
// there is no quoting. It still type-checks, so this is a runtime contract.
fx.water(10);

light.associate(actors["lift_a"]!, actors["door_a"]!, "dummy", 1, 1, 1, 0.5);
music.play_environmental_sound(3);
game.say("regrouping at the bridge"); // free text: spaces are fine here
gameConsole.write_log("a note with spaces");

// damage() takes the attacker's team for frag credit; -1 (the default) is nobody.
if (someone) {
  someone.damage(10);
  someone.damage(10, true);
  someone.damage(10, true, 2);
}

// --- the broadcasting members (authority-gated, safe to call unguarded) --------

camera.track(actors[12]!);
camera.bezier_track({ x: 0 }, { x: 10 }, { x: 20 }, { x: 30 });
fx.airstrike({ x: 0, z: 0 }, { x: 50, z: 50 });
fx.smoke(actors["hark"]!);
fx.stop_particles(actors["hark"]!);
fx.texture_animate(actors["hark"]!, 2, [0, 0, 0.5, 0.5]);
light.add({ x: 1, y: 2, z: 3 }, 1, 0.5, 0);
light.add_blinking({ x: 1 }, 1, 1, 1);
light.shadow(actors["hark"]!);
units.play_animation(actors["hark"]!, 3);
units.give_control(actors["hark"]!, 2);
units.player_select(actors["hark"]!);
units.speak(actors[12]!, "watch the bridge");
inventory.remove_item("plasmagnum");
tracks.set("lift_a", { x: 0 }, { x: 1 }, { x: 2 }, { x: 3 }, true);
tracks.open_door(1);
tracks.toggle_door(1);
screen.stats();

// --- mods ----------------------------------------------------------------------

const modCount: number = mods.count;
const modsGameDir: string = mods.game_dir;
const modsUp: boolean = mods.available;
const modsServed: number = mods.served;
const modsRecent: string[] = mods.recent;

// load, then enable: two steps, because they answer different questions. Nothing
// scans for mods and there is no mods directory - a mod is named, by a script or
// out of config, absolute or relative to the profile.
const loadedMod: Mod = mods.load("mods/extra.zip");
const modAnywhere: Mod = mods.load("D:/gunlok-mods/extra.zip");
const enabledCount: number = mods.enable(loadedMod);
const enabledFromPaths: number = mods.enable("C:/mods/a.zip", loadedMod, [
  "C:/mods/b.zip",
  loadedMod,
]);
const fromConfig: string[] = ["mods/10-alpha", "mods/20-beta.zip"];
const enabledFromConfig: number = mods.enable(fromConfig);
const unmodded: number = mods.enable();

const resolved: string | null = mods.resolve(mods.game_dir + "rif/units/bug.rif");
const modHas: boolean = mods.exists("rif/units/bug.rif");
const modText: string | null = mods.read("scripts/defaults.gsh");
const modBytes: ArrayBuffer | null = mods.read_bytes("rif/units/bug.rif");
const modFiles: string[] = mods.files();
const modScripts: string[] = mods.files("scripts");

// The collection is the enabled set in load order, weakest first: the last index
// wins a conflict.
const allLoaded: Mod[] = mods.loaded;
for (const mod of mods) {
  const name: string = mod.name;
  const entry: string = mod.entry;
  const path: string = mod.path;
  const isArchive: boolean = mod.archive;
  const isEnabled: boolean = mod.enabled;
  const order: number = mod.order;
  const author: string = mod.author;
  const website: string = mod.website;
  const license: string = mod.license;
  const version: string = mod.version;
  const readme: string = mod.readme;
  const problems: string[] = mod.problems;
  if (mod.has_icon_small) {
    const icon: ArrayBuffer | null = mod.icon_small();
  }
  if (mod.has_icon_big) {
    const bigIcon: ArrayBuffer | null = mod.icon_big();
  }
}
if (mods[0] !== undefined) {
  const first: string = mods[0].name;
}
if (mods["20-tweaks.zip"] !== undefined) {
  const byName: string = mods["20-tweaks.zip"].path;
}
// @ts-expect-error - enabling is an ordered declaration of the whole set, not an
// assignment through the indexer
mods[0] = loadedMod;
// @ts-expect-error - `served` is a count the host owns
mods.served = 0;
// @ts-expect-error - the enabled set is `enable`'s to declare
mods.loaded = [];
// @ts-expect-error - there is no directory scan
mods.discover();
// @ts-expect-error - and no mods directory: a mod can live anywhere
mods.dir.length;
// `mods.base` cannot be asserted away: the name index signature that makes
// mods["20-tweaks.zip"] work types any unknown property as `Mod | undefined`, so
// reading one is legal for every name. Calling one is not, which is what the line
// above actually catches.

// --- settings ------------------------------------------------------------------
//
// The document read and written as an object, plus the members on the root.

settings.mymod = { window: { x: 10, y: 20 }, list: [1, 2, 3] };
settings.mymod.window.x = 40;
const settingsWindowX: number = settings.mymod.window.x;
const settingsAo: boolean = settings.core?.render?.ao ?? false;
delete settings.mymod.list;
const settingSections: string[] = Object.keys(settings);
for (const section of settingSections) {
  const value: unknown = settings[section];
}

// The dotted-path half: a fallback in the same breath, and the only way to
// create a path whose intermediate objects do not exist yet.
const settingWindow = settings.get("mymod.window", { x: 0, y: 0 });
const settingWindowY: number = settingWindow.y;
const settingUnknown: unknown = settings.get("mymod.nothing");
settings.set("mymod.a.b.c", 7);
const settingRemoved: boolean = settings.remove("mymod.a");
settings.save();
settings.reload();
const settingsPath: string = settings.path;
const settingsAll: Record<string, unknown> = settings.all;

// @ts-expect-error - `path` is where the file is, not something to move it
settings.path = "C:/elsewhere/settings.json";
// @ts-expect-error - `all` is a detached copy; write through the tree instead
settings.all = {};
// @ts-expect-error - a path is a string, not a key array
settings.get(["mymod", "window"]);

// --- the Vulkan renderer's material override ---------------------------------

const overrideReadback: MaterialOverridesState = render.material.override("gunlok_mk2", {
  texture: "hark_512",
  tint: [1, 0.25, 0.25],
  hide: false,
});
render.material.override("bitmaps\water.rim", { tint: [0.2, 0.4, 1, 0.5] });
render.material.override("gunlok_mk2", null);
render.material.override("gunlok_mk2");
const overrides: MaterialOverridesState = render.material.overrides;
// The two readbacks are structures now, not preformatted text. The text is still
// there, on the measurement surface where a report belongs.
const firstKey: string | undefined = overrides.entries[0]?.key;
const firstMatch: string | undefined = overrides.entries[0]?.images[0]?.name;
const bloomState: BloomState = render.bloom.layers;
const bloomWhy: BloomStatus = bloomState.status;
const layerSigma: number = bloomState.layers[0]!.sigma_px;
const layerBlend: BloomBlend = bloomState.layers[0]!.blend;
// @ts-expect-error - the report is text; the readback is not
const notText: string = render.bloom.layers;
render.material.clear();
// @ts-expect-error - a tint is [r, g, b] or [r, g, b, a], not a packed number
render.material.override("gunlok_mk2", { tint: 0xff00ff });
// @ts-expect-error - the readback is the host's
render.material.overrides = "";

render.lighting_map.chrome_scale = 1.0;
render.lighting_map.chrome_blur = 4.0;
render.lighting_map.chrome_texgen = false;
// @ts-expect-error - texgen is a choice between two coordinates, not a blend
render.lighting_map.chrome_texgen = 0.5;

// --- actor identity is the object, everywhere -------------------------------
//
// One rule, and these are what pin it: an actor is passed as the wrapper and
// never as an id or a token name. The id form is the dangerous one to leave
// working, because `JS_ToInt32` on an object yields 0 rather than throwing - so
// a member that accepted an id silently retargeted actor 0 when handed the
// object it looked like it wanted.

const target = actors["hark"]!;
someone.set_target(target);
someone.set_target(target, 0);
// @ts-expect-error - an id is not an actor
someone.set_target(12);
// @ts-expect-error - nor is a token name
someone.set_target("hark");

game.selected_actor = target;
game.selected_actor = null;
// @ts-expect-error - the setter takes what the getter returns
game.selected_actor = 12;

camera.track(target);
camera.center_on(target);
// @ts-expect-error - command-backed members take the object too
camera.track(12);

// The command-backed namespaces are the ones that used to speak names.
units.set_ai(target, "turret");
// @ts-expect-error - a name is not an actor
units.set_ai("hark", "turret");
// @ts-expect-error - and neither is an id
units.give_control(12, 2);
// @ts-expect-error - set_activity acts on game.selected_actor and takes no actor
units.set_activity(target, "PATROL");
// @ts-expect-error - set_scale acts on game.actor_under_cursor and takes no actor
units.set_scale(2, target);

// A trigger watches actors, not names.
const armed = triggers.create({
  kind: triggers.kind.death,
  targets: [target],
  script: { kind: "down" },
});
const stillThere: boolean = armed.valid;
const armedKind: number = armed.kind;
const wasRemoved: boolean = armed.remove();
// @ts-expect-error - a target is an actor, not a token name
triggers.create({ kind: triggers.kind.death, targets: ["hark"] });
// @ts-expect-error - the handle is the host's; there is nothing to assign
armed.valid = true;

triggers.remove_shot({ x: 1, y: 2, z: 3 });
// @ts-expect-error - remove_shot matches by position, not by name
triggers.remove_shot("shot_a");

// A role reaches a command as the object as well, since these resolve a role by
// name and a role reliably has one.
inventory.give(target, roles["plasmagnum"]!);
inventory.give(target, "plasmagnum");
// @ts-expect-error - the recipient is an actor, so a role is not interchangeable
inventory.give(roles["gunlok"]!, "plasmagnum");

// An owner is an actor too.
roles["gunlok"]!.spawn(0, { x: 0, y: 0, z: 0 }, null, target);
// @ts-expect-error - not an id
roles["gunlok"]!.spawn(0, { x: 0, y: 0, z: 0 }, null, 12);

// --- render is exactly typed; render.debug is the loose half -----------------
//
// The whole point of the split. `Render` used to carry `[key: string]: any`,
// which meant a misspelled knob type-checked and silently did nothing - for all
// 135 members, not just the undeclared ones. The index signature now lives on
// `RenderDebug` alone, where a member genuinely comes and goes with whatever is
// being measured.

render.hdr.enabled = true;
render.sun_shadow.softness = 0.02;
render.hdr.tonemap = "agx";
// @ts-expect-error - THE case this split exists for: a typo used to be silent
render.shadow_softnes = 0.02;
// @ts-expect-error - and so did a knob that never existed
render.definitely_not_a_knob = 1;
// @ts-expect-error - a tonemap operator is one of six names, not any string
render.hdr.tonemap = "filmick";

// The bisects and the probes moved, and reaching for the old spelling is an error
// rather than an `any`.
// @ts-expect-error - a measurement knob, so it is on render.debug
render.draw_hide = "12";
// @ts-expect-error - likewise the A/B composite, which reads back derived
render.stock = true;
// @ts-expect-error - and the read-only reports
const movedReport: string = render.vulkan_report;

render.debug.draw_hide = "12";
render.debug.stock = true;
const debugReport: string = render.debug.vulkan_report;
// Undeclared by design: the investigation surface is loose on purpose.
render.debug.whatever_the_next_measurement_needs = 1;

// The three knobs that were reachable only through the old index signature are
// declared now, so they are checked like everything else.
render.dynamic_shadow.enabled = true;
render.dynamic_shadow.bias = 1.5;
render.local_light.shadow_taps = 4;
// @ts-expect-error - a count, not a switch
render.local_light.shadow_taps = true;

// --- the families are sub-objects --------------------------------------------
//
// Ten of the group names were themselves knobs - `render.ao` was the switch - so
// each group's switch is `enabled` now, which is what lets a family and its
// switch share a name.
render.ao.enabled = true;
render.ao.radius = 2.5;
render.tess.enabled = false;
render.tess.pn_strength = 0.5;
render.lighting_map.chrome_blur = 4;
render.material.clear();
// The four with no family stayed put.
render.msaa = 4;
render.specular = true;
// @ts-expect-error - the flat spellings are gone
render.ao_radius = 2.5;
// @ts-expect-error - including the switch that used to be the family's root
render.ao = true;
// @ts-expect-error - a group is the host's; there is nothing to assign
render.ao = {};

// The key types are generic over the group now, because the namespace is nested.
const boolKey: RenderBoolKey<Render["ao"]> = "enabled";
const numberKey: RenderNumberKey<Render["sun_shadow"]> = "softness";
const stringKey: RenderStringKey<Render["hdr"]> = "tonemap";
// @ts-expect-error - `enabled` is a boolean, so it is not a number key
const wrongKind: RenderNumberKey<Render["ao"]> = "enabled";
// @ts-expect-error - and a knob from another group is not a key of this one
const wrongGroup: RenderBoolKey<Render["ao"]> = "soft_blur";

// --- one failure convention --------------------------------------------------
//
// Failure throws; a boolean return means a *question was answered*. The two used
// to be indistinguishable - `levels.start()` returning false meant "did not
// start", `actor.damage()` returning false means "did not land", and nothing in
// the surface said which kind you were holding.
levels.start(arena);
settings.save();
make.ammo({ ammo_type: "flares" });
// @ts-expect-error - failure throws, so there is nothing to test
if (levels.start(arena)) {
}
// @ts-expect-error - likewise
const saved: boolean = settings.save();

// These stay boolean, because each answers a question rather than reporting a
// failure.
const landed: boolean = someone.damage(10);
const wasPresent: boolean = settings.remove("mymod.stale");
const wasThere: boolean = mods.exists("Graphics\Bitmaps\Main Menu 01.RIM");

// --- namespace boundaries ----------------------------------------------------
//
// `screen` is presentation. Session control is `game`; the level list is
// `levels`. Every one of these used to be on `screen` because it happens to be a
// console command.
game.toggle_pause();
game.set_speed(1);
game.say("on my way");
game.quit();
levels.next();
levels.add_file("level02.gls", "level02.gcs");
// @ts-expect-error - moved to game
screen.toggle_pause();
// @ts-expect-error - moved to levels
screen.next_level();
// @ts-expect-error - it was a second name for levels.quit()
screen.main_menu();

// --- a vector is {x, y, z}, and an array is not one --------------------------
camera.position = { x: 1, y: 2, z: 3 };
camera.position = { y: 2 }; // a partial update is deliberate
// @ts-expect-error - and this used to be a silent no-op at runtime
camera.position = [1, 2, 3];

// --- console severity --------------------------------------------------------
gameConsole.level = "warn";
gameConsole.warn("this still shows");
gameConsole.debug("this does not");
// @ts-expect-error - not one of the five
gameConsole.level = "verbose";

// --- objectives.add is typed from the handler --------------------------------
objectives.add(0, "primary", 1, 2, 3);
// @ts-expect-error - the priority is one of three words
objectives.add(0, "urgent", 1, 2, 3);
// @ts-expect-error - and the -1 sentinel is the binding's business, not a parameter
objectives.add(0, "primary", 1, -1, 2, 3);

// --- the unexplained internals are gone --------------------------------------
// @ts-expect-error - a count of hierarchy nodes in an unexplained slot range,
// which no script could act on
const nodes: number = roles[0]!.num_hier_nodes_26_30;
// Actor and Role agree on the spelling now.
const actorShields: number = someone.shields;
const roleShields: number = roles[0]!.shields;
