# Concealment and Fog of War

Two unrelated subsystems that the manual describes in one breath. They share nothing:
concealment is executor-side actor state replicated to the client for rendering; fog of war
is a **client-only** greyscale grid that the executor never reads.

Everything below is measured in the Ghidra DB unless a claim is explicitly marked *inferred*
or *not established*.

---

## 1. Crouch and concealment are two separate flags

| Where | Offset | Slot | Name |
|---|---|---|---|
| `MobileActor` (executor) | `+0x187` | 63 getter `MobileActor_IsCrouched` @ 0x0054f160 | `is_crouched` |
| `MobileActor` (executor) | `+0x186` | 8 getter `MobileActor_IsConcealed` @ 0x0054f180, 9 setter `MobileActor_SetConcealed` @ 0x0054e8a0 | `is_concealed` |
| `Unit` (client) | `+0x19f` | 92 `Unit_IsCrouched` @ 0x004cfe50 | crouched |
| `Unit` (client) | `+0x19e` | 8 `Unit_IsConcealed` @ 0x004cfe70 | concealed |

**Both slots are overridden only by `MobileActor` and its descendants** — `Actor`'s bases
(0x0054f150 slot 63, 0x0054f170 slot 8) return false and `PickupActor` does *not* override
either. That settles the `src/Actors.h` contradiction: `+0x187` is the crouch flag, and the
old names `can_be_picked_up` / `is_mine` / `CanBePickedUp` / `IsMine` were both wrong.
Read the vtable slot 63 column: only `MobileActor`, `CharacterActor`, `NodeActor`,
`TurretActor`, `PresidentActor` carry 0x0054f160; `PickupActor`, `ProjectileActor`,
`BlockerActor`, `TrackObjectActor` all carry the base.

Corroboration from a shipped name: `WaitCond_ActorIsHidden` @ 0x005703c0 (a `WAIT FOR`
condition, reached only through the function-pointer table entry at 0x0066a380) is exactly

```
IsMobile() && Unit::IsCrouched() && Unit::IsConcealed()
```

i.e. the engine's own definition of "hidden" is crouched **and** concealed.

### 1.1 The toggle

`MobileActor::ToggleCrouchAndCamouflage` @ **0x00536090**, MobileActor vtable **slot 83**,
`__thiscall void(MobileActor *this)`, plain `RET` (no stack args).
Previously named `UpdateMineDetectionAndBounds`; renamed.

1. Early-out unless `HierarchyHasNode(this->entity->hierarchy, 0x13)` — a model with no
   crouch node cannot crouch.
2. `is_crouched = !is_crouched` (`MOV CL,[EDI+0x187]` / `SETZ AL` / `MOV [EDI+0x187],AL`
   at 0x00536105).
3. Entering the crouch → re-evaluate `is_concealed` (§2). Standing up → clear
   `is_concealed`, but **only if `MobileActor+0x150 == -1`** (0x005362d3).
4. Collision box: crouched sets `anim_object[0x1c4] = 1` and halves the standing box
   (`(hi - lo)/2` on the Y extent), preferring hierarchy node `0x66` when the model has one;
   standing restores the full box from `character->height_times_size` or the model bounds.
5. Broadcasts 9 bytes, unreliable: `{id = 0x4c + 2*(!is_crouched), actor_id, is_concealed}` —
   so **0x4c = now crouched, 0x4e = now standing**, and the concealed flag rides along.

Client side, updates 0x4c / 0x4d / 0x4e all land in `Unit_SetCrouchedAndConcealed`
@ **0x004bc7f0** (Unit vtable slot 93, `__thiscall(bool crouched, bool concealed, bool
instant)`, `RET 0xc`), which stores `+0x19f` / `+0x19e` and swaps the model bounds the same
way. **0x4d** is the "instant" variant, emitted by `FUN_005317b0` (called only from
`LoadGame` @ 0x005069ea) to resync crouch state after a savegame load.

### 1.2 Who calls the toggle

- `ExecutorThreadProc` @ 0x00509428ff — the move-order path. `if (!IsAutoCrouchOn
  (0x006abe20) && actor->IsCrouched()) return;` otherwise `CALL [vtbl+0x14c]` (slot 83) to
  stand up first. This is what the Prefs "Auto Crouch" toggle does.
