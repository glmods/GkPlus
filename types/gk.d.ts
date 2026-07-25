// Types for the "gk" module registered by src/Script.cpp.
//
// Hand-written from the binding sources (src/Js*.cpp) - unlike types/imgui.d.ts,
// which is generated. Keep this in step with them; the bindings are the truth.
//
// Two conventions worth knowing before reading:
//
//   * Every lookup mints a fresh wrapper, so `actors[1] !== actors[1]`. Compare
//     `.id`, never object identity. (A MenuItem is the one exception: add_item
//     returns the same object its callback receives.)
//   * An Actor or Role wrapper holds the raw game pointer. The game frees those
//     without telling us, so a wrapper kept across frames can go stale: `.valid`
//     is the check, and any other access on a destroyed one throws.

declare module "gk" {
  // --- vectors ---------------------------------------------------------------

  export interface Vec3 {
    x: number;
    y: number;
    z: number;
  }

  export interface Vec4 {
    x: number;
    y: number;
    z: number;
    w: number;
  }

  /** Any subset of a Vec3: the components you leave out keep their current
   *  value, so `{z: 100}` raises something without moving it. */
  export type Vec3Like = Partial<Vec3>;

  /** Any subset of a Vec4. Orientations are quaternions; identity is
   *  `{x: 0, y: 0, z: 0, w: 1}`. */
  export type Vec4Like = Partial<Vec4>;

  // --- camera ----------------------------------------------------------------

  export interface Camera {
    /** The camera's world position. Assigning a partial object moves only the
     *  components it names. */
    get position(): Vec3;
    set position(value: Vec3Like);
    /** Distance from the camera to its focus point. */
    distance: number;
    /** The zoom-out limit the game clamps `distance` to. */
    max_distance: number;
  }

  // --- console ---------------------------------------------------------------

  /** The *game's* console (` to open), not the host's global `console`. */
  export interface Console {
    /** Writes one line to the console. */
    print(text: string): void;
    /** ARGB, as 0xAARRGGBB. */
    text_color: number;
    /** ARGB, as 0xAARRGGBB. */
    cursor_color: number;
  }

  // --- actors ----------------------------------------------------------------

  /** What `actor.kind` reports - one per class in src/ActorClasses.inc.h. */
  export type ActorKind =
    | "actor"
    | "mobile"
    | "character"
    | "popup"
    | "turret"
    | "centibody"
    | "centipede"
    | "node"
    | "president"
    | "pickup"
    | "blocker"
    | "projectile"
    | "track_object"
    | "tumbleweed"
    | "background_creature"
    | "flying_background_creature";

  /** The members every actor has, whatever its class. Written out as a separate
   *  interface so each class can carry its own literal `kind` and `Actor` can be
   *  a discriminated union - `if (a.kind === "turret") a.turret_enabled` is the
   *  intended way to reach a subclass member. */
  export interface ActorBase {
    /** Stable for the actor's lifetime, and the key it lives under in
     *  `actors`. Readable even after the actor is destroyed. */
    readonly id: number;
    /** The token that names this actor, or null if none does. Actors have no
     *  name field: the engine names them through the token table. */
    readonly name: string | null;
    readonly kind: ActorKind;
    /** False once the game has destroyed this actor. Never throws, so it is the
     *  one member safe to read on a wrapper you have held across frames. */
    readonly valid: boolean;
    readonly role: Role | null;
    /** What this actor is currently attacking. */
    readonly target: Actor | null;
    /** The actor's centre, which is not its position - `set_position` moves the
     *  origin, this is the middle of its bounding volume. */
    readonly center: Vec3;
    readonly hotspot: string | null;
    readonly ammo: number;
    readonly size: number;

    readonly alive: boolean;
    readonly attacking: boolean;
    readonly enabled: boolean;
    readonly moving: boolean;
    readonly visible: boolean;
    readonly targetable: boolean;
    readonly interactable: boolean;
    readonly can_be_picked_up: boolean;
    readonly has_pending_orders: boolean;

