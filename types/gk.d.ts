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

  /** Yaw/roll/pitch in degrees - the units `SET CAMERA ORI` and the .gcs use. */
  export interface CameraOrientation {
    yaw: number;
    roll: number;
    pitch: number;
  }

  /** A partial orientation: components left out keep their current value, the
   *  same rule `camera.position` follows. */
  export type CameraOrientationLike = Partial<CameraOrientation>;

  export interface Camera {
    /** The camera's world position. Assigning a partial object moves only the
     *  components it names. Assigning also rebuilds the view matrix, which is
     *  what `SET CAMERA POS` does - writing the global alone leaves the view
     *  stale until something else happens to rebuild it. */
    get position(): Vec3;
    set position(value: Vec3Like);

    /** All three Euler angles at once, in degrees - `SET CAMERA ORI`. Reading
     *  builds a fresh object; assigning a partial one leaves the rest alone and
     *  costs a single matrix rebuild. */
    get orientation(): CameraOrientation;
    set orientation(value: CameraOrientationLike);

    /** Individual angles, in degrees. Each assignment rebuilds the view matrix,
     *  so prefer `orientation` when setting more than one. */
    yaw: number;
    roll: number;
    pitch: number;

    /** Distance from the camera to its focus point. Writing it snaps: the game
     *  keeps a target and a current value and sets both, so the smoothing has
     *  nothing left to interpolate. */
    distance: number;
    /** The zoom-out limit the game clamps `distance` to. */
    max_distance: number;

    /** The point the camera is pinned to - `SET CAMERA FOCUS`. Null when no
     *  focus is latched, and assigning null is `FREE CAMERA FOCUS`. */
    get focus(): Vec3 | null;
    set focus(value: Vec3Like | null);

    /** Whether the camera is following any actor. Read-only because the two
     *  halves are asymmetric: `stop_tracking()` is local state, while `track()`
     *  broadcasts. */
    readonly tracking: boolean;
    /** `STOP TRACKING`: drops the track list and hands the controls back. */
    stop_tracking(): void;
    /** `TRACK <id>`: follows an actor. Broadcasts (update 0xb4), so it is
     *  authority-gated - a no-op on a joining client, which then receives the
     *  host's. */
    track(actor_id: number): void;
    /** `CAMERA TRACK`: a Bezier camera move through four control points, the
     *  first the start and the last the end. Broadcasts (update 0xaa). */
    bezier_track(p1: Point, p2: Point, p3: Point, p4: Point): void;

    // --- the interpolated moves ---------------------------------------------
    //
    // Unlike the properties above these are command-backed: each one arms a
    // dozen unnamed interpolation globals (start value, target, start time,
    // duration, a mode flag) whose pairing is only written down in the handler.

    /** `SET REQUIRED POS`: eases the focus to `where` over `seconds`. */
    move_to(where: Point, seconds?: number): void;
    /** `SET REQUIRED ORI`, in degrees, over `seconds`. */
    turn_to(yaw: number, roll: number, pitch: number, seconds?: number): void;
    /** `SET REQUIRED DISTANCE`: eases the zoom. */
    zoom_to(distance: number, seconds?: number): void;
    move_and_zoom_to(where: Point, distance: number, seconds?: number): void;
    /** `SET JERKY DISTANCE`: zooms with two deliberate jerks on the way. */
    jerky_zoom_to(distance: number): void;
    /** `ROTATE CAMERA`: n units right (negative for left) over m seconds. */
    rotate(units: number, seconds: number): void;
    /** `ELEVATE CAMERA`: n units down (negative for up) over m seconds. */
    elevate(units: number, seconds: number): void;
    /** `ALTER CAMERA`: offsets by screen fractions - 1 is a whole width. */
    nudge(dx: number, dy: number): void;
    /** `CENTRE`: centres on a unit, by id. */
    center_on(actor_id: number): void;
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

    /** `ECHO ON|OFF`: when false the console's own printing is suppressed. The
     *  `@` command is this toggled for the span of one line. */
    echo: boolean;

    /** Every command the game has registered - 280 registrations, 272 distinct
     *  names, rebuilt on each read.
     *
     *  Worth having because **fifteen of those names are localized**: `EXIT`,
     *  `QUIT`, `MENU`, `HELP`, `LINES`, `CONSOLE APPEAR`, `SAY`, `TIME`, `DATE`,
     *  `LIST COMMANDS`, `CLEAR HISTORY BUFFER`, `HISTORY BUFFER SIZE`,
     *  `HISTORY_BUFFER_LENGTH`, `QUEUE SIZE` and `QUEUE LENGTH` are registered
     *  under strings from `glres<lang>.dll`, so a hard-coded
     *  `execute("QUIT")` does nothing on a French or German install. Searching
     *  this list is the portable way to spell them. */
    readonly commands: ConsoleCommand[];

    // --- administration -------------------------------------------------------
    //
    // Six of these are registered under localized names, which is exactly why
    // they are bound rather than left to `execute`.

    hide(): void;
    /** Writes a note to the game's LOGFILE.TXT. */
    write_log(note: string): void;
    print_version(): void;
    /** Visible lines on the console. */
    set_lines(lines: number): void;
    /** True makes the console appear instantly instead of dropping down. */
    set_appear(instant: boolean): void;
    clear_history(): void;
    /** With no argument, prints the current size. */
    history_size(lines?: number): void;
    queue_size(entries?: number): void;
  }

  /** One entry of `console.commands`. */
  export interface ConsoleCommand {
    /** As registered. Matching is case-insensitive and by longest prefix, so
     *  `SET CAMERA POS` wins over `SET` for a line starting with it. */
    name: string;
    help: string | null;
    /** The minimum `CommandCondition` the engine requires before dispatching
     *  this command. Registrations use 0, 1 and 3. */
    condition: number;
    /** Whether `condition` currently passes. True for everything in a stock
     *  build, where the gate sits at 3 and nothing lowers it. */
    available: boolean;
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
  /** The members every actor has, whatever its class.
   *
   *  **Not every mutator replicates.** Some engine setters broadcast on their
   *  way through and some do not, and the ones that do not are the ones the
   *  console reaches through a command that broadcasts *around* them. The
   *  difference is invisible in single player and a silent desync otherwise, so
   *  each member below says which it is. The short version:
   *
   *  - **Replicates**: `health`, `damage()` (on a mobile actor), `frag()`,
   *    `remove()`, `die()`, `set_target()`, `clear_target()`, `associate()`,
   *    `dissociate()`, `set_weapon()`, `set_ammo_type()`, `attack_target()`,
   *    `attack_position()`, `stop_attacking()`, `set_pickup_enabled()`,
   *    `set_team()`.
   *  - **Local only**: `armor`, `shield`, `mine`, `set_position()`, `goto()`,
   *    `turret_enabled`, `set_required_item()`.
   *
   *  For a local-only one, `console.execute` with the matching command is the
   *  replicating route until the command's update id is bound. */
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
    /** The team this actor is on. Read-only by design: writing goes through
     *  `set_team()`, because changing a team re-registers the actor on two team
     *  actor lists and broadcasts, which an assignment would not look like. */
    readonly team: number;
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

    /** Replicates: `Actor::SetHealth` broadcasts on its way through. */
    health: number;
    /** **Local only** - `Actor::SetArmorValue` reaches no broadcast, while the
     *  `SET ACTOR ARMOUR` command sends update 0x93 around a call to it. Same
     *  trap as `set_position`: correct in single player, a desync otherwise. */
    armor: number;
    /** **Local only**, for the same reason as `armor`. */
    shield: number;
    /** health / the role's strength, so 1 is untouched and 0 is dead. */
    readonly strength_ratio: number;
    /** Whether this actor belongs to the local player.
     *  **Local only** - `SetIsMine` reaches no broadcast. */
    mine: boolean;

    /** Applies damage, returning whether it landed. A negative amount heals.
     *
     *  `attacker_team` is who to credit: in Deathmatch `MobileActor::ApplyDamage`
     *  scores the frag against it when it differs from the victim's team, and it
     *  is the payload of the 0x9b update. The default -1 is "nobody", which is
     *  what an unattributed script hit should be.
     *
     *  On a **mobile** actor this replicates and honours shields, armour and god
     *  mode. On a pickup, blocker or projectile it takes `Actor::ApplyDamage`,
     *  which has none of that and broadcasts nothing - but such an actor has no
     *  networked health anyway, and the outcome that matters, the `Frag` it ends
     *  in at zero strength, *does* replicate. */
    damage(amount: number, flag?: boolean, attacker_team?: number): boolean;
    /** Destroys the actor with its death effects. The wrapper is dead
     *  afterwards: every later access throws. */
    frag(): void;
    /** Removes the actor silently. Also kills the wrapper. */
    remove(): void;
    /** Attaches a trigger script, run when the actor is used or picked up. A
     *  string is a .gcs by name; anything else is a `ScriptMessage`. */
    associate(script: string | ScriptMessage, one_shot?: boolean): void;
    dissociate(): void;
    /** Takes the target's **id**, not the wrapper - see `actor.id`. */
    set_target(id: number, mode?: number): void;
    clear_target(): void;
    /** Moves the actor to another team. Read the current one with `actor.team`.
     *
     *  Goes through `ChangeOwnerAndTeam` (slot 80) rather than the raw
     *  `SetTeamId`, which fixes two things at once: the raw setter writes the
     *  field and nothing else, leaving the actor on its **old team's actor
     *  list**, and it broadcasts nothing. Slot 80 does the list move and sends
     *  updates 0x58 and 0x50 - the latter being what `GIVE CONTROL` sends. */
    set_team(id: number): void;
    /** Moves the actor. Goes through the engine's own setter, so the
     *  orientation and the update timestamps stay consistent.
     *
     *  **Local only.** `Actor::SetPositionAndOrientation` reaches no broadcast,
     *  while the `TELEPORT` command sends two updates (0x3d and 0x70) around its
     *  call to the same setter. So this is `TELEPORT` in single player and a
     *  desync in multiplayer - use `console.execute("TELEPORT ...")` when every
     *  player has to see the move. */
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
     *  own result code. Named after the engine's slot, not a typo.
     *
     *  **Local only** - `MobileActor::Goto` reaches no broadcast, while the
     *  `GOTO` console command issues the order inside the executor on the
     *  authority machine. On a joining client this moves a local ghost. */
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
    /** **Local only** - `SetTurretEnabled` reaches no broadcast. */
    turret_enabled: boolean;
  }

  /** Members PickupActor adds. */
  export interface PickupMembers {
    set_pickup_enabled(enabled: boolean): void;
    /** The name of the role the player must be carrying to collect this.
     *  **Local only** - `SetRequiredItem` reaches no broadcast, unlike
     *  `set_pickup_enabled`, which does. */
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

  /** The engine's named float table - and the way it names actors.
   *
   *  Two things worth knowing before using it as script state:
   *
   *  - **Writes are local.** `SetOrCreateToken` and `SetTokenValue` reach no
   *    broadcast, so a token set on the host is not set on a joining client.
   *    Set it from a `message_received` that every machine runs, not behind a
   *    `game.simulation_running` guard.
   *  - **Tokens are saved.** `SaveGame` writes them and the loader rebuilds
   *    them through `SetOrCreateToken`, which makes them the natural home for
   *    script progress that has to survive a save - see `triggers.create`. */
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

  // --- game ------------------------------------------------------------------

  export interface Game {
    /** Whether the simulation runs in **this** process: true in single player
     *  and on a multiplayer host, false on a joining client and before a level
     *  has started one.
     *
     *  This is the engine's own authority test (`IsExecutorRunning`), which 97 of
     *  its 249 console handlers consult before touching the world. It matters
     *  because **a message is delivered on every machine**: a `message_received`
     *  that spawns, damages or moves anything must do it only where this is true,
     *  or a joining client creates a local ghost the host knows nothing about -
     *  and then receives the host's copy as well.
     *
     *      if (game.simulation_running) roles["Rol_GunLok"].spawn(2, msg.position);
     */
    readonly simulation_running: boolean;
    /** The current game mode, as one of the six `GameModeName` spellings:
     *  `"single_player"`, `"cooperative"`, `"last_man_standing"`, `"president"`,
     *  `"deathmatch"`, `"capture_the_flag"`. */
    readonly mode: string;
    /** The raw `GameState` enum. 5, 6, 7 and 0x12 are the in-level states the
     *  camera commands require; everything else is a front-end or loading
     *  screen. */
    readonly state: number;
    /** Whether the single-player "active pause" is on. Read-only: toggling it
     *  is a clock handshake in multiplayer, so that half stays `PAUSE GAME`. */
    readonly paused: boolean;

    /** Difficulty, as `"easy" | "medium" | "hard" | "extreme"`. Assignable by
     *  name or by the raw 0..3.
     *
     *  This is what the `EASY` / `MEDIUM` / `HARD` / `EXTREME` gate commands
     *  test - each one re-runs the rest of its line only on a match, which is
     *  an ordinary `if` once the value is readable. */
    get difficulty(): DifficultyName;
    set difficulty(value: DifficultyName | number);

    /** `REB GOD`. */
    god_mode: boolean;
    /** `REB INFINITE AMMO`. */
    infinite_ammo: boolean;
    /** `FRIENDLY FIRE`. The same storage as the Preferences menu toggle. */
    friendly_fire: boolean;
    /** `CONTROLS ON|OFF`, phrased as the affirmative. */
    controls_enabled: boolean;
    /** `EPW`. */
    epw: boolean;
    /** `CHROME`. */
    chrome: boolean;
    /** `WIREFRAME` - bit 0x200000 of the renderer's state word. */
    wireframe: boolean;
    /** `SWITCH DETAIL LEVEL`: the low-poly variants of some shapes. */
    low_detail: boolean;
    /** `VISION`. */
    vision_cones: boolean;

    /** `BATTLE NUMBER`: enemies needed before the battle music escalates. */
    battle_number: number;
    /** `SET TRAINING AREA`, 1..6. The offset `TRNTXT` and `REPTXT` add. */
    training_area: number;

    /** The unit `SELECT` picked, or null. Assign an actor **id** (or null to
     *  clear) - a wrapper is a fresh object on every lookup, so ids are the
     *  currency between the bindings and the game. */
    get selected_actor(): Actor | null;
    set selected_actor(value: number | null);
    /** What the mouse is over, or null - what `GET ACTOR ID` reports. */
    readonly actor_under_cursor: Actor | null;

    /** `SPAWN` / `SPAWN TEAM`, without their difficulty scaling: the commands
     *  pass 1, 2 or 3 depending on `difficulty`, which is a one-liner here.
     *  `team` defaults to -1, the commands' own default.
     *
     *  A silent no-op where the simulation is not running - the engine holds
     *  that gate itself, and raising would break a `message_received` that
     *  every machine runs. */
    spawn(amount?: number, team?: number): void;
  }

  export type DifficultyName = "easy" | "medium" | "hard" | "extreme";


  // --- the broadcast-free command namespaces -----------------------------------

  /** A world position. These namespaces accept any `{x, y, z}`; components left
   *  out are sent as 0, which is how the console's own parser treats a short
   *  argument list. */
  export type Point = Vec3Like;

  /** Arguments accepted by the command-backed namespaces below.
   *
   *  Those namespaces format a console command line and run the game's own
   *  handler, rather than reimplementing it. That is deliberate: every one of
   *  those handlers *is* an argument parser, with defaults that come from the
   *  map bounds or the cursor, so running it is faithful by construction. What
   *  the binding adds is typed arguments, locale-independent numbers, a name
   *  that does not depend on the language DLL, and a length check the engine
   *  does not do.
   *
   *  A `boolean` becomes `ON`/`OFF`, a number a decimal, a `{x, y, z}` three
   *  numbers, an array its elements, and `undefined`/`null` ends the argument
   *  list. **A string may not contain whitespace** - the console splits on it
   *  and has no quoting - and that throws rather than silently splitting. */
  export type CommandArg =
    | number
    | boolean
    | string
    | Point
    | readonly CommandArg[]
    | undefined
    | null;

  /** World effects and particles. Everything here is fire-and-forget; nothing
   *  reads back. Positions default to the cursor or the map bounds when
   *  omitted, exactly as the commands do. */
  export interface Fx {
    /** `WATER height corner1 corner2 detail flag` - a body of water over an
     *  area. Omitting the corners uses the map bounds. */
    water(height?: number, corner1?: Point, corner2?: Point, detail?: number, animate?: boolean): void;
    lava(height?: number, corner1?: Point, corner2?: Point, detail?: number, animate?: boolean): void;
    oil(height?: number, corner1?: Point, corner2?: Point, detail?: number, animate?: boolean): void;
    sea(height?: number, corner1?: Point, corner2?: Point, animate?: boolean): void;
    swamp(height?: number, corner1?: Point, corner2?: Point, detail?: number, animate?: boolean): void;

    /** `where` must be any point near the body of liquid. */
    raise_water(where: Point, amount: number): void;
    lower_water(where: Point, amount: number): void;
    raise_lava(where: Point, amount: number): void;
    lower_lava(where: Point, amount: number): void;
    set_water_speed(where: Point, speed: number): void;
    set_water_direction(where: Point, direction: Point): void;

    /** An electrical crackle between two points; amplitude is in decimetres. */
    electricity(start: Point, end: Point, amplitude?: number): void;
    /** Takes either endpoint of a crackle created with `electricity`. */
    deactivate_electricity(endpoint: Point): void;
    laser_fence(start: Point, end: Point): void;

    pulse_rings(start: Point, end: Point, radius: number, travel_time: number, spacing: number): void;
    /** Takes either end of a `pulse_rings` run. */
    remove_pulse_rings(endpoint: Point): void;
    ring(where: Point, sides: number, min_radius: number, max_radius: number): void;
    basin(where: Point, sides: number, lower_radius: number, upper_radius: number): void;

    /** Defaults to the cursor position. */
    explode(where?: Point): void;
    /** `EWS` - explode with smoke. */
    explode_with_smoke(where?: Point): void;
    sparks(where?: Point): void;

    rain(on: boolean): void;
    snow(on: boolean): void;
    lightning(on: boolean): void;
    particle_tester(type: number, where?: Point): void;
    particle_rate(rate: number): void;

    // Broadcasting members. Safe to call unguarded: the handler checks
    // IsExecutorRunning itself, so a joining client no-ops and receives the
    // authority machine's update instead.

    /** Searchlights and explosions over an area. Broadcasts (0xc2). */
    airstrike(corner1: Point, corner2: Point): void;
    /** Smoke billowing from a character. Broadcasts (0xbe). */
    smoke(actor: string): void;
    /** Stops every particle effect on a character. Broadcasts (0xbf). */
    stop_particles(actor: string): void;
    /** `TEXTURE ANIMATE frames u0 v0 u1 v1 ...`. Broadcasts. */
    texture_animate(actor: string, frames: number, uvs: readonly number[]): void;
  }

  /** The lighting commands that do not broadcast. `world` carries the sun,
   *  ambient and fog, which have state to read back; these are one-shot. */
  export interface Light {
    /** Sets ambient to (0,0,0) **and** tears down the dynamic light list, which
     *  `world.set_ambient({r:0,g:0,b:0})` does not. */
    dark(): void;
    fade_in(): void;
    fade_out(): void;
    fade_to_black(seconds?: number, hold_seconds?: number): void;
    fade_from_black(seconds?: number): void;
    corona(where?: Point, color?: readonly number[]): void;
    spotlight(where?: Point): void;
    ray(where?: Point): void;
    /** Recolours every light ray on the level. */
    ray_color(r: number, g: number, b: number): void;
    cylinder(where: Point): void;
    remove_cylinders(): void;
    /** `LIGHTON actor dummy r g b a` - a corona light on an actor's dummy. */
    light_on(actor: string, dummy: string, r: number, g: number, b: number, a: number): void;
    /** Sphere-maps the actor under the cursor. */
    reflect(): void;
    /** A light at a position, RGB each 0..1. Broadcasts (0xc0). */
    add(where: Point, r: number, g: number, b: number): void;
    /** As `add`, but blinking. Broadcasts (0xc1). */
    add_blinking(where: Point, r: number, g: number, b: number): void;
    /** Shadowing on an actor. Broadcasts (0xc4). */
    shadow(actor: string): void;
    /** `ASSOCIATELIGHT lift actor dummy r g b a` - binds a corona light to a
     *  lift so it travels with it. */
    associate(lift: string, actor: string, dummy: string,
              r: number, g: number, b: number, a: number): void;
  }

  /** Mission objectives and the training-level text. */
  export interface Objectives {
    add(...args: CommandArg[]): void;
    /** Moves one step closer to completing the objective. */
    complete(objective: number): void;
    fail(objective: number): void;
    /** Prints an objective string from the localized string table. */
    print(objective: number): void;
    start_printing(delay?: number): void;
    /** `TRNTXT` - offset by `game.training_area`. */
    training_text(index: number): void;
    /** `REPTXT` - the string re-printed by a `wait_for`. */
    repeat_text(index: number): void;
  }

  /** CD music and sound effects. `PLAY ENVIRONMENTAL SOUND` is deliberately
   *  absent: its handler parses a sound id and then calls the player with a
   *  hard-coded argument, discarding it - the command is broken in the game. */
  export interface Music {
    play_sound(id: number): void;
    /** **Broken in the game, and bound anyway.** The handler parses the id,
     *  tests it non-zero, then calls the player with a hard-coded argument and
     *  discards it - so every id plays the same sound. Present because it is a
     *  registered command with no broadcast; omitting it would misrepresent
     *  what the console can do. */
    play_environmental_sound(id: number): void;
    /** `looping` first, then the track numbers. */
    cd_play(looping?: boolean, tracks?: readonly number[]): void;
    cd_stop(): void;
    /** Volume runs 0..65535. */
    cd_fade(volume: number, seconds: number): void;
    cd_auto(on: boolean): void;
    cd_set_volume(volume: number): void;
    cd_tracks(
      category: "AMBIENT" | "SUSPENSE" | "BATTLE" | "VICTORY" | "UPGRADE",
      tracks: readonly number[]
    ): void;
    /** Kills needed before the victory music plays. */
    victory_kills(count: number): void;
  }

  /** Presentation, briefing screens and session control. */
  export interface Screen {
    /** Widescreen bars. Width 0 with a speed scrolls them off. */
    borders(width: number, speed?: number): void;
    borders_off(): void;
    cursor(on: boolean): void;
    system_cursor(): void;
    clear(): void;
    bitmap(name: string): void;
    briefing_text(index: number): void;
    training_debrief_text(index: number): void;
    end_briefing(): void;
    credits(): void;
    /** One of the two end-game FMVs. */
    end_game_fmv(which: "win" | "lose"): void;
    play_fmv(name: string): void;
    play_cutscene(name: string): void;
    status_window(mode: "GENERAL" | "WATCH" | "OFF"): void;
    /** Single player only; in multiplayer it is a vote sent to the server. */
    toggle_pause(): void;
    /** 1 is normal, 0 is the "active pause". Prints the current value when
     *  called with no argument. */
    game_speed(speed?: number): void;
    next_level(): void;
    /** Adds a .gls level to Choose Level. `levels.add` is the script-defined
     *  equivalent and does not need a file. */
    add_mission(script: string, console_script?: string): void;
    add_multiplayer_mission(script: string, console_script?: string): void;
    /** Quits the current game to the front end. The command name is localized,
     *  so this resolves it from the language DLL rather than spelling it. */
    main_menu(): void;
    /** Exits to the desktop. Localized name, as above. */
    quit(): void;
    /** Broadcasts a chat message to the other players. The message may contain
     *  spaces - this handler takes the rest of the line, unlike most.
     *
     *  It does reach the network, but through a native that owns the send
     *  (like `SpawnRole`), not by building an update itself - so running the
     *  real command, which is what this does, is correct. */
    say(message: string): void;
    /** The end-of-mission statistics screen. Unlike the rest of the
     *  broadcasting members this one sends *to* the server, which is the right
     *  direction from a client, so it needs no authority gate. */
    stats(): void;
  }

  /** Per-actor AI and behaviour. Actors are named by **token**, the way the
   *  console names them - see `actor.name`.
   *
   *  The members that broadcast (`ANIM`, `BOARD`, `DEFOGGER`, `FOGGER`,
   *  `GIVE CONTROL`, `PLAYER SELECT`, `REMOVEBB`) are absent. */
  export interface Units {
    set_ai(actor: string, ai: string): void;
    alert_node(node: string): void;
    set_activity(actor: string, activity: "PATROL" | "STOP" | "GOTO"): void;
    add_waypoint(where?: Point): void;
    add_patrol_point(where?: Point): void;
    new_node_waypoint_list(node: string): void;
    make_hunter(actor: string): void;
    make_flare_firer(actor: string): void;
    turn_vision_cone(on: boolean, actor: string): void;
    turn_hearing_range(on: boolean, actor: string): void;
    turret_los(on: boolean, turret: string): void;
    set_scale(scale: number, actor?: string): void;
    delete_team(team: number): void;
    /** Like selecting, but also opens the status window. */
    watch(actor: string): void;
    create_president(): void;
    list_team(team: number): void;
    /** `triggers.create` has no removal counterpart; this is it. */
    remove_trigger(...args: CommandArg[]): void;
    /** Bounds for wandering background creatures. */
    set_upper_left_bound(where: Point): void;
    set_lower_right_bound(where: Point): void;
    /** `VULNERABILITY target vulnerable_to delay type_or_script`. */
    set_vulnerability(
      target: string,
      vulnerable_to: string,
      delay: number,
      type_or_script: string | number
    ): void;

    // Broadcasting members - see the note on `fx`.

    /** Plays an animation on an actor. Broadcasts (0xba). */
    play_animation(actor: string, animation: number): void;
    /** Sends every actor to an object, boarding it if it can be boarded.
     *  Broadcasts (0xb5). */
    board(target: string): void;
    /** Makes the actor defog the map as it moves. Broadcasts (0xb7). */
    make_defogger(actor: string): void;
    /** Reverses `make_defogger`. Broadcasts (0xb8). */
    clear_defogger(actor: string): void;
    /** Moves an actor to a team **and** replicates - this is the command
     *  behind update 0x50, and the same thing `actor.set_team` now does. */
    give_control(actor: string, team: number): void;
    /** As if the player clicked the actor. Broadcasts (0xc3). */
    player_select(actor: string): void;
    /** Drops an actor's bounding box so it stops being clipped out.
     *  Broadcasts (0xbb). */
    remove_bounding_box(actor: string): void;
    /** Anyone within earshot of the unit receives the message, which may
     *  contain spaces. Broadcasts. */
    speak(unit: number, message: string): void;
  }

  /** Giving and equipping items. `REMOVE ITEM` broadcasts and is absent. */
  export interface Inventory {
    give(character: string, item: string): void;
    give_and_say(character: string, item: string): void;
    give_and_equip(character: string, item: string): void;
    give_and_equip_and_say(character: string, item: string): void;
    give_role(role: string, item: string): void;
    /** Gives to the last respawned actor, if it matches the role. */
    give_role_id(item: string): void;
    give_role_team(role: string, team: number, item: string): void;
    give_and_equip_role(role: string, item: string): void;
    give_and_equip_role_id(item: string): void;
    give_and_equip_role_team(role: string, team: number, item: string): void;
    /** Fills a named actor with one of a list of items. */
    heap(actor: string, ...items: string[]): void;
    respawn_heap(index: number, actor: string, ...items: string[]): void;
    next_respawn_id(): void;
    /** Runs `command` only if the item is carried. The condition needs the
     *  engine's inventory walk, which has no binding, so this stays a command. */
    if_carrying(item: string, command: string): void;
    /** Removes every instance of a role from friendly inventories.
     *  Broadcasts (0x7d). */
    remove_item(role: string): void;
  }

  /** Track objects - lifts and moving platforms - plus doors and attachment.
   *
   *  `SET TRACK`, `SET SPEED`, `SET LOOP TIME` and the three door
   *  open/close/toggle commands all broadcast and are absent. */
  export interface Tracks {
    run(name: string): void;
    pause(name: string): void;
    unpause(name: string): void;
    /** Declares a door: a location and an identifying number. */
    declare_door(where: Point, id: number): void;
    /** Attaches a lift to the level so characters can walk on and off it. */
    attach(actor: string): void;
    detach(actor: string): void;

    // Broadcasting members - see the note on `fx`.

    /** `SET TRACK name p1 p2 p3 p4 carries_passengers` - the Bezier the actor
     *  follows. The actor must be AI type `track object`. Broadcasts (0xa9). */
    set(name: string, p1: Point, p2: Point, p3: Point, p4: Point,
        carries_passengers?: boolean): void;
    /** A speed scale for the named track objects. Broadcasts (0xab). */
    set_speed(scale: number, ...names: string[]): void;
    /** Seconds between automatic `run` commands. Broadcasts (0xac). */
    set_loop_time(seconds: number): void;
    /** Doors are addressed by the number given to `declare_door`.
     *  Broadcasts (0xbc / 0xbd; toggle picks one). */
    open_door(id: number): void;
    close_door(id: number): void;
    toggle_door(id: number): void;
  }

  /** Demo recording and playback. */
  export interface Demo {
    record(): void;
    playback(): void;
    save(name: string): void;
    load(name: string): void;
  }

  /** The console command queue's own pacing.
   *
   *  These suspend the **console** queue, not a script, so they only affect work
   *  queued with `console.execute_file`. A script's own sequencing is ordinary
   *  JavaScript - and note there is still no way to schedule a JS callback for
   *  later except a `time` trigger. */
  export interface ScriptPacing {
    wait(seconds: number): void;
    real_wait(seconds: number): void;
    real_wait_or_click(seconds: number): void;
    wait_for(condition: string): void;
    check_wait_for(): void;
    cancel_wait_for(): void;
    print_wait_for(): void;
  }

  // --- world -------------------------------------------------------------------

  /** An RGBA colour with components running 0..1, not 0..255. */
  export interface Color {
    r: number;
    g: number;
    b: number;
    a: number;
  }

  /** A partial colour: components left out keep their current value. */
  export type ColorLike = Partial<Color>;

  /** The fog of discovery. Every property reads through a pointer that is
   *  **null outside a level**, so check `available` before trusting a value -
   *  the getters report 0/false rather than throwing.
   *
   *  These five commands are registered by `BeginLevelSession` rather than at
   *  startup, which is why they only exist during a level in the first place. */
  export interface Fog {
    /** Whether a level is loaded and the fog object exists. */
    readonly available: boolean;
    /** `FOG ON|OFF`. Reading is `mode !== 0`. */
    enabled: boolean;
    /** The raw fog mode the engine picked: 1, 2 or 3 depending on which device
     *  extensions it found. `enabled` is the useful form of this. */
    readonly mode: number;
    /** `FOGVALUE`: how fogged the discovered areas are, 0..1. */
    value: number;
    /** `FOGUPDATE`: complete updates per second. */
    update_rate: number;
    /** `FOGTRANSITION`: metres between fully fogged and fully clear. Setting it
     *  also maintains the reciprocal the renderer actually samples. */
    transition: number;
    /** `FOGCOLOUR`. Assigning a partial colour keeps the rest. */
    get color(): Color;
    set color(value: ColorLike);
  }

  /** The level's atmosphere: the sun, the ambient light and the fog.
   *
   *  Separate from `game` because it is per-level rather than per-session. All
   *  of it is client-side - none of the commands behind it broadcasts, so these
   *  are plain setters with no multiplayer caveat. */
  export interface World {
    /** `SUNANGLE`, in degrees. The engine stores no angle: it derives a
     *  direction vector and discards the angle, so reading recomputes it from
     *  the vector and a round-trip is lossy in the last bits. */
    sun_angle: number;
    /** `SUNANGLE2`, in degrees. Setting it re-applies `sun_angle`, because the
     *  second angle only takes effect through the same direction rebuild. */
    sun_angle2: number;
    /** `SUNBRIGHTNESS`. A method rather than a property because it is genuinely
     *  write-only - the renderer converts and stores it in its own form, so
     *  there is no honest value to hand back. Components default to 1. */
    set_sun_brightness(color: ColorLike): void;
    /** The normalised direction `sun_angle` derives. Read-only: writing it
     *  without renormalising and without the shadow rebuild would leave the two
     *  inconsistent. */
    readonly sun_direction: Vec3;
    /** `AMBIENT`. A method for the same reason as `set_sun_brightness`; RGB
     *  default to 0 and alpha to 1, which is the command's own default.
     *
     *  Not the same as `DARK`, which also tears down the dynamic light list
     *  before setting a constant ambient - only the ambient half is here. */
    set_ambient(color: ColorLike): void;
    readonly fog: Fog;
  }

  // --- script-queue messages -------------------------------------------------

  /** A message payload: anything `JSON.stringify` can encode **except a bare
   *  string**, because a string is a .gcs file name - that is the legacy
   *  meaning, and it is kept.
   *
   *  The engine has one channel for "something happened": a trigger fires and
   *  its payload goes on the script queue, which broadcasts it so every player
   *  consumes its own copy. GkPlus carries JSON on that channel, so a payload
   *  can be data instead of a file, and it arrives at the loaded level's
   *  `message_received`.
   *
   *  `object` rather than a recursive JSON type on purpose: an interface without
   *  an index signature is not assignable to one, which would make ordinary
   *  message types unusable. A value `JSON.stringify` refuses - a function, a
   *  symbol - throws a TypeError at the call instead. */
  export type ScriptMessage = object | number | boolean | null;

  // GkPlus sends no messages of its own: every payload on the queue is either a
  // .gcs the engine asked for, or one a script sent. The engine's own respawn and
  // flag-capture events still queue their stock .gcs, now properly encoded.

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
    /** Overloaded per kind: a **radius** for the location and proximity
     *  kinds, and for the time kinds (`time`, `time_if_alive`, `time_limit`,
     *  `frag_score`) a **delay in seconds** - not a deadline, and not ticks.
     *
     *  `RegisterTriggers` converts it itself:
     *  `deadline = GetServerTime64() + ticks_per_second * value`, using the
     *  *calling thread's* tick rate. So it is seconds from now on the server
     *  clock, which is what the console means by `add trigger time 10`. */
    value?: number | bigint;
    team?: number;
    /** What the trigger fires. A **string** is a .gcs file to run, by name, as
     *  it always was. Anything else is a **message**: it is JSON-encoded and
     *  arrives at the loaded level's `message_received` instead of being opened
     *  as a file.
     *
     *  Either way it reaches every machine - the engine broadcasts the payload
     *  and each player consumes its own copy. */
    script?: string | ScriptMessage;
    /** The actors the trigger watches, **by token name**. */
    targets?: string[];
  }

  export interface Triggers {
    /** Registers a trigger. Silently registers nothing when no level is
     *  running - the engine drops it rather than reporting.
     *
     *  **Local, and that is correct.** `RegisterTriggers` does not broadcast:
     *  in a stock level every machine runs the same `.gcs` and registers its
     *  own copy. So call this from `setup`, which likewise runs everywhere, and
     *  do **not** put it behind `game.simulation_running` - that would leave
     *  joining clients without the trigger. What *is* replicated is the payload
     *  when it fires, so one machine's trigger still reaches every player.
     *
     *  It is also **the only durable scheduling the engine has**. `SaveGame`
     *  writes each trigger's record and its script payload, so a delay armed
     *  here survives a save/load and a process restart - the identity that
     *  comes back is the payload's `kind`, a string in the file. Nothing held
     *  in the JavaScript heap survives, and nothing can: a closure cannot be
     *  serialized. Anything that must outlive a save belongs here, with its
     *  progress in `tokens` (which are saved too). */
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
    /** Runs a script when the object dies. `script` is GLS field 0x00, whose
     *  keyword is `name` - it is spelled `script` here because that is what it
     *  is: the only thing that reads it hands it to the script queue. So it takes
     *  a `ScriptMessage` too, like every other script field. */
    | { kind: "replace"; script: string | ScriptMessage; replace?: boolean }
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
    /** An AI keyword or its id. The keyword is the **underscored** name from
     *  `roles.ai_types` (`bot`, `turret`, `background_creature`), matched with a
     *  plain strcmp - the GLS spelling `background creature` raises a RangeError,
     *  it is not folded. */
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
    /** A .gcs by name, or a `ScriptMessage` - the beam's effect goes on the
     *  script queue like a trigger's. */
    interface_beam_script?: string | ScriptMessage;
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
    /** Needs a loaded level: it offsets the path by the map origin. `name` is
     *  matched against the cutscene tracks in the .rif named by `file`; both
     *  are required. */
    camera_track(desc: {
      name: string;
      file: string;
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
    /** This level's ScriptFileName - `gkplus\<slug>.gls`. A virtual name: no file
     *  of that name exists, and nothing on any load path opens it. */
    readonly script_file: string;
    /** A snapshot of the map description, rebuilt on every read. */
    readonly map: Required<LevelMap>;
    /** True while any of this level's load callbacks is running: `define`,
     *  `populate` or `setup`. */
    readonly loading: boolean;

    /** `levels.start(this, options)` - starts this level, no menus, no
     *  briefing. Deferred to the next turn of the message loop; see
     *  `levels.start` for what that means and what it throws. */
    start(options?: { difficulty?: DifficultyName | number }): boolean;

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
    /** Puts a payload on the engine's script queue, exactly as a firing trigger
     *  does: this machine delivers it from the per-frame pop, every other player
     *  from the broadcast. A string sends a .gcs by name, so
     *  `send("wave2.gcs")` runs that file everywhere.
     *
     *  Not level-scoped despite living here - the queue has one channel, so the
     *  payload reaches whichever level is loaded when it is popped. It sits on
     *  `Level` because that is where `message_received` is, and because inside
     *  that hook this wrapper is the only handle a module has. */
    send(message: string | ScriptMessage): void;
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
    /** The level's inbox: a payload from the script queue that was not a file
     *  name - see `ScriptMessage`. Fires during play rather than during a load,
     *  once per machine, on the main thread.
     *
     *  `level` comes second so the signature reads `message_received(msg)`. It
     *  is there because this is the one hook that runs outside a load, so
     *  `levels.current` is null while it does, and a module has no other way to
     *  reach its own `Level`.
     *
     *  The one `any` in this file, for the reason `JSON.parse` returns one: the
     *  payload's shape is the sender's business, and `unknown` would force a
     *  cast on every field read in a plain .mjs. */
    message_received?: (message: any, level: Level) => void;
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

    /** Every level `start` will take by name: the campaign missions the main
     *  menu seeds, anything `screen.add_mission` added, and every level
     *  `levels.add` registered - they all live in the game's one LevelList, in
     *  Choose Level order. */
    readonly startable: readonly StartableLevel[];

    /** Starts a level outright, with no menus and no briefing screen.
     *
     *  The target is a `Level` from `levels.add`, a title from `startable`
     *  (case-insensitive), or `{script, console}` for a .gls that was never
     *  registered. `difficulty` takes the same names `game.difficulty` does
     *  and defaults to "medium".
     *
     *  **The load happens at the next turn of the message loop, not here.**
     *  LoadLevel may not run inside the renderer, and in a level the script
     *  host's frame callback is driven from inside PresentScene - so this is
     *  callable from anywhere (the REPL, `draw_gui`, a `message_received`) and
     *  returns as soon as the request is accepted. Everything that can be
     *  wrong with the *request* still throws right here; watch `game.state` or
     *  `actors.count` for the result.
     *
     *  Single player only: the sequence forces `GameMode`, so it throws while
     *  a multiplayer session is live. */
    start(
      target: Level | string | { script: string; console?: string },
      options?: { difficulty?: DifficultyName | number },
    ): boolean;

    /** Ends the session and returns to the main menu. Deferred like `start`. */
    quit(): boolean;

    /** True between `start`/`quit` and the load actually running. A second
     *  request while one is pending throws rather than replacing it. */
    readonly start_pending: boolean;

    [Symbol.iterator](): IterableIterator<Level>;
  }

  /** One entry of the game's LevelList - what `levels.start` accepts by name. */
  export interface StartableLevel {
    /** Position in Choose Level, which is also the list order. */
    readonly index: number;
    readonly title: string;
    /** The .gls, or a script-defined level's virtual `gkplus\\<slug>.gls`. */
    readonly script: string;
    /** The .gcs. Empty for every script-defined level. */
    readonly console: string;
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
  export const game: Game;
  export const world: World;
  export const fx: Fx;
  export const light: Light;
  export const objectives: Objectives;
  export const music: Music;
  export const screen: Screen;
  export const units: Units;
  export const inventory: Inventory;
  export const tracks: Tracks;
  export const demo: Demo;
  export const script: ScriptPacing;

  // `menus` is not exported: it is only ever setup_menus' argument, because
  // adding a front-end item is a boot-time act. Keep the argument if you need
  // it later.

  /** The default export carries the same twenty-one objects: `gk.actors === actors`. */
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
    game: Game;
    world: World;
    fx: Fx;
    light: Light;
    objectives: Objectives;
    music: Music;
    screen: Screen;
    units: Units;
    inventory: Inventory;
    tracks: Tracks;
    demo: Demo;
    script: ScriptPacing;
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