- `ExecutorThreadProc` @ 0x0050a074, 0x0050b29b (other order kinds).
- `SyncPositionAndBroadcast` @ 0x00535167 / 0x00535176, `ReceiveObject` @ 0x0053937b.
- HUD/order-menu callers `FUN_004a17e0` / `FUN_004a17b0` / `FUN_004a4250`.

---

## 2. The camouflage eligibility test

Evaluated **only at the instant of entering the crouch**, never continuously. Three gates,
in order:

### 2.1 Precondition — nobody is looking

`Actor_IsUnseenByEnemies` @ **0x0052f4c0**, `__fastcall bool(Actor *this)` (ECX only, bare
`RET`). Renamed from `FUN_0052f4c0`; its only caller is the toggle.

For every team slot `t != this->team_id` whose `TeamSlots[t] + 0x6b` is 0, for every actor
in that team's list that is alive (slot 6) and mobile (slot 36):

```
d  = this->position(+0xa0) - enemy->position(+0xa0)
|d|^2 < enemy->sight_range(+0x168)^2          // in range
|d|^2 > 4.0                                    // and NOT closer than 2 m
cos(enemy->sight_angle(+0x174) as BAM) < dot2D(d.xz, enemyForward.xz) / (|d.xz| |fwd.xz|)
```

If any enemy passes, the function returns 0 (`XOR AL,AL` @ 0x0052f88e); the loop running to
completion returns 1 (`MOV AL,1` @ 0x0052f82b). The `|d|^2 > 4.0` clause is not a typo — an
enemy standing within 2 m is treated as *not* seeing you, so hiding at point-blank range is
allowed.

If this returns 0, control falls into the "standing up" arm and concealment is *cleared*
(subject to the `+0x150 == -1` test), so crouching in plain sight actively un-hides you.

### 2.2 Cover test — an indestructible pickup nearby (the scrap pile)

Walk the `actors` hash (0x007ba0d8). A candidate qualifies when

```
candidate->IsPickup()                    // Actor vtable slot 45
candidate->is_destructible (+0x12c) == 0 // PickupActor field
dist2(this, candidate) < 2.0 * candidate->Actor::field0x110 ^ 2
```

(`FLOAT_006520a8 == 2.0`, so the radius is `sqrt(2) * field0x110`.) First match sets
`is_concealed = 1` and breaks. See the pre-comment now at 0x00536160.

`Actor::field0x110` is a `float` on the base `Actor`; **what writes it is not established**
here, so the absolute metre value of the scrap-pile radius is unknown — it is per-actor, not
a constant.

### 2.3 Terrain test — nav-polygon flag 0x800 (the water)

Fallback, only if the cover test found nothing (0x005362a8-0x005362cb):

```
poly = this->GetField0x118()   // Actor vtable slot 12, the nav polygon cached by slot 51
if (poly) is_concealed = (poly->flags(+0x14) >> 11) & 1
```

`level_loading_notes.md` §5.5: `NavPolygon+0x14` is `SHPPOLYS.flags & 0x3fffc1` plus 0x100
for a too-steep face. **Bit 0x800 survives that mask, so it is authored per polygon in the
level geometry**, exactly like the 0x100 blocker bit. A full scan of `.text` for
`TEST/AND …,0x800` and `SHR …,0xb` finds **this as the only reader of bit 0x800 anywhere in
the binary** (every other hit is C++ EH-state bookkeeping).

So "deeper water" is not a depth computation and there is no water volume test: it is a
per-triangle authoring flag. "Move closer to the pile or into deeper water" reduces to
"stand on a polygon carrying flag 0x800, or within `sqrt(2)*r` of an indestructible pickup,
and do it while no enemy has you in its cone".

### 2.4 What else clears concealment

`is_concealed` is set at exactly two sites and cleared at seven:

| Site | Effect |
|---|---|
| `ToggleCrouchAndCamouflage` 0x005362a1 / 0x005362cb | set (cover / terrain) |
| `EquipObject?` 0x005376cf | set |
| `ToggleCrouchAndCamouflage` 0x005362dc | clear on standing up (if `+0x150 == -1`) |
| `AttackPosition` 0x00540b20, `AttackTarget` 0x00540df2 | clear — **firing breaks camouflage** |
| `Update` (MobileActor) 0x00535a22 | clear |
| `Dissociate` 0x00535f8f | clear |
| `Die` 0x0053a126 | clear |
| `ActivateInWorld` 0x0053bc10 | clear |

