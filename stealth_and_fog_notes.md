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
`__thiscall void(MobileActor *this)`, plain `RET` (no stack args). Nothing in it detects mines.

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
way. **0x4d** is the "instant" variant, emitted by `Actor_FixupAfterLoad` @ 0x005317b0 (the savegame
fixup pass, called only from `LoadGame` @ 0x005069ea) - so 0x4d is what a **restored** actor
publishes to resync its crouch state.

### 1.2 Who calls the toggle

- `ExecutorThreadProc` @ 0x00509428ff — the move-order path. `if (!IsAutoCrouchOn
  (0x006abe20) && actor->IsCrouched()) return;` otherwise `CALL [vtbl+0x14c]` (slot 83) to
  stand up first. This is what the Prefs "Auto Crouch" toggle does.
- `ExecutorThreadProc` @ 0x0050a074, 0x0050b29b (other order kinds).
- `MobileActor::Update` (slot 70) @ 0x00535167 / 0x00535176, `ReceiveObject` @ 0x0053937b.
- HUD/order-menu callers `EnterFlareMode` @ 0x004a17e0 / `ExitFlareMode` @ 0x004a17b0 /
  `OnCommandWheelClick` @ 0x004a4250 (renamed from `CommitPendingOrderTarget`: it is the LMB click
  handler registered for cursor mode `CommandWheel`, and cases 0, 1 and 4 of its state machine are
  cursor-mode changes and cancel rather than order-target commits).

---

## 2. The camouflage eligibility test

Evaluated **only at the instant of entering the crouch**, never continuously. Three gates,
in order:

### 2.1 Precondition — nobody is looking

`Actor_IsUnseenByEnemies` @ **0x0052f4c0**, `__fastcall bool(Actor *this)` (ECX only, bare
`RET`). Its only caller is the toggle.

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
| `MobileActor::UseInventoryItem` 0x005376cf | set - the audio-cloak arm (`pickup_class` 10, `weapon` 5), guarded on `+0x154 == 0 && +0x150 == -1` |
| `ToggleCrouchAndCamouflage` 0x005362dc | clear on standing up (if `+0x150 == -1`) |
| `AttackPosition` 0x00540b20, `AttackTarget` 0x00540df2 | clear — **firing breaks camouflage** |
| `Update` (MobileActor) 0x00535a22 | clear |
| `Dissociate` 0x00535f8f | clear |
| `Die` 0x0053a126 | clear |
| `ActivateInWorld` 0x0053bc10 | clear |

On the client, `Unit+0x19e` is cleared by `Unit_SetAttackPosition` @ 0x004c324c and
`Unit_SetAttackTarget` @ 0x004c3405 (slots 109 and 108), and by `Unit_UpdateMovement`
@ 0x004bb760 (slot 57), `Unit_Dissociate` @ 0x004bc740 (slot 55) and `MobileUnit::LeaveWorld`
@ 0x004c0f80 (slot 91, was `Unit_Slot91`). The Actor-side `Dissociate` @ 0x00535f8f and `Unit_Dissociate` are the same
operation in the two trees, joined by update **0x97**.

---

## 3. How concealment enters detection: a hard skip, not a modifier

Every AI target-acquisition loop that keys on **`sight` range** reads slot 8 on the **candidate**
and drops it outright. The call is compiled as `MOV EAX,[reg+0x20]` + `CALL EAX`, not
`CALL dword ptr [reg+0x20]`, which is why a naive scan for the latter finds nothing.

**That qualifier is load-bearing and was added after the fact.** This section used to say *every*
acquisition loop, without restriction, and that is false: the two **hearing-range** consumers do not
test slot 8 at all, and neither does `AiThink_Swarm`. See §4, which now resolves rather than defers
the question.

| Site | Enclosing function | Shape |
|---|---|---|
| 0x00451a24 | `AiThink_Bot` @ 0x00451220 | `if (!alive) skip; if (IsConcealed()) skip` |
| 0x00456514, 0x00456804 | `AiThink_Turret` @ 0x00455de0 (installed by `TurretActor::Update` @ 0x0054b0b6, not by `Actor_SetAiBehaviour`) | same |
| 0x00459296 | `FindNearestVisibleEnemy` @ 0x004591e0 | same |
| **0x00459532** | `CollectDetectableEnemies` @ 0x00459440 | same |
| 0x0045aa1f | `AiThink_Node` @ 0x0045a850 | same |
| 0x0053e3b2 | `CharacterActor::Update` (slot 70) @ 0x0053d8d0 | same |
| 0x0054a9df | `PopupActor::Update` (slot 70) @ 0x0054a8f0 | same |
| 0x0054c6bf | `TurretActor::Update` (slot 70) @ 0x0054b000 | same |

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

