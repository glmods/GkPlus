# The `"gk"` JavaScript bindings and their type definitions

What a script can reach, and how the binding layer is put together. The QuickJS rules that
govern *writing* a binding - the `JS_CGETSET_DEF` abort, the own-property lookup order, the
`JS_UpdateStackTop` thread rule - stay in `CLAUDE.md` under Conventions, because they have
to be read before the code is written, not after.

### JavaScript bindings (`src/Js*`)

**The console command registry is the map of what is still missing, and it is measured**:
`console_command_notes.md` has all 280 registrations (272 distinct names) classified against the
JS surface — 7 replaced by plain JavaScript, 33 already reachable, **223 bound**, 9 dev-only, and
**no gaps**. Every registered command is now reachable through a typed binding. Read it before adding a binding; §1 has the recipe for re-extracting the
registry from the binary if the classification ever needs rebuilding.

Two facts from it that change how bindings get written:

- **Fifteen command names live in `glres<lang>.dll`, not in the exe.** `EXIT`, `QUIT`, `MENU`,
  `HELP`, `LINES`, `CONSOLE APPEAR`, `SAY`, `TIME`, `DATE`, `LIST COMMANDS`, `CLEAR HISTORY
  BUFFER`, `HISTORY BUFFER SIZE`, `HISTORY_BUFFER_LENGTH`, `QUEUE SIZE` and `QUEUE LENGTH` are
  registered under `GetResourceString` results, so `console.execute("QUIT")` is a no-op on a
  non-English install. `console.commands` enumerates the registry so those are discoverable.
- **What makes a command hard to bind is its broadcast, not its setter.** Almost every
  world-mutating handler is `IsExecutorRunning` → `SuspendExecutor` → mutate →
  `BroadcastToPlayers(id, …)` → `ResumeExecutor`, and the update id and payload are per-command.
  §4.1 of the notes lists all 27 broadcasters with their recovered update ids and payload sizes.
  **But a broadcast is only wire-format work for a *native* binding.** Dispatching the command runs
  the handler, which broadcasts itself — which is why all 25 broadcasting gaps closed for free once
  the command-backed surface existed. They are also safe to call unguarded: 24 of the 25 check
  `IsExecutorRunning`, so a joining client no-ops and takes the host's update instead.
- **There are two kinds of binding here, and the split is load-bearing.** *Native* wrappers
  (`camera`, `game`, `world`, the `console` colours and registry) exist wherever there is **state
  to read back**. *Command-backed* namespaces (`fx`, `light`, `objectives`, `music`, `screen`,
  `units`, `inventory`, `tracks`, `demo`, `script`, the interpolated `camera` moves, the `console`
  administration) format a console command line and run the game's own handler — because every one
  of those handlers **is** an argument parser whose defaults come from the map bounds or the cursor,
  so dispatching it is faithful by construction where a reimplementation would silently diverge.
  What they add over a raw string: typed arguments, locale-independent numbers, names that survive a
  translated install, a length check the engine does not do, and per-command whitespace rules.
  `src/JsCommands.cpp`'s header comment is the full argument.
- **`ExecuteCommand` @ 0x004d6090 has no length check.** It copies into `ConsoleCommandLine`
  (`char[252]`) with an unbounded byte loop, and `SmallFont` @ 0x007b6a54 is next. `fgets`
  caps a batch line at 249 so the game cannot reach it, but a script can — `gk::ExecuteCommand`
  therefore refuses anything over `kConsoleCommandLineMax` and returns false.
- **Nine binding members do not replicate, and the ones next to them do.** `actor.armor`,
  `actor.shield`, `actor.set_position`, `actor.set_team`, `actor.concealed`, `actor.goto`,
  `turret.turret_enabled`, `pickup.set_required_item` and `tokens[name] = value` all reach no
  broadcast, while `actor.health`, `frag`, `remove`, `die`, `set_target`, `associate`, the attack
  methods and `set_pickup_enabled` do. The console commands beside them broadcast *around* the same
  setters, which is what makes the difference invisible in single player. `set_team` is the sharpest
  case: `Actor::ChangeOwnerAndTeam` @ 0x00530470 replicates and `SetTeamId` does not, and the
  binding took the latter — now fixed, and it was two bugs, because bare `SetTeamId` also left the
  actor on its **old team's actor list**. **Check the setter, not the command** —
  `console_command_notes.md` §6 is the full table.