On the client, `Unit+0x19e` is cleared by `Unit::SetTarget`-family
`FUN_004c3170` @ 0x004c324c and `FUN_004c32a0` @ 0x004c3405, and by
`FUN_004bb760` / `FUN_004bc740` / `FUN_004c0f80`.

---

## 3. How concealment enters detection: a hard skip, not a modifier

Every AI target-acquisition loop in the binary reads slot 8 on the **candidate** and drops
it outright. The call is compiled as `MOV EAX,[reg+0x20]` + `CALL EAX`, not
`CALL dword ptr [reg+0x20]`, which is why a naive scan for the latter finds nothing.

| Site | Enclosing function | Shape |
|---|---|---|
| 0x00451a24 | `AiThink_Bot` @ 0x00451220 | `if (!alive) skip; if (IsConcealed()) skip` |
| 0x00456514, 0x00456804 | `FUN_00455de0` (turret AI, installed by TurretActor slot 70 @ 0x0054b0b6) | same |
| 0x00459296 | `FUN_004591e0` | same |
| **0x00459532** | `CollectDetectableEnemies` @ 0x00459440 | same |
| 0x0045aa1f | `FUN_0045a850` (Node AI) | same |
| 0x0053e3b2 | `CharacterActor` slot 70 @ 0x0053d8d0 | same |
| 0x0054a9df | `PopupActor` slot 70 @ 0x0054a8f0 | same |
| 0x0054c6bf | `TurretActor` slot 70 @ 0x0054b000 | same |

Because the candidate is removed from the list *before* any range, cone, hearing or
line-of-sight work, concealment defeats **both** the sight and the hearing channel, and it
is binary — there is no probability and no range attenuation. `hearing range` /
`alert radius` are simply never reached for a concealed actor.

**For reconciliation with the perception agent**: the sweep scanner is
`AdvanceScanArcAndFilterCandidates` @ **0x00459930**, called only from
`CollectDetectableEnemies` @ **0x00459440**, which is the function that performs the
concealment skip at 0x00459532. So the "scanner" path is *inside* the concealment filter,
not outside it. `AiThink_Bot` @ 0x00451220 is the other entry point. AI-think-proc
assignment table is `Actor_SetAiBehaviour` @ 0x00450550.

`FUN_0045e220` @ 0x0045e220 is a second "is any enemy within sight range" helper that also
consults slot 8 — it has **zero references, code or data**, i.e. dead code.

---

## 4. The bypass ("alternative scanning you cannot hide from") — NOT ESTABLISHED

No perception path was found that ignores `is_concealed`. What was checked and ruled out:

- All eight consult sites above (every AI think proc that acquires targets at all).
- The AI-think dispatch `Actor_SetAiBehaviour` @ 0x00450550 assigns only: `AiThink_Bot`
  (bot/centipede/president, and only when the actor has a weapon), `FUN_004556e0`
  (scavenger), `FUN_004552a0` (mine), `AiThink_Minebot` @ 0x00456c50, `FUN_00456bb0`
  (waiting/centibody/nodewaiting/popup/turret — a 149-byte timer that only fires Actor
  slots 79 and 81, no perception), `LAB_004556d0` (pathfinder), `FUN_0045a850` (node),
  `FUN_0045b620` (swarm), and nothing for blocker/pickup/tumbleweed/background creature.
  The three that never touch slot 8 (`FUN_004556e0`, `AiThink_Minebot`, `FUN_0045b620`)
  were not read in full, so a concealment-ignoring acquisition inside one of them cannot be
  excluded — that is the place to look next.
- Two `Actor::flags (+0x7c)` bits set by console commands look like AI-capability flags and
  are the other plausible home for it: **0x20** set by `CommandHunter` @ 0x004492d1, and
  **0x40** set by `CommandFlareFirer` @ 0x00449301. Neither has a reader that this pass
  located (both are read via `MOV reg,[x+0x7c]` followed by a test outside a 5-instruction
  window). Bit **0x80** on the same word is the "spawn a flare now" request, consumed at
  0x0053e645 in `CharacterActor` slot 70, which then `GetRoleByName("flare")`; it is
  cleared in `AiThink_Bot` (0x00454a42, 0x00455032) and `AiThink_Minebot` (0x00457203).

The recon-mode strings and the `character` GLS fields `scan delay` (0x0e), `scan acceptance
angle` (0x0f) and `angular scan rate` (0x10) all belong to the sweep scanner, which — as
shown above — **does** respect concealment. So the manual's claim has no counterpart found
in the perception layer as it stands.

---