`IsAnyEnemyWithinSightRange` @ 0x0045e220 is a second "is any enemy within sight range" helper that also
consults slot 8 — it has **zero references, code or data**, i.e. dead code.

---

## 4. The bypass ("alternative scanning you cannot hide from") — ESTABLISHED

**There are two, and the useful distinction is the sensory modality, not the proc.** What was checked
and ruled out first:

- All eight consult sites above (every AI think proc that acquires targets at all).
- The AI-think dispatch `Actor_SetAiBehaviour` @ 0x00450550 assigns only: `AiThink_Bot`
  (bot/centipede/president, and only when the actor has a weapon), `AiThink_Scavenger`
  @ 0x004556e0, `AiThink_Mine` @ 0x004552a0, `AiThink_Minebot` @ 0x00456c50, `AiThink_Waiting`
  @ 0x00456bb0 (waiting/centibody/nodewaiting/popup/turret — a 149-byte timer that only fires
  `Actor` slots 79 and 81 off the `+0x24` / `+0x2c` float deadlines, no perception),
  `AiThink_Pathfinder` @ 0x004556d0, `AiThink_Node` @ 0x0045a850, `AiThink_Swarm` @ 0x0045b620,
  and nothing for blocker/pickup/tumbleweed/background creature.
  The three that never touch slot 8 have now been read, and **they resolve to three different
  verdicts** — see §4.1. Two are genuine bypasses and one is inapplicable.
- Two `Actor::flags (+0x7c)` bits set by console commands look like AI-capability flags and
  are the other plausible home for it: **0x20** set by `CommandHunter` @ 0x004492d1, and
  **0x40** set by `CommandFlareFirer` @ 0x00449301. Neither has a reader that this pass
  located (both are read via `MOV reg,[x+0x7c]` followed by a test outside a 5-instruction
  window). Bit **0x80** on the same word is the "spawn a flare now" request, consumed at
  0x0053e645 in `CharacterActor` slot 70, which then `GetRoleByName("flare")`; it is
  cleared in `AiThink_Bot` (0x00454a42, 0x00455032) and `AiThink_Minebot` (0x00457203).

The recon-mode strings and the `character` GLS fields `scan delay` (0x0e), `scan acceptance
angle` (0x0f) and `angular scan rate` (0x10) all belong to the sweep scanner, which — as
shown above — **does** respect concealment.

### 4.1 The three procs that never touch slot 8, resolved

Measured with a backward-register-walk detector (the `MOV EAX,[reg+0x20]` + `CALL EAX` form),
**validated against both known positives first** — `AiThink_Bot` @ 0x00451a27 and
`CollectDetectableEnemies` @ 0x00459535 — and then applied to each body. All three bodies are fully
disassembled, so the silence is a statement about the binary and not about the listing.

| proc | verdict | keyed on | evidence |
|---|---|---|---|
| `AiThink_Swarm` @ 0x0045b620 | **CONFIRMED bypass — sight** | `sight_range_squared`, `MobileActor+0x168` | its candidate loop is `AiThink_Bot`'s with the six-instruction slot-8 test **deleted**. Indirect-call offsets present: 0x18, 0x30, 0x38, 0x48, 0x4c, 0x58, 0xf8, 0x110, 0x13c, 0x144, 0x168 — **no 0x20** |
| `AiThink_Minebot` @ 0x00456c50 | **CONFIRMED bypass — hearing** | `hearing_range_squared`, `Character+0x34` @ 0x00456f09 | omnidirectional, no bearing or facing test. Offsets: 0x18, 0x28, 0x38, 0x48, 0x4c, 0x110, 0x13c, 0x144 — **no 0x20**; the single `+0x191` read @ 0x00456d5d is on `this` |
| `AiThink_Scavenger` @ 0x004556e0 | **INAPPLICABLE — not a bypass** | nothing | it acquires no *actor*. It walks `AiStimulusList` @ 0x006af824 for 0x30-byte **stimulus records** within 6 m, so there is no candidate for slot 8 to be asked about. Offsets: 0x28, 0x38, 0x13c, 0x144 only. **Its authored `sight angle` / `sight range` / `hearing range` are inert** and it has no state machine at all |