- **The `RET`-form sweep never covered the free functions a mirror resolves by address**, and the
  first in-game run of `make` and `console.commands` found three defects sitting in that gap — a
  missing dereference in `GetCommandTable`, a `FastCall` that is really `__thiscall` with two stack
  args (`HierarchyResolveNamedPointPos`, which made every `make.role` with a `hotspot` fault), and
  `MakeCameraTrack`'s bind (0x005aa920 is `__fastcall(track /*ECX*/, rif /*EDX*/, name,
  Vec3 by value)` — all four wrong). `console_command_notes.md` §6.5 has all three and the recipe:
  for a declared arity, `__fastcall` puts the first two *integral* args in registers (floats never)
  and `__thiscall` only `this`, so the expected operand is `4 * stack_args`. That sweep has now been
  run over **every** `GetObjectAtOffset` in `src/` and is clean (§6.5.1) — read it before adding a
  native call, because two of its three traps (per-site declaration resolution, and the four
  tail-jump functions with no `RET` of their own) make a naive run report defects that are not there.
- **The `RET` test cannot see a register argument, so pair it with an ECX/EDX read check.** Calling
  `AcquireLevelRifForLocators` @ 0x00483da0 with *no* argument — which `MakeCameraTrack` did, on the
  belief that it falls back to the loaded level rif — pops exactly as many bytes as calling it
  correctly. It does not fall back: it `strlen`s ECX with no null check, and `ToCameraTrack` passes
  the section's own `file` field. The check is "does the target read ECX (or EDX) before writing it?"
  and it costs one pass over the first few instructions.
- **`ApplyDamage` takes three stack arguments and the mirror declared two.** Both overrides end in
  `RET 0xc`; `__thiscall` is callee-clean, so each call drifted ESP by 4 bytes — the exact failure
  the calling-convention warning above describes. The third is the attacker's team id (-1 for none),
  used for Deathmatch frag credit and as the 0x9b payload. The base and mobile versions are also
  genuinely different functions, not a base and an override that forgot to broadcast: see §6.2.
- **`triggers.create` is local, and that is correct.** Every machine runs the same `.gcs` and
  registers its own copy; the payload is what replicates when it fires. So register triggers in
  `setup` (which runs everywhere) and do *not* guard that with `game.simulation_running` — the
  opposite of the rule for world mutation.
- **A time trigger's `value` is a delay in seconds, not a tick deadline.** `RegisterTriggers` does
  `deadline = GetServerTime64() + ticks_per_second * value` at the calling thread's rate. `gk.d.ts`
  said "game-tick deadline" and was wrong on both counts.
- **The trigger is the only durable scheduling there is** (§6.1). `SaveGame` writes the trigger
  list, its payloads, the WAIT deadlines and the tokens — all of it a deadline plus a name, which is
  precisely why it can be saved. A JavaScript closure cannot be, so an in-heap timer is
  session-scoped by construction and a savegame load rewinds the clock under it.

**`game.simulation_running` is the authority test, and any script that mutates the world from a
message needs it.** It is `IsExecutorRunning` @ 0x00502da0 (`gk::IsSimulationRunning` in `Misc.h`) —
true in single player and on a multiplayer host, false on a joining client and before a level has
started one. A message is delivered on *every* machine, so a `message_received` that spawns
unguarded makes a joining client build a local ghost the host never hears about, and then receive
the host's copy as well. It is an accessor on a namespace object rather than a plain export because
a C module's named exports are fixed at instantiation — a top-level `simulation_running` could only
ever report what was true before any level existed.

The `"gk"` QuickJS C module, exposing 25 namespaces to scripts. `src/Js.h` is the public surface
— `RegisterGkModule`, plus `Log` / `ReportException` / `ReleaseCallbacks` for the host — and
`src/JsGk.cpp`'s `Namespaces` table builds them. Fifteen come one per translation unit —
`JsCamera`, `JsConsole`, `JsActors`, `JsRoles`, `JsTokens`, `JsTriggers`, `JsLevels`, `JsMake`,
`JsGls`, `JsGame`, `JsWorld`, `JsMods`, `JsRender`, `JsText`, `JsProf` — and the remaining ten are the
command-backed clusters
`JsCommands.cpp` supplies (`fx`, `light`, `objectives`, `music`, `screen`, `units`, `inventory`,
`tracks`, `demo`, `script`), over shared helpers in `src/JsBindings.h` / `src/JsCommon.cpp`.
**`JsMenus` is not in that table** — see below. `JsGk.cpp` also owns
`ReleaseCallbacks`, which fans out to the per-TU `Release*Callbacks` of the two namespaces that
hold script values (menus and levels).

```js
import gk, { camera, console, actors, roles, tokens, triggers, levels, make, gls } from "gk";
camera.distance = 900;                              // live accessors
for (const a of actors) if (a.alive) a.health = 50; // iterable
actors[12].frag();                                  // by id
actors["tbaa"].set_target(actors["hark"].id, 0);    // by token name
roles["gunlok"].spawn(0, {x: 0, y: 0, z: 0});
for (const [name, value] of Object.entries(tokens)) console.print(`${name}=${value}`);
console.log("actors:", actors.count);               // the host's own logging - no global console
console.execute("GOD ON");                          // and the game's command surface
tokens["score"] = 0;                                // upsert; actors/roles throw
for (const mod of mods) console.log(mod.priority, mod.name);   // what is mounted
console.log(mods.served, mods.recent[0]);           // ... and what it actually served
levels.add("Test Arena", arena);                    // `import * as arena` first
levels.start("Test Arena", {difficulty: "hard"});   // no menus, no briefing