## 5. The renderer: how a camouflaged unit is drawn

`Unit_Draw` @ **0x004b6ae0**, branch at **0x004b708d** (pre-comment added). Gated on
`Unit::IsConcealed()` and `Unit+0x127 == 0`.

A concealed unit is submitted through `RenderQueue_Submit` with material
**`Mat_Translucent`** and an explicit alpha object as the 9th argument — a 0xc-byte
`pool_alloc` `{vptr = 0x00664024, refcount = 1, float alpha}`:

- **Own team** (`Unit+0xb4 == DAT_006a58e0`), or a Cooperative ally
  (`TeamSlots[team]+0x6a != 0`): **alpha = 0.7** (`0x3f333333`). That is the manual's
  "turns darker" — a constant 70% translucency, not a colour change.
- **Any other team**: find the minimum squared distance from this unit to any entry of
  `ObjectList` @ 0x007b6928 (the player's own named characters), with the running minimum
  seeded at the threshold `T`:
  - `T = 49.0` (7 m) in `SinglePlayer` and `Cooperative`;
  - `T = 196.0` (14 m) in every other game mode.
  - Nothing inside `T` → **alpha = 0.0**, plus `submitFlags |= 0x1000000`: the unit is not
    visible at all.
  - Something inside `T` → `alpha = (1.0 - dmin2 / T) * n`, where `n` is a
    `[0,1)` value drawn from the calling thread's PRNG **twice per frame** — so a
    partially-revealed camouflaged enemy shimmers, and gets steadily more solid as one of
    your characters closes in.

There is no separate "dark" material and no tint colour: the whole effect is the alpha term
on the translucent submit.

---

## 6. The fog-of-war data structure

One object, `FogSystem` @ **0x006b0144** (`FogOfWar *`, name already in the DB). `pool_alloc`
of **0xf4 bytes** in `LoadLevel` @ 0x004e0dd6, constructed by `FogOfWar_Ctor`
@ **0x00467820** (`__thiscall`, `RET 4`) with `gridDim = 0x100`; cleared to null by
`FUN_004e2090` @ 0x004e214a on level teardown. It is **client-only** — nothing on the
executor side reads it.

| Offset | Type | Meaning |
|---|---|---|
| 0x00..0x0c | `float[4]` | world bounds `{minX, minZ, maxX, maxZ}`, squared up by `FogOfWar_SetWorldBounds` @ 0x00468e10 |
| 0x10 | `float` | `1 / extent` |
| 0x14 | `float` | `gridDim / extent` (world units -> grid cells) |
| 0x18 | `int` | enabled level: 0 = off, 1 or 2 (3 on Voodoo 2/3). `FogOfWar_SetEnabledLevel` @ 0x004697d0. **0 makes every sample return 0** |
| 0x1c | `byte *` | **explored** grid, `gridDim^2`. Persistent minimum; this is the one that is saved |
| 0x20 | `byte *` | **current** grid, `gridDim^2`. Rebuilt every frame |
| 0x24 | `byte *` | **static layer** snapshot, `gridDim^2` — the defog areas only |
| 0x28..0x5b | | fog system texture record ("Fog System Texture" name at +0x54) |
| 0x5c..0x8f | | fog video texture record ("Fog Video Texture" name at +0x88) |
| 0x90,0x94,0x98,0x9c | `int` | dirty rect `x0, y0, x1, y1` (also used as a `RECT` for `CopyRects`) |
| 0xa0 | `int` | `gridDim`, clamped to `DAT_006ab970` |
| 0xa4 | `float` | `FOGVALUE` — fog level in discovered areas, 0..1 |
| 0xa8 | `float` | `FOGUPDATE` — complete updates per second |
| 0xac / 0xb0 | `float` | `FOGTRANSITION` metres / its reciprocal |
| 0xb4 | `unsigned` | fog colour packed into the surface format |
| 0xb8..0xc4 | `float[4]` | `FOGCOLOUR` RGBA (also drives `D3DRS_TEXTUREFACTOR` and `ClearColour`) |
| 0xc8 | ptr | chosen surface-format record (bit shifts used by the texture expanders) |
| 0xcc..0xd8 | `List` | **defog areas** (sentinel ptr, count, cache, cache_valid) |
| 0xdc..0xe8 | `List` | **defogger units** |
| 0xec / 0xf0 | `AwMaterial *` | fog material / fog video material |

**Cell encoding: 0 = fully revealed, 0x7f (127) = fully fogged.** `FogOfWar_SampleTotal`
returns `current[i] + explored[i]`, so the sampled range is 0..254 and `Unit_Draw` divides
by `254.0`.

Grid resolution is **256 x 256 cells** over the level's square bounds — cell size is
`extent / 256` world units, so it varies per level.

### 6.1 Per-frame pipeline

`RunInGameFrame` @ 0x0046e7cb calls `FogOfWar_MarkAllDirtyAndUpdate` @ 0x00468d90 (full
rect) when `DAT_00738ff4` is set, otherwise `FogOfWar_Update` @ 0x004688f0 directly.
`FogOfWar_Update` does, in order:

1. `FogOfWar_StampDefoggerUnits` @ 0x00468b60 — `current = staticLayer`, then one
   `StampCircle` per defogger unit.
2. `FogOfWar_AccumulateExplored` @ 0x00468ad0 — `explored[i] = min(explored[i],
   current[i])` over the dirty rect. This is the whole persistence mechanism.
3. `FogOfWar_UploadTexture` @ 0x00467080 — expands `explored + current` into the fog
   texture (`FogOfWar_ExpandRect8bpp/16bpp/32bpp` @ 0x004674b0 / 0x00467330 / 0x00467220)
   and `CopyRects` onto the video texture; then resets the dirty rect to empty.
4. Ages the timed defog areas by the elapsed ticks and drops the expired ones; if any
   expired, `FogOfWar_RebuildStaticLayer` @ 0x00468c80.

`FogOfWar_StampCircle` @ **0x004676b0**, `__thiscall(int cx, int cy, int radiusCells, char
partial)`, `RET 0x10`:

```
value = base + ((dx*dx + dy*dy) * 0x7f) / (r*r)      base = 0x3f if partial else 0
current[y*gridDim + x] = min(current[...], value)
```

so a `partial` stamp can only get a cell down to 0x3f (half-fogged) while a full stamp
reaches 0.

### 6.2 Saved state

`SaveGame` @ 0x00508750 writes `FogSystem+0xa0` (`gridDim`, u32) then `gridDim^2` bytes from
**`+0x1c`, the explored grid** — matching `save_system_notes.md` "Fog of war". `LoadGame`
@ 0x005073ac reads it back only when `gridDim` matches the freshly built level (otherwise it
reads and discards), and resets the dirty rect to the full grid. Current/static layers are
not saved; they are rebuilt from the live defoggers and defog areas.

### 6.3 Fog of war is OFF by default

`FogOfWar_Ctor` writes `+0x18 = 0` at 0x0046784b, and the only writer of `+0x18` is
`FogOfWar_SetEnabledLevel`, whose only caller is `SetIsFogEnabled` @ 0x00472230, whose only
caller is the **`FOG` console command** `CommandFog` @ 0x004e2ec0. So a level has no fog of
war unless its `.gcs` (or a script) turns it on.

---

## 7. The reveal paths

### 7.1 Defogger units — the continuous reveal

A unit on the list at `FogSystem+0xdc` gets a `StampCircle` every frame at

- centre = `unit->position` (`Unit+0x98` x, `+0xa0` z) mapped through `+0x10`/`+0x14`;
- radius = `Unit` vtable **slot 66** `Unit_GetDefogRadius` @ 0x004b6900 —
  `role(+0xb8)->character(+0x60)->sight_range(+0x28)`, or **53.0f** when the role has no
  character. Override @ 0x004cf900 returns `Unit+0x180`;
- `partial` = `Unit` vtable **slot 38** `Unit_GetFogStampIsPartial` @ 0x004cf510 (base 0,
  one class returns 1 @ 0x004cf520).

So **a unit reveals fog out to its own GLS `sight range`**.

Membership is `FogOfWar_AddDefoggerUnit` @ **0x004693c0** / `FogOfWar_RemoveDefoggerUnit`
@ **0x00469450** (both `__thiscall(Unit *)`, `RET 4`), which also refcount the unit and set
`Unit+0x211` through slot 27 `Unit_SetIsDefogger` @ 0x004d0000. Added from `LoadGame`,
`FUN_004bb410` (unit construction, `+0x211 = 1` at 0x004bb4ed), `ApplyUpdateMessage`
0x004fecca (spawn path) and case **0xb7**; removed from `FUN_004c0f80`, `FUN_004c4a50` and
case **0xb8**.

Console `DEFOGGER` (`CommandDefogger` @ 0x00442a10) broadcasts **0xb7**, 8 bytes
`{0xb7, actorId}`; `FOGGER` (`CommandFogger` @ 0x00442b10) broadcasts **0xb8**, same shape.
Neither does anything locally — both are pure broadcasts gated on `IsExecutorRunning()` and
`LevelLoadReason != 3`.

### 7.2 Defog areas — the one-shot / timed reveal

Record, 0x18 bytes, pool-allocated:

```
+0x00 Vec3  centre
+0x0c float radius (world units)
+0x10 int   kind      0 = permanent, 1 = timed
+0x14 int   ticks_left
```

`FogOfWar_AddDefogArea` @ **0x004694e0** (`__thiscall(DefogArea *)`, `RET 4`) takes
ownership; if an existing area's centre is within **3.0** world units (`dist2 < 9.0`) the
new record is *merged* into it (larger radius wins, longer life wins, kind 0 sticks) and
freed. Otherwise it is appended to the list at `+0xcc`. Either way it ends with
`FogOfWar_RebuildStaticLayer`. Removal: `FogOfWar_RemoveDefogArea` @ 0x00469700, and the
ageing loop inside `FogOfWar_Update`.

**All defog areas are stamped `partial = 1`**, so a defog area alone only brings a cell down
to 0x3f — half-lit. Full brightness needs a defogger unit.

Every area arrives over the wire, update **0xb6**, 24 bytes:

```
+0x00 u32   0xb6
+0x04 f32   x
+0x08 f32   y
+0x0c f32   z
+0x10 f32   radius
+0x14 u16   kind      (0 permanent / 1 timed)
+0x16 u16   seconds   (multiplied by the thread's ticks-per-second)
```

Four producers:

| Producer | Radius | Kind / life |
|---|---|---|
| `EvaluateTriggers` @ **0x0050e55d** — the **DEFOG trigger** (`TriggerKind::Defog`, 15). Centre and radius come from the trigger record's `data+0x10..0x18` and `data+0x1c`, i.e. `coords[1]` and `coords[2].x` in `trigger_system_notes.md`'s layout | trigger's `defog_radius` | **0, permanent** |
| `FUN_0045e050` @ **0x0045e05a** — AI "go investigate", called twice from `AiThink_Minebot` @ 0x00456f7c / 0x004571b0. Centre is the AI actor's own position | **20.0** | 1, **3 s** |
| `ApplyUpdateMessage` **case 0x46** (spawn projectile / weapon fire) @ 0x004ff... — built inline when `msg[4] != DAT_006a58e0`, i.e. when the shooter is *not* on the local player's team. Centre is the fire position `msg[6..8]` | **25.0** | 1, **60 s** |
| `ApplyUpdateMessage` **case 0x47** — same shape, centre `msg[4..6]` | **20.0** | 1, **60 s** |

**The manual's "firing a flare reveals a zone" is only partly pinned down.** The flare
itself is `Actor::flags (+0x7c)` bit **0x80** ("fire a flare now"), consumed at
0x0053e645 in `CharacterActor` slot 70 @ 0x0053d8d0, which does
`GetRoleByName("flare")` (string @ 0x0066958c; `"flare_light"` @ 0x006695a8 is used at
0x005454e8). `CommandFlareFirer` @ 0x004492e0 sets bit **0x40** ("this actor fires
flares"). What was *not* found is code linking the flare projectile or its light to a
0xb6 broadcast — the reveal that accompanies an investigation is the 20 m / 3 s one from
`FUN_0045e050`, and the two 60 s ones come off weapon fire generally. See "unknown" below.

### 7.3 Console fog commands

| Command | Handler | Effect |
|---|---|---|
| `FOG` | `CommandFog` @ 0x004e2ec0 | `SetIsFogEnabled` -> `FogSystem+0x18` |
| `FOGCOLOUR` | `CommandFocColor` | `FogSystem+0xb8..0xc4`, via `FUN_00469270` |
| `FOGVALUE` | `CommandFogValue` @ 0x004e2f80 | `FogSystem+0xa4`, clamped 0..1 |
| `FOGUPDATE` | `CommandFogUpdate` @ 0x004e30d0 | `FogSystem+0xa8` |
| `FOGTRANSITION` | `CommandFogTransition` @ 0x004e3020 | `FogSystem+0xac` (m) and `+0xb0` (1/m) |
| `DEFOGGER` / `FOGGER` | 0x00442a10 / 0x00442b10 | broadcast 0xb7 / 0xb8 |

---

## 8. What the fog gates

**Rendering and effects only.** `FogOfWar_SampleTotal` @ 0x00468770 has 31 call sites and
every one is a client draw or effect-spawn path: `Unit_Draw` (x4), `DrawWorldEffects` (x6),
`SpawnSparks`, `HudItem_DrawByKind`-adjacent `FUN_004a4130`, the world-effect updaters
`FUN_00513xxx`-`FUN_00515xxx`, `FUN_00488400`, `FUN_00558d30`, and four effect-spawn cases
in `ApplyUpdateMessage`. `FogOfWar_SampleCurrent` @ 0x00468830 has exactly one caller,
`FUN_004b68c0`, which picks between the two based on `Unit+0x118` bits 0x10 / 0x8 and is
itself a draw helper.

The result is written as a 0..1 fade factor into the render node's `+0x38`
(`Unit_Draw` @ 0x004b7d94, 0x004b7fee, 0x004b839c). Nothing on the executor thread — no
targeting, no selection, no AI — reads the fog: the whole object is allocated, updated and
sampled on the client. Selection and targeting of a fogged enemy are therefore **not**
blocked by fog; visibility of an enemy in fog is a rendering consequence only.

The one non-render consumer of the fog *system* is `SubmitAndFlushMapGeometry`, which uses
the fog material (`FogSystem+0xec`) as the map's material when fog is on.

### How it reaches the screen: the world's second texture stage

Measured from the Vulkan capture layer on a paused level02 frame
(`vulkan_renderer_notes.md` §4.51), which is what finally identified this from the
rendering side. The fog material's second stage is the grid:

```
stage 0: the surface's own .RIM     MODULATE(TEXTURE, DIFFUSE)             texcoord 0
stage 1: the 256x256 fog grid       BLENDTEXTUREALPHA(TEXTURE, CURRENT)    texcoord 1
                                    alpha SELECTARG2 - alpha is untouched
```

Three things follow, and each was a standing question in one file or the other:

- **The grid is uploaded as `D3DFMT_A8`** — 256x256, one mip, 65536 bytes, exactly one
  byte per cell, matching the grid's own dimensions. An `A8` texture samples as
  `(0, 0, 0, a)`, so the stage computes `lerp(current, black, a)`: it darkens toward
  black by the cell's value. `world.fog`'s colour reads `{0, 0, 0, 1}`, which is the
  same statement from the other end.
- **It is the texture with no name.** The engine creates it rather than loading a `.RIM`,
  so nothing that names an image by its asset ever sees it — which is why the renderer's
  texture inventory carries exactly one unnamed image.
- **It rides on the world's second UV set**, i.e. FVF `0x252`'s `uv1`. That coordinate is
  *generated* — the `.rif` carries one UV list — and where it is generated is still not
  established (`rendering_notes.md` §5 leaves that geometry builder undissected). A
  world-space planar projection is the obvious shape for addressing a level-wide grid,
  but no uv1 value has been read.

The causal test, rather than the arithmetic: `world.fog.enabled = false` takes a world
draw from two stages to one, and setting it back restores the stage.

**This was mistaken for a baked lightmap for thirty-odd sections of the renderer's notes**,
on the strength of the `.gls` `map` section naming a per-level `bitmaps\<level>.rim`. That
file is the minimap — a 512x512 DXT1 top-down picture of the level, not resident while a
level is up. Gunlok bakes its map lighting into per-vertex colours (`SHPVTINT`) and into
nothing else.

---

## 9. Cross-references to fix elsewhere

- `src/Actors.h` lines 353-354 and 387-389: `is_mine` -> `is_concealed`, `can_be_picked_up`
  -> `is_crouched`, slot 63 `CanBePickedUp` -> `IsCrouched`, slots 8/9 `IsMine`/`SetIsMine`
  -> `IsConcealed`/`SetConcealed`, slot 83 `UpdateMineDetectionAndBounds` ->
  `ToggleCrouchAndCamouflage`. **None of these had a `static_assert` behind them.**
- `actor_vtable_notes.md` line 243 (MobileActor slot 83 row) — the "name is doubtful" note
  can be replaced with the measurement above.
- `Role::limit` (+0x54, GLS field 0x78) was suggested as a possible defogger consumer. It
  is **not**: the defogger radius comes from `Character+0x28` (`sight range`) through
  `Unit_GetDefogRadius`, and no reader of `Role+0x54` was found on any fog path. The
  `// defogger` comment in `plasma.gsh` remains authoring intent with no code behind it.

---

## 10. What is still unknown

- **`Actor::field0x110`** — the radius the scrap-pile proximity test scales by
  (`sqrt(2) * field0x110`). Its writer was not traced, so the camouflage cover range has no
  metre value yet. This is the single most useful next measurement.
- **The bypass.** No perception path ignoring `is_concealed` was found (§4). The three
  unread think procs (`FUN_004556e0` scavenger, `AiThink_Minebot` @ 0x00456c50,
  `FUN_0045b620` swarm) and the readers of `Actor::flags` bits 0x20 (`HUNTER`) and 0x40
  (`FLARE FIRER`) are where to look.
- **The flare's own reveal.** Bit 0x80 -> role `"flare"` is measured; a 0xb6 broadcast tied
  to the flare projectile is not. The 25 m / 60 s reveal in update case 0x46 fires on
  *any* enemy weapon discharge, which may be what the manual is describing loosely.
- **`MobileActor+0x150`**, the `== -1` guard on clearing concealment when standing up.
- **`Unit+0x127`**, the second gate on the concealed draw branch, and `Unit+0x118` bits
  0x8 / 0x10 which choose between the two fog samplers.
- **`DAT_006ab970`** (max grid dimension) and `DAT_00738ff4` (the "full-rect update this
  frame" flag) are zero-init `.data`; their runtime values were not read.
- Which `Unit` subclass returns 1 from `Unit_GetFogStampIsPartial` (slot 38 @ 0x004cf520) —
  i.e. which kind of defogger only half-reveals.
- Whether a defog area's `partial` stamp is intentional or a defect: as measured, a DEFOG
  trigger can never bring a cell below 0x3f on its own.

---

## Appendix: addresses

| Address | Name |
|---|---|
| 0x00536090 | `MobileActor::ToggleCrouchAndCamouflage` (slot 83) |
| 0x0052f4c0 | `Actor_IsUnseenByEnemies` |
| 0x0054f160 / 0x0054f180 / 0x0054e8a0 | `MobileActor_IsCrouched` / `_IsConcealed` / `_SetConcealed` |
| 0x004bc7f0 | `Unit_SetCrouchedAndConcealed` (Unit slot 93) |
| 0x004cfe50 / 0x004cfe70 / 0x004cf5d0 | `Unit_IsCrouched` / `Unit_IsConcealed` / `Unit_SetConcealed` |
| 0x005703c0 | `WaitCond_ActorIsHidden` (ptr at 0x0066a380) |
| 0x004b6ae0 (branch 0x004b708d) | `Unit_Draw`, concealed-alpha path |
| 0x006b0144 | `FogSystem` (`FogOfWar *`, 0xf4 bytes) |
| 0x00467820 / 0x004679d0 | `FogOfWar_Ctor` / `_Dtor` |
| 0x004676b0 | `FogOfWar_StampCircle` |
| 0x00468b60 / 0x00468ad0 / 0x00468c80 | `_StampDefoggerUnits` / `_AccumulateExplored` / `_RebuildStaticLayer` |
| 0x004688f0 / 0x00468d90 | `FogOfWar_Update` / `_MarkAllDirtyAndUpdate` |
| 0x00468770 / 0x00468830 | `FogOfWar_SampleTotal` / `_SampleCurrent` |
| 0x004694e0 / 0x00469700 | `FogOfWar_AddDefogArea` / `_RemoveDefogArea` |
| 0x004693c0 / 0x00469450 | `FogOfWar_AddDefoggerUnit` / `_RemoveDefoggerUnit` |
| 0x00467080 | `FogOfWar_UploadTexture` |
| 0x004697d0 / 0x00472230 | `FogOfWar_SetEnabledLevel` / `SetIsFogEnabled` |
| 0x00442a10 / 0x00442b10 | `CommandDefogger` (0xb7) / `CommandFogger` (0xb8) |
| 0x0050e55d | DEFOG trigger -> update 0xb6 |
| 0x0045e050 | AI investigate -> update 0xb6 (20 m, 3 s) |
| 0x00459440 / 0x00459930 | `CollectDetectableEnemies` / `AdvanceScanArcAndFilterCandidates` (perception agent's) |
| 0x00451220 | `AiThink_Bot` |
| 0x00450550 | `Actor_SetAiBehaviour` (AIType -> think proc) |
