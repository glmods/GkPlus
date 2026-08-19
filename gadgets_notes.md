# Gadgets: mines, decoys, scavenging, laser fences

Four small tactical subsystems recovered from `gl.exe`. Everything below is measured in the
Ghidra DB unless a claim is explicitly marked **inferred** or listed in §9.

One framing correction that runs through the whole file: **three of the four "gadgets" are not
gadget code at all.** A mine is an ordinary `MobileActor` with an AI behaviour function, a decoy
is a mine with a different `weapon` value, a laser fence is a render-queue node with no gameplay
reader, and a scrap pile is a `PickupActor` whose `Role*` was swapped. There is no bespoke
proximity volume, no noise-event bus, no fence collision primitive and no container object.

---

## 1. Mines

### 1.1 What `ai mine` produces

`CreateActor` @ 0x00510760, `case Mine:` -> `pool_alloc(0x230)` + `MobileActor::Ctor`. So on the
**executor** side `ai mine` is a plain `MobileActor` (0x230). `level_loading_notes.md` §7's
"Mine 0x238" row is the **client** side under `ClientSpawnActorForTeam` @ 0x004fce90 and is also
correct - the two trees are not comparable. Use `Actor::GetSize` (slot 35) as the size oracle in the
executor tree; slot 35 is `GetSize` in the client tree too.

**But 0x238 is not a mine-specific class on the client either.** It is `MobileUnit` (vtable
0x0066491c), and `ai mine` lands there only because a mine carries no weapon: the discriminator is
`character->weapon == 0x21`, the *no-weapon* sentinel, so `MobileUnit` is **shared** by `ai mine`
and every other unarmed character. `CharacterUnit` (0x2e0) is what an armed one gets, and it derives
from `MobileUnit` rather than sitting beside it. `rendering_notes.md` §5.1 has the whole 16-class
tree.

**`CreateActor`'s other mine-looking test is not about mines.** The `character->weapon == 0x21`
arm (0x00510c80, and the same compare in `ClientSpawnActorForTeam`, `DoSpawn`,
`CharacterActor::Ctor` @ 0x0053c83d, `CharacterUnit::CharacterUnit` @ 0x004c1100, `CreateUnit`
@ 0x004fd450) is the **"no weapon"**
sentinel: `CharacterActor::Ctor` initialises its own weapon slot `+0x2a4` to 0x21 at 0x0053c825,
and `src/Roles.h` already records 33 as the unnamed "none". A weaponless character becomes a
`MobileActor` rather than a `CharacterActor`. Nothing there concerns mines.

### 1.2 `MineWeaponType` - the five ids are 0..4, and this is proven behaviourally

`Character::weapon` (+0xac) carries a **dense 0..4 sub-enum** for the mine family. Three
independent dispatches agree, and each one pins a different id:

| id | GLS keyword | role identifier | `AiThink_Mine` arm | `Mine_OnDeployed` arm |
|----|-------------|-----------------|--------------------|------------------------|
| 0 | `standard mine` | `mine` | proximity scan -> detonate | - |
| 1 | `remote mine` | `remote_mine` | **nothing** (never self-triggers) | broadcast 0x3e, register with the detonator |
| 2 | `timed mine` | `timed_mine` | detonate immediately | next-think = now + **10 s** (the fuse) |
| 3 | `decoy mine` | `decoy` | lure tick, +6 s re-arm, broadcast 0x9f | broadcast 0x3e, expiry/next-think = INT64_MAX |
| 4 | `EMP mine` | `EMP_mine` | proximity scan -> detonate | - |

- The **role-name switch** at `MobileActor::UseInventoryItem` 0x0053730e maps 0..4 to the literals
  `"mine"`, `"remote_mine"`, `"timed_mine"`, `"decoy"`, `"EMP_mine"` (jump table 0x00537d90).
  Two more copies of the same table exist: `HudItem_DrawByKind` @ 0x0055fbd0 (0x005636a0) and
  `Unit::Unit_ConsumeInventoryItem` @ 0x004bd670 (0x004bd8c0, table 0x004bd8c0).
- The compare is `CMP EAX,0x4 / JA` with **no subtraction**, at all three sites, so the values
  really are 0..4 rather than 33..37 or 34..38.
- Behaviour corroborates every id: `Mine_OnDeployed` gives id 2 a fuse and id 1 a detonator
  registration; `AiThink_Mine` gives ids 0 and 4 the proximity scan, id 2 an immediate blast and
  id 3 the lure; and the executor's detonate-all sweep (0x00509d15) filters on `weapon == 1`,
  which is exactly what "remote" means.

**Consequence for the C++ mirror:** `src/Roles.cpp`'s `WeaponTypes[]` (`plasma pistol` = 0,
`plasma pistol training` = 1, `plasmagnum` = 2 ...) **cannot** be used for the mine keywords -
the numbering collides head-on. The `character` section's `weapon` and the `ammo` section's
`weapon type` are not one enum for these five keywords, whatever `src/Roles.h:469-477` hedges.
Nothing breaks in the game because an id is only interpreted inside a pickup category:
`GetWeaponPickupRole` requires pickup type 4, minelayers are type 5, and `UseInventoryItem`'s
case 5 is the only reader of `+0x20` for a minelayer.

The DB now carries this as enum `/gunlok/MineWeaponType`.

### 1.3 There is no F1-F12 slot table

