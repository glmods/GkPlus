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
  gls,
  levels,
  make,
  roles,
  tokens,
  triggers,
} from "gk";
// @ts-expect-error - `menus` is not an export: it is only setup_menus'
// argument, because the game's own items must be in place before a script adds
// one.
import { menus as notAnExport } from "gk";
import type {
  Actor,
  GameAsset,
  GlsField,
  GlsSection,
  Level,
  LevelLocator,
  LevelModule,
  Menus,
  MenuItem,
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

// --- console ------------------------------------------------------------------

gameConsole.print("hello");
gameConsole.text_color = 0xff00ff00;
gameConsole.log("host logging", 1, true);
gameConsole.warn("also here");
gameConsole.execute("SPAWN gunlok");
const queuedFile: boolean = gameConsole.execute_file("level01.gcs");

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
  someone.set_team(2);
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
  targets: ["hark", "gunlok"],
  script: "ambush",
});
// @ts-expect-error - `kind` is required
triggers.create({ script: "x" });
// @ts-expect-error - not a trigger kind
const noSuchKind = triggers.kind.explode;

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
const ammoOk: boolean = make.ammo({
  ammo_type: "plasma bolts",
  weapon_type: "plasma pistol",
  role: Rol_Bug,
});
const infoOk: boolean = make.ammo_info({ ammo_type: "flares", shape: hcy, max_per_slot: 50 });
make.camera_track({ name: "camtrack1" });

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
};
const fromModule: Level = levels.add("From A Module", asModule);

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