    health: number;
    armor: number;
    shield: number;
    /** health / the role's strength, so 1 is untouched and 0 is dead. */
    readonly strength_ratio: number;
    /** Whether this actor belongs to the local player. */
    mine: boolean;

    /** Applies damage, returning whether it landed. A negative amount heals. */
    damage(amount: number, flag?: boolean): boolean;
    /** Destroys the actor with its death effects. The wrapper is dead
     *  afterwards: every later access throws. */
    frag(): void;
    /** Removes the actor silently. Also kills the wrapper. */
    remove(): void;
    /** Attaches a trigger script, run when the actor is used or picked up. */
    associate(script: string, one_shot?: boolean): void;
    dissociate(): void;
    /** Takes the target's **id**, not the wrapper - see `actor.id`. */
    set_target(id: number, mode?: number): void;
    clear_target(): void;
    set_team(id: number): void;
    /** Moves the actor. Goes through the engine's own setter, so the
     *  orientation and the update timestamps stay consistent. */
    set_position(
      position: Vec3Like,
      orientation?: Vec4Like | null,
      stamp?: number
    ): void;
    toString(): string;
  }

  /** Members MobileActor adds. */
  export interface MobileMembers {
    /** Orders a walk to `destination`. Gated on the actor's strength and the
     *  priority of the order it already has; the return value is the engine's
     *  own result code. Named after the engine's slot, not a typo. */
    goto(destination: Vec3Like, priority?: number): number;
    /** Plays the death sequence. Ends in the actor being freed, so the wrapper
     *  is dead afterwards. */
    die(): void;
    /** 0x21 is "none". */
    set_weapon(type: number): void;
  }

  /** Members CharacterActor adds. */
  export interface CharacterMembers {
    /** The three trailing arguments are unidentified engine parameters; the
     *  engine's own callers pass 0 for an ordinary ranged attack. */
    attack_target(actor: Actor, a?: number, b?: number, c?: number): void;
    attack_position(position: Vec3Like, a?: number, b?: number, c?: number): void;
    stop_attacking(reason?: number): void;
    set_ammo_type(type: number): void;
  }

  /** Members TurretActor adds. */
  export interface TurretMembers {
    turret_enabled: boolean;
  }

  /** Members PickupActor adds. */
  export interface PickupMembers {
    set_pickup_enabled(enabled: boolean): void;
    /** The name of the role the player must be carrying to collect this. */
    set_required_item(name: string): void;
  }

  /** An actor of the base class exactly - `kind === "actor"`. Rare; almost
   *  every actor in a level is one of the subclasses. */
  export interface BaseActor extends ActorBase {
    readonly kind: "actor";
  }

  export interface MobileActor extends ActorBase, MobileMembers {
    readonly kind: "mobile";
  }

  export interface CharacterActor
    extends ActorBase,
      MobileMembers,
      CharacterMembers {
    readonly kind: "character";
  }

  export interface PopupActor extends ActorBase, MobileMembers, CharacterMembers {
    readonly kind: "popup";
  }

  export interface TurretActor
    extends ActorBase,
      MobileMembers,
      CharacterMembers,
      TurretMembers {
    readonly kind: "turret";
  }

  export interface CentibodyActor
    extends ActorBase,
      MobileMembers,
      CharacterMembers {
    readonly kind: "centibody";
  }

  export interface CentipedeActor
    extends ActorBase,
      MobileMembers,
      CharacterMembers {
    readonly kind: "centipede";
  }

  export interface NodeActor extends ActorBase, MobileMembers {
    readonly kind: "node";
  }

  export interface PresidentActor extends ActorBase, MobileMembers {
    readonly kind: "president";
  }

  export interface PickupActor extends ActorBase, PickupMembers {
    readonly kind: "pickup";
  }

  export interface BlockerActor extends ActorBase {
    readonly kind: "blocker";
  }