**Hearing looks like an exempt modality by design, not a per-proc oversight.** The engine's other
hearing consumer, `PostAiStimulus` @ 0x0044f960, reads the **same** `Character+0x34` and likewise has
no slot-8 test (its only indirect-call offsets are 0x18, 0x1c, 0x28 = slots 6, 7, 10). Two
independent hearing paths behaving identically is a pattern; one would have been a bug. What is *not*
settleable from the binary is whether concealment was **intended** to suppress hearing — so §3's
sentence is a counterexample to itself, but not necessarily to the design.

**Gameplay statement — crouching does not hide you from these roles:**

- the six `ai swarm` `Chr_Scuttler` variants in `scuttler.gsh` (aggression 1) — the **Scuttlers**;
- `Rol_Smartbot` and `Rol_mini_Smartbot`, also `ai swarm`;
- `Rol_Walking_Mine` (identifier `"minebot"`) and `Rol_Mini_Minebot` (`"mini_minebot"`), the two
  `ai minebot` roles.

Note those are the **roles**, not the characters: `Chr_Walking_Mine` and `Chr_Mini_Minebot` back both
the `ai minebot` and the `ai swarm` pairs, so the same character definition appears on this list for
two different reasons. And because `Chr_Walking_Mine` authors `sight range 15` and `hearing range 15`
**equal**, the sight/hearing distinction above is **unobservable in play** for that family — it
matters for reading the code, not for predicting behaviour.

---

## 5. The renderer: how a camouflaged unit is drawn

`Unit_Draw` @ **0x004b6ae0**, branch at **0x004b708d** (pre-comment added). Gated on
`Unit::IsConcealed()` and `Unit+0x127 == 0`.

A concealed unit is submitted through `RenderQueue_Submit` with material
**`Mat_Translucent`** and an explicit alpha object as the 9th argument — a 0xc-byte
`pool_alloc` `{vptr = 0x00664024, refcount = 1, float alpha}`:

- **Own team** (`Unit+0xb4 == LocalPlayerTeam` @ 0x006a58e0), or a Cooperative ally
  (`TeamSlots[team].player_controlled != 0`, `+0x6a`): **alpha = 0.7** (`0x3f333333`). That is the manual's
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
`UnloadLevel` @ 0x004e2090 (at 0x004e214a) on level teardown. It is **client-only** — nothing on the
executor side reads it.

| Offset | Type | Meaning |
|---|---|---|
| 0x00..0x0c | `float[4]` | world bounds `{minX, minZ, maxX, maxZ}`, squared up by `FogOfWar_SetWorldBounds` @ 0x00468e10 |
| 0x10 | `float` | `1 / extent` |
| 0x14 | `float` | `gridDim / extent` (world units -> grid cells) |
| 0x18 | `int` | enabled level: 0 = off, 1 or 2 (3 on Voodoo 2/3). `FogOfWar_SetEnabledLevel` @ 0x004697d0. **0 makes every sample return 0** |
| 0x1c | `byte *` | **explored** grid, `gridDim^2`. Persistent minimum; this is the one that is saved. `malloc`'d in `FogOfWar_Ctor` |
| 0x20 | `byte *` | **current** grid, `gridDim^2`. The per-frame working grid — `FogOfWar_StampDefoggerUnits` opens with `memcpy(+0x20, +0x24, dim*dim)` |
| 0x24 | `byte *` | **static layer** snapshot, `gridDim^2` — the defog areas only |
| 0x28 | **`AwTexture` (embedded, 0x34 bytes)** | **"Fog System Texture"**, `D3DPOOL_SYSTEMMEM`, `gridDim x gridDim`, occupying 0x28..0x5b. **Not a pointer** — see below. `d3d_texture` is its first field, which is the one `FogOfWar_UploadTexture` `LockRect`s; `name` is at +0x2c into the record, i.e. 0x54 |
| 0x5c | **`AwTexture` (embedded, 0x34 bytes)** | **"Fog Video Texture"**, `D3DPOOL_MANAGED`, same size, occupying 0x5c..0x8f (name at 0x88). The `CopyRects` destination — this is the one that is sampled |
| 0x90,0x94,0x98,0x9c | `int` | dirty rect `left, top, right, bottom`, **inclusive bounds** (both expander loops terminate on `JLE`). `UploadTexture` `INC`s `+0x98`/`+0x9c` at 0x0046712d-0x00467142 to build the exclusive `RECT` `CopyRects` wants, then resets the rect to empty |
| 0xa0 | `int` | `gridDim`, clamped to `MaxTextureDimension` @ 0x006ab970. `LoadLevel` passes literal 0x100 at 0x004e0df4, and both D3D textures are created `gridDim x gridDim` |
| 0xa4 | `float` | `FOGVALUE` — fog level in discovered areas, 0..1 |
| 0xa8 | `float` | `FOGUPDATE` — complete updates per second |
| 0xac / 0xb0 | `float` | `FOGTRANSITION` metres / its reciprocal |
| 0xb4 | `unsigned` | fog colour packed into the surface format — **RGB only, no alpha**, duplicated into the high word when the format is 16 bpp. Read by the 16bpp and 32bpp expanders; **the 8bpp expander never reads it** |
| 0xb8..0xc4 | `float[4]` | `FOGCOLOUR` RGBA, **unclamped** (also drives `D3DRS_TEXTUREFACTOR` and `ClearColour`). `ClearColour` @ 0x007c1204 is the **backbuffer** clear colour, not a fog-local value: its only writer is `FogOfWar_SetColour` (0x004692d8) and its only reader `ClearBackBufferAndZ` (0x00574e81), which passes it as the `Color` argument of `Clear(0, NULL, D3DCLEAR_TARGET\|D3DCLEAR_ZBUFFER, ClearColour, 1.0f, 0)`. So `FOGCOLOUR` changes what the whole frame is cleared to |
| 0xc8 | `SurfaceFormatRec *` | chosen surface-format record, picked by `FogOfWar_BuildTextures`. 0x24 bytes: `0x00 shiftR, 0x04 posR, 0x08 shiftG, 0x0c posG, 0x10 shiftB, 0x14 posB, 0x18 shiftA, 0x1c posA, 0x20 D3DFORMAT`, where `shift = 8 - channel bits` and `pos = trailing zeros of the mask`. Built by `FillSurfaceFormatChannelTable` @ 0x005a48f0 from `SurfaceFormatChannelMasks` @ 0x006ac380 (`dword[11][4]` of `{Rmask,Gmask,Bmask,Amask}`, indexed by `format - 20`) via `ChannelMaskToShiftAndPos` @ 0x005a5200 |
| 0xcc..0xd8 | `List` | **defog areas** (sentinel ptr, count, cache, cache_valid) |
| 0xdc..0xe8 | `List` | **defogger units** |
| 0xec | `AwMaterial *` | `fog_material` — the opaque one, cloned from `Mat_Opaque` |
| 0xf0 | `AwMaterial *` | `fog_material_blended` — cloned from `Mat_Translucent` and installed as `fog_material->blended_variant` (`AwMaterial+0x34`) |