The inventory is an unbounded `List` hanging off `MobileActor+0x194`; item ids come from
`NextInventoryItemId` @ 0x007b6e48. What does exist is a **body-slot table of eight**, and it is
about where an item hangs on the model, not about hotkeys:

- `MobileActor::EquipToFirstOpenSlot` @ 0x00536830 walks the string table
  `BodySlotHotspotNames` @ **0x006672c4**, stride **7**, entries `slota`..`sloth` (8).
- Slot index = 2 + letter, i.e. **2..9**, and that index lives in `InventoryItem+0x0c`
  (`iVar1 - 2U < 8` at the occupancy scan).
- Occupancy is a local 8-byte bitmap rebuilt per call from the inventory list; a slot is only
  usable if the unit's own hierarchy really has that named node
(`Renderable_GetNodeWorldPosition` @ 0x0059d270).
- Overflow: no free slot -> the function simply returns without equipping. No error, no
  eviction.

Decoys share nothing special here: a minelayer pickup is one inventory item like any other.

### 1.4 `InventoryItem` (0x38 bytes) - `Inventory_AddItemFromRole` @ 0x004e4790

`__thiscall(Inventory* /*ECX*/, int category, Role*, int item_id, int count)`, **`RET 0x10`**.
`category` is `GetPickupType(role)` = `round(character->aggression * 10)`.

| off | meaning |
|-----|---------|
| 0x00 | category (the pickup type; 5 = minelayer) |
| 0x04 / 0x08 | shape / hierarchy (from `InventoryInfo` if present, else `role+0x1c` / `role+0x18`) |
| 0x0c | body slot index 2..9 |
| 0x14 | icon (`InventoryInfo+8`) |
| 0x18 | **count** = `round(character->walking_speed * DAT_00652190)` (that constant is 1/65536 - `walking_speed` is 16.16 fixed point), or the caller's `count` |
| 0x1c | role id |
| 0x20 | **`character->weapon` verbatim** - the `MineWeaponType` for a minelayer |
| 0x24 | item id |
| 0x28 | amount (health/armour categories 3 and 9) |
| 0x30 | required-flags mask (`role+0x54`, GLS `limit`) |

Two arms never allocate: **category 2 returns immediately** (that is the "nothing" pickup -
`Chr_Nothing_Pickup`, aggression 0.21 - and so the origin of the "Junkpile Empty" message), and
category 6 forwards to the ammo path `Inventory::AddAmmo` @ 0x004e53e0.

`Chr_Minelayer_Pickup`'s `walking speed 5 // number of mines carried` is therefore literally the
item count, and `walking speed 10` on `Chr_Decoylayer_Pickup` is ten decoys.

### 1.5 Deploying: `MobileActor::UseInventoryItem` case 5 @ 0x0053730e

Guards, in order: `is_moving == false`, `+0x12c == 0` (not already laying), `is_crouched ==
false`, and `GetRoleByName(<mine role>) != NULL`. Then:

```
SetMoveState(this, 0); ClearRouteWaypoints();
this->+0x134 = mine role;  this->+0x138 = the inventory item;  this->+0x12c = 1;
PlayAnimation(this, 0x60, ...);        BroadcastToPlayers(0x4f, 0x19 bytes)
```

`MobileActor::Update` @ 0x00533720 (vtable slot 70) runs the rest as a small state machine on `+0x128`
(0 -> 1), driven by an animation-event byte at `+0x12e`, with a deadline float at `+0x130`:

- state 0 -> 1 at 0x005353d3, broadcast **0x53** (0x10 bytes).
- state 1, once the clock passes `+0x130` (0x00535509): `DEC [item+0x18]` (the count),
  broadcast **0x7f** (0x10 bytes) with `{actor id, item id, remaining}`; if the count hit zero,
  remove the item from the inventory and broadcast **0x7e** (0xc bytes).
- the actual spawn, 0x005356de:
  `SpawnRole(team = this->team_id, role = this->+0x134, &this->position (0xa0),
  &this->orientation (0xac), owner = this->id)` - **the fifth argument is the owner actor id**,
  which is the only ownership record a deployed mine gets.
- then, when `team != 0 && team != 2` and the mine role's `character->weapon` is 1 or 3
  (remote or decoy), a further broadcast **0x8e** (8 bytes) carrying 0x2f3a.

### 1.6 AI: `AiThink_Mine` @ 0x004552a0 and `Mine_OnDeployed` @ 0x0045a640

`Actor_SetAiBehaviour` @ 0x00450550 (the AI-behaviour installer) sets, for `AIType::Mine` only,
**two** function pointers: `Actor+0x34` = `AiThink_Mine` and `Actor+0x38` = `Mine_OnDeployed`.
Every other AI type sets only `+0x34`. The two are not peers: `+0x34` is the per-tick proc, while
`+0x38` is a one-shot **on-placed hook** called from vtable slot 51
(`Actor::InitPositionAndTiming`) @ 0x0052dd18 - `ExecutorActorTick` reads neither field.

`AiThink_Mine` is `__thiscall(Actor*, uint now_lo, int now_hi)` and rate-limits itself to **1 Hz**
against the calling thread's `ClockTicksPerSecond`, storing the next-think deadline in
`Actor+0x58/+0x5c`. Arms as in the table in §1.2. The proximity scan (ids 0 and 4) walks
**`TeamSlots`**, skips the mine's own team, skips Cooperative allies via `TeamSlot+0x6a`, and
tests squared distance against `Actor+0x168` squared; a victim already targeting the mine, or one
whose `+0x189` byte is set, is skipped.