  export interface ProjectileActor extends ActorBase {
    readonly kind: "projectile";
  }

  export interface TrackObjectActor extends ActorBase {
    readonly kind: "track_object";
  }

  export interface TumbleweedActor extends ActorBase {
    readonly kind: "tumbleweed";
  }

  export interface BackgroundCreatureActor extends ActorBase {
    readonly kind: "background_creature";
  }

  export interface FlyingBackgroundCreatureActor extends ActorBase {
    readonly kind: "flying_background_creature";
  }

  /** Any actor. This is the type to write in your own signatures; narrow it
   *  with `kind` to reach a subclass member. */
  export type Actor =
    | BaseActor
    | MobileActor
    | CharacterActor
    | PopupActor
    | TurretActor
    | CentibodyActor
    | CentipedeActor
    | NodeActor
    | PresidentActor
    | PickupActor
    | BlockerActor
    | ProjectileActor
    | TrackObjectActor
    | TumbleweedActor
    | BackgroundCreatureActor
    | FlyingBackgroundCreatureActor;

  /** Every actor that can move - what `instanceof actors.classes.MobileActor`
   *  narrows to. */
  export type AnyMobileActor =
    | MobileActor
    | CharacterActor
    | PopupActor
    | TurretActor
    | CentibodyActor
    | CentipedeActor
    | NodeActor
    | PresidentActor;

  export type AnyCharacterActor =
    | CharacterActor
    | PopupActor
    | TurretActor
    | CentibodyActor
    | CentipedeActor;

  export type AnyPopupActor = PopupActor | TurretActor;
  export type AnyCentibodyActor = CentibodyActor | CentipedeActor;
  export type AnyBackgroundCreatureActor =
    | BackgroundCreatureActor
    | FlyingBackgroundCreatureActor;

  /** A handle on one Actor class, for `instanceof`. It deliberately has no
   *  construct signature: `new actors.classes.PickupActor()` throws at runtime,
   *  since scripts cannot create actors, so it should not type-check either.
   *  Extending Function is what keeps it usable as the right operand of
   *  `instanceof`, and `prototype` is what makes that narrow. */
  export interface ActorClass<T> extends Function {
    readonly prototype: T;
  }

  /** `actors.classes` - one handle per class in the hierarchy, so
   *  `a instanceof actors.classes.MobileActor` works. Prefer narrowing on
   *  `kind`: it is cheaper, and it reaches the exact class. */
  export interface ActorClasses {
    readonly Actor: ActorClass<Actor>;
    readonly MobileActor: ActorClass<AnyMobileActor>;
    readonly CharacterActor: ActorClass<AnyCharacterActor>;
    readonly PopupActor: ActorClass<AnyPopupActor>;
    readonly TurretActor: ActorClass<TurretActor>;
    readonly CentibodyActor: ActorClass<AnyCentibodyActor>;
    readonly CentipedeActor: ActorClass<CentipedeActor>;
    readonly NodeActor: ActorClass<NodeActor>;
    readonly PresidentActor: ActorClass<PresidentActor>;
    readonly PickupActor: ActorClass<PickupActor>;
    readonly BlockerActor: ActorClass<BlockerActor>;
    readonly ProjectileActor: ActorClass<ProjectileActor>;
    readonly TrackObjectActor: ActorClass<TrackObjectActor>;
    readonly TumbleweedActor: ActorClass<TumbleweedActor>;
    readonly BackgroundCreatureActor: ActorClass<AnyBackgroundCreatureActor>;
    readonly FlyingBackgroundCreatureActor: ActorClass<FlyingBackgroundCreatureActor>;
  }

  export interface ActorsMembers {
    /** How many actors exist. Not enumerable, so it stays out of
     *  Object.keys(actors). */
    readonly count: number;
    readonly classes: ActorClasses;
    [Symbol.iterator](): IterableIterator<Actor>;
  }