**Two corrections to the two texture rows and the two material rows** (measured; the earlier
pointer typing was wrong):

- `+0x28` and `+0x5c` are **embedded `AwTexture` objects, not pointers.** `FogOfWar_BuildTextures`
  @ 0x00467580 does `memset(this+0x28, 0, 0x34)` then `*(char**)(this+0x54) = "Fog System Texture"`,
  and `0x54 - 0x28 = 0x2c` is **exactly** `AwTexture.name`'s offset while `0x34` is exactly
  `sizeof(AwTexture)`; same shape at `+0x5c`/`+0x88`. The decisive evidence is
  `FogOfWar_MakeFogMaterial` @ 0x00468ef0 doing `*(int *)(mat + 0x3c) = this + 0x5c` —
  `AwMaterial.stages[0].texture = &this->video_texture`, **taking the address**, which a pointer
  field could not supply.
- **`FogOfWar_BuildTextures` never touches `+0xf0`.** Its source is `FogOfWar_Ctor` @ 0x00467820:
  `PUSH [Mat_Opaque]; CALL 0x00468ef0; MOV [ESI+0xec],EAX` then
  `PUSH [Mat_Translucent]; CALL 0x00468ef0; MOV [ESI+0xf0],EAX`, followed by
  `MOV EAX,[ESI+0xf0]; MOV [ECX+0x34],EAX` — the blended variant hookup.
  `FogOfWar_RecreateDeviceObjects` @ 0x00468dc0 compiles both. So anyone tracing a fog material
  through `BuildTextures` will not find where `+0xf0` comes from.
