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
    /** Crouched. Slot 63, and it is `MobileActor`-only: `PickupActor` carries
     *  the base implementation, which always answers false. */
    readonly crouched: boolean;
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
    /** Camouflaged - hidden in water or a scrap pile, which the AI treats as a
     *  hard skip in every target-acquisition loop. Set by crouching next to
     *  cover, not directly by the player; see `stealth_and_fog_notes.md`.
     *  **Local only** - the setter (slot 9) reaches no broadcast, while the
     *  game's own crouch toggle broadcasts 0x4c/0x4e carrying this flag. */
    concealed: boolean;

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

  // --- the game's own text layer ---------------------------------------------

  /** One of the four fonts `InitConsole` builds. The names are GkPlus's - the
   *  game has no string for any of them - and describe the role each actually
   *  plays, established from the `.RIM` it is built from and its consumers.
   *
   *  - `small` - `small font.RIM`. The default UI text: the console, both menu
   *    systems, the inventory, briefing body text, the version stamp.
   *  - `large` - `large font.RIM`, a higher-resolution sheet with ~26% wider
   *    glyphs. The emphasis font: loading messages, the credits.
   *  - `hud` - `small font 2.RIM`. The in-game HUD, target info and reticule.
   *  - `heading` - `large font.RIM` at 2x-3x. The oversized heading on the
   *    briefing, training-debrief and stats pages, and the only font whose
   *    `line_height` is not 25px. */
  export type FontName = "small" | "large" | "hud" | "heading";

  /** A normalised 0..1 screen rectangle: the layout box, not the text's bounds.
   *  Omitted members default to the full screen.
   *
   *  `bottom` never reaches the drawn item - it is purely the limit that
   *  `clip_to_bottom` truncates against. */
  export interface TextRect {
    left?: number;
    top?: number;
    right?: number;
    bottom?: number;
  }

  export interface TextDrawOptions {
    /** The string. Longer than `text.max_length` is **truncated**, not refused:
     *  the engine's layout buffer is 1028 bytes and unbounded, and a longer
     *  string smashes its stack rather than rendering. */
    text: string;
    /** Defaults to `"small"`, the font the game's own version stamp, every
     *  console line and 32 of its 39 text draws use. */
    font?: FontName;
    rect?: TextRect;
    /** A D3DCOLOR, e.g. `0xff00e500`. Defaults to opaque white. */
    color?: number;
    /** A bitwise-or of `text.flags`. */
    flags?: number;
    /** Characters to lay out; 0 or absent means the whole string. */
    max_chars?: number;
    /** 0..1 between the target camera's near and far z planes. 0 is nearest,
     *  which is what an overlay wants. */
    depth?: number;
    /** Only read when `flags` includes `last_char_alt_color`, and only then does
     *  the final character use it instead of `color`. */
    alt_color?: number;
    /** Scroll offset: start at this line and shift the text up accordingly. */
    skip_lines?: number;
  }

  /** The nine layout/render flag bits. Four are acted on when the text is
   *  queued, five when it is drawn. */
  export interface TextFlags {
    /** Lay out and measure, emitting nothing. */
    readonly measure_only: number;
    readonly align_center: number;
    /** The console's caret - glyph 0x40 at `ConsoleCursorPos`. Unrelated to
     *  `last_char_alt_color`. */
    readonly console_cursor: number;
    readonly no_layout: number;
    /** The **final character only** is drawn in `alt_color`. */
    readonly last_char_alt_color: number;
    /** A black outline: the glyph is drawn four more times at +-1px diagonals,
     *  so five draws per glyph. */
    readonly outline: number;
    /** Grow upwards from the bottom of the rect. */
    readonly anchor_bottom: number;
    /** Truncate at the last line above `rect.bottom`. **Only honoured when
     *  `anchor_bottom` is clear** - the engine tests the pair together. */
    readonly clip_to_bottom: number;
    /** `align_center` wins if both are set. */
    readonly align_right: number;
  }

  /** The engine's own text layer.
   *
   *  **`draw` queues; it does not draw.** The string is laid out immediately and
   *  appended to the font's pending list, which the game's per-frame overlay
   *  pass rasterizes and then frees - so **a string drawn once is on screen for
   *  one frame**. Anything meant to persist has to be drawn again every frame.
   *
   *  For a panel, the ImGui object handed to `draw_gui` is the better tool. This
   *  is for text that has to look like the game's, because it goes through the
   *  game's fonts, colours, layout and batching.
   *
   *  Main thread only. */
  export interface Text {
    /** Queue one string for this frame. Returns the number of lines laid out, or
     *  0 if it clipped. */
    draw(options: TextDrawOptions): number;
    /** Line height as a fraction of screen height - the unit `rect`'s vertical
     *  members are in. Defaults to `"small"`.
     *
     *  All four fonts are built at 25px; `heading` reports double because a
     *  scale factor is applied to it after construction, so ask rather than
     *  assuming. */
    line_height(font?: FontName): number;
    readonly fonts: readonly FontName[];
    /** 1027. See `TextDrawOptions.text`. */
    readonly max_length: number;
    readonly flags: TextFlags;
  }

  /** The backchannel to whatever is connected to the REPL socket.
   *
   *  The REPL is a request/reply channel: a client sends source, the game sends
   *  back a value. This is the direction that has no request. A poller can only
   *  ever sample - it sees whichever frame its request landed in - so anything
   *  that *happens* between two polls (a trigger fired, a role spawned, a
   *  message arrived) is invisible unless the script says so. This is how it
   *  says so.
   *
   *  The line a client receives is `{"event": …, "data": …}`, with no `ok` -
   *  which is the whole rule for telling a notification from a reply.
   *
   *  **Off unless the game was launched with `GKPLUS_REPL_PORT`.** With the
   *  channel closed every call is a branch and a 0, so notifications can be left
   *  in shipped script. Main thread only. */
  export interface Repl {
    /** Queue `{event, data}` to every connected client, returning how many it
     *  reached. 0 is the ordinary answer - the channel is usually closed, and
     *  even open it may have nobody attached.
     *
     *  `data` is encoded with `JSON.stringify`, so it follows those rules: a
     *  `Date` becomes its ISO string, `undefined` and functions vanish, and a
     *  circular structure or a getter that raises **throws** rather than being
     *  swallowed. It is omitted entirely when not passed.
     *
     *  Nothing is written to the socket here - the line goes out with the next
     *  frame's pump, so this can never block on a client that stopped reading.
     *  A client far enough behind (8 MiB of unread lines) is dropped, and does
     *  not count towards the return value. */
    notify(event: string, data?: unknown): number;
    /** How many clients are connected right now. Test this before building a
     *  payload that is expensive to construct - `notify` itself only skips the
     *  encoding when the channel is *closed*, not when it is merely empty. */
    readonly clients: number;
    /** Whether the channel is listening at all, i.e. whether `GKPLUS_REPL_PORT`
     *  named a port and the listener opened. Fixed for the process. */
    readonly open: boolean;
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
    /**
     * Vestigial - set it or omit it, nothing observable changes. It names an object in
     * the level .rif (`"camhund"`, a flat quad above the map) and the engine derives a
     * world-space plane from it that no code reads, plus picks which shape the `.loc`
     * sidecar collapses onto - a choice the omitted-field fallback makes identically in
     * every shipped level. Omitted means `none`, as 11 of the shipped levels ship it.
     * See `level_loading_notes.md` §4.1.
     */
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

  // --- mods ------------------------------------------------------------------

  /** One mounted mod: an archive or a directory under
   *  `<Gunlok>\gkplus\mods`. */
  export interface Mod {
    /** The entry name inside the mods directory, e.g. `"20-tweaks.zip"`. */
    readonly name: string;
    /** Absolute path on disk. */
    readonly path: string;
    /** False for a plain directory. */
    readonly archive: boolean;
    /** Search-path position: 0 wins a conflict. Same as the collection index. */
    readonly priority: number;
  }

  export interface ModsMembers {
    readonly count: number;
    [Symbol.iterator](): IterableIterator<Mod>;

    /** `<Gunlok>\gkplus\mods\`, with a trailing backslash. */
    readonly dir: string;
    /** Where `gl.exe` lives, with a trailing backslash. Every VFS path is
     *  relative to this. */
    readonly game_dir: string;
    /** False only if the mod filesystem could not start at all. Having no mods
     *  installed is not the same thing and reports true. */
    readonly available: boolean;
    /** How many file opens have been answered from a mod rather than from disk.
     *  The only way to tell "mounted" from "actually being read", since a
     *  replaced asset looks identical from outside the game. */
    readonly served: number;
    /** The VFS paths behind the last 64 of those, newest first. Answers "is my
     *  file being picked up, and under what name" - which nothing else can,
     *  because the engine assembles that name from a GLDir and a string inside
     *  a `.gls`. */
    readonly recent: string[];

    /** Mounts an extra archive or directory at the highest priority. Throws if
     *  PhysicsFS cannot read it. */
    mount(path: string): void;
    /** The VFS path the engine would get if it opened `gamePath` right now, or
     *  null if no mod provides it. Resolved against the process's current
     *  directory exactly as a game open is, so prefer `mods.game_dir + ...`. */
    resolve(gamePath: string): string | null;
    /** Whether a mod provides this VFS path. */
    exists(vpath: string): boolean;
    /** The file's contents decoded as UTF-8, or null. Use `read_bytes` for
     *  anything the engine would read as ANSI bytes. */
    read(vpath: string): string | null;
    read_bytes(vpath: string): ArrayBuffer | null;
    /** Every regular file at or below `dir` (the whole VFS if omitted), as VFS
     *  paths, sorted. */
    files(dir?: string): string[];
  }

  /** The mod filesystem: archives and directories layered over Gunlok's data
   *  tree, so a mod can add or replace any file the engine loads without
   *  touching the base install.
   *
   *  A mod is a `.zip` (or any archive PhysicsFS reads) or a directory under
   *  `<Gunlok>\gkplus\mods`, and **its contents mirror the game's own directory
   *  tree** - `rif/units/bug.rif`, `scripts/defaults.gsh`, `sound/robots.dat`.
   *  That is forced by the engine rather than chosen: every loader chdirs to one
   *  of its seven configured directories and then opens a relative name, so
   *  "where in the game tree" is the only thing an interception can reconstruct.
   *
   *  Mods mount in ascending name order and **a later name wins**, so
   *  `20-tweaks.zip` overrides `10-base.zip`. Index 0 is the highest priority.
   *
   *  Paths are case- and separator-insensitive. Music and FMV are the one gap:
   *  Bink opens those inside its own DLL, out of reach of the interception.
   *
   *      for (const mod of mods) console.log(mod.priority, mod.name);
   *      mods.files("scripts").filter(p => p.endsWith(".gsh"));
   */
  export type Mods = ModsMembers & {
    readonly [index: number]: Mod | undefined;
    readonly [name: string]: Mod | undefined;
  };

  // --- the Vulkan renderer ---------------------------------------------------

  /** What a material override says about every draw that samples one texture.
   *
   *  Every field is optional; an override with none of them registers a key
   *  that matches images and changes nothing, which is a legitimate way to ask
   *  `render.material_overrides` what a key would match. */
  export interface MaterialOverrideSpec {
    /** A case-insensitive substring of another loaded texture's `.rim` path.
     *  Draws sampling the overridden texture sample this one instead, at
     *  whichever stage it was bound to; the stage keeps its own sampler and its
     *  own colour and alpha operations. */
    texture?: string;
    /** `[r, g, b]` or `[r, g, b, a]` in 0..1, multiplied into the fragment's
     *  final colour - after the texture stages, after the alpha test and after
     *  specular. Applying it after the alpha test is deliberate: a tint that
     *  could change which fragments are discarded would move silhouettes. */
    tint?: [number, number, number] | [number, number, number, number];
    /** Drop every draw whose *stage 0* texture matches. The draw never reaches
     *  the frame's list, so nothing it would have painted appears - but a
     *  stencil shadow cast by the same object is a separate draw with its own
     *  texture and survives. */
    hide?: boolean;
  }

  /** The Vulkan renderer, under `GKPLUS_RENDERER=vulkan`.
   *
   *  Only the material override is declared here. The rest of the namespace is
   *  the measurement surface `vulkan_renderer_plan.md` documents - counters,
   *  histograms, verifiers and bisect knobs - which moves with the
   *  investigation in progress and would be stale in a `.d.ts` more often than
   *  it was right; the index signature is what keeps it reachable and says
   *  plainly that it is not typed.
   *
   *  Nothing here alters what the *game* draws: the capture layer forwards
   *  every call to the original runtime unchanged, so an override repaints the
   *  Vulkan frame and leaves `GKPLUS_RENDERER=d3d8` and `d3d9` untouched. */
  export interface Render {
    /** Register, replace or (with a null spec) remove the override for `name`,
     *  a case-insensitive substring of a texture's `.rim` path.
     *
     *  Returns the readback rather than nothing, because the one thing that can
     *  go wrong is silent: a substring matching no live image, or matching more
     *  than was meant, is not an error and cannot be seen from the call.
     *
     *      render.material_override("gunlok_mk2", {tint: [1, 0, 1]});
     *      render.material_override("gunlok_mk2", {texture: "hark_512"});
     *      render.material_override("gunlok_mk2", null);
     */
    material_override(name: string, spec?: MaterialOverrideSpec | null): string;
    /** Every registration, the live images each key matches, and how many draws
     *  they have touched. A key that matches an asset the camera cannot see
     *  resolves and reports exactly like one that is on screen - the draw
     *  counts are what tell them apart. */
    readonly material_overrides: string;
    clear_material_overrides(): void;

    /** How much of the last frame's geometry carries **smooth** normals, as text.
     *
     *  The measurement a tessellation stage has to be built against. PN triangles
     *  curve a patch by exactly the term `dot(Pj - Pi, Ni)`, so a corner whose
     *  normal is perpendicular to both of its edges contributes nothing at all:
     *  every control point collapses to the linear one and the patch **is** the
     *  flat triangle it started as. That makes a hard edge free rather than a
     *  heuristic -- and it also means a mesh whose vertex normals are all face
     *  normals is one tessellation cannot change.
     *
     *  The reported number is `|dot(normalize(edge), normal)|`, the tangent term
     *  normalised by edge length, taken as the worse of a corner's two edges. It
     *  is the quantity the construction actually uses rather than a proxy: a
     *  corner reading `d` bulges its edge by about `d * length / 3`.
     *
     *  Bucketed into the level mesh and everything else, because the two are
     *  authored by different tools. A REPL diagnostic only -- it reads the arena
     *  back, which submits and waits twice per draw. */
    normal_census(): string;

    /** **PN-triangle amplification over the level mesh.** Off by default.
     *
     *  Hardware tessellation, with the generated points placed on a cubic
     *  Bezier patch fitted to each triangle's three corner positions and corner
     *  normals (Vlachos et al.). Its edge control point,
     *  `(2*P1 + P2 - dot(P2 - P1, N1) * N1) / 3`, collapses to the linear one
     *  exactly when the corner normal is the face normal -- so a flat-shaded
     *  wall reproduces itself at any tessellation factor, with no threshold and
     *  no per-material opt-in, while a smooth-normalled pipe or boulder curves.
     *  Hard edges cost nothing because the arithmetic leaves them alone.
     *
     *  It is also watertight across a shared edge by construction: the two
     *  control points on edge (P1,P2) are functions of P1, P2, N1 and N2 alone,
     *  so the triangles either side build the same boundary curve.
     *
     *  Off by default because it changes the level's silhouette rather than
     *  reproducing D3D. Reads back `false` on a device with no
     *  `tessellationShader` even after being set -- the getter answers "is this
     *  happening", not "was it asked for". */
    tessellation: boolean;

    /** Which draws are amplified: `"map"` (the level mesh, the default),
     *  `"all"` (props and units too) or `"off"`.
     *
     *  **`"map"` is the right default, and that is measured rather than assumed.**
     *  `render.normal_census()` across all sixteen shipped levels: the props and
     *  units are **88.6% flat corners and 86.7% fully-flat triangles**, with only
     *  7.6% genuinely curved and 77% of their corners carrying no normal at all.
     *  `"all"` therefore amplifies ~9,450 prop triangles to change 7.6% of their
     *  corners. The level mesh is where the curvature is: 30.6% curved corners
     *  over 29,278 triangles.
     *
     *  Level02 alone says the opposite -- 53% of its props' corners are curved --
     *  and it is the outlier, not the rule. */
    tess_set: "map" | "all" | "off";

    /** Whether the shadow passes amplify with the colour pass. On.
     *
     *  Separable because the bake is where the cost is, so a frame-time
     *  regression can be pinned to one half. Note the shadow passes take one
     *  pipeline for their whole caster set, so with `tess_set = "map"` a prop's
     *  shadow follows a smoothed silhouette its geometry does not have. */
    tess_shadows: boolean;

    /** The screen-space edge length, in the render target's pixels, that a
     *  tessellation factor aims for. An edge covering twice this gets 2. */
    tess_edge_pixels: number;
    /** The ceiling on a factor, clamped to the device's
     *  `maxTessellationGenerationLevel`. */
    tess_max: number;
    /** The floor. Above 1 it forces uniform amplification, which is how the
     *  shape can be judged without the factors varying underneath it. */
    tess_min: number;
    /** The shadow passes' factor, uniform over every edge -- which makes those
     *  passes watertight for free, since a constant cannot disagree with itself
     *  across a shared edge. */
    tess_shadow_factor: number;

    /** How much of the PN tangent term survives: 1 the full construction, 0
     *  exactly linear.
     *
     *  At 0 the surface is the untessellated one however high the factors go,
     *  which makes it the A/B that separates "the amplification is wrong" from
     *  "the curvature is wrong". Measured on level02: `pn_strength = 0` with
     *  tessellation on sits at 0.00928 MAD against an off-vs-off floor of
     *  0.00795, so everything the feature does visibly comes from the curvature
     *  rather than from subdividing. */
    pn_strength: number;

    /** A normalised tangent term at or below this is snapped to exactly zero,
     *  making its corner flat.
     *
     *  The census is why this exists: only **6.5%** of level02's map triangles
     *  have all three corners flat, so the construction's free hard-edge
     *  identity covers far less of that level than its reputation suggests, and
     *  a mean term of 0.094 domes a typical edge by ~3% of its length. This
     *  restores the identity for the near-flat majority.
     *
     *  Across all sixteen levels the figure is **36.3%**, and the spread is
     *  enormous -- 4.3% on level04 against 75.6% on level15. Level02 is near the
     *  *curved* end, which makes it a pessimistic place to tune this and a safe
     *  one to inherit a default from.
     *
     *  It stays watertight, which is why the threshold is on this quantity and
     *  not on, say, the triangle's own flatness: the term is a function of
     *  `(Pi, Pj, Ni)` alone, so the triangle across the edge tests and snaps the
     *  identical number. */
    pn_flat_threshold: number;

    /** The ceiling `pn_flat_threshold`'s floor cannot reach: how far, in world
     *  units, a control point may sit off its chord. Default 0.08.
     *
     *  The bulge is `term * length / 3`, and `pn_flat_threshold` is normalised by
     *  the edge length on purpose - so it means the same thing at every scale, and
     *  for that same reason it cannot bound an absolute distance. Gunlok builds
     *  round objects from few, long segments, so level02's map mesh reaches a
     *  **1.104-unit** control-point offset against a 1.952-unit mean edge.
     *
     *  **It is not the cure for the inflated-pipe look, and the sweep says so**
     *  (§4.74). At 0.08 it caps 10.7% of level02's map half-edges and takes 23.6%
     *  of the frame's displacement, for 0.41 MAD of a 2.83 total; the picture only
     *  really moves at 0.03-0.05, by which point it has removed most of the
     *  tessellation everywhere. The rounding and the inflation are the same
     *  displacement. `pn_strength` is the knob that trades them off - 0.5 sits at
     *  2.19 MAD against 2.83.
     *
     *  Clamped rather than zeroed, so a bulge that merely overshoots becomes
     *  exactly the cap instead of snapping flat, and watertight for the same
     *  reason the floor is: a function of `(Pi, Pj, Ni)` alone. */
    pn_max_offset: number;

    /** D3D8's light sum, evaluated **per pixel** instead of per vertex.
     *
     *  On by default, and the first thing in this namespace that deliberately
     *  departs from the original rather than reproducing it. The equation is
     *  unchanged - the same lights, the same attenuation and spot terms, the same
     *  `N·L > 0` gate on the specular sum. What changes is what gets interpolated
     *  across a triangle: a finished colour, or the position and normal that
     *  colour is computed from.
     *
     *  Gouraud shading cannot represent a highlight smaller than a triangle or a
     *  point light's falloff across one, so the difference concentrates on
     *  curved and finely-tessellated geometry - a unit, a projectile - and is
     *  near zero on flat ground. Judge it there, not on a whole-frame average.
     *
     *  `false` restores the fixed-function path bit-identically, which is what
     *  makes it A/B-able on one paused frame.
     *  `GKPLUS_VK_PER_PIXEL_LIGHTING=0` is the launch-time form. */
    per_pixel_lighting: boolean;

    /** Replace the level's **baked** per-vertex lighting with a per-pixel evaluation
     *  of the light rig that produced it.
     *
     *  Every level ships the `STDLIGHT` set its per-vertex colours were baked from,
     *  and the shipped engine loads it and never reads it. The model used here was
     *  recovered by fitting against those baked colours rather than chosen -- a
     *  linear falloff over the light's range, `max(0, N·L)`, and a cone about the
     *  light's own axis on everything but the omnidirectional ones. It reaches
     *  r 0.87-0.96 against the real bake on three of the four levels measured.
     *
     *  **Off by default**, and on performance grounds rather than fidelity ones: it
     *  is brute force over every light in the level, per pixel, with no culling yet.
     *
     *  Applies to the map's own geometry only. A prop or a unit carries its own
     *  file's bake from its own rig, and substituting the level's there measures
     *  worse; `map_lighting_all` lifts the restriction so that stays checkable. */
    map_lighting: boolean;
    /** Bin the map's lights into a world-space grid so a fragment reads only its
     *  own cell, instead of looping every light in the level. On.
     *
     *  **Off must be bit-identical, not merely close.** A light's range is a hard
     *  cutoff, so a light whose sphere misses a cell contributes exactly zero to
     *  every fragment in it -- the grid drops nothing. That makes this a
     *  correctness A/B rather than a quality trade, and the only test that can
     *  catch a cell quietly missing a light. */
    map_light_cull: boolean;
    /** Substitute on every lit draw rather than only the map geometry. Off, and the
     *  default is a measurement -- see `map_lighting`. */
    map_lighting_all: boolean;
    /** The model's one free parameter. Default 1.35, the mean of the values fitted per
     *  level (1.1 on level01, 1.5 on level04 and level05) -- so it is a lever rather
     *  than a calibration.
     *
     *  It moved from 1.2 when the falloff's tail was windowed to kill the visible rim
     *  around every light, and had to: a dimmer tail refits to a brighter gain. */
    map_light_gain: number;
    /** Which `.rif` the lights came from, how many, the ambient floor, and their world
     *  bounds beside the map's own -- which is the reading that says the unit scale
     *  and origin were applied correctly. */
    readonly map_light_report: string;

    /** Lighting maps: a companion `<texture> lighting.dds` beside a `.RIM`, in a
     *  mod under `gkplus/mods` or in the install itself, giving that one texture
     *  a bump/metallic/roughness response the game never had.
     *
     *  Nothing registers it - the rule is the file name. A texture the renderer
     *  knows as `Ground\gunlok rust.RIM` takes
     *  `graphics/ground/gunlok rust lighting.dds` (or `..._lighting.dds`), and
     *  the three channels are **R height, G highlight intensity, B highlight
     *  sharpness**. The normal is derived from R at draw time, so the file is an
     *  ordinary DXT1 or uncompressed DDS with no tangent data anywhere.
     *
     *  On by default. Off interns every material exactly as the build before this
     *  existed, so it A/Bs on one paused frame at a 0.000 noise floor - and
     *  setting it back to `true` **re-reads every file**, which is how a map
     *  edited while the game is running is picked up. */
    lighting_maps: boolean;
    /** Which names were probed, what was found for them and where it came from,
     *  plus the current knobs. Worth reading before concluding a map does not
     *  work: a texture with no companion file is the normal case, so a misnamed
     *  file and a stock install look identical from the screen. */
    readonly lighting_map_report: string;
    /** How far the height gradient may tilt the normal. 1.0 (the default) makes a
     *  full-contrast step across one texel a 45-degree slope. Per texel, so a
     *  higher-resolution map is gentler for the same artwork. */
    bump_scale: number;
    /** How much of the derived normal reaches the *diffuse* term, 0 to 1. At 0
     *  the map only shapes highlights, which leaves a bump invisible wherever
     *  `metallic` is 0; 1 (the default) relights the surface per pixel. */
    bump_diffuse: number;
    /** A multiplier on the added highlight. The default is **0.25**, not 1.0,
     *  because Gunlok over-drives its lights - level02's key light is `4.0 4.0
     *  4.0` - so a fully-metallic texel at 1.0 saturates to white. */
    specular_scale: number;
    /** Which colour the highlight reflects: the light's own specular colour at 0,
     *  its diffuse colour at 1 (the default). Every light reaching level02's
     *  ground authors `specular 0 0 0`, so at 0 the metallic channel does nothing
     *  over most of a level; 0 is still the game's own answer, and what the
     *  fixed-function specular term uses. */
    specular_from_diffuse: number;
    /** The specular exponent at roughness 1 - the broadest highlight. Default 4. */
    gloss_min: number;
    /** ... and at roughness 0, the sharpest. Default 256. The two are
     *  interpolated in log2, so the knob behaves evenly across the range. */
    gloss_max: number;

    /** How much of Gunlok's own chrome pass survives, before the lighting map's
     *  metallic channel scales it. 1.0 (the default) is the engine's own
     *  strength; 0 removes the reflection.
     *
     *  A `reflective` role - 48 of the shipped ones - is drawn a second time
     *  with `units\reflect.rim` ADDSIGNED over its own texture. That pass's
     *  stage 0 is the same texture as the base pass, so the same lighting map
     *  applies, and its **metallic** channel weighs the reflection exactly as it
     *  weighs the highlight. All three `chrome_*` knobs are inert on a texture
     *  with no companion file. */
    chrome_scale: number;
    /** How far the roughness channel may blur the reflection, in mip levels.
     *  B already means the highlight's sharpness, and a rough surface reflecting
     *  less sharply is that same statement made in the sphere map.
     *
     *  **Defaults to 0 because it does not work yet.** Sweeping it 0 to 20 on
     *  level02 moves 0.012/255 against a 0.010 floor - noise. The texture, the
     *  channel and the push constant have all been ruled out; the open lead is
     *  the chrome stage's `D3DTEXF_NONE` sampler and the `maxLod` clamp that
     *  reproduces it. See `LightingMapParams::chrome_blur` in src/VkLighting.h. */
    chrome_blur: number;
    /** Generate the chrome pass's texture coordinate from the bumped normal
     *  instead of reading the mesh's second UV set. Default true.
     *
     *  The generated one is the only coordinate that can respond to the height
     *  field at all, and the formula is the engine's own - Gunlok's map-wide
     *  chrome variant asks D3D for `D3DTSS_TCI_CAMERASPACENORMAL`, which is the
     *  camera-space normal's xy. False reproduces the engine's per-unit path. */
    chrome_texgen: boolean;

    /** Run the specular term of the per-vertex light sum. The mirror image of
     *  `GKPLUS_NO_SPECULAR`, which forces `D3DRS_SPECULARENABLE` off in the
     *  *forwarded* call only: with both, the term can be removed from one paused
     *  frame of each renderer and the two bases compared directly, which is what
     *  separates "we add specular the original does not" from "we add more of it"
     *  (§4.46).
     *
     *  On by default. */
    specular: boolean;

    /** Skip the upload path entirely when a vertex or index buffer is unlocked
     *  from a `D3DLOCK_READONLY` lock, which by contract changed nothing (§4.84).
     *
     *  **Worth 1.2 ms of a 6.2 ms level02 frame**, because Gunlok uses
     *  `ProcessVertices` to have the driver transform geometry into screen space
     *  and then locks the destination buffer read-only to read the result back.
     *  From the capture layer that read looks exactly like a refill, so the whole
     *  64 KB was being converted to canonical vertices, staged and copied to the
     *  GPU on the unlock of a *read* - 126,600 `D3DFVF_XYZRHW` vertices a frame,
     *  84% of all per-frame vertex conversion, for two SYSTEMMEM buffers no draw
     *  has ever named as its stream source.
     *
     *  On by default. Off restores the previous behaviour, which is how to A/B it
     *  inside one session - and the two states are **bit-identical** on screen,
     *  measured on blink-phase-matched frames, since a read-only lock cannot have
     *  changed what the skipped upload would have written. */
    skip_readonly_unlocks: boolean;

    /** Compute `IDirect3DDevice8::ProcessVertices` in the capture layer instead of
     *  forwarding it to D3D9's software vertex pipeline (§4.85).
     *
     *  **Worth 0.23 ms of a 4.80 ms level02 frame.** Gunlok uses ProcessVertices
     *  only for mouse picking - projecting a node's 8 bounding-box corners, then
     *  the whole mesh if the cursor is inside one - at ~20 vertices a call and
     *  ~28 calls a frame. Forwarded, that was 6.7 us a call, almost all of it
     *  d3d9 setting up and tearing down a pipeline for eight corners; the
     *  transform itself is microseconds.
     *
     *  Handles a deliberately narrow case (XYZRHW destination, mirrored
     *  transform, readable source) and forwards anything else, so
     *  `render.stats.process_vertices_forwarded` is the coverage figure - 96.5%
     *  of calls and 100% of vertices are handled on level02, the remainder being
     *  calls with no vertices at all.
     *
     *  On by default. Off restores forwarding, which is both the A/B and the way
     *  out if it is ever wrong. Check it with `verify_process_vertices` first. */
    software_process_vertices: boolean;

    /** Check every ProcessVertices against D3D9's own answer (§4.85).
     *
     *  **Non-destructive by construction**: it runs D3D9, leaves D3D9's result in
     *  the buffer, and only compares ours against it - so the software path is
     *  never installed while this is armed, and it is safe to leave on for a
     *  whole session. That matters because this is the one computation in the
     *  capture layer whose errors are invisible: a wrong screen position selects
     *  the wrong unit and nothing on screen says so.
     *
     *  Measured over 3.4M vertices and several camera distances, the worst
     *  disagreement is **1/16 pixel** in x and y - a subpixel quantization step,
     *  so it is D3D9 rounding its output and ours that is exact - with z at
     *  3.6e-07 depth and rhw at 2.0e-07 relative. Read the result with
     *  `process_vertices_report`. */
    verify_process_vertices: boolean;

    /** ProcessVertices call counts, how many took the software path against how
     *  many were forwarded, and the worst disagreement the verifier has seen.
     *  The coverage line is the one to read: the software path refuses anything
     *  it cannot vouch for, so a large forwarded share means the win is not being
     *  taken. */
    readonly process_vertices_report: string;

    /** Which vertex buffers the converter is spending its time on, and whether
     *  anything draws the result, as text.
     *
     *  `render.stats.converted_layouts` counts by *layout* and that is one
     *  question short: it cannot say whether 28 calls a frame are one buffer
     *  refilled 28 times or 28 buffers refilled once, and it cannot say whether
     *  the result is ever read. Each row carries the FVF, size, converted and
     *  skipped vertex counts, unlock count, pool, usage, the flags of the last
     *  lock, whether it has been a `ProcessVertices` destination, and the game
     *  function that locked it - symbolized through the Ghidra map, so it names a
     *  producer rather than an address. This is the report §4.84 was found in. */
    readonly vertex_buffer_load: string;

    /** Take a pre-transformed (`D3DFVF_XYZRHW`) vertex's `z` as the depth value,
     *  clamped to the viewport's `MinZ..MaxZ`, instead of running the viewport's
     *  depth range over it. That is what D3D does, measured with `depth_probe`;
     *  Vulkan has no bypass, so without this every screen-space draw sits
     *  `MinZ * (1 - z)` too far away and the effect layers - fire especially -
     *  come and go with camera distance.
     *
     *  On by default. Setting it false restores the previous behaviour, which is
     *  how to A/B it on one paused frame. */
    rhw_depth_raw: boolean;

    /** Honour `D3DVIEWPORT8`'s rectangle per draw, as the Vulkan viewport and
     *  scissor, instead of covering the whole render target with one (§4.47).
     *
     *  Gunlok sets two rectangles: the whole backbuffer for everything in a
     *  level, where it makes no difference, and `32,24 575x431` for the upgrade
     *  screen. With it off that screen is stretched over the full 640x480 and
     *  anchored at 0,0, and because the same frame also carries draws at the
     *  full rectangle, the two halves end up displaced relative to each other.
     *
     *  On by default. Setting it false restores the pre-§4.47 behaviour, which
     *  is how to A/B it inside one session. */
    viewport_rect: boolean;

    /** Arm the depth probe: one opaque magenta quad at 16,340..144,436, drawn
     *  last against a depth buffer cleared to `clear_z` under a viewport slice of
     *  `min_z..max_z`, with `ZFUNC LESS` and no depth write.
     *
     *  It answers whether D3D runs the viewport transform over a pre-transformed
     *  vertex - the quad is either there or it is not, so the reading needs no
     *  precision. **Read it in `d3d8` or `d3d9`**: the clear goes straight to the
     *  forwarded runtime, so under `vulkan` the quad tests against whatever the
     *  scene left. Returns what it armed and what each answer predicts.
     *
     *      render.depth_probe(true, 0.6, 0.7, 0.5, 1.0);  // drawn => z is the depth
     *      render.depth_probe(false);                     // disarm
     */
    depth_probe(
      armed?: boolean,
      quad_z?: number,
      clear_z?: number,
      min_z?: number,
      max_z?: number
    ): string;

    /** Arm the viewport-rectangle probe: one opaque magenta `D3DFVF_XYZRHW` quad
     *  drawn last under a viewport whose `X`/`Y` are not zero, at 20 pixels in
     *  from that origin.
     *
     *  It answers the other half of what a viewport does to a pre-transformed
     *  vertex - whether D3D **adds** the rectangle's origin to it. It does not:
     *  the same quad under `0,0 200x150` and under `100,60 200x150` moves by the
     *  `100,60` its own coordinates moved by, not by twice that (§4.47). It also
     *  shows the clipping, since a quad hanging over the edge is cut there.
     *
     *  **Read it in `d3d8`**, like `depth_probe`: the question is what D3D does.
     *  Returns what it armed and where each answer puts the quad.
     *
     *      render.viewport_probe(true, 100, 60, 200, 150);
     *      render.viewport_probe(false);                    // disarm
     */
    viewport_probe(
      armed?: boolean,
      x?: number,
      y?: number,
      width?: number,
      height?: number
    ): string;

    /** A real shadow map from the sun - the first shadow in Gunlok that is not a
     *  blob under a unit. Four concentric cascades in one 2x2 atlas of 2048
     *  tiles, centred on the camera's orbit pivot, 3x3 PCF.
     *
     *  On by default. A level with no sun set produces no matrix and therefore
     *  no shadow, which from the screen is the same as this being off -
     *  `render.draws` is what tells the two apart. */
    sun_shadows: boolean;

    /** How many of the four cascades are live, 1..4.
     *
     *  **1 is the single map this started as**, at the same texel density, which
     *  is what makes cascading A/B-able on one paused frame. Each cascade is half
     *  the extent of the one outside it, so four of them put the near field at
     *  0.0085 world units per texel against a single map's 0.068 - and cost about
     *  1.7 ms, since the caster list is walked once per cascade. */
    shadow_cascades: number;

    /** The sun shadow's depth offset, **in shadow-map texels of whichever cascade
     *  the fragment landed in**.
     *
     *  Texels rather than depth units because that is the unit acne is measured
     *  in: the depth error a flat surface accumulates across one texel is the
     *  texel's world size times the surface's slope in light space. So one value
     *  holds on every cascade, on every level and at every `shadow_extent`, which
     *  a value in depth units does not.
     *
     *  2.5 by default, which is the knee of a sweep rather than a guess. Below it
     *  level04 shadows itself everywhere; above it the shadow shrinks away from
     *  its caster at 0.04-0.3% of the frame per texel. */
    shadow_bias: number;

    /** How dark a sun-shadowed fragment goes, 0 to 1. **The one knob here that is
     *  not a fidelity question**, because the game never had a real shadow and so
     *  has no ground truth for it.
     *
     *  1 is the physically correct value rather than the maximum one: the shadow
     *  attenuates only the diffuse and specular terms, so 1 means "no sunlight
     *  reaches here" while the ambient and the level's own baked colour still
     *  light the surface. 0.7 is the default, and both bounds are measured -
     *  0.55 leaves level04's unit shadows reading as a smudge, and 1.0 takes
     *  level02's covered start to 36% of its authored brightness. */
    shadow_strength: number;

    /** Half the width of the box the **outermost** cascade covers, in world
     *  units, centred on the camera's orbit pivot. So this is the range at which
     *  shadows stop, and `extent / 2^(cascades-1)` is the sharp near field.
     *
     *  70 by default, which is Gunlok's own `camera.max_distance` of 75 rounded
     *  down: it covers everything the camera can ever see. Raising it to 200 buys
     *  0.2% of the frame. */
    shadow_extent: number;

    /** Draw the game's **own** blob shadow as well as the sun's map. Off by
     *  default, since otherwise a unit carries both.
     *
     *  Its three passes are identified by stencil being enabled, which is exact
     *  rather than a heuristic: over sixteen shipped levels the game draws with
     *  22 distinct pipeline configurations, exactly 3 of them use stencil, and
     *  all 3 are that shadow. A level someone else writes is not covered by that,
     *  which is what this exists to check. */
    stencil_shadow: boolean;

    /** Real shadows for the level's own `STDLIGHT` rig - one six-face cube per
     *  light in a 32 MB atlas, baked once per level from the map's own geometry.
     *
     *  **On, and play is what settled it.** It shipped off because no measurement
     *  could say whether the picture with these shadows was the right one - the
     *  game never had them - and then the first report from actually playing was
     *  that the map lights do not cast any. Cost was never the objection: 0.50 ms
     *  on level01, the level with the most map lights in the game, and nothing
     *  measurable on level02.
     *
     *  Needs `map_lighting`, which is what evaluates that rig at all. The bake is
     *  gated on this too, so off costs nothing; turning it back on restarts it,
     *  and `map_shadow_report` says when it has finished. */
    map_shadows: boolean;

    /** How far a map-light shadow lookup is moved along the surface normal
     *  before it is projected, in atlas texels at that fragment's own distance
     *  from the light.
     *
     *  A **normal** offset rather than a depth one because a 64-texel cube face
     *  is coarse - a texel is `distance / 32` world units - so the error is
     *  dominated by the surface's slope, and a depth offset large enough to
     *  cancel it would detach every shadow by metres. 1.0 by default, the larger
     *  of two knees: level02's acne is gone by 0.25 and level04's needs about 1.
     *
     *  **`= 0` is the sharpest picture of what the atlas holds** - per-light acne
     *  with cube-face stair-stepping and coloured fringes, one colour per light. */
    map_shadow_bias: number;

    /** How many map lights the bake does per frame, picking up where it left off.
     *
     *  256 with indirect drawing and 4 without, taken from the path at startup.
     *  With one indirect command a face the whole bake is a few milliseconds, so
     *  level01's 682 lights land in three frames; the fallback issues a draw call
     *  per caster per face and wants spreading. Writing this **re-bakes from the
     *  start**, so only write it when it has actually changed. */
    map_shadow_rate: number;

    /** Whether the bake submits one `vkCmdDrawIndexedIndirect` per cube face or a
     *  draw call per caster per face - 4,092 commands against 804,924 on level01.
     *
     *  On wherever the device has `multiDrawIndirect`. **The two must produce the
     *  same atlas and this is the only thing that can say so**, so writing it
     *  rebuilds the pipeline and re-bakes; on a device without the feature it
     *  does nothing and reads back false. */
    map_shadow_indirect: boolean;

    /** What the map-light shadow atlas holds, what it refused for want of a slot,
     *  and how far the bake has got.
     *
     *  Not optional reading: a level with no map lights, an atlas that could not
     *  be created and a bake that has not finished all look identical from the
     *  screen. */
    readonly map_shadow_report: string;

    /** Screen-space ambient occlusion, **with no blur pass**.
     *
     *  The kernel is one fixed Poisson disc shared by every pixel rather than a
     *  randomised one, so the output is not noise and needs no blur to become
     *  usable - which is what removes the halo around every silhouette that a
     *  blurred AO carries. It matters more here than it would generally: Gunlok
     *  renders 640x480, where a blur radius is a large fraction of a character.
     *
     *  Two passes. A prepass rasterises the frame's opaque geometry - the same
     *  caster set the sun's shadow uses - and writes a **world position** and
     *  normal per pixel; the resolve walks the disc in screen space, reconstructs
     *  nothing, and counts how many taps land inside the half-ball at the centre
     *  pixel. There is no matrix anywhere in it.
     *
     *  **Off.** The game never had ambient occlusion, so nothing here can be
     *  measured as closer to D3D8 and off is bit-identical to the build before it
     *  existed. Judge it on `ao_debug` and on a region, not on a whole-frame
     *  number. */
    ao: boolean;

    /** The hemisphere's radius, in **world units** - what "near enough to
     *  occlude" means. Level02's mean map edge is 1.952 units and the sun's sharp
     *  cascade is 8.75 across, which is the scale to think in.
     *
     *  3, and that is a sweep: at level02's settled camera the occluded
     *  fraction of the debug view goes 0.341 / 0.384 / 0.412 / 0.417 for 0.75 /
     *  1.5 / 3 / 6, so 3 is the knee past which the disc binds instead and only
     *  the over-darkening keeps growing. */
    ao_radius: number;

    /** The disc's radius, as a **fraction of the frame's height**, and
     *  deliberately independent of `ao_radius`. Constant across the frame, which
     *  is the technique's whole performance argument - every pixel walks the same
     *  texel pattern, the friendliest case there is for the texture cache. A
     *  value derived once per frame from the target size is still one constant,
     *  so this costs that nothing.
     *
     *  Not a pixel count, because the render extent is not a constant: 640x480 on
     *  the machine the notes' numbers come from, 3072x1728 on another. 0.07,
     *  which is 34 pixels at 480 lines and 121 at 1728.
     *
     *  A constant screen radius is affordable here in a way it would not be
     *  generally: Gunlok's camera is a fixed-height orbit, so one frame's depth
     *  spread is narrow and a constant screen radius is very nearly a constant
     *  world one. */
    ao_screen_radius: number;

    /** How far along the normal a tap has to be before it counts, in world units.
     *  The self-occlusion knob: too low and a flat wall shades itself out of its
     *  own quantisation, too high and a shallow crease stops registering. 0.05. */
    ao_bias: number;

    /** A scale on the occlusion before it leaves the resolve pass, so the target
     *  already carries the artistic weight and the world shader stays a plain
     *  multiply. 1. */
    ao_strength: number;

    /** How much the occlusion also scales **D3D's own** diffuse sum - the sun and
     *  the level's dynamic lights. Not the map rig, which is occluded in full
     *  whatever this says.
     *
     *  The split is what each set of lights *is*. The `STDLIGHT` rig is 51 static
     *  lights on level02 whose whole job was to bake that level's vertex colours -
     *  an environment, and exactly what occlusion is about. D3D's are few and
     *  dynamic, and every one of them already has a shadow map answering "is this
     *  light blocked" exactly, per light - so 0 here is the no-double-counting
     *  setting rather than a taste one. 1 darkens them too, for a stylised look.
     *
     *  The specular is never occluded at any setting: a highlight is a mirror of
     *  one light in one direction, and nearby geometry says nothing about whether
     *  that particular path is clear. */
    ao_direct: number;

    /** How many of the 64-point disc to walk, 1..64. **All of them**, and this is
     *  not a quality dial with a cheap end.
     *
     *  A fixed kernel cannot trade its artefact for noise the way a randomised one
     *  does, so an under-sampled disc does not go grainy - it goes *structured*.
     *  Each tap contributes a shifted copy of every occluder's silhouette, and at
     *  32 points over a wide disc those copies are individually visible: measured
     *  on level02, where each character left a fan of its own outlines. A blur
     *  would hide that, and there is no blur here to hide it.
     *
     *  **32 is exactly the pattern the technique's author published** - a lattice
     *  with each point nudged off its cell, not blue noise and not random. The
     *  other 32 are the same lattice half a cell over. The whole set is
     *  maximin-ordered, so any smaller count is still a well-spread subset.
     *
     *  The cost is linear in this and in nothing else. */
    ao_taps: number;

    /** Restrict the term to the map's own geometry. **On**, and it is the same
     *  restriction runtime map lighting carries, for the same reason: a prop or a
     *  unit is a separate `RBOBJECT` whose vertex colours were baked from its own
     *  file's lights, and that bake already contains occlusion. Applying this on
     *  top of it darkens the same crease twice. */
    ao_map_only: boolean;

    /** Replace the shaded frame with the occlusion term itself, as grey.
     *
     *  Not optional when tuning: `ao_radius` and `ao_taps` are close to invisible
     *  in a shaded frame and obvious in the term. It ignores `ao_map_only` on
     *  purpose - the buffer covers every opaque draw the prepass rasterised, and
     *  seeing the half the restriction throws away is the point of looking. */
    ao_debug: boolean;

    /** Shadows from **the game's own D3D point and spot lights** - level02's fires,
     *  and anything a `.gcs` adds with `ADD LIGHT`. A different light system from
     *  `map_shadows` above, sharing the same static atlas: sixteen of its 682
     *  slots are reserved for these.
     *
     *  On. It is affordable because a LEVEL's own local lights are few and do not
     *  move - five on level02's start, twelve at its fire camera, four on level04,
     *  none on prison - so a cube per light is baked once and sampled behind the
     *  same range, `N·L` and cone rejections the map lights' lookup sits behind.
     *  Nothing measurable on level02.
     *
     *  **An effect's light is a different animal and gets nothing.** An explosion's
     *  light rides a particle, so it moves every frame, never survives the gate
     *  below and never casts - one `fx.explode` in view leaves ~30 distinct
     *  contents behind. Effects are also where the game's only spot lights come
     *  from.
     *
     *  **A light that moves gets no shadow rather than a wrong one.** A D3D light
     *  has no identity across frames, so a slot is held under a key made of the
     *  light's position, range and cone - deliberately not its colour, since
     *  `ADD BLINKING LIGHT` rewrites exactly that - and a key must survive four
     *  frames before it claims one. A light on a track, or a mod's light on a
     *  projectile, therefore never claims a slot and costs nothing. */
    local_shadows: boolean;

    /** What the local half of that atlas holds: keys live, how many hold a baked
     *  cube, how many are still moving, and how many held still but found no free
     *  slot.
     *
     *  Not optional reading, and the pair to read is `waiting out the stability
     *  gate` against `held still but found no free slot`. The first is the feature
     *  working - a light that moves lives there permanently - and only the second
     *  is a limit. */
    readonly local_shadow_report: string;

    /** Whether D3D's point and spot lights are in the light sum at all. On.
     *
     *  **A diagnostic, and the one that prices `local_shadows`**: off drops them
     *  and keeps the directionals, so a paused A/B paints exactly the pixels they
     *  reach - and since a shadow only ever removes light, that set strictly
     *  contains anything shadowing them could change. It measures 0.75% of
     *  level02's settled start, 2.34% at its fire camera and 0.63% on level04,
     *  against the sun's 17%. */
    local_lights: boolean;

    /** Window the range cutoff on D3D's point and spot lights. On by default.
     *
     *  D3D8 switches a light off HARD at Range while its attenuation is still
     *  well above zero there - 0.309 for level02's fires - so per pixel the
     *  boundary is a step in the value, and the eye reads it as a disc. It was
     *  invisible in the original because D3D8 lit per vertex and interpolation
     *  destroyed the step; going per pixel exposed it, exactly as it exposed the
     *  map lights' rim.
     *
     *  Off restores the hard cutoff, so the two can be compared inside one
     *  paused frame - which is the only way to read it, since two settles of a
     *  level drift by more than the change is worth. */
    local_light_window: boolean;

    /** The last complete frame's D3D lights, deduplicated by **contents**, with how
     *  many draws each reached and how many frames it has survived.
     *
     *  A `GpuLight` is deduplicated by enable mask within a frame and carries no
     *  identity across one, so "how many distinct point lights does a frame have"
     *  was otherwise unanswerable. Read `distinct this frame` against `distinct
     *  over the session`: a rig that never moves makes the second converge on the
     *  first, and one the game re-authors leaves a new key behind every frame. */
    readonly frame_lights: string;

    [key: string]: any;
  }

  // --- the profiler ----------------------------------------------------------

  /** One presented frame. `ms` is present-to-present wall clock. */
  export interface ProfFrame {
    readonly index: number;
    readonly ms: number;
    /** True when the frame was waiting for a vertical blank - FIFO, or a D3D
     *  presentation interval. **A throttled frame time measures the monitor, not
     *  the game**, so an A/B taken across one says nothing (see
     *  `vulkan_renderer_notes.md` §4.79, where three sections of renderer work
     *  were measured against exactly this and had to be struck out).
     *  `GKPLUS_VK_PRESENT_MODE=immediate` is the way out. */
    readonly throttled: boolean;
    readonly present: string;
    readonly events: number;
    readonly samples: number;
  }

  /** One instrumented call site, aggregated over the queried window. */
  export interface ProfZone {
    readonly name: string;
    /** Slot index into `prof.threads`. The same site recorded from both game
     *  threads is two rows, never one - they are different work. */
    readonly thread: number;
    readonly calls: number;
    readonly incl_ms: number;
    /** Inclusive minus the direct children, exact rather than estimated. */
    readonly self_ms: number;
    /** The worst single call in the window. A mean hides a stutter; this does not. */
    readonly max_ms: number;
    /** `self_ms` divided by the window, which is the number that compares against
     *  a frame time. */
    readonly self_ms_per_frame: number;
  }

  /** One address in the flat sampled profile. */
  export interface ProfSample {
    /** `module+0xrva`, or a symbol name once `prof.symbols` has loaded a map for
     *  that module. Resolve ours offline with
     *  `llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address <rva>`. */
    readonly name: string;
    readonly address: number;
    readonly samples: number;
    /** The instrumented zone the thread was inside when it was sampled, or "".
     *  This is what joins the two sources: it answers "of the time inside
     *  ConvertVertices, where exactly does it go". */
    readonly zone: string;
    readonly thread: number;
    readonly pct: number;
  }

  export interface ProfSampler {
    readonly running: boolean;
    /** What was asked for. */
    readonly hz: number;
    /** What was achieved - **read this one**. A periodic waitable timer does not
     *  hold 1 kHz under load (1000 asked, 566 measured), and a flat profile only
     *  stands for time if the sampling was uniform. */
    readonly effective_hz: number;
    readonly taken: number;
    /** A suspend or GetThreadContext the OS refused. A few are normal; a rising
     *  count means the profile is missing time rather than that there was none. */
    readonly missed: number;
    readonly skipped: number;
    readonly ticks: number;
    /** Frame-pointer walks attempted, and ones that yielded no caller. A few percent barren is
     *  normal (a sample landing in a prologue); tens of percent means the walk is being
     *  rejected rather than terminating. */
    readonly walks: number;
    readonly barren: number;
    readonly drift_ms: number;
  }

  /** One distinct call stack and how often it was sampled. */
  export interface ProfStack {
    /** **Leaf first**: `stack[0]` is the sampled instruction, the rest are its callers
     *  outwards, symbolized the same way `ProfSample.name` is. */
    readonly stack: readonly string[];
    readonly samples: number;
    readonly zone: string;
    readonly thread: number;
    readonly pct: number;
  }

  /** Which frames a query covers. A plain number is `{last: n}`.
   *
   *  `around` is the one that matters for a stutter: `prof.worst()` will find a slow frame long
   *  after it happened, and without this there was no way to narrow a query onto it — "the last
   *  n frames" was the only expressible window, so a frame whose samples were still in the ring
   *  could be seen and not profiled. */
  export type ProfWindow =
    | number
    | { last?: number }
    | { around: number; pre?: number; post?: number }
    | { capture: number };

  /** One window the trigger saved out of the rings before they could overwrite it. */
  export interface ProfCapture {
    /** Pass as `{capture: index}` to any query. */
    readonly index: number;
    /** The frame that fired the trigger. */
    readonly frame_index: number;
    readonly ms: number;
    /** The median it was measured against. */
    readonly baseline_ms: number;
    /** Whether that frame was waiting for a vertical blank anyway — if so the trigger was
     *  measuring the monitor and the capture is probably not what you wanted. */
    readonly throttled: boolean;
    readonly frames: number;
    readonly events: number;
    readonly samples: number;
    /** The capture hit its fixed capacity and holds only part of the window. Raise
     *  `capture_events` / `capture_samples`, or narrow `pre`/`post`. */
    readonly truncated: boolean;
  }

  /** When the profiler should save a window out of the rings by itself. */
  export interface ProfTrigger {
    enabled: boolean;
    /** A frame fires it when it exceeds **both** — an absolute floor so a fast steady game does
     *  not trip on jitter, and a multiple of the running median so one number is not wrong for
     *  every scene. */
    min_ms: number;
    multiple: number;
    /** Frames of context kept either side. `post` is not padding: a stutter's cause often shows
     *  in the recovery, so the snapshot is deferred that many frames rather than taken the
     *  instant the trigger fires. */
    pre: number;
    post: number;
    /** The median frame time it is currently comparing against. Read-only. */
    readonly baseline_ms: number;
  }

  /** What `prof.symbols` reports about a map it loaded. */
  export interface ProfSymbolLoad {
    readonly entries: number;
    /** The map's recorded file size disagrees with the module actually loaded, so every name it
     *  produces is suspect — a different build shifts every RVA, which makes the names
     *  confidently wrong rather than absent. It still loads; `note` says what happened. */
    readonly stale: boolean;
    readonly note: string;
  }

  /** How the rings are currently sized — the same keys `configure` takes. */
  export interface ProfConfig {
    readonly mask: number;
    readonly events_per_thread: number;
    readonly frames: number;
    readonly sampler: boolean;
    readonly sampler_hz: number;
    readonly samples: number;
    readonly stacks: boolean;
    readonly stack_depth: number;
    readonly captures: number;
    readonly capture_events: number;
    readonly capture_samples: number;
  }

  export interface ProfThread {
    readonly slot: number;
    readonly id: number;
    /** "main", "executor", or "thread-<id>". */
    readonly name: string;
    readonly events: number;
    /** Events overwritten before anything read them. Non-zero means the window
     *  you are asking for may be incomplete - raise `events_per_thread`. */
    readonly lost: number;
  }

  /** The CPU profiler (`src/Profiler.h`): instrumented zones plus a sampling
   *  thread over both game threads, read back a frame at a time.
   *
   *  **Every query is over a window of recent frames, never the session.** The
   *  thing worth finding is usually a frame that was 17 ms when its neighbours
   *  were 5, and a running total is precisely the instrument that cannot see one.
   *
   *  Armed at boot with `GKPLUS_PROFILER=1` (`=zones` for no sampler,
   *  `GKPLUS_PROFILER_HZ` for the rate), or at any time with `prof.enabled`. */
  export interface Prof {
    /** Allocates the rings and starts the sampler. The rings are **never freed** -
     *  a recorder may be inside one on the other thread - so disabling stops
     *  recording but keeps the memory. */
    enabled: boolean;
    /** Which categories record, as a bitmask of `prof.categories`. `draw` is off
     *  by default: at ~60 ns a zone and ~700 draws a frame it is 0.04 ms, which is
     *  1% of a level frame and therefore visible in what it measures. */
    mask: number;
    readonly categories: Readonly<Record<string, number>>;
    /** What the rings are sized at now, which is not the documented defaults once
     *  `GKPLUS_PROFILER_HZ` or an earlier `configure` has been through. */
    readonly config: ProfConfig;
    /** The frame that has just ended - the REPL and `draw_gui` both run at
     *  Present - or null before the first one. */
    readonly frame: ProfFrame | null;
    /** What the profiler costs, priced from a zone cost calibrated at arm time
     *  against what the last frame actually recorded. Read it before believing
     *  anything else here. */
    readonly overhead_ms: number;
    readonly threads: readonly ProfThread[];
    readonly sampler: ProfSampler;
    /** Every site whose code has run at least once, armed or not. The answer to
     *  "is this path instrumented". */
    readonly sites: readonly { name: string; category: string }[];

    /** Newest last; no argument means the whole ring. */
    frames(count?: number): ProfFrame[];
    /** The slowest frames in the ring, worst first. Where a stutter is — the frame ring is far
     *  cheaper per entry than the event ring, so this reaches much further back than anything
     *  that can still be profiled. Use the `frame_index` it reports with `{around: …}`. */
    worst(count?: number): ProfFrame[];
    /** Aggregated over `window` (default: the last 60 frames), sorted by self time. */
    zones(window?: ProfWindow): ProfZone[];
    /** The flat sampled profile over the same window, sorted by count. */
    samples(window?: ProfWindow): ProfSample[];

    /** The flight recorder. A stutter rarer than the event ring's reach — 10 to 60 seconds, and
     *  shrinking the faster the game runs — cannot be caught by looking afterwards, because its
     *  zones are overwritten before anyone notices. This watches for one and copies the
     *  surrounding window somewhere the rings cannot reach.
     *
     *  **Unthrottle first.** Under FIFO the frame time is quantized to the refresh interval, so
     *  a 6 ms hitch is absorbed entirely and a 20 ms one reads as one extra interval — both the
     *  threshold and the baseline would be measuring the monitor.
     *  `GKPLUS_VK_PRESENT_MODE=immediate`.
     *
     *  Assign a boolean to arm it with the current settings, or an options object (which arms
     *  it unless `enabled: false` is given explicitly). */
    trigger: boolean | Partial<Omit<ProfTrigger, "baseline_ms">> | ProfTrigger;
    /** Windows the trigger has saved, oldest first. **`index` is the handle, not the array
     *  position** — the ring drops the oldest, so the two diverge. Read one with
     *  `{capture: c.index}`. */
    readonly captures: readonly ProfCapture[];
    clear_captures(): void;
    /** Distinct call stacks over the same window, most frequent first. Empty unless the sampler
     *  was armed with `stacks` (`GKPLUS_PROFILER=stacks`, or
     *  `prof.configure({stacks: true})`). `limit` defaults to 40; 0 means all.
     *
     *  gl.exe keeps frame pointers, so a walk runs straight through the game — on level02 the
     *  hot chain reads `BuildDrawRecord <- Aw_DrawIndexedPrimitiveUP <- SubMesh_DrawIndexed <-
     *  SceneNode_Render`, and `SceneNode_Render` recursing down the scene graph is what
     *  saturates the default depth of 12. */
    stacks(window?: ProfWindow, limit?: number): ProfStack[];

    /** A zero-duration event on the current thread's timeline. */
    mark(name: string): void;
    /** A named number plotted against the frame timeline rather than summed. */
    count(name: string, value: number): void;
    /** Times `fn` as a zone and returns whatever it returned. */
    scope<T>(name: string, fn: () => T): T;

    /** A Chrome-trace / Perfetto document - one track per thread, zones as
     *  complete events, samples as instants, frames on their own track. Open it in
     *  `chrome://tracing` or ui.perfetto.dev. Returns the path. */
    trace(path: string, window?: ProfWindow): string;
    /** Where a map is looked for automatically: `<game dir>\gkplus\symbols\`. The first time a
     *  name is wanted for a module, `<module>.sym` is tried there once — so installing a map is
     *  all it takes, with no call to `symbols()`. */
    readonly symbol_dir: string;
    /** Loads `<hex rva> <hex size> <name>` lines for one module, so a sampled profile reads in
     *  names instead of RVAs. gl.exe ships no symbols but its Ghidra database is heavily named:
     *  `utils/symdump/gl_symbols.py` exports it (12,487 functions, 62% named). Throws if the
     *  file cannot be read or parses to nothing. */
    symbols(module: string, path: string): ProfSymbolLoad;
    /** Re-arms with new sizes. Rings grow and never shrink. */
    configure(config: {
      mask?: number;
      events_per_thread?: number;
      frames?: number;
      sampler?: boolean;
      sampler_hz?: number;
      samples?: number;
      stacks?: boolean;
      /** Frames per sample, capped at 32. Each costs 4 bytes on every sample. */
      stack_depth?: number;
      /** The trigger's storage, reserved here so taking a capture is a memcpy rather than a
       *  malloc in the middle of the frame after a stutter. */
      captures?: number;
      capture_events?: number;
      capture_samples?: number;
    }): void;
    /** Empties the rings and the frame history. Does not disarm. */
    reset(): void;
  }

  // --- the module ------------------------------------------------------------

  export const prof: Prof;
  export const render: Render;
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
  export const text: Text;
  export const repl: Repl;
  export const mods: Mods;
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

  /** The default export carries the same twenty-six objects: `gk.actors === actors`. */
  const gk: {
    prof: Prof;
    render: Render;
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
    text: Text;
    repl: Repl;
    mods: Mods;
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