  /** Every actor in the level, keyed by id. A name key (`actors["hark"]`) is a
   *  lookup convenience that goes through the token table; only ids are
   *  enumerated, so Object.keys(actors) is the ids in decimal.
   *
   *  Read-only: `actors[1] = x` throws. */
  export type Actors = ActorsMembers & {
    readonly [id: number]: Actor | undefined;
    readonly [name: string]: Actor | undefined;
  };

  // --- roles -----------------------------------------------------------------

  /** The AI type that decides which Actor subclass a role spawns as. */
  export type AIType =
    | "bot"
    | "scavenger"
    | "mine"
    | "minebot"
    | "reserved"
    | "blocker"
    | "waiting"
    | "pathfinder"
    | "track_object"
    | "tumbleweed"
    | "pickup"
    | "background_creature"
    | "flying_background_creature"
    | "centipede"
    | "centibody"
    | "node"
    | "node_waiting"
    | "swarm"
    | "popup"
    | "president"
    | "turret";

  /** A snapshot of `role.character`, not a live view: re-read the property to
   *  see later changes. Speeds are in the engine's own units. */
  export interface CharacterInfo {
    walking_speed: number;
    turning_speed: number;
    aim: number;
    scan_delay: number;
    sight_angle: number;
    sight_range: number;
    hearing_range: number;
    alert_radius: number;
    aggression: number;
    size: number;
    damage_multiplier: number;
    shot_speed_multiplier: number;
    target_cycle_delay: number;
    alarm_delay: number;
    weapon_cycle_time: number;
    strength: number;
    max_weapon: number;
    max_ammo: number;
    max_module: number;
    weapon: number;
    secondary_weapon: number;
    can_turn: boolean;
    alertable: boolean;
    always_cpu_controlled: boolean;
    description: string | null;
  }

  export interface ProjectileInfo {
    gravity: boolean;
    /** Negative damage heals. */
    damage: number;
    sound: number;
    max_range: number;
    blast_damage: number;
    blast_range: number;
  }

  export interface LightInfo {
    red: number;
    green: number;
    blue: number;
    specular_red: number;
    specular_green: number;
    specular_blue: number;
    range: number;
  }

  export interface InventoryInfo {
    description: string | null;
    pickup_name: string | null;
    pickup_radius: number;
    action_on_death: number;
  }

  /** A role is a spawnable template - the GLS definition an actor is made
   *  from. Roles outlive actors; they are destroyed wholesale on level unload. */
  export interface Role {
    readonly id: number;
    readonly name: string | null;
    readonly valid: boolean;
    /** Writable, but only affects actors spawned afterwards. */
    ai: AIType;
    readonly hotspot: string | null;
    readonly alternate_hotspot: string | null;
    readonly hotspot_point: Vec3;
    readonly alternate_hotspot_point: Vec3;
    readonly meta_sound: string | null;
    readonly sever_points: string[];

    /** Null for a role that has no such sub-object. */
    readonly character: CharacterInfo | null;
    readonly projectile: ProjectileInfo | null;
    readonly light: LightInfo | null;
    readonly inventory_info: InventoryInfo | null;

    armor: number;
    shields: number;
    recharge_rate: number;
    resistance_factor: number;
    alpha: number;
    readonly resistance: number;
    readonly limit: number;
    readonly num_hier_nodes_26_30: number;
    readonly num_hier_nodes_21_25: number;

    alpha_fogging: boolean;
    per_vertex_fogging: boolean;
    no_lighting: boolean;
    reflective: boolean;
    destination_selectable: boolean;
    destroy_after_collection: boolean;
    hit_test_ignore: boolean;
    frag_control: boolean;
    moves_on_lifts: boolean;
    status_display: boolean;

    /** Spawns an actor from this role. Null if the engine refused - which it
     *  does silently outside a running level. */
    spawn(
      team?: number,
      position?: Vec3Like | null,
      orientation?: Vec4Like | null,
      owner?: number
    ): Actor | null;
    toString(): string;
  }