- `FogOfWar_MakeFogMaterial` @ 0x00468ef0 is `AwMaterial * __thiscall(FogOfWar *this,
  AwMaterial *template)`: it clones the template, sets `this->enabled_level` from the capability
  global `DAT_006ab974` (0 -> 0, 1 -> 2, >= 2 -> 1, with a Voodoo 2/3 three-stage path), binds
  `&this->video_texture` into a stage, compiles, and retries once at a lower level on compile
  failure.

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

   **Which expander runs is not a field of `FogOfWar`.** It is
   `BitsPerPixelOfFormatRecord(FogOfWar+0xc8)` @ 0x005a4d50 — literally
   `MOV ECX,[ECX+0x20]; JMP BitsPerPixelOfD3DFormat` — so it is the `D3DFORMAT` at **+0x20 of
   the `SurfaceFormatRec` in `FogOfWar+0xc8`**, mapped to 8/16/24/32 bpp: `8 -> 8bpp`,
   `0x10 -> 16bpp`, `0x20 -> 32bpp`. Anything else — in practice only 24 (`D3DFMT_R8G8B8`) —
   runs **no expansion at all but still does the `CopyRects`**. That is a latent hole and is
   currently unreachable: `R8G8B8` has zero alpha bits and `FogOfWar_BuildTextures` skips
   every zero-alpha candidate.

   `FogOfWar_BuildTextures` @ 0x00467580 picks the record with the **most alpha bits**
   (`8 - shiftA`), tie-broken by the **fewest bits per pixel**, over formats 20..30. That is
   **`D3DFMT_A8` (28)** — 8 alpha bits, 8 bpp — on any device that offers it; runner-up is
   `D3DFMT_A8R8G8B8` (21, 32 bpp), and `A8R3G3B2` (29) ties on alpha and loses on bpp.
   Confirmed live: `render.textures` on level02 reports the fog grid as 256x256, 1 level,
   **`format 28`**, 65536 bytes. So the 8 bpp expander is the one that actually runs. Both
   textures are created by `CreateTextureOfFormat` @ 0x005a4460 — `+0x28` with pool flag 2
   (`D3DPOOL_SYSTEMMEM`), `+0x5c` with 1 (`D3DPOOL_MANAGED`).
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

### 6.4 `FOGCOLOUR` re-uploads the whole texture

`FogOfWar_SetColour` @ **0x00469270**, `void __thiscall(FogOfWar *this, const float rgba[4])`,
`RET 0x4` — is **not a setter**. It does four things, in order:

1. `MOVUPS` the four floats to `+0xb8..0xc4`. No clamping.
2. Pack `ARGB(0xff, r*255, g*255, b*255)` into the global `ClearColour` @ 0x007c1204 and, when
   `direct3d_device` @ 0x007c121c is non-null, `SetRenderState(D3DRS_TEXTUREFACTOR /*0x3c*/, …)`
   through device vtable +0xc8.
3. Rebuild the format-packed colour at `+0xb4` from the shift/pos table at `+0xc8`, duplicating
   it into the high word when the format is 16 bpp.
4. `+0x90 = +0x94 = 0; +0x98 = +0x9c = gridDim - 1;` then **`FogOfWar_UploadTexture`**.

There is **no incremental path and no early-out**. Step 4 is a full re-expansion of every cell
in the grid, and at `gridDim` 256 (inclusive bounds, so 256 x 256 = 65,536 cells) it costs:

| format picked | iterations | instrs/iter | expand instrs | grid bytes read | surface written | `CopyRects` bytes |
|---|---|---|---|---|---|---|
| `D3DFMT_A8` (8 bpp) — **what the game gets** | 65,536 | 19 | ~1.25 M | 131,072 | 65,536 | 65,536 |
| 16 bpp fallback | 32,768 | 42 | ~1.38 M | 131,072 | 131,072 | 131,072 |
| `A8R8G8B8` fallback | 65,536 | 20 | ~1.31 M | 131,072 | 262,144 | 262,144 |

plus, per call and regardless of format: one `LockRect` + `UnlockRect`, two `GetSurfaceLevel`
and two `Release`, one full-surface `CopyRects` into a `D3DPOOL_MANAGED` texture, and one
`SetRenderState`. The 8 bpp loop reloads `shiftA` and `posA` from `+0xc8` on **every texel**.

**Measured in the running game: ~85 us a call** — 80-90 us under `GKPLUS_RENDERER=d3d8` (the
original runtime), 90-100 us under `vulkan`, over 200 REPL-driven calls against a 200-iteration
property-read control. The two renderers agreeing is the reading: the cost is the **CPU
expansion loop**, not the upload path, so a faster upload path does not remove it. That is about
**0.5% of a 16.6 ms frame**. Full write-up in `vulkan_renderer_notes.md` §4.90.1.

**And in the `D3DFMT_A8` configuration the game actually runs in, every byte of that is wasted.**
`FogOfWar_ExpandRect8bpp` writes `((explored[i] + current[i]) >> shiftA) << posA` — **alpha
only** — and never reads `+0xb4` at all. The colour is not baked into the texels; it reaches the
screen through the `D3DRS_TEXTUREFACTOR` set in step 2. So the re-upload is **incidental, not
inherent**: it is only load-bearing at 16 or 32 bpp, where the expanders do `OR` `+0xb4` into
each texel. (This round documents the cost; it does not optimise it. `src/World.h` carries a
proposed cheap path, guarded on the format being 28.)