`Mine_OnDeployed` is where the **timed fuse lives: `now + 10 * ClockTicksPerSecond`**, i.e.
**10 seconds**, on the calling thread's clock. It also ORs 0x100 into `Actor+0x7c` on every arm.

**`mine_laying_time` (`Character+0x18`, default 8.0) has no reader anywhere in the binary** -
see §9.

### 1.7 Detonation: `MineDetonate` @ 0x004507b0

`__thiscall(Actor* /*ECX*/)`. The blast is **not** a projectile and **not** `Frag`. It is a
fourth executor-side `ApplyDamage` (slot 68, `vtbl+0x110`) call site, at **0x00450c7e**:

```
ScaleDamageForResistance(out, TeamSlots[victim.team]+0x6a, damage,
                         victim_role->resistance_factor (+0x90),
                         victim_role->armor_value (+0x94), 0x21)
ApplyDamage(victim, *out, 1, mine->team_id)          ; three stack args, RET 0xc
```

`0x21` is the mine damage **kind**. The damage magnitude comes from `role->+0x54` (GLS `limit`,
which `mine.gsh` comments as `// damage done`: 200 standard, 200 remote, 300 timed, 0 EMP,
40 enemy_mine), read at 0x0045095a. Victims come from a sweep of `actors` @ 0x007ba0d8 taken
through `HashTable_Actor_CopyCtor` @ 0x0045c730 - a **whole-table snapshot copy**, not an
iterator, so the sweep survives the deletions it causes (the iterator is
`HashTable_Actor_IteratorCtor` @ 0x0044cad0). Radius test at 0x00450b2b; the call at the end of
that test is `AreFriendlyMinesEnabled` @ 0x00512a40 - an 11-byte read of the `AreFriendlyMinesOn`
game-rule option (`CMP dword ptr [0x006abe1c],0 / SETNZ AL / RET`), **not** a line-of-sight test.
Whatever culls victims behind cover, it is not this call.

Two arms worth naming:

- **EMP**: when the detonator's `weapon == 4`, the victim gets `vtbl+0x10c` (slot 67) instead of
  damage, gated on `TeamSlots[victim.team]+0x6a`. That is why `Rol_Mine_EMP` ships with
  `limit 0`.
- **Chain reaction**: a victim whose `ai_type == Mine` is re-entered through `MineDetonate`
  itself (0x00450bea) - **but only when the detonator is not an EMP mine** (`weapon != 4`
  guard at 0x00450bde).

Finally `vtbl+0x104` (slot 65) deletes the mine.

**Remote detonation is an executor command, not a console command.** `ExecutorThreadProc`
@ 0x00509050 has two arms:

- 0x00509c34: addresses one unit as `TeamSlots[team].roster[slot]` (stride 0xc4), requires
  `character->weapon != 0`, then `weapon == 1` -> `MineDetonate`, `weapon == 3` ->
  `Decoy_Dismiss(team, slot)` @ 0x00450f60.
- 0x00509cdd: the **detonate-all sweep** - iterate `actors`, keep
  `ai_type == 2 (Mine) && character->weapon == 1 && team_id == payload.team`, `MineDetonate` each.
  This is the `D` key's server-side arm.

---

## 2. The decoy

Two objects, and neither is a noise event.

**Arming it is a client-side mode, and it has its own flag.** `ActivateGadgetOnSelection`
@ 0x004a1670 walks `SelectedUnits` for an inventory entry with `[0x00] == 5` and `[0x20] == arg2` —
pickup class 5 (minelayer) with a sub-type. Sub-type **3** is the decoy, and its arm
(0x004a1769-0x004a178b) sets `DecoyTargetPending` @ 0x007b3f52 to 1, then prints
`GetResourceString(GL_UITXT_DECOY_LOCATION)` to the console and calls
`SetCursorMode(GroundTarget)`. Both cursor-mode cancels clear it beside `FlareModeActive`
@ 0x007b3f51 (0x004a449e in `OnGroundTargetClick`, 0x004a44fd in `CancelGroundTargeting`).

The next **ground click** is what fires it: `IssueMoveOrderToSelection` @ 0x0049f06f reads that
byte and dispatches client `Unit` vtable slot **80**, `CharacterUnit::Unit_SendThrowDecoy`
@ 0x004c4040, instead of the slot-75 move order — which sends command **`0x2b`**, 24 bytes
`{0x2b, unit_number, f32 GetGameTimeSeconds, Vec3 pos}`, to `MobileActor::ThrowDecoy`
@ 0x00541170.

**A queued decoy click is silently dropped — a game defect.** With `queued` set (Active Pause, or
shift-click) and the flag armed, the dispatch falls through `TEST AL,AL` / `JNZ 0x0049f0ce`
@ 0x0049f0bb and issues *nothing*: no command, no message, and the flag stays armed. Only the
unqueued click works. `orders_notes.md` §8.3 has the four-way table.

**The lure is fired.** `ProjectileActor::OnPrePhysics` @ 0x00542ae0: on the no-actor-hit branch
(0x005434db) it compares the projectile's role against `GetRoleByName("decoy_projectile")`; on a
match it computes `impact + {0, -1, 0}` and calls

```
SpawnRole(team = projectile->team_id (0xbc), role = GetRoleByName("decoy"),
          &pos, &projectile->orientation (0xac), owner = -1)
```