  export interface RolesMembers {
    readonly count: number;
    /** name -> numeric AIType, for comparing against a raw engine value. */
    readonly ai_types: Readonly<Record<AIType, number>>;
    [Symbol.iterator](): IterableIterator<Role>;
  }

  /** Every role the level defined, keyed by id, with the GLS `identifier` as a
   *  name key on top. Read-only; mutate through a Role. */
  export type Roles = RolesMembers & {
    readonly [id: number]: Role | undefined;
    readonly [name: string]: Role | undefined;
  };

  // --- tokens ----------------------------------------------------------------

  export interface TokensMembers {
    readonly count: number;
    [Symbol.iterator](): IterableIterator<number>;
  }

  /** The engine's global name -> float table: mission variables, and the actor
   *  names (a token's value IS an actor id).
   *
   *  Unlike the other collections this one is writable, and assignment is an
   *  upsert: `tokens["score"] = 0` creates the token if it does not exist.
   *  Lookup is case-insensitive, and `tokens["rand(6)"]` is a pseudo-token that
   *  returns a fresh integer in [0, 6) on every read. */
  export type Tokens = TokensMembers & {
    [name: string]: number | undefined;
  };

  // --- triggers --------------------------------------------------------------

  export type TriggerKindName =
    | "death"
    | "location"
    | "location_specified"
    | "location_all"
    | "location_timed"
    | "instant_death"
    | "instant_displace"
    | "time"
    | "escort"
    | "proximity"
    | "door"
    | "door_once"
    | "doors_either"
    | "four_doors"
    | "light_up"
    | "defog"
    | "shot"
    | "being_attacked"
    | "frag_score"
    | "time_limit"
    | "time_if_alive"
    | "been_alerted";

  export interface TriggerOptions {
    /** One of `triggers.kind`. */
    kind: number;
    /** Up to four points; what they mean depends on the kind. */
    coords?: Vec3Like[];
    /** Overloaded per kind: a radius for the location and proximity kinds, a
     *  game-tick deadline for the time ones. */
    value?: number | bigint;
    team?: number;
    /** The trigger script to run, by name. */
    script?: string;
    /** The actors the trigger watches, **by token name**. */
    targets?: string[];
  }

  export interface Triggers {
    /** Registers a trigger. Silently registers nothing when no level is
     *  running - the engine drops it rather than reporting. */
    create(options: TriggerOptions): void;
    readonly kind: Readonly<Record<TriggerKindName, number>>;
  }

  // --- menus -----------------------------------------------------------------

  /** The identifiers from src/Menus.inc.h. Menu name lookup is
   *  case-insensitive, so `menus.Main` and `menus["main"]` are the same menu. */
  export type MenuName =
    | "Main"
    | "Options"
    | "ScreenMode"
    | "GfxCard"
    | "Keyboard"
    | "ChooseSinglePlayerLevel"
    | "MouseControls"
    | "SinglePlayer"
    | "LoadSinglePlayerGame"
    | "NewSinglePlayerGame"
    | "Multiplayer"
    | "JoinGame"
    | "JoinIPGame"
    | "HostIPGame"
    | "MultiplayerLevel"
    | "MultiplayerOptions"
    | "MultiplayerPlayers"
    | "MultiplayerGameType"
    | "LoadGameInGame"
    | "Preferences"
    | "ConfirmScreenMode"
    | "TrainingLevel"
    | "Video"
    | "Controls"
    | "GraphicDetails"
    | "Audio"
    | "GamePreferences"
    | "IPGame"
    | "Camera"
    | "Mines"
    | "Character"
    | "Gameplay"
    | "ActivePause"
    | "Recon"
    | "Formations"
    | "Music";

  /** The renderer's name for an item type. */
  export type MenuItemType = "plain" | "value" | "toggle" | "choice" | "unknown";