**Nothing in the shipped game calls it per frame.** It has exactly two call sites: 0x004e2f6b in
`CommandFocColor` @ 0x004e2ee0 (the `FOGCOLOUR` console command) and 0x0046769f, the tail call in
`FogOfWar_BuildTextures` — reached from `FogOfWar_Ctor` (level load, 0x00467958) and from
`FogOfWar_RecreateDeviceObjects` @ 0x00468dc0 (device reset / alt-tab, via `SetVideoMode` and
`OnActivateApp`). That tail call is what restores `D3DRS_TEXTUREFACTOR` after
`ResetDefaultRenderStates` @ 0x005906b6 has zeroed it.

Compare the per-frame path: `FogOfWar_Update` @ 0x004688f0 uploads with an **accumulated** dirty
rect and only forces the full rect when a timed defog area expires. It also brackets that call
with `AccumulateThreadClock()` into the counter at 0x00803c18, read by `RunGameFrame` at
0x0046e237 — the engine already instruments exactly this call.

---

## 7. The reveal paths

### 7.1 Defogger units — the continuous reveal

A unit on the list at `FogSystem+0xdc` gets a `StampCircle` every frame at

- centre = `unit->position` (`Unit+0x98` x, `+0xa0` z) mapped through `+0x10`/`+0x14`;
- radius = `Unit` vtable **slot 66** `Unit_GetDefogRadius` @ 0x004b6900 —
  `role(+0xb8)->character(+0x60)->sight_range(+0x28)`, or **53.0f** when the role has no
  character. Override @ 0x004cf900 returns `Unit+0x180`;
- `partial` = `Unit` vtable **slot 38**, base body 0x004cf510 (returns 0), one class returning 1
  @ 0x004cf520.

  **That slot is `IsProjectile`, and the class is `ProjectileUnit`.** It was named
  `Unit_GetFogStampIsPartial` here from this one consumer; slot 38 is in fact rung 3 of the client
  tree's 15-wide RTTI predicate ladder (slots 36–50), whose base bodies are all `XOR AL,AL; RET`
  and whose overrides are all `MOV AL,0x1; RET` — and 0x004cf520 appears in exactly one vtable,
  `ProjectileUnit`'s @ 0x00664c88. `rendering_notes.md` §5.1 has the ladder and its index→predicate
  mapping, which is identical to the executor `Actor` tree's. So the fog code is not asking "is this
  a partial defogger", it is asking **"is this defogger a projectile"** — and the answer to §10's
  open question is `ProjectileUnit`, i.e. a shot in flight, not a character. The DB name is now
  `Unit::IsProjectile` / `ProjectileUnit::IsProjectile`.

So **a unit reveals fog out to its own GLS `sight range`**.

Membership is `FogOfWar_AddDefoggerUnit` @ **0x004693c0** / `FogOfWar_RemoveDefoggerUnit`
@ **0x00469450** (both `__thiscall(Unit *)`, `RET 4`), which also refcount the unit and set
`Unit+0x211` through slot 27 `Unit_SetIsDefogger` @ 0x004d0000. Added from `LoadGame`,
`Unit_SetTeamWithInventory` @ 0x004bb410 (`Unit` slot 33, and it is **`SetTeam`** - the base
implementation at 0x004cf3b0 is literally `*(int *)(this + 0xb4) = arg; return;`) at 0x004bb4ed,
`ApplyUpdateMessage` 0x004fecca (spawn path) and case **0xb7**; removed from `MobileUnit::LeaveWorld`
@ 0x004c0f80, `ProjectileUnit::~ProjectileUnit` @ 0x004c4a50 and case **0xb8**. The `+0x211 = 1` is therefore
**not a construction-time event**: a unit becomes a defogger whenever its team becomes the local
player's team - or, in Cooperative, any team whose `TeamSlots[team].player_controlled` (`+0x6a`) is set - so every team
change re-evaluates it.

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
| `AiBeginInvestigate` @ **0x0045e05a** — AI "go investigate", called twice from `AiThink_Minebot` @ 0x00456f7c / 0x004571b0. Centre is the AI actor's own position | **20.0** | 1, **3 s** |
| `ApplyUpdateMessage` **case 0x46** (spawn projectile / weapon fire) @ 0x004ff... — built inline when `msg[4] != LocalPlayerTeam` @ 0x006a58e0, i.e. when the shooter is *not* on the local player's team. Centre is the fire position `msg[6..8]` | **25.0** | 1, **60 s** |
| `ApplyUpdateMessage` **case 0x47** — same shape, centre `msg[4..6]` | **20.0** | 1, **60 s** |