(0x005435aa-0x005435bd). Note the owner is **-1**, unlike a hand-laid mine.

**The lure is then an actor, not a stimulus.** The spawned `decoy` role is `ai mine` with
`Chr_Decoy` (`strength 50`, `sight range 5`, `hearing range 5`, `weapon decoy mine` = 3). Its
"radius" is therefore whatever radius the *enemy's* perception has - the decoy is simply a
hostile-looking actor to be shot at. `AiThink_Mine`'s id-3 arm re-arms itself every 6 seconds,
calls `PostAiStimulus(pos, pos, deadline, ...)` @ 0x0044f960 and broadcasts update
**0x9f** (8 bytes, reliable) until the expiry in `Actor+0x90/+0x94`, at which point it deletes
itself through slot 65.

**What makes the decoy's own tick reachable.** `Mine_OnDeployed`'s id-3 arm sets *both* the
expiry (`Actor+0x90/+0x94`) and the next-think (`Actor+0x58/+0x5c`) to `INT64_MAX`, and
`AiThink_Mine` returns while `now <= next-think` - so on placement the id-3 arm can never run.
`Decoy_Dismiss` @ 0x00450f60 is the writer that changes that: from `ExecutorThreadProc`'s
`character->weapon == 3` arm of command 0x30 (0x00509ca7, i.e. the owning player pressing that
unit's detonate key) it sets `+0x58/+0x5c` to the current clock (**next-think = now**), re-arms
`+0x90/+0x94` to `clock + 60 * ClockTicksPerSecond`, broadcasts `0x3e` (12 bytes, unreliable,
`{actor id, 0x7fffffff}`) and clears the team's deployed-decoy slot. So a decoy is inert until it
is dismissed, and then lures for sixty seconds.

`Mine_OnDeployed`'s own call site is traced: `Actor+0x38` is read at
`Actor::InitPositionAndTiming` @ 0x0052dd18 and at `Actor_FixupAfterLoad` @ 0x00531860, and
nowhere else in the image.

**Reconciliation point for the perception agent:** the stimulus entry point is
`PostAiStimulus` @ 0x0044f960 (the decoy's only outward call), and the mine's own detection
uses no perception function at all - `AiThink_Mine` reads `TeamSlots` directly.

---

## 3. Scavenging (`HEAP` / `RESPAWN HEAP`)

`CommandHeap` @ **0x00445210** (bare `RET`), `CommandRespawnHeap` @ **0x004455a0** (bare `RET`).

### 3.1 There is no container - capacity is exactly 1

`HEAP <target> <item-role> [<item-role> ...]`:

1. First word: `GetRoleByName`; if that fails, `ConsoleParseInt` as an actor id.
2. Then words are consumed as item roles into a **temporary** `List<Role*>` (pool nodes,
   vtable `HeapVtbl` @ 0x0065208c) until one is not a role name. Unbounded.
3. **One is chosen at random**, and the list is thrown away (`List_Dtor` @ 0x0044cec0 at the end).
4. Guarded by `IsExecutorRunning()` + `SuspendExecutor`/`ResumeExecutor` - a joining client
   no-ops.

So the shipped `level04.gcs` line that heaps two items onto `garbage_a` does not put two items
in a pile; it names two **candidates** and one is picked at command time. Capacity is 1, and a
second `HEAP` on the same target simply overwrites.

### 3.2 Two different stores, depending on what the target is

- **target `IsPickup()`** (a scrap pile): `actor->entity (+0xc0) = chosen role`, then
  `local_1c = character->aggression * 10; ROUND(...)` and a call through `vtbl+0x148`. The pile
  literally **becomes** the item - which is why walking over it collects that item and why the
  pile is consumed by the normal pickup path rather than by anything scavenge-specific.
  This branch issues **no broadcast of its own**.
- **any other actor** (`heap runner grenade_plus`): `Actor_SetHeapDropRole` @ 0x0052f2d0,
  `__thiscall(Actor*, Role*)`, sets `actor->+0xc4 = role` and broadcasts update **0xb9**
  (12 bytes, reliable) `{actor id, role id}`. `Actor::Delete` @ 0x0052f117 then spawns that role
  at the corpse (`SpawnRole(team, role, &pos (0xa0), identity quat, owner = this->id)`,
  0x0052f1bc). `Actor+0xc4` also round-trips through `WriteActorFixups` / `ReadActorFixups`, so
  it survives a savegame.

For a role-name target the loop **re-randomises per actor**, so each matching actor gets its own
independent draw.

### 3.3 PRNG - yes, and it is the calling thread's

Both draws inline the BSD additive LFG exactly as everywhere else, selecting the table with
`GetCurrentThreadId() == ExecutingThread`. `HEAP` reaches this from the console command pump,
which is **main-thread**, so it draws from **`RandomState` @ 0x006a3008**, not the executor copy.
The value used is `(*fptr += *rptr) >> 1`, then `% count`.

That is a determinism hazard only in principle: the whole mutation sits behind
`IsExecutorRunning()`, so on a joining client nothing is drawn and nothing is chosen - the state
arrives as update 0xb9 (drop case) or not at all (pile case; see §9).

### 3.4 "Junkpile Empty"

`Inventory_AddItemFromRole` refuses **category 2** outright (`if (param_1 != 2)` wrapping the
whole body). `Chr_Nothing_Pickup`'s `aggression 0.21` -> `GetPickupType` = 2, so an empty pile is
a pickup that adds nothing. The 7343 string itself lives in `glres<lang>.dll`, not in `gl.exe`
(the only 0x1caf literal in the exe is in `GSHTokenize` and unrelated), so the id is reached
through `role->pickup_name_id` + `GetResourceString` - **not measured here**.

`role_system_notes.md` §7's "2 = default / non-pickup" should read "2 = the *nothing* pickup, and
the one category the inventory refuses".

### 3.5 Cursor hit test / shape selection

**Not established.** The pincers cursor and the pile's model come from the swapped-in `Role`
(§3.2), so there is nothing scavenge-specific to find; the generic pickup cursor logic was not
traced.

---

## 4. Laser fences and electricity

### 4.1 Handlers

| command | handler | `RET` | worker |
|---|---|---|---|
| `LASER FENCE` | `CommandLaserFence` @ **0x00447490** | bare `RET` @ 0x00447593 | `CreateLaserFence` @ 0x0051c0f0 |
| `ELECTRICITY` | `CommandElectricity` @ **0x004475a0** | bare `RET` @ 0x004476c7 | `CreateElectricity` @ 0x0051a150 |
| `DEACTIVATE ELECTRICITY` | `CommandDeactivateElectricity` @ **0x004476d0** | bare `RET` @ 0x00447798 | `RemoveWorldEffectNearPoint` @ 0x0051a2b0 |

All three are `void()` console handlers gated on `GameState in {5,6,7,0x12}`.

`CreateLaserFence` is `__fastcall(Vec3f *start /*ECX*/, Vec3f *end /*EDX*/)`, bare `RET` -
`src/Math.h:16-18`'s `FastCall<void, Vec3*, Vec3*>` mirror is **correct**. `CreateElectricity` is
`__fastcall(Vec3f* /*ECX*/, Vec3f* /*EDX*/, float amplitude /*stack*/)`, **`RET 0x4`**.

### 4.2 The grammar question, settled

Both commands take their endpoints through **`ConsoleParsePosition`** @ 0x0044ece0, which accepts
*either* form per endpoint:

- `ConsoleNextArgIsNumeric()` true -> three `ConsoleParseFloat`s, i.e. literal X Y Z;
- otherwise -> `ConsoleParseWord` and a linear scan of **`MapAuxObjectList`** @ 0x00739098,
  skipping entries whose type string (`obj+0x38`) is `"Sound"`, comparing `*(char**)obj` to the
  word with **`lstrcmpiA`**.

So **`types/gk.d.ts:879`'s `laser_fence(start: Point, end: Point)` is safe**: six numbers is a
legal `LASER FENCE`, even though no shipped script uses it. And `LASER FENCE` genuinely takes
**no amplitude** - the handler never calls `ConsoleParseFloat` after the two positions.

`ELECTRICITY`'s amplitude is parsed by an **unchecked** `ConsoleParseFloat(&amplitude)`: the
return is discarded, and on an exhausted command line the callee writes its out-parameter from an
uninitialised stack slot (`*param_1 = in_stack_00000004` at 0x004d6aea). Treat "amplitude is
optional" as "amplitude is garbage if omitted".

### 4.3 The case-insensitive compare - it is not a name match on the fence

**There is no name on a fence node.** `DEACTIVATE ELECTRICITY <name>` resolves `<name>` to a
*position* through the same `ConsoleParsePosition`, and the node is then found by **coordinates**.
The case-insensitivity the shipped corpus shows (`"fence a10"` created, `"fence A10"`
deactivated, x40) is `lstrcmpiA` at **0x0044ee4f** inside `ConsoleParsePosition`, against
`MapAuxObjectList` dummy-object names - the same call the *creating* command used, which is why
the two always agree.

That also explains "it matches against either endpoint" without any special logic: the endpoint
name is just a dummy object, and the removal tests both stored points.

### 4.4 Node layout (0x80 bytes, `pool_alloc`, ctor `WorldEffect_Ctor` @ 0x0052abe0)

| off | laser fence (kind 5) | electricity (kind 4) |
|-----|----------------------|----------------------|
| 0x00 | **kind tag = 5** | **kind tag = 4** |
| 0x08 | *never written* | creation time, seconds, calling thread's clock |
| 0x14 | `Renderable*` (0x1f0, from `LaserFenceSprite` @ 0x007b9f6c) | 0 |
| 0x18..0x24 | embedded `List` (sentinel 0xc, vtable 0x00663f40), count, cache, cacheValid | same |
| 0x28 | `LightSet*` (0x5c; ambient/emissive from 0x00664430..0x0066443c) | 0 |
| 0x2c | 0 | 0 |
| 0x30 | -> `Vec3 pts[2]` | -> `Vec3 pts[2]` |
| 0x34 | 0 | 0 |
| 0x3c | UV/phase toggle, **read and flipped by the renderer, never initialised** | - |
| 0x40 | *never written* | amplitude |
| 0x4c..0x58 | Vec4 {1,1,1,1} from the ctor | same |
| 0x5c / 0x74 | 2 / 2 | same |
| 0x64..0x70 | four 1.0f | same |

The point block is a separate `malloc(0x1c)` laid out `{int n = 2; Vec3 pts[2]}`, and **`+0x30`
points 4 bytes in**, straight at `pts[0]`; `pts[1]` is at `+0x0c`. That matches
`save_system_notes.md:370-371`'s "`{f32[3] pointA; f32[3] pointB}`" for kind 5 exactly.

Insertion is `WorldEffectList_Insert` @ 0x0052b100, `__fastcall(node /*ECX*/, List* /*EDX*/)`,
into `WorldEffectList` @ 0x007b9ebc.

### 4.5 `DEACTIVATE ELECTRICITY` does filter on the kind tag

`RemoveWorldEffectNearPoint` @ 0x0051a2b0, `__fastcall(Vec3f* /*ECX*/)`, bare `RET`. It scans
`LightEffectList` @ 0x007b9ecc **first**, then `WorldEffectList` @ 0x007b9ebc, and only considers
nodes whose `+0x00` is **3, 4 or 5**; every other kind is left alone. The pulse ring is kind
**0xf**, not a low kind: `RemovePulseRingsNearPoint` @ 0x0051b000, the sole callee of the
`REMOVE PULSE RINGS` console command, tests `data->+0x00 == 0xf`.

For each candidate it tests **both** endpoints of the `+0x30` block against the point with a
squared-distance epsilon `WorldEffectMatchEpsilonSq` @ **0x00667be8 = 0.00390625** = `(1/16)^2`
- i.e. an essentially exact position match, which is fine because both ends came from the same
dummy object. Kinds 4 and 5 read the endpoints at `pts[0]` / `pts[1]`; kind 3 reads them 0x48
bytes apart, so kind-3 nodes carry a longer point array.

**First match wins**: the node is unlinked, `free_sized(node, 0x80)`, and the function returns.
A second fence sharing an endpoint survives. Kind 4 takes a different removal helper
(`List_EraseAtCursor` @ 0x0052b4a0, the generic `List<T>` erase-at-iterator, nothing
effect-specific) from kinds 3 and 5.

### 4.6 Verdict: decoration, not damage and not a blocker

The **only** consumer of a kind-5 node is `DrawWorldEffects` @ 0x005201c0 (reached from
`ScenePass_WorldEffects` @ 0x00552000 for `WorldEffectList` and from `RunInGameFrame` @ 0x0046e88d
for `LightEffectList`). Its `case 5:` arm (0x00520486) does a visibility test on both endpoints
(`FogOfWar_SampleTotal` @ 0x00468770 `< 0xfe`), builds a 0x48-byte render-state block, flips `node+0x3c`, and issues one
`RenderQueue_Submit(..., Mat_Translucent, ...)`. Nothing else.

Full reader inventory for `WorldEffectList` @ 0x007b9ebc: `CreateElectricity`,
`CreateLaserFence`, the nine other `Create*` effect functions, `CommandRemoveLightCylinder`,
`RemoveWorldEffectNearPoint`, `RemovePulseRingsNearPoint` @ 0x0051b000,
`ClearAllWorldEffects` @ 0x00523ae0 (which drains all three lists, not one),
`SaveGame`, `ScenePass_WorldEffects`. **No damage function, no collision function, no physics
query.**

So the gameplay behaviour of a fence is entirely in the level's data: a `Rol_Technobox`
(`ai blocker`, `armour 0`, `reflective yes`, `destructibility Des_Explode`) supplies the actual
obstruction, the `vulnerability ... elint ... script` line supplies the disarm, and the script it
runs both `OPEN DOOR`s and `DEACTIVATE ELECTRICITY`s. The fence graphic and the blocker are
unrelated objects that happen to be authored in the same place. This is also consistent with
`console_command_notes.md` §4.1 not listing any of the three commands as broadcasters - the
graphic is created locally on every machine by every machine's own copy of the `.gcs`.

---

## 5. The interface beam

### 5.1 `CommandVulnerability` @ 0x0044a600

Parse order, left to right:

```
VULNERABILITY <target-role | actor-id> <attacker-role> <delay:int> <effect> [<extra>] <weapon-role>
```

- target: `ConsoleParseWordKeep` + `GetRoleByName`; on failure `ConsoleParseInt` as an actor id
  (negative -> `GL_ERROR_NEGATIVE_ACTOR_NUMBER`). Note the role branch consumes **one extra
  word** before rejoining, at 0x0044a6f7.
- attacker role -> `Vulnerability::entity` (+0x00). Compared against `GetRoleByName("elint")` to
  latch a flag.
- **delay** -> `+0x08`, `ConsoleParseInt(-1)`, and it is **required**: negative prints
  `"No vulnerability delay specified."` and aborts.
- effect keyword, matched with `__mbsicmp`: `SHUTDOWN`, `DESTROY`, `CONFUSE <duration>`,
  `CHARM <duration>`, **anything else** -> `SCRIPT`, whose next word is the `.gcs`, `strdup`'d
  through `malloc` + an inlined `strcpy`. `duration` -> `+0x0c`, default -1.
- weapon role -> `Vulnerability::vuln_entity` (+0x04), compared against
  `GetRoleByName("interface_beam")`.

`Vulnerability` is 0x1c: `{entity 0x00, vuln_entity 0x04, delay 0x08, duration 0x0c, script 0x10,
type 0x14, actor_scoped 0x18}`. `VulnerabilityType` is a dense 0..4 (jump table 0x005409c4) and
`SCRIPT` is **4**, matching `interface beam effect 4` in GLS.

**Units are seconds.** At 0x0053f74c the consumer does
`float now (a seconds clock) + (float)vuln->duration` and hands the result to vtable slot 80, so
`duration` is integer seconds. `delay` is the same shape and the GLS field documents itself
(`interface beam delay 2 // How long it takes Elint to deactivate it`) - **inferred**, not
measured.

### 5.2 `AddInterfaceBeamVulnerability` @ 0x00510fe0

`__fastcall(Actor* /*ECX*/)`, called from `SpawnRole` and `ServerSpawnActorForTeam`. Skips when
`role->interface_beam_delay < 0`, or when the actor already carries an
(`elint`, `interface_beam`) pair. Otherwise `pool_alloc(0x1c)` and:

```
entity       <- GetRoleByName("elint")
vuln_entity  <- GetRoleByName("interface_beam")
delay        <- role->interface_beam_delay      (+0x80)
duration     <- role->interface_beam_duration   (+0x8c)
script       <- role->interface_beam_script     (+0x88)   ** by pointer **
type         <- role->interface_beam_effect     (+0x84)
actor_scoped <- 1
```

then appends a 0x10-byte node (vtable 0x00652070) to `actor->vulnerabilities` and invalidates the
cache.

### 5.3 The completion function - and the ownership contradiction, resolved

The completion is inside `CharacterActor::Update` @ 0x0053d8d0, in the
`type == SCRIPT` arm, at **0x0053f892-0x0053f8c5**:

```
vuln = node->data ([ESI+0xc])
if (vuln->script != 0) {
    SetCurrentDirectoryToGLDir(0)    ; lock
    QueueScriptExecution(vuln->script)
    free(vuln->script)              ; 0x005e3f7b, the pool free thunk
    vuln->script = 0
    SetCurrentDirectoryToGameRoot()  ; unlock
}
```

So `threading_model_notes.md` is right - the string is freed right after queueing - and
`src/Roles.h`'s "leaked" is right about `RoleDtor`, which touches neither `+0x88` nor `+0x8c`.
Both cannot be reconciled, and that is the point:

> **`AddInterfaceBeamVulnerability` copies `Role::interface_beam_script` by pointer, and the
> completion frees it while `Role::interface_beam_script` still points at it.** After the first
> hack of a role with `interface beam effect script`, the `Role` field dangles. A second actor of
> that role spawned afterwards copies the dangling pointer and, on its own completion, frees it
> again - a double free.

It is dormant in normal play because a level spawns its actors once and the hack comes later; it
is reachable through multiplayer respawn and through anything that re-spawns a role mid-level. The
`CommandVulnerability` path is unaffected: it `strdup`s its own copy.

**This matters to GkPlus directly.** `ScriptQueueSystem` hooks `ToRole` to wrap
`Role::interface_beam_script` in a JSON envelope; that wrapped allocation is the one that gets
freed here, and the `Role` field is what dangles.

---

## 6. Flares

`CommandFlareFirer` @ 0x004492e0 is four lines: `IsExecutorRunning` -> `SuspendExecutor` ->
`ConsoleParseActorName` -> `OR [actor+0x7c], 0x40` -> `ResumeExecutor`. `CommandHunter`
@ 0x004492b0 is the same with `0x20`. Neither broadcasts, so neither replicates -
`units.make_flare_firer` and `units.make_hunter` are single-player-only in effect.

**The flare-firing test is a different bit.** `CharacterActor::Update`
@ 0x0053e645:

```
TEST byte ptr [EDI + 0x7c], 0x80
JZ   keep_the_weapon's_own_projectile
ECX = "flare"; GetRoleByName        ; swap the projectile role to `flare`
```

The default projectile role is `weapon_object->[0]` (`CharacterActor+0x2b8` deref). A **whole-
binary scan finds exactly two instructions touching `Actor+0x7c` with 0x40 or 0x80**: the `OR
...,0x40` in `CommandFlareFirer` and this `TEST ...,0x80`. There is **no reader of 0x40 and no
writer of 0x80 anywhere in `gl.exe`.**

So, on the evidence: **`FLARE FIRER` sets a bit nothing reads**, and the bit that actually swaps
the projectile to a flare is never set by the shipped binary. Whether flares "require a plasma
weapon" is therefore not a live gate at all - see §9 for the one adjacent test that is
(`weapon_object->+8 == 0xd` at 0x0053e667, i.e. weapon type 13 `repair arm`, which selects a
different branch entirely).

`flare_light` @ 0x006695a8 and `flare` @ 0x0066958c are also read by
`ProjectileActor::Update` @ 0x00544460 (0x005453f9 / 0x005454e8), which is where
an in-flight flare gets its light - not traced further.

---

## 7. Update ids touched by this subsystem

| id | size | reliable | source |
|----|------|----------|--------|
| 0x3e | 0xc | no | `Mine_OnDeployed`, ids 1 and 3 |
| 0x4c / 0x4e | 9 | no | `MobileActor::ToggleCrouchAndCamouflage` (the crouch toggle, not mine detection) |
| 0x4f | 0x19 | no | mine-lay animation start |
| 0x53 | 0x10 | no | mine-lay state 0 -> 1 |
| 0x7e | 0xc | yes | inventory item exhausted and removed |
| 0x7f | 0x10 | yes | inventory item count decremented |
| 0x8e | 8 | no | mine laid, remote/decoy only, payload 0x2f3a |
| 0x9f | 8 | yes | decoy tick |
| 0xb9 | 0xc | yes | `Actor_SetHeapDropRole` |
| 0x7b / 0x7c | 0xc | yes | item activated (`say` selects which) |

None of the laser-fence or electricity commands broadcasts, confirming
`console_command_notes.md` §4.1.

---

## 8. Address index

| address | name | note |
|---|---|---|
| 0x00445210 | `CommandHeap` | bare `RET` |
| 0x004455a0 | `CommandRespawnHeap` | bare `RET` |
| 0x00447490 | `CommandLaserFence` | bare `RET` |
| 0x004475a0 | `CommandElectricity` | bare `RET` |
| 0x004476d0 | `CommandDeactivateElectricity` | bare `RET` |
| 0x004492b0 | `CommandHunter` | sets `Actor::flags` 0x20 |
| 0x004492e0 | `CommandFlareFirer` | sets `Actor::flags` 0x40 |
| 0x0044a600 | `CommandVulnerability` | |
| 0x0044ece0 | `ConsoleParsePosition` | numeric triple **or** `MapAuxObjectList` name via `lstrcmpiA` @ 0x0044ee4f |
| 0x0044d4d0 | `List<T>::operator[]` (unnamed) | builds the cached array |
| 0x004507b0 | `MineDetonate` | `__thiscall`; `ApplyDamage` slot 68 @ 0x00450c7e |
| 0x00450550 | AI-behaviour installer (unnamed) | sets `+0x34` think and, for Mine only, `+0x38` |
| 0x004552a0 | `AiThink_Mine` | 1 Hz; switch on `character->weapon` |
| 0x0045a640 | `Mine_OnDeployed` | timed fuse = 10 s |
| 0x004ae340 | `GetPickupType` | `round(aggression*10)` |
| 0x004e4790 | `Inventory_AddItemFromRole` | `RET 0x10`; item is 0x38 |
| 0x004e5290 | inventory remove (unnamed) | |
| 0x00510fe0 | `AddInterfaceBeamVulnerability` | |
| 0x00510760 | `CreateActor` | `ai mine` -> `MobileActor` 0x230 |
| 0x0051a150 | `CreateElectricity` | `RET 0x4`; kind 4 |
| 0x0051a2b0 | `RemoveWorldEffectNearPoint` | kinds 3/4/5, eps 1/256 |
| 0x0051c0f0 | `CreateLaserFence` | bare `RET`; kind 5 |
| 0x005201c0 | `DrawWorldEffects` | the only kind-5 consumer |
| 0x0052abe0 | `WorldEffect_Ctor` | 0x80-byte node |
| 0x0052b100 | `WorldEffectList_Insert` | `__fastcall(node, List*)` |
| 0x0052f2d0 | `Actor_SetHeapDropRole` | `Actor+0xc4`, update 0xb9 |
| 0x0052f0d0 | `Actor::Delete` | spawns `+0xc4` at 0x0052f1bc |
| 0x00536830 | `MobileActor::EquipToFirstOpenSlot` | 8 body slots, indices 2..9 |
| 0x005370d0 | `MobileActor::UseInventoryItem` | case 5 = mine deploy @ 0x0053730e |
| 0x0053d8d0 | `CharacterActor::Update` (slot 70) | interface-beam completion @ 0x0053f892; flare swap @ 0x0053e645 |
| 0x00542ae0 | `ProjectileActor::OnPrePhysics` | decoy spawn @ 0x005435aa |
| 0x00509050 | `ExecutorThreadProc` | remote detonate @ 0x00509cca, detonate-all @ 0x00509d2d |
| 0x006672c4 | `BodySlotHotspotNames` | `slota`..`sloth`, stride 7 |
| 0x00667be8 | `WorldEffectMatchEpsilonSq` | 0.00390625 |
| 0x007b9ebc / 0x007b9ecc | `WorldEffectList` / `LightEffectList` | |

---

## 9. What is still unknown

- **`Character::mine_laying_time` (+0x18) has no reader.** A whole-binary scan for the field
  produced writers only; the lay duration used at runtime is the deadline in `MobileActor+0x130`,
  computed at 0x0053542c from the actor's own animation data (`weapon_object+0x18`), not from
  this field. It looks vestigial. Settling it: breakpoint-free check would be to set it to an
  absurd value in a `.gsh` and time the lay animation.
- **The `0x8e` payload constant `0x2f3a`** (mine laid, remote/decoy only) was not identified -
  resource id or sound id.
- **`Actor::flags` bit 0x80 has no writer** and bit 0x40 has no reader (§6). Either the flare
  path is dead code in the shipped build, or one of the two bits is written through a
  computed/OR'd mask this scan's literal-operand pattern misses.
- **The pickup branch of `HEAP` does not obviously replicate.** It overwrites `actor->entity` and
  calls `vtbl+0x148`; whether that slot broadcasts was not checked. The drop branch definitely
  does (0xb9).
- **The scrap-pile cursor hit test and shape selection** were not traced (§3.5) - they are the
  generic pickup path, not scavenge-specific.
- **"Junkpile Empty" (7343)** is a `glres<lang>.dll` string; the exact field carrying it
  (`role->pickup_name_id`, GLS 0x1b) was not followed to its `GetResourceString` call.
- **`Vulnerability::delay` units** are inferred from the GLS field's own comment plus the
  measured seconds-shape of `duration`; the instruction that consumes `delay` was not located.
- **`destroy after collection` (`Role::flags` bit 0x20)** - no reader was searched for; it stays
  unmeasured, as `gls_system_notes.md` already says.
- **`DrawWorldEffects` kind 3** reads its endpoints 0x48 bytes apart in the `+0x30` block, so a
  kind-3 node's point array is longer than two `Vec3`s. Its creator was not identified.