  /** One entry of `menu.items` - a snapshot of what is on the menu, the game's
   *  own items included. */
  export interface MenuItemInfo {
    index: number;
    /** Already resolved through the localized string table. */
    label: string | null;
    type: MenuItemType;
  }

  /** An item this script added. The same object comes back from `add_item` and
   *  arrives at the callback, so `item === e` holds. */
  export interface MenuItem {
    readonly label: string;
    /** Where the item sits in its menu, or -1 until that menu has been drawn
     *  once - items are appended lazily, after the game's own. */
    readonly index: number;
    readonly menu: Menu;
    /** The toggle state, or undefined for a plain item - which makes
     *  `item.value !== undefined` a working "is this a toggle" test. Reading is
     *  always safe; *assigning* to a plain item's throws. */
    value: boolean | undefined;
    toString(): string;
  }

  export type MenuItemCallback = (item: MenuItem) => void;

  /** One of the 36 front-end menus. In-game (HUD) menus are not exposed. */
  export interface Menu {
    /** 0-35, its index in the game's Menus array. */
    readonly id: number;
    readonly name: MenuName;
    /** The localized title, or "" for a menu whose id has no string in the
     *  active language. */
    readonly title: string;
    readonly count: number;
    /** A snapshot, rebuilt on every read. */
    readonly items: MenuItemInfo[];

    /** Appends an item. It shows up the next time this menu is drawn - after
     *  the game's own items, so the game's indices keep working - and comes
     *  back by itself if the game rebuilds the menu.
     *
     *  There is no removal, and only six rows are visible at a time: a long
     *  menu scrolls. */
    add_item(label: string, callback?: MenuItemCallback): MenuItem;
    /** Appends an ON / OFF item. It flips itself before the callback runs. */
    add_toggle(
      label: string,
      initial?: boolean,
      callback?: MenuItemCallback
    ): MenuItem;
    /** Navigates to this menu. `remember` makes the current menu its parent, so
     *  Back returns here. */
    open(remember?: boolean): void;
    toString(): string;
  }

  export interface MenusMembers extends Record<MenuName, Menu> {
    /** Always 36. */
    readonly count: number;
    /** The menu the front end is showing. Meaningless while a level is
     *  running - the game keeps the last value. */
    readonly current: Menu | null;
    [Symbol.iterator](): IterableIterator<Menu>;
  }

  /** The front-end menus, keyed by id 0-35 and by name. */
  export type Menus = MenusMembers & {
    readonly [id: number]: Menu | undefined;
    readonly [name: string]: Menu | undefined;
  };

  // --- the module ------------------------------------------------------------

  export const camera: Camera;
  export const console: Console;
  export const actors: Actors;
  export const roles: Roles;
  export const tokens: Tokens;
  export const triggers: Triggers;
  export const menus: Menus;

  /** The default export carries the same seven objects: `gk.actors === actors`. */
  const gk: {
    camera: Camera;
    console: Console;
    actors: Actors;
    roles: Roles;
    tokens: Tokens;
    triggers: Triggers;
    menus: Menus;
  };
  export default gk;

  // --- the entry module's contract -------------------------------------------

  /** The shape of `export function setup_menus`. Called once at startup, after
   *  the game has filled its own menus. */
  export type SetupMenus = (menus: Menus) => void;

  /** The shape of `export function draw_gui`. Called every frame the F11
   *  overlay is open, inside an active ImGui frame. Throwing disables it for
   *  the rest of the session. */
  export type DrawGui = (imgui: typeof import("ImGui")) => void;
}

/** The host installs its own console - QuickJS core has none. It writes to the
 *  game console and to the debugger. Not the same object as the `console`
 *  exported by "gk", which is the game's console. */
declare const console: {
  log(...args: unknown[]): void;
  info(...args: unknown[]): void;
  warn(...args: unknown[]): void;
  error(...args: unknown[]): void;
  debug(...args: unknown[]): void;
};