export function setup_menus(menus) {                // `menus` is not an export
  menus.Main.add_item("Open console", (item) => {});  // menus[0], menus["main"] too
  menus[1].add_toggle("Cheats", false, (item) => log(item.value));
}
```

**`text` queues; it does not draw.** `text.draw(...)` appends to the font's pending list and the
game's per-frame overlay pass rasterizes it and then frees it, so a string drawn once is on screen
for **one frame** — anything meant to persist has to be drawn again every frame. It is the wrong
tool for a panel (that is the ImGui object `draw_gui` is handed) and the right one for text that
has to look like the game's, because it goes through the game's fonts, colours, layout and
batching. The binding clamps at `text.max_length` (1027): past that the engine smashes its own
stack, so an unclamped `text.draw` would hand every script a crash. See `src/Font.h`.

**`menus` is the argument to `setup_menus` and nothing else** — `NewMenusNamespace` is declared in
`src/Js.h` rather than `JsBindings.h`, and `JsGk.cpp`'s `Namespaces` table deliberately omits it.
Adding a front-end item is a boot-time act (the game's own items must already be there, or every
index in `OnMenuItemClicked` shifts), so the object is scoped to the callback that runs at that
moment. It is a plain collection otherwise, and `menus.current` / `menu.open` work whenever — a
script that wants them later keeps the argument in a module-level variable.

`actors`, `roles`, `tokens`, `menus` (the argument), `levels` and `mods` are all **exotic-property
collections** built by the same scaffolding in `JsCommon.cpp`: indexable, `in`-testable,
`Object.keys`/`values`/`entries`-able, and iterable over a snapshot. `NewCollection` gives every one
of them a non-enumerable `count` and `Symbol.iterator`; a namespace only supplies per-collection
extras (`roles.ai_types`, `menus.current`, `levels.add` / `levels.current`). Actors, roles, menus
and levels are keyed by **id** (names are a lookup convenience and are not enumerated) — for
`levels` that id is registration order, which is also the Choose Level order; tokens are keyed by
**name** and resolve to the bare value, with `lookup_id` left null so a token called `"5"` still
works.

Every lookup mints a **fresh wrapper**, so `menus[0] !== menus[0]` and likewise for actors and
roles. Compare `.id`, not object identity. The exceptions are `MenuItem` and `Level`: `add_item`
and `levels.add` return the same object the callback receives, because the binding holds it for the
registration's lifetime — and `levels.current` returns that same object too.

`menus` covers the front end only (`Menus[36]`, keyed 0-35 and by the `Menus.inc.h` name, matched
case-insensitively). A `Menu` wrapper has `id`, `name`, `title` (localized), `count`, `items` (a
snapshot of the game's own entries as `{index, label, type}`), `add_item`, `add_toggle` and
`open(remember)`. Unlike the Actor and Role wrappers, `Menu` and `MenuItem` hold pointers that
outlive every context — into the `.data` array and into a never-freed registration — so neither
needs a finalizer and neither can dangle.

**Every field that held a `.gcs` name now takes a message object too**, through one helper:
`js::ToScriptPayload` in `JsCommon.cpp`. A **string** becomes a `file` envelope, so a field written
from a script and one written from a `.gls` end up identical; **anything else** is
`JS_JSONStringify`d into a `message` envelope. Both go through `FileScriptPayload` /
`MessageScriptPayload` in `ScriptQueue.h`, which exist so that nothing outside `ScriptQueue.cpp`
spells a kind. `JSON.stringify` refusing a value (a function, a symbol) is a `TypeError` at the
call, not a payload named "undefined".

**Four fields have it, and that number comes from the call-site inventory rather than from
guessing.** `threading_model_notes.md` traces the argument of all seven `QueueScriptExecution`
callers; every one of them that reads a script-writable field is covered —
`triggers.create({script})` → `TriggerData+0x54`, `actor.associate(script)` →
`PickupActor+0x134`, `make.role({interface_beam_script})` → `Vulnerability+0x10`, and
`make.role({destructibility: {kind: "replace", script}})` → `ReplaceDestructibility+0x08`. The
last was missed on the first pass, which is the argument for doing the inventory: it is spelled
`script` here although GLS spells the field `name`, because a queue push is its only reader.

`ToScriptPayload` returns a **complete envelope**, which is also exactly what the queue wants, so
`Level.send(msg)` — the one caller that skips the field — hands it to `QueueScriptPayload` rather
than to `QueueScriptMessage`. That split matters: `QueueScriptMessage` takes a message *body* and
wraps it, so passing an envelope to it would bury the kind and turn `send("wave2.gcs")` into a
message whose body is an envelope. Anything else queueing directly has the same choice to make.

Writes go through `CollectionOps::assign`, which only `tokens` supplies — `tokens["score"] = 0` is
an upsert. The handler **throws** rather than returning false: quickjs hands an exotic
`set_property` result straight back to the caller (quickjs.c:10209-10213) instead of converting
false into the strict-mode TypeError the ordinary read-only path raises, so returning false would
make `actors[1] = x` a silent no-op. Own properties resolve first (quickjs.c:10137 vs :10203), so
`tokens.count = 5` hits the read-only accessor and cannot reach the table.

`Actor`/`Role` wrappers hold the **raw game pointer**, so a wrapper kept past its object's
destruction reads recycled pool memory — `.valid` is the escape hatch, and `frag()`, `remove()` and
`die()` null the pointer because those are the destructions the binding can see. `Actor`'s surface
is the vtable, not its fields; `id` (our own snapshot) and `name` (a token lookup) are the two
documented exceptions. Tokens are not wrapped at all: an 8-byte `{name, value}` pair has no
identity to re-resolve against, so the collection yields plain values.

**The Actor hierarchy is a real JS prototype chain.** `src/ActorClasses.inc.h` is an X-macro of
`(Name, Parent, Predicate, Kind)` over all fifteen subclasses, and `JsActors.cpp` generates the
per-class `JSClassID`s, the `JSClassDef`s, the `kind` strings, the RTTI dispatch ladder and the
chained prototypes from it. `NewActorWrapper` runs the ladder once and picks the class, so
`JS_NewObjectClass` installs the right prototype:

```js
actors[4] instanceof actors.classes.MobileActor;   // true for a turret
typeof actors[5].goto;                             // "undefined" - a pickup cannot move
```

Three consequences worth knowing:

- **A subclass member is absent, not throwing.** `if (a.goto)` is a valid feature test, and walking
  a whole prototype chain reading every property is safe. Under the previous flat prototype
  `turret_enabled` sat on the *base*, so merely reading it on a character raised.
- **The chain does not replace the checked downcasts.** The prototype is chosen once, at wrap time,
  and the actor it was chosen for can be destroyed afterwards, so
  `ResolveMobile`/`ResolveCharacter`/`ResolvePickup`/`ResolveTurret` still re-run the predicate on
  every call. A borrowed method (`MobileActor.prototype.goto.call(pickup, …)`) still throws.
- **`Resolve` re-derives the actor from its id on every access; the stored `Actor *` is only a
  cache.** It used to be the identity, and that was a **use-after-free rather than a stale read**:
  the *executor thread* frees actors with no notification and `pool_free` recycles the page, so a
  wrapper held across one frame could be pointing at an unrelated object of a different class by
  the next property read. Nulling `ptr` in `frag()`/`remove()` covered only the destructions
  initiated from JS - the two that were never the problem. `GetActorById(w->id)` now gates every
  access, and an id that resolves to a *different* object than the wrapper was made from is
  refused too, so a script that captured a corpse cannot start driving whatever inherited its id.
  `valid` already worked this way; every other accessor now agrees with it.
  Residual, stated rather than hidden: that lookup walks a bucket chain the executor can relink,
  so a single read is narrowed from "reads freed memory" to "may read a chain mid-relink".
  Closing it would cost an `ExecutorPause` - a thread round trip - per property read. The paths
  that can afford one take it: `CollectActorKeys`, `CountActors`, and the three mutators
  `frag`/`remove`/`die`, which is also what the engine's own `Command*` handlers do.
- **One ordering rule drives everything**, and `JsActors.cpp` `static_assert`s it: every class is
  listed **before its own base**. The predicates are inherited (`IsMobile()` is true for a turret),
  so the ladder must test most-derived first; the chain is built by walking the list backwards,
  which needs each base's prototype to exist already.

Classes that add no JS surface (`PopupActor`, `CentibodyActor`, the background creatures…) still get
a prototype and a class id, so `instanceof` does not lie about a class an actor genuinely is.
`actors.classes` holds one non-enumerable handle per class purely for the brand check and for
`constructor.name`; calling one throws, since scripts cannot create actors. Like `count`, being an
own property means it shadows the indexer, so a token literally named `"classes"` is unreachable
through `actors[…]`.

`EnsureClass` has a six-argument overload for this: unlike the five-argument one, it *always*
creates a prototype (a class adding no members still needs one in the chain) and chains it to the
parent's, re-read per context because `JS_SetClassProto` is per-context.

### Type definitions (`types/`)

Ambient `.d.ts` declarations for everything a script can reach, so an editor type-checks a plain
`.mjs` with no build step. `types/README.md` is the user-facing half; what matters here is who
maintains what.

- **`types/gk.d.ts` is hand-written and must be updated alongside `src/Js*.cpp`.** The bindings are
  the truth; nothing enforces the correspondence but `types/typecheck.ts`.
  **`render` is the one namespace that is only partly declared**, and deliberately: its
  material-override members are typed because they are a feature a mod uses, while the rest is the
  Vulkan investigation's measurement surface, which would be stale here more often than right. The
  `[key: string]: any` on `Render` is what keeps it reachable *and* says it is untyped — do not
  read it as a licence to leave a new namespace loose.
- **`types/imgui.d.ts` is generated** — re-run `python3 types/gen-imgui-dts.py` after touching
  `imgui-quickjs.cpp`. The generator reads the export list for names, each wrapper *body* for types
  (which `JS_To*` a parameter goes through, which `JS_New*` the result comes from, and the
  `JS_GetPropertyStr` keys of an options object), and the doc comment for parameter *names* only.
  It currently types all 197 functions and 28 enums with **zero `any`**, and prints the count of
  anything it could not infer — if that number stops being 0, the C++ grew a shape the generator
  does not know.
- **`npx -y -p typescript tsc -p types/tsconfig.json` is the check** — TypeScript is not a
  dependency of this repo and there is no `package.json`, so a bare `npx tsc` refuses to run
  ("This is not the tsc command you are looking for") and `-p typescript` is what fetches it.
  `examples/jsconfig.json` is the same invocation, and checks the shipped `.mjs` against the same
  declarations. `types/typecheck.ts` asserts in both
  directions: ordinary lines must compile, and every `@ts-expect-error` line must not. A vacuous
  declaration file (everything `any`) fails it, because all twenty-seven expect-error directives
  would go unused.

Three modelling decisions that took a round-trip through `tsc` to settle:

- **`Actor` is a discriminated union on `kind`, not an inheritance chain.** Interfaces cannot
  narrow an inherited property to a different literal, so `interface CharacterActor extends
  MobileActor` and `kind: "character"` are mutually exclusive. The members are therefore composed
  from mixin interfaces (`ActorBase`, `MobileMembers`, `CharacterMembers`…), each class gets its
  own literal `kind`, and `Actor` is the union of all sixteen — which is what makes
  `if (a.kind === "turret") a.turret_enabled` work. Users write `Actor`, never `ActorBase`.
- **`instanceof` narrows through a widened `prototype`.** `actors.classes.MobileActor` is typed
  `ActorClass<AnyMobileActor>`, not `ActorClass<MobileActor>`, because at runtime the test is true
  for every descendant; narrowing to the exact class would be *wrong*. `ActorClass<T>` extends
  `Function` and deliberately has **no construct signature**, so `new actors.classes.PickupActor()`
  is a type error — matching the runtime, which throws.
- **A collection is an intersection**, `Members & { readonly [key: string]: T | undefined }`.
  Declaring `count` beside a string index signature is illegal in a single interface (the property
  must conform to the index type); in an intersection the declared property still wins on lookup,
  so `actors.count` is `number` while `actors["hark"]` is `Actor | undefined`. `tokens` is the one
  whose index signature is not `readonly`.