**The manual's "firing a flare reveals a zone" is only partly pinned down — but the flare itself is
NOT dead code, and this paragraph used to be scoped as though it were.** Everything below concerns
the **executor/AI** route, which *is* dead. The **player** flare is a separate and fully live
feature: the `GL_CONTROLS_FIREFLARE` key binding (default DIK_F) calls `EnterFlareMode` @ 0x004a17e0,
which sends wire command 0x19 so that `SetWeaponAmmoType` @ 0x004b1da0 rewrites
`weapon->projectile_role` to `Rol_Flare`, after which an ordinary left click discharges it
(`orders_notes.md` §8.5, `gadgets_notes.md` §6). **So the manual entry is not describing dead code.**
What is still open is only the second half — whether the flare *projectile* drives a fog reveal.

The dead AI route: `Actor::flags (+0x7c)` bit **0x80** ("fire a flare now"), consumed at
0x0053e645 in `CharacterActor` slot 70 @ 0x0053d8d0, which does
`GetRoleByName("flare")` (string @ 0x0066958c; `"flare_light"` @ 0x006695a8 is used at
0x005454e8). `CommandFlareFirer` @ 0x004492e0 sets bit **0x40** ("this actor fires
flares"). Note bit 0x80 is not merely unwritten but **redundant**: `Rol_Flare` carries
`identifier "flare"` and `ammo Ammo_PlasmaPistol_Flares` names `projectile Rol_Flare`, so the dead
AI path and the live player path resolve to the *same role object*. What was *not* found is code
linking the flare projectile or its light to a
0xb6 broadcast — the reveal that accompanies an investigation is the 20 m / 3 s one from
`AiBeginInvestigate`, and the two 60 s ones come off weapon fire generally. See "unknown" below.

### 7.3 Console fog commands

| Command | Handler | Effect |
|---|---|---|
| `FOG` | `CommandFog` @ 0x004e2ec0 | `SetIsFogEnabled` -> `FogSystem+0x18` |
| `FOGCOLOUR` | `CommandFocColor` @ 0x004e2ee0 | `FogSystem+0xb8..0xc4`, via `FogOfWar_SetColour` @ 0x00469270 — **also re-uploads the whole fog texture and overwrites the backbuffer clear colour, ~85 us; see §6.4** |
| `FOGVALUE` | `CommandFogValue` @ 0x004e2f80 | `FogSystem+0xa4`, clamped 0..1 |
| `FOGUPDATE` | `CommandFogUpdate` @ 0x004e30d0 | `FogSystem+0xa8` |
| `FOGTRANSITION` | `CommandFogTransition` @ 0x004e3020 | `FogSystem+0xac` (m) and `+0xb0` (1/m) |
| `DEFOGGER` / `FOGGER` | 0x00442a10 / 0x00442b10 | broadcast 0xb7 / 0xb8 |

---

## 8. What the fog gates

**Client-side only, and all but one of them rendering or effects.** `FogOfWar_SampleTotal`
@ 0x00468770 has 31 call sites: `Unit_Draw` (x4), `DrawWorldEffects` (x6), `SpawnSparks`, the
world-effect updaters `FUN_00513xxx`-`FUN_00515xxx`, `SceneLightSet_SelectLightsForBounds`
@ 0x00488400, `FUN_00558d30`, and four effect-spawn cases in `ApplyUpdateMessage`. **The one
exception is `OnClickSelectOrTrack` @ 0x004a4130** (name **PROPOSED**), which is neither a draw
nor an effect spawn and is not adjacent to `HudItem_DrawByKind`: it is an interface **click
handler**, registered into the 0x007b41f8 handler matrix with mask 0x501 from `BeginLevelSession`,
with no direct callers at all, and it uses the fog sample (`> 0xfd`, i.e. fully fogged, on a
`Role+0x7c == 10` unit) to decide whether the click may select the unit under the cursor. So fog
gates one *input* decision as well as the drawing.

`FogOfWar_SampleCurrent` @ 0x00468830 has exactly one caller, `Unit_SampleFogVisibility`
@ 0x004b68c0, which picks between the two samplers on `Unit+0x118` bits 0x10 / 0x8. It is **not**
a draw helper: its own single caller is `Unit::Unit_Update` @ 0x004b5d50, base `Unit` vtable **slot 57, the per-tick
`Update`**. Where the sampled value goes from there is not established.
The result is written as a 0..1 fade factor into the render node's `+0x38`
(`Unit_Draw` @ 0x004b7d94, 0x004b7fee, 0x004b839c). Nothing on the executor thread — no
targeting, no selection, no AI — reads the fog: the whole object is allocated, updated and
sampled on the client. Selection and targeting of a fogged enemy are therefore **not**
blocked by fog; visibility of an enemy in fog is a rendering consequence only.

Two of those rendering consumers are worth naming, because neither is a per-unit fade:
`FogOfWar_SetColour` @ 0x00469270, which pushes the fog colour to the device as
`D3DRS_TEXTUREFACTOR` and to the global backbuffer `ClearColour` (§6.4), and the **per-light
specular scale** in `SceneLightSet_SelectLightsForBounds` @ 0x00488400 — see
`rendering_notes.md` for the mechanism, which is not restated here.

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

- `src/Actors.h` carries the field and slot 8/9/63 names already (`is_concealed`, `is_crouched`,
  `IsConcealed`/`SetConcealed`, `IsCrouched`). Still outstanding there: **slot 83**, declared as
  `virtual void ToggleCrouchAndCamouflage() = 0;` in `src/Actors.h` (`MobileActor` slot 83; the
  declaration was called `UpdateMineDetectionAndBounds` when this was written), which is
  `ToggleCrouchAndCamouflage`. **None of these has a `static_assert` behind it**, so nothing but
  review catches a wrong slot name.
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
  unread think procs (`AiThink_Scavenger` @ 0x004556e0, `AiThink_Minebot` @ 0x00456c50,
  `AiThink_Swarm` @ 0x0045b620) and the readers of `Actor::flags` bits 0x20 (`HUNTER`) and 0x40
  (`FLARE FIRER`) are where to look.
- **The flare's own reveal.** Bit 0x80 -> role `"flare"` is measured; a 0xb6 broadcast tied
  to the flare projectile is not. The 25 m / 60 s reveal in update case 0x46 fires on
  *any* enemy weapon discharge, which may be what the manual is describing loosely.
- **`MobileActor+0x150`**, the `== -1` guard on clearing concealment when standing up.
- **`Unit+0x127`**, the second gate on the concealed draw branch, and `Unit+0x118` bits
  0x8 / 0x10 which choose between the two fog samplers.
- **`MaxTextureDimension`** @ 0x006ab970 (the grid-dimension clamp) and `DAT_00738ff4` (the "full-rect update this
  frame" flag) are zero-init `.data`; their runtime values were not read.
- ~~Which `Unit` subclass returns 1 from slot 38~~ — **closed**: slot 38 is `IsProjectile` and the
  class is `ProjectileUnit` (see §7's stamp bullet). What is still open is *why* a projectile's
  stamp is the partial one.
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
| 0x00469270 | `FogOfWar_SetColour` (§6.4 — full re-upload, ~85 us) |
| 0x00467580 / 0x00468dc0 | `FogOfWar_BuildTextures` / `_RecreateDeviceObjects` |
| 0x004674b0 / 0x00467330 / 0x00467220 | `FogOfWar_ExpandRect8bpp` / `_16bpp` / `_32bpp` |
| 0x007c1204 | `ClearColour` (backbuffer clear; written only by `FogOfWar_SetColour`) |
| 0x006ac380 | `SurfaceFormatChannelMasks` (`dword[11][4]`, formats 20..30) |
| 0x005a48f0 / 0x005a5200 | `FillSurfaceFormatChannelTable` / `ChannelMaskToShiftAndPos` |
| 0x005a4d50 / 0x005a4700 | `BitsPerPixelOfFormatRecord` / `BitsPerPixelOfD3DFormat` |
| 0x005a4460 | `CreateTextureOfFormat` |
| 0x004697d0 / 0x00472230 | `FogOfWar_SetEnabledLevel` / `SetIsFogEnabled` |
| 0x00442a10 / 0x00442b10 | `CommandDefogger` (0xb7) / `CommandFogger` (0xb8) |
| 0x0050e55d | DEFOG trigger -> update 0xb6 |
| 0x0045e050 | AI investigate -> update 0xb6 (20 m, 3 s) |
| 0x00459440 / 0x00459930 | `CollectDetectableEnemies` / `AdvanceScanArcAndFilterCandidates` (perception agent's) |
| 0x00451220 | `AiThink_Bot` |
| 0x00450550 | `Actor_SetAiBehaviour` (AIType -> think proc) |
