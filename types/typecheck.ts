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
  menus,
  roles,
  tokens,
  triggers,
} from "gk";
import type { Actor, Menus, MenuItem, Role, TurretActor, Vec3 } from "gk";
import * as ImGui from "ImGui";

// --- the module ---------------------------------------------------------------

const sameObject: boolean = gk.actors === actors;
const namespaces: Menus = gk.menus;

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
console.log("host console", 1, true);
console.warn("also here");

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
  console.log(`clicked ${item.label} on ${owner.name}`);
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

// --- ImGui --------------------------------------------------------------------

export function draw_gui(imgui: typeof ImGui): void {
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
