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
  /** The game's console and the host's logging, in one object. There is no
   *  global `console`: import this one. */
  export interface Console {
    /** Writes one line to the game console. */
    print(text: string): void;

    /** Joins its arguments with spaces and writes them to the game console and
     *  to the debugger, a line at a time. All five are the same function - the
     *  host has no severity levels. */
    log(...args: unknown[]): void;
    info(...args: unknown[]): void;
    warn(...args: unknown[]): void;
    error(...args: unknown[]): void;
    debug(...args: unknown[]): void;

    /** Runs one console command, exactly as typing it would. */
    execute(command: string): void;
    /** *Queues* a file of console commands - what a level's `.gcs` is. The
     *  lines are appended to the console's command queue, which runs one per
     *  frame, so nothing has happened yet when this returns. The path is
     *  resolved against the game's current directory, which moves during a
     *  level load. `//` starts a comment and a leading `#` is a directive
     *  (`ONLY IF SAFE`, `ONLY IF HINTS ON`, `CLEAR BATCH`,
     *  `EXECUTE IMMEDIATELY`, `NORMAL EXECUTION`); lines are capped at 249
     *  characters. Returns whether the file opened. */
    execute_file(path: string): boolean;

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

  // --- gls: definitions without the parser -------------------------------------

  /** The fifteen GLS section keywords, spelled as `gls` factory names. */
  export type GlsSection =
    | "shape"
    | "hierarchy"
    | "pgenerator"
    | "light"
    | "projectile"
    | "destructibility"
    | "frag_data"
    | "replace_destructibility"
    | "role"
    | "character"
    | "ammo"
    | "ammo_info"
    | "camera_track"
    | "map"
    | "directory";

  /** How a field's value is typed, as its section's constructor declared it. */
  export type GlsFieldType =
    | "boolean"
    | "integer"
    | "float"
    | "string"
    | "object"
    | "none";

  /** One field of a section, read off the game rather than from a table here. */
  export interface GlsField {
    /** The JS spelling - the GLS keyword with spaces as underscores. */
    name: string;
    /** The GLS keyword itself, e.g. "walking speed". */
    keyword: string;
    type: GlsFieldType;
    /** True when the object will not convert until this field is assigned. */
    required: boolean;
    /** String and object fields only: whether `null` (GLS `none`) is accepted. */
    none_ok: boolean;
    min?: number;
    max?: number;
  }

  // --- make: building game objects natively ------------------------------------

  /** A Shape or Hierarchy out of the .rif cache. Borrowed - the cache owns it, so
   *  the same handle can be given to any number of roles. */
  export interface GameAsset {
    readonly __asset: unique symbol;
  }

  /** Either an existing handle, or `{rif, object}` to look one up inline. A
   *  hierarchy may also carry the hotspot names, which is where GLS puts them. */
  export type AssetRef =
    | GameAsset
    | { rif: string; object: string; hotspot?: string; alternate_hotspot?: string };

  /** A `character` block. Values are in .gls units - degrees, seconds, metres,
   *  animation cycles per second - and every default is the game's own. */
  export interface CharacterDesc {
    walking_speed?: number;
    turning_speed?: number;
    strength?: number;
    aim?: number;
    sight_angle?: number;
    sight_range?: number;
    hearing_range?: number;
    aggression?: number;
    scan_delay?: number;
    scan_acceptance_angle?: number;
    angular_scan_rate?: number;
    mine_laying_time?: number;
    damage_multiplier?: number;
    shot_speed_multiplier?: number;
    target_cycle_time?: number;
    weapon_cycle_time?: number;
    weapon_cycle_time2?: number;
    alarm_delay?: number;
    gun_yaw_angle?: number;
    elevation_angle?: number;
    alert_radius?: number;
    /** 0 means "use the model's bounding box" - the default, and what almost
     *  every shipped character relies on. */
    radius?: number;
    height?: number;
    size?: number;
    initial_first_person_range?: number;
    maximum_first_person_range?: number;
    /** A weapon keyword, or the raw id. 33 = none, which has no keyword. */
    weapon?: string | number;
    secondary_weapon?: string | number;
    description?: number;
    status_window_u?: number;
    status_window_v?: number;
    blob_shadow?: number;
    generation_limit?: number;
    max_weapon?: number;
    max_ammo?: number;
    max_module?: number;
    can_turn?: boolean;
    draw_vision_cone?: boolean;
    draw_hearing_range?: boolean;
    always_cpu_controlled?: boolean;
    alertable?: boolean;
    latch_trigger?: boolean;
    customisation_hierarchy?: AssetRef;
    shadow_hierarchy?: AssetRef;
  }

  export interface LightDesc {
    red?: number;
    green?: number;
    blue?: number;
    specular_red?: number;
    specular_green?: number;
    specular_blue?: number;
    range?: number;
  }

  export interface ProjectileDesc {
    gravity?: boolean;
    damage?: number;
    sound?: number;
    max_range?: number;
    blast_damage?: number;
    blast_range?: number;
    hit_light?: LightDesc;
  }

  export interface ParticleGeneratorDesc {
    /** A particle keyword (`smoke`, `corona`, `laser trail`, ...) or its id. */
    type?: string | number;
    rate?: number;
    coords?: Vec3;
    red?: number;
    green?: number;
    blue?: number;
    alpha?: number;
    start_scale?: number;
    end_scale?: number;
    spin?: number;
    /** Converted to ticks at the calling thread's clock rate. */
    particle_ttl_seconds?: number;
    generate_generators?: boolean;
    /** GLS `life`, as its raw dword pair - the encoding of `infinite` is not
     *  recovered, so copy it off a parsed generator if you need to match one. */
    life_low?: number;
    life_high?: number;
  }

  /** A role's death behaviour. The three variants of the destructibility family,
   *  discriminated by `kind`. */
  export type DestructibilityDesc =
    | { kind: "explode" | "splatter" }
    | { kind: "replace"; name: string; replace?: boolean }
    | {
        kind: "frag";
        role?: Role;
        replace_role?: Role;
        remove?: string;
        scale?: number;
        replace?: boolean;
        symmetric?: boolean;
        blast_range?: number;
        blast_damage?: number;
      };

  /** A `role` block. Sub-objects are described inline and built inside
   *  `make.role`, because the Role takes ownership of every one of them. */
  export interface RoleDesc {
    /** GLS `identifier` - this, not `name`, is what `roles["..."]` finds. */
    identifier?: string;
    /** Set at most one. A hierarchy is what enables hotspot resolution. */
    shape?: AssetRef;
    hierarchy?: AssetRef;
    hotspot?: string;
    alternate_hotspot?: string;
    character?: CharacterDesc;
    light?: LightDesc;
    projectile?: ProjectileDesc;
    pgen?: ParticleGeneratorDesc;
    pgen2?: ParticleGeneratorDesc;
    destructibility?: DestructibilityDesc;
    inventory_shape?: AssetRef;
    inventory_hierarchy?: AssetRef;
    /** An AI keyword (`bot`, `turret`, `background creature`, ...) or its id. */
    ai?: string | number;
    /** `must drop` / `must not drop`, or the id. */
    action_on_death?: string | number;
    /** `resists laser` / `resists small arms` / `resists explosives` /
     *  `resists epulsar`, or the id. Not a bitmask - these do not combine. */
    resistance?: string | number;
    resistance_factor?: number;
    /** Comma-separated node names, exactly as the GLS field takes them. */
    sever_points?: string;
    meta_sound?: string;
    description?: number;
    pickup_name?: number;
    pickup_radius?: number;
    recon_name?: number;
    recon_ai_short?: number;
    recon_ai_number?: number;
    recon_ai_long?: number;
    recon_ai_long2?: number;
    interface_beam_delay?: number;
    /** A VulnerabilityType id; no keyword table has been recovered for it. */
    interface_beam_effect?: number;
    interface_beam_script?: string;
    interface_beam_duration?: number;
    limit?: number;
    alpha?: number;
    armor?: number;
    shields?: number;
    recharge_rate?: number;
    /** Wins over per_vertex_fogging, which is forced off when this is set. */
    alpha_fogging?: boolean;
    per_vertex_fogging?: boolean;
    no_lighting?: boolean;
    reflective?: boolean;
    destination_selectable?: boolean;
    destroy_after_collection?: boolean;
    hit_test_ignore?: boolean;
    frag_control?: boolean;
    moves_on_lifts?: boolean;
    status_display?: boolean;
  }

  export interface AmmoDesc {
    ammo_type?: string | number;
    weapon_type?: string | number;
    name?: string;
    file?: string;
    round_time?: number;
    reload_time?: number;
    life_timer?: number;
    magazine_size?: number;
    salvo_size?: number;
    sound?: number;
    firing_speed?: number;
    role?: Role;
  }

  export interface AmmoInfoDesc {
    ammo_type?: string | number;
    shape?: AssetRef;
    hierarchy?: AssetRef;
    ammo_name?: number;
    description?: number;
    max_per_slot?: number;
  }

  /** Builds live game objects through the game's own conversion logic,
   *  reimplemented natively - no parser, and a definition costs a few dozen bytes
   *  rather than the 0x1b60 a parsed object does.
   *
   *      const role = make.role({
   *        identifier: "bug",
   *        hierarchy: { rif: "units\\bug.rif", object: "bug", hotspot: "head" },
   *        character: { walking_speed: 1.5, strength: 1, aggression: 0.1 },
   *        ai: "background creature",
   *      });
   *
   *  `make.role` registers the Role in the roles hash, so call it once per level
   *  load - the hash is cleared between levels - not once at startup. */
  export interface Make {
    /** A .rif lookup. The cache owns the result, so the handle is reusable. */
    shape(rif: string, object: string): GameAsset;
    hierarchy(rif: string, object: string): GameAsset;
    /** Builds and registers a Role, with every sub-object built inline. */
    role(desc: RoleDesc): Role;
    /** Fills the Ammo slot at [ammo_type + weapon_type * 19]. False when that
     *  slot is already taken - the first definition of a pair wins. */
    ammo(desc: AmmoDesc): boolean;
    /** Fills AmmoInfos[ammo_type]. */
    ammo_info(desc: AmmoInfoDesc): boolean;
    /** Needs a loaded level: it binds against the level .rif by name. */
    camera_track(desc: {
      name: string;
      pgen?: ParticleGeneratorDesc;
      pgen2?: ParticleGeneratorDesc;
    }): boolean;
  }

  /** What only the GLS *parser* can answer. Building objects is `make`. */
  export interface Gls {
    /** The fifteen section keywords, as `schema` takes them. */
    readonly sections: GlsSection[];
    /** The field table a section constructor declares about itself - types,
     *  keywords, which are required, and the bounds CheckValue enforces. Read
     *  off the game, so it cannot drift from it. */
    schema(section: GlsSection): GlsField[];
    /** Recovers the integer behind a GLS enum keyword (`ai bot`, `type explode`)
     *  by handing the parser a one-field section per name and reading the stored
     *  value back. `null` means the parser rejected that keyword, which doubles
     *  as a validity check.
     *
     *  These keywords are compiled into the lexer's DFA rather than stored as
     *  strings, so this is the only way to learn their values short of a
     *  debugger. It parses a throwaway script, so do NOT call it while a level is
     *  loading - the GLS parser uses destructive global state. */
    probe(
      section: GlsSection,
      field: string,
      names: string[]
    ): Record<string, number | null>;
    /** Parses arbitrary .gls text and returns how many objects reached the
     *  parsed-object list, or -1 if none did. The bisection tool for working out
     *  why a generated section will not parse - note that -1 covers both "syntax
     *  error" and "every section was demoted to abstract", which the parser does
     *  not distinguish to callers.
     *
     *  Nothing is converted, so it is safe to call repeatedly - but it is the
     *  same destructive global parser state, so not during a level load. */
    try_parse(source: string): number;
  }

  // --- levels ------------------------------------------------------------------

  /** The `map { ... }` block a custom level replaces its .gls with, field for
   *  field. Only `rif` and `object` are required; leaving an optional string out
   *  is the same as omitting the line from a .gls. */
  export interface LevelMap {
    /** The level geometry .rif, relative to the RIFs directory - GLS `file`. */
    rif: string;
    /** The object *inside* that .rif to build the map from - GLS `name`. */
    object: string;
    /** Loading/HUD bitmap; omitted means `none`. */
    bitmap?: string;
    /** Name of a .loc locator; omitted means `none`. */
    camera_plane?: string;
    /** 10..500. Defaults to 60. */
    max_camera_distance?: number;
    max_camera_focus_height?: string;
    min_camera_focus_height?: string;
    shadow_object_rif?: string;
    shadow_object_name?: string;
    /** 10..10000. Defaults to 200. */
    max_vertices_per_section?: number;
  }

  /** One object of a given name in the level .rif, already in world space - the
   *  `for "<rif object>"` half of a .gls `use` clause. */
  export interface LevelLocator {
    position: Vec3;
    orientation: Vec4;
  }

  /** Extra arguments to `level.spawn`. */
  export interface SpawnOptions {
    /** Creates a token holding the new actor's id, which is how the engine names
     *  actors - the `as "<token>"` clause of a .gls `use`. */
    as?: string;
  }

  /** A level registered by this script. The same object comes back from
   *  `levels.add` and arrives at every load callback. */
  export interface Level {
    readonly title: string;
    /** The generated prelude script, which is this level's ScriptFileName. */
    readonly script_file: string;
    /** A snapshot of the map description, rebuilt on every read. */
    readonly map: Required<LevelMap>;
    /** True while any of this level's load callbacks is running: `define`,
     *  `populate` or `setup`. */
    readonly loading: boolean;

    /** Every object of that name in the level .rif, in the coordinates the game
     *  would have spawned a placed object at. Throws outside a load callback,
     *  and is empty before the geometry exists. */
    locators(name: string): LevelLocator[];
    /** Spawns `role` for `team` at `where` - a locator, or any `{x, y, z}`.
     *  Returns the new actor id, or -1 if nothing spawned. Throws outside a
     *  load callback, before the geometry exists (so not from `define`), or if
     *  no role of that name is registered. */
    spawn(
      role: string,
      team: number,
      where: LevelLocator | Vec3,
      options?: SpawnOptions
    ): number;
    toString(): string;
  }

  /** The parts of a level that are not the map itself. A level module supplies
   *  them as named exports; an inline description as properties. */
  export interface LevelBody {
    /** .gls/.gsh files defining the roles, characters and ammo the level's
     *  actors need, relative to the game's Scripts directory. They are parsed in
     *  one pass, so their `#include` guards work exactly as in a hand-written
     *  level.
     *
     *  Leave this out entirely if the level builds its definitions with `gls`
     *  instead - that is the no-GLS-parsing path. */
    includes?: string | string[];
    /** Registers this level's roles, characters and ammo. Runs once per load,
     *  before the map is even converted - where a .gls's `#include` block sits.
     *
     *  It has to be per load rather than once at startup, because the roles hash
     *  is cleared between levels; the `GlsObject`s themselves are reusable, so
     *  keep them at module scope and call `register()` from here. */
    define?: (level: Level) => void;
    /** Runs once per load of this level, after the geometry exists and before
     *  the camera settles - the window a .gls's placed objects spawn in. */
    populate?: (level: Level) => void;
    /** Stands in for the level's .gcs, and runs exactly where the game would
     *  have run one: last, with the world already built and the camera settled.
     *  This is where triggers get armed, fog and camera bounds get set, and
     *  console commands get issued.
     *
     *  Skipped when a savegame is restored, for the same reason the .gcs is -
     *  the save already holds whatever it set up. */
    setup?: (level: Level) => void;
  }

  /** What `levels.add` takes when the level is described inline: the map fields
   *  flat on the object, beside the hooks. */
  export interface LevelDescription extends LevelMap, LevelBody {}

  /** The shape of a level module - and, because a module namespace *is* an
   *  object with the map nested under `map`, a description `levels.add` accepts
   *  as-is:
   *
   *      // arena.mjs
   *      export const map = { rif: "...", object: "Land" };
   *      export const includes = ["gunlok.gsh"];
   *      export function populate(level) { ... }
   *      export function setup(level) { ... }
   *
   *      // main.mjs
   *      import * as arena from "./levels/arena.mjs";
   *      levels.add("Test Arena", arena);
   */
  export interface LevelModule extends LevelBody {
    map: LevelMap;
  }

  export interface LevelsMembers {
    readonly count: number;
    /** The level being loaded right now, or null outside a load callback. */
    readonly current: Level | null;
    /** Registers a level built from a script instead of a .gls + .gcs pair, and
     *  appends it to Choose Level - along with a "Choose Level" item on Single
     *  Player, since the game's own one needs `-chooselevel`.
     *
     *  `source` is the description object: either flat (`LevelDescription`) or
     *  with the map nested under `map`, which is what a level module's namespace
     *  gives you - `import * as arena` and pass it straight in.
     *
     *  Either way the map is validated here, through the game's own field
     *  checks, so a bad value throws now rather than halfway through a load. */
    add(title: string, source: LevelDescription | LevelModule): Level;
    [Symbol.iterator](): IterableIterator<Level>;
  }

  /** The levels this script registered, keyed by registration order and by
   *  title. The game's own campaign levels are not in here. */
  export type Levels = LevelsMembers & {
    readonly [index: number]: Level | undefined;
    readonly [title: string]: Level | undefined;
  };

  // --- the module ------------------------------------------------------------

  export const camera: Camera;
  export const console: Console;
  export const actors: Actors;
  export const roles: Roles;
  export const tokens: Tokens;
  export const triggers: Triggers;
  export const levels: Levels;
  export const make: Make;
  export const gls: Gls;

  // `menus` is not exported: it is only ever setup_menus' argument, because
  // adding a front-end item is a boot-time act. Keep the argument if you need
  // it later.

  /** The default export carries the same nine objects: `gk.actors === actors`. */
  const gk: {
    camera: Camera;
    console: Console;
    actors: Actors;
    roles: Roles;
    tokens: Tokens;
    triggers: Triggers;
    levels: Levels;
    make: Make;
    gls: Gls;
  };
  export default gk;

  // --- the entry module's contract -------------------------------------------

  /** The shape of `export function setup_menus`. Called once at startup, after
   *  the game has filled its own menus.
   *
   *  The argument is the only way to reach `menus`: there is no export for it,
   *  because the game's own items must be in place before a script adds one. */
  export type SetupMenus = (menus: Menus) => void;

  /** The shape of `export function draw_gui`. Called every frame the F11
   *  overlay is open, inside an active ImGui frame. Throwing disables it for
   *  the rest of the session.
   *
   *  The argument is the only way to reach ImGui: there is no module to import
   *  it from, because nothing it offers works outside this callback. */
  export type DrawGui = (imgui: ImGui) => void;
}

// There is deliberately no global `console` declared here, because the host
// installs none: QuickJS core has no console object, and GkPlus puts log/info/
// warn/error/debug on the `console` exported by "gk" instead of adding a second
// one. `import { console } from "gk"` is how a script gets it.
