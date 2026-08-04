# Gunlok Combat System - Reverse Engineering Notes

How a shot is ordered, aimed, launched, flown and resolved into damage. The declarative half
(`role` / `character` / `projectile` / `ammo` / `destructibility` / `vulnerability` GLS sections)
is in `role_system_notes.md` and `role_subobjects_notes.md`; this file is the runtime logic that
consumes it.

Everything below is measured against the Ghidra database unless a paragraph says otherwise.
Section 12 lists what is inferred and what is still unknown.

---

## 1. There is no hitscan. Every shot is a `ProjectileActor`

The only slot-68 (`ApplyDamage`, `CALL [reg+0x110]`) call sites in the binary are

| Address | In |
|---|---|
| 0x0052e637 | `Actor::Frag` @ 0x0052e220 - the `frag data` blast |
| 0x00543d22 | `ProjectileActor::OnPrePhysics` @ 0x00542ae0 - projectile splash |
| 0x005436fd | `ProjectileActor::OnPrePhysics` - projectile direct hit |
| 0x0050a9fd | `ExecutorThreadProc` - the network/console damage command |

So **all weapon damage arrives through a `ProjectileActor`**, including the laser and the
flamethrower - they are fast/short-lived projectiles, not traces. Nothing in
`CharacterActor::Update` or `TurretActor::Update` applies damage directly; both only construct a
`ProjectileActor`.

This settles the `Weapon+0x08` vs `CharacterActor+0x2a4` "hitscan-vs-projectile" ambiguity in
`src/Actors.h`: neither is a branch selector. `Weapon+0x08` is the **weapon type id** (index into
the ammo table and into `ScaleDamageForResistance`'s switch), and it selects *variants* of the
projectile path (arc solve, team assignment), never a different damage mechanism.

---

## 2. The fire path

`CharacterActor::Update` is vtable **slot 70** @ 0x0053d8d0 (the DB name
`CharacterActor::SyncPositionAndBroadcast` is the family-wide slot-70 name; slot 70 is really
"Update" - see `actor_vtable_notes.md`). The whole fire sequence is inline in it; the projectile
is constructed at 0x00540649, not through `SpawnProjectileActor`.

### 2.1 Order to fire

`CharacterActor::AttackTarget` @ 0x00540c60 - vtable slot 96,
`__thiscall void(CharacterActor *this, Actor *target, int range, char mode, char close_range)`,
`RET 0x10`.
`CharacterActor::AttackPosition` @ 0x00540a10 - slot 97, same shape with a `Vec3 *`.

It stores `attack_target` (+0x2d8, refcounted), `attack_mode_param` (+0x261),
`close_range_attack` (+0x288), `attack_range` (+0x290), sets `is_attacking` (+0x2d4), zeroes
`Actor+0x74`, and computes the launch speed:

```
speed = weapon->muzzle_speed                     // Weapon+0x04
if (close_range) speed *= CloseRangeSpeedFactor  // 0x00666234 = 0.65
if (character)   speed *= character->shot_speed_multiplier   // Character+0x58
this->muzzle_speed = speed                       // CharacterActor+0x2ec
```

Broadcast id is `0x41 + (close_range != 0)` for `AttackTarget` and `0x3f + (close_range != 0)`
for `AttackPosition`.

### 2.2 The gate in slot 70

The shot is skipped unless **all** of these hold (0x0053ff00 onwards):

- `is_attacking` (+0x2d4) != 0, `weapon` (+0x2b8) != NULL, `attack_mode_param` (+0x261) == 0
- the three animation-ready flags +0x2e8, +0x2e9, +0x2ea are all set
- the elevation solution was accepted (see 3.3)
- `now >= next_shot_time`, an int64 at **CharacterActor+0x248**
- `rounds_in_magazine` (+0x304) != 0

`Actor::Frag`-style destructibles and the AI aside, the range test (5) and the
line-of-sight/targetable tests all sit ahead of this and end in `StopAttacking` (slot 98) at
0x0053e94a rather than in a silent skip.

### 2.3 Which projectile

`Weapon+0x00` is a `Role *` - the projectile role - copied out of `Ammo::role` by
`SetWeaponAmmoType`. Two overrides:

- **Flares**: if `Actor::flags & 0x80` is set, the projectile role is replaced with
  `GetRoleByName("flare")` at 0x0053e64b. This is a straight substitution and applies to whatever
  weapon is held - the "flares only from a plasma weapon" rule is *not* enforced here, it comes out
  of the ammo table (section 6). The bit is almost certainly the `FLARE FIRER` console command
  (help string @ 0x00650d64: "The specified actor will fire flares to investigate noises."), but
  its writer was not located - the scan for `OR [reg+0x7c],0x80` found only the read.
- **Weapon type 0x0d** (`repair arm`): the projectile is spawned with team **0** instead of the
  shooter's team (0x0053f0..; `if (weapon_type == 0xd) team = 0`), which is what makes it
  non-hostile.

### 2.4 Spawn and cadence

```
proj = pool_alloc(0x178)
ProjectileActor::Ctor(proj, team, projectile_role,
                      &this->aim_direction,   // +0x2bc - the MUZZLE WORLD POSITION
                      &orientation_quat,      // yaw*pitch*spread, see 3
                      &this->muzzle_speed,    // +0x2ec
                      this->id,               // owner actor id -> ProjectileActor+0x12c
                      target_actor_id,        // -1 when none -> refcounted Actor* at +0x164
                      weapon->weapon_type,    // -> ProjectileActor+0x160
                      num_actors)
proj->slot 0xcc (timing)
proj->SetTargetPosition(this->attack_position)          // slot 84
proj->damage_scale (+0x130) = character->damage_multiplier   // Character+0x54
```

Then `MissionShotsFired` (0x007b9cf8, or `...Team2` 0x007b9d00 for team 2) is incremented, but
only when `projectile_role->projectile->damage > 0` and the team is not 0.

`CharacterActor_ConsumeRoundAndReload` @ 0x00541700 runs next and its return value picks the
cadence:

| return | meaning | next shot at |
|---|---|---|
| 0 | magazine still has `>= salvo_size` rounds | `now + Weapon+0x0c` (`shot_interval`) |
| 1 | magazine was refilled | `now + Weapon+0x10` (`reload_interval`) |
| 2 | out of ammo | `StopAttacking` (slot 98), no reschedule |

`next_shot_time` is the int64 at CharacterActor+0x248; the float interval goes through the
`__ftol` helper @ 0x005e471e, so both intervals are in **clock ticks**.

A projectile whose `GetProjectileFlags()` (slot 83) has bit **0x200** set also forces
`StopAttacking` instead of a reschedule, unless the shooter's team is 0 or 2.

### 2.5 The muzzle hotspot alternates

When there is no selected ammo, `CharacterActor_ConsumeRoundAndReload`'s only action is to swap
`hotspot` (+0x2c8) and `alternate_hotspot` (+0x2cc). That is the dual-muzzle alternation; the
spawn position `aim_direction` (+0x2bc) is resolved from whichever is current
(`HierarchyResolveNamedPointPos`, 0x0053e999).

### 2.6 Turrets

`TurretActor::Update` @ 0x0054b000 is the same slot 70 with its own copy of the sequence
(`MissionShotsFired` at 0x0054c54f, `MobileActor::aim` read at 0x0054bcbc / 0x0054be4d). It was
not traced field-by-field; the header's note that `TurretActor` uses +0x2bc as a muzzle position
and +0x2ec as a muzzle speed is correct, and is correct for `CharacterActor` too.

---

## 3. Aiming

### 3.1 Yaw

`yaw = atan2(dx, dz) * RadiansToTrigTableIndex` (0x0053f0e8), where `d = attack_position -
aim_direction`, i.e. target minus muzzle. All angles downstream are **BAM**: 4096 per turn,
indexing `SinTable`/`CosTable` masked `& 0xfff`. `_DAT_0066624c = 651.899 = 4096/(2*pi)` is the
radians->BAM constant.

**+Y is down** in Gunlok world space (`level_loading_notes.md` §5.5: walkable iff `normal.y < 0`),
so a positive pitch aims downward. That sign convention is what makes the ballistic algebra below
come out as the textbook form.

### 3.2 Pitch - `SolveLaunchPitch` @ 0x00541c90

```
__fastcall float *SolveLaunchPitch(float *out_pitch_bam /*ECX*/, Vec3 *delta /*EDX*/,
                                   float *muzzle_speed, char *use_gravity, char *pick_low_arc)
RET 0xc
```

`H = sqrt(dx*dx + dz*dz)`, `v = *muzzle_speed`, `G = GravityAcceleration @ 0x006a8688 = 9.81`.

- `*use_gravity == 0`: `out = atan(dy / H)` in BAM. That is all.
- `*use_gravity != 0`: with `k = G*H*H / (2*v*v)` it solves

  ```
  k*t^2 + H*t + (k - dy) = 0        t = tan(pitch)
  ```

  through `SolveQuadraticNormalized` @ 0x0045d4a0
  (`__fastcall int(float *root0 /*ECX*/, float *root1 /*EDX*/, float *a, float *b, float *c)`,
  returns 0 or 2), takes `atan` of both roots, and returns

  | `*pick_low_arc` | result | trajectory |
  |---|---|---|
  | 0 | `max(angle0, angle1)` | flatter, larger downward pitch - **low arc** |
  | != 0 | `min(angle0, angle1)` | **high / lobbed arc** |

This is the exact closed form; it was checked against the standard
`tan θ = (v² ± sqrt(v⁴ − g(g x² + 2 y v²)))/(g x)` with `y = −dy`.

### 3.3 What selects the arc - the SHIFT behaviour

Call sites, all inside `CharacterActor::Update`:

| Condition | `use_gravity` | `pick_low_arc` |
|---|---|---|
| no projectile role, no `Projectile`, or `projectile->gravity == false` (0x0053edb9) | 0 | `&close_range_attack` (ignored) |
| gravity projectile, `weapon_type != 7` (0x0053ed8e) | 1 | forced 0 (low arc) |
| gravity projectile, `weapon_type == 7` = grenade launcher (0x0053ece8) | 1 | `&this->close_range_attack` (+0x288) |

So the SHIFT-modified attack order does two things at once, and only for the grenade launcher does
the second one exist:

1. `AttackTarget`/`AttackPosition` scale the muzzle speed by **0.65**, which raises the arc for any
   gravity projectile;
2. for weapon type 7 it also picks the **high** root of the ballistic quadratic.

Afterwards the result is clamped: if `|pitch| > character->elevation_angle` (Character+0x44,
already in BAM) the solve is **re-run with `pick_low_arc = 0`** (0x0053ed69), and if the flat
solution is still outside the elevation limit the actor does not fire at all - `+0x2f4`
(`gun_pitch_bam`) is only written when the angle passes (0x0053ee1d region).

### 3.4 Per-shot spread from `aim`

Two draws from the calling thread's PRNG (`threading_model_notes.md`), each producing a float in
`[1,2)` from `bits >> 1 | 0x3f800000`:

```
roll      = (u1 - 1.0) * 4096.0                  // uniform over the full circle, BAM
deviation = (u2 - 1.0) * (u2 - 1.0) * this->aim  // MobileActor+0x1b4, BAM, biased toward 0
if (IsMoving())                deviation *= 2.0  // shooter moving
if (target && target->IsMoving()) deviation *= 5.0
q_spread = { sin(dev)*sin(roll), sin(dev)*cos(roll), 0, cos(dev) }
orientation = q_spread * (q_yaw * q_pitch)
```

`deviation` is used as a quaternion **half**-angle, so the achieved angular error is up to
`2 * aim` BAM. `MobileActor::aim` is `Character::aim` converted degrees->BAM by `ToCharacter`
(`(d*4096)/360`), so `aim 4` (`Chr_DefaultBaddie`) is ~45.5 BAM = ~4°, giving up to ~8° of
error; `aim 1` (`Chr_DefaultGoodie`) up to ~2°; `aim 0` (mines) is exact. Lower is better, and
the square biases most shots well inside the bound.

`SpawnProjectileActor` (the script/effect-side spawner, section 4) applies **no** spread.

### 3.5 The optical tracker / `target imager`

`MobileActor_RecomputeAimSpread` @ 0x00536b20,
`__thiscall void(MobileActor *this, int delta)`:

```
this->aim_level (+0x1b8) += delta;  if (aim_level < 0) aim_level = 0
this->aim (+0x1b4) = role->character->aim * 0.5^aim_level
```

**Each aim module halves the spread.** It is called from the equip path @ 0x00536ba0 for inventory
items whose module subtype (`item+0x20`) is **6**, passing `item+0x18` as the delta. In
`scripts\body_slot_upgrades.gsh` those are the `aim accuracy pickup` roles (`weapon target imager`,
with the comment `walking speed 1 // 1 = standard aim pickup`).

The same equip function shows the sibling module kinds: item kind 3 adds to the shield
(slots 19/29), kind 9 adds to armour (slots 18/28), kind 4 calls `SetWeapon` (slot 94).

### 3.6 `Actor+0x74` - a consecutive-miss counter

Not a combat stat. Zeroed by `AttackTarget` @ 0x00540e72, `AttackPosition` @ 0x00540b62 and by
`ProjectileActor::OnPrePhysics` @ 0x0054421e when the projectile hits its intended target;
incremented at 0x00544239 (hit a friendly/neutral) and 0x00544417 (hit nothing).
`CharacterActor::Update` tests `> 1` at 0x005400cc / 0x00540174 on the team-0 / team-2 branch,
which is a give-up-and-restart heuristic.

---

## 4. `SpawnProjectileActor` @ 0x00503bd0 - the other launcher

```
__fastcall void SpawnProjectileActor(Role *projectile_role /*ECX*/, Vec3 *from /*EDX*/,
    Vec3 *to, int owner_actor_id, int target_actor_id, int team, int weapon_type,
    float muzzle_speed)     RET 0x14
```

Used by `Actor::Frag`, `EvaluateTriggers`, the executor thread and two console handlers - never by
a character firing a weapon. It solves the same problem with a **different, inlined** closed form
(no quadratic helper, no arc choice, no spread):

```
yaw = atan2(dx, dz)
if (!projectile->gravity)  tan(pitch) = dy / H
else                       tan(pitch) = (dy - G*D*D/S) / H
       S = v*v + G*dy + sqrt(v^4 + 2*G*dy*v^2 - G*G*H*H)      (0 if the discriminant is negative)
```

with `G = ProjectileGravity @ 0x006a8250 = 9.81` (a second copy of the constant, read only here),
`H` the horizontal distance and `D` the full 3D distance. Equivalent to a flight time of
`t^2 = 2*D^2/S`, i.e. the **low** arc always.

### 4.1 Update `0x46`, 64 bytes, unreliable - corrected layout

Both spawners build the same payload. Read off the stack slots at 0x00540766-0x00540809 (which
sum to exactly 0x40):

| Off | Field |
|---|---|
| 0x00 | `0x46` |
| 0x04 | **shooter** `Actor::id` |
| 0x08 | **target** `Actor::id`, `-1` when firing at a position |
| 0x0c | projectile `Role::id` |
| 0x10 | team (byte-merged into the dword) |
| 0x14 | game time (float seconds) |
| 0x18 | muzzle world position (`Vec3`, from `CharacterActor+0x2bc`) |
| 0x24 | launch orientation quaternion (`Vec4`) |
| 0x34 | muzzle speed |
| 0x38 | weapon type |
| 0x3c | new `ProjectileActor::id` |

`directplay_protocol_notes.md` §8.6 records `+0x08` as the team; it is the target actor id, and the
team is the byte at `+0x10`. `+0x18` is a **position**, not a direction - the direction is the
quaternion at `+0x24`.

---

## 5. Range

`Projectile::max_range` (+0x0c, GLS field 0x2d) is copied **raw** by `ToProjectile` @ 0x0047e4e0 -
no squaring - while `blast_range` (+0x14, GLS 0x28) *is* squared into `blast_range_squared`
(+0x18) by the same function. Both compare sites use a squared distance:

- **Aim-time gate**, `CharacterActor::Update` @ 0x0053e782:
  `dist2(Actor::position +0xa0, attack_position +0x2dc) <= projectile->max_range`
  -> keep firing; otherwise fall through to `StopAttacking`.
- **In-flight gate**, `ProjectileActor::OnPrePhysics` @ 0x005443a9, against
  `GetWeapon()->projectile_role->projectile->max_range`.

So **`max range` is authored pre-squared** (metres squared) and `blast range` is authored in plain
metres. The shipped headers agree (`2500 // range squared (50 metres)`,
`blast range 10 // in metres`), and so do the two shipped bugs:

- `interface_device.gsh:42` `max range 50 // in metres squared` - 50 is not a square; the device
  reaches `sqrt(50)` ~ **7.07 m**, not 50 m.
- `repair_arm.gsh:39` `max range 40 // in metres` - reaches `sqrt(40)` ~ **6.32 m**.

The aim-time gate is a **3D** distance including Y, so height by itself *shortens* horizontal
reach through this test. See §12 for the "greater height, greater range" question.

---

## 6. Ammo

### 6.1 The compatibility predicate is an empty-table-slot test

Everywhere it is asked, the question is spelled the same way:

```
AmmoTable[weapon_type * 19 + ammo_type] != NULL        // AmmoTable = 0x007b5ec0
```

Sites: `CharacterActor_SelectAmmo` @ 0x00541900, `SetWeaponAmmoType` @ 0x004b1da6,
`MobileActor::EquipObject` @ 0x0053723e, `CharacterActor::Update`'s auto-equip @ 0x0053dd7b, the
inventory/command-wheel UI @ 0x004a6a79 and @ 0x004bd710.

There is **no weapon-id compare anywhere**. The discriminating case in the brief resolves in
favour of the table: `plasma pistol training` (weapon type 1) can fire flares (ammo type 1) if and
only if some `ammo` section declares the pair, exactly as for the normal pistol. "Flares only from
a plasma weapon" is therefore a property of the shipped `.gsh` data, not of the code.

One extra, hard-coded rule sits on top of it in `CharacterActor_SelectAmmo`'s reselect pass:
**ammo type 1 (`flares`) is segregated in both directions** - a weapon currently on flares will
only reselect flares, and a weapon on anything else will never reselect flares:

```
(item->ammo_type != 1) != (weapon->ammo_type == 1)
```

### 6.2 The magazine model

`CharacterActor_SelectAmmo` @ 0x00541800 - `__fastcall int(CharacterActor *this)`:

- `weapon->ammo_type == 0x12` (`none needed`, melee): `rounds_in_magazine = 1`, return 0.
- Pass 1, same ammo type: the inventory item (kind **6**) whose `ammo_type` (+0x20) matches and
  whose stack count (+0x2c) is `>= weapon->salvo_size` (+0x1c), largest stack wins.
- Pass 2, reselect: kind 6, the flares rule above, the table-slot test, and
  `count >= Ammo::salvo_size`.
- On success: `selected_ammo` (+0x300) = the item and
  `rounds_in_magazine` (+0x304) = `min(weapon->magazine_size, stack_count)`.
- Returns 0 (same type), 1 (switched type), 2 (nothing usable).

`CharacterActor_ConsumeRoundAndReload` @ 0x00541700 - one call per shot:

```
rounds_in_magazine -= weapon->salvo_size          (clamped at 0)
stack->count       -= weapon->salvo_size          (clamped at 0)
if (rounds_in_magazine >= salvo_size) return 0
if (stack->count >= magazine_size) { rounds_in_magazine = magazine_size; return 1 }
if (stack->count >= salvo_size)    { rounds_in_magazine = stack->count;  return 1 }
rounds_in_magazine = 0
if (stack->count == 0) RemoveInventoryItem(stack)     // 0x004e5290
return (SelectAmmo() != 2) ? 1 : 2
```

So the HUD's "10/20" is `rounds_in_magazine` (+0x304) over slot 22 `GetAmmoCount`, which returns
`Weapon::magazine_size` (+0x18). The **reserve** is the inventory stack's own count.

### 6.3 `SetWeaponAmmoType` @ 0x004b1da0 fills the whole `Weapon`

`__fastcall void(Weapon *w /*ECX*/, int ammo_type /*EDX*/)`. A silent no-op when the table slot is
empty. Otherwise, with `a = AmmoTable[weapon_type*19 + ammo_type]`:

| Weapon | <- | Note |
|---|---|---|
| +0x00 `projectile_role` | `Ammo::role` (+0x20) | GLS `projectile` 0x0b, converted with `ToRole` |
| +0x04 `muzzle_speed` | `Ammo::firing_speed` (+0x24) | GLS **`firing speed`** 0x2e - a *speed*, not a rate |
| +0x0c `shot_interval` | `Ammo::round_time` (+0x00) x ticks/s | GLS **`round time`** 0x14 - this is the rate of fire |
| +0x10 `reload_interval` | `Ammo::reload_time` (+0x04) x ticks/s | GLS `reload time` 0x15 |
| +0x14 `life_timer` | `(float)Ammo::life_timer` (+0x08) | GLS `life timer` 0x16 |
| +0x18 `magazine_size` | `Ammo::magazine_size` (+0x0c) | GLS `magazine size` 0x12 |
| +0x1c `salvo_size` | `Ammo::salvo_size` (+0x14) | GLS `salvo size` 0x13 - rounds charged per shot |
| +0x20 `ammo_type` | the argument | |
| +0x24 `sound` | `Ammo::sound` (+0x10) | |

The tick rate is the **calling thread's** (`DAT_007c07e0` main / `DAT_007c07b0` executor), so a
weapon configured on one thread and fired on the other would have the wrong cadence. Callers:
`CharacterActor::SetAmmoType` (slot 99), `CharacterActor_SelectAmmo`, `ReadActorFixups` and three
UI paths.

`CharacterActor::SetWeapon` (slot 94) @ 0x00541ed0 allocates the 0x28 bytes and sets only
`weapon_type`; everything else comes from the call above. Weapon type **0x21** means "none" and no
`Weapon` is allocated. One hard-coded special case: weapon type 0x0c (`epulsar`) on the role named
`gunlok` halves `shot_interval` (0x00542038).

### 6.4 `Role::limit` (+0x54) is a weapon/module exclusion mask

Two readers, both testing it against an inventory item's `+0x30` bitmask:

- `MobileActor::EquipObject` @ 0x00536c64: `if ((item->+0x30 & role->limit) == 0)` before equipping.
- `CharacterActor::Update`'s auto-equip @ 0x0053...: the same test while choosing the
  highest-numbered weapon item (`kind == 4`, type `< 0x0d`) that the role is allowed to hold.

That is the reader the repo had not recorded for GLS field 0x78.

---

## 7. Damage resolution

### 7.1 The arithmetic, in order

**Step 1 - raw**, `ProjectileActor::OnPrePhysics` @ 0x005436a1:

```
raw = this->damage_scale * projectile->damage
    = shooter character->damage_multiplier (Character+0x54) * Projectile+0x04
```

**Step 2 - global scale and resistance**, `ScaleDamageForResistance` @ 0x004fc740:

```
__fastcall float *ScaleDamageForResistance(float *out /*ECX*/, char shooter_is_player /*DL*/,
    float damage, int resistance, float resistance_factor, int weapon_type)   RET 0x10
```

`shooter_is_player` is `TeamSlots[shooter_team].+0x6a`; `resistance` / `resistance_factor` are
`Role+0x90` / `Role+0x94` **of the victim**.

```
if (damage <= 0) return damage
damage *= shooter_is_player ? DamageScalePlayerTeam : DamageScaleOtherTeam
if (weapon_type is in the victim's resistance class)
    damage *= (1.0 - resistance_factor)
```

The class table, straight off the jump table:

| weapon types | resisted when `resistance` is |
|---|---|
| 0,1,2,3,0x10,0x11,0x12,0x1c,0x1d,0x1e (plasma family) | 1 or 8 |
| 4,0x13,0x14 (laser family) | 2 or 8 |
| 5,6,0x15,0x1b | 2 |
| 7,8,0x16..0x1a,**0x21** (grenades, missiles) | 4 |
| 9,10 (flamethrower) | 5 or 8 |
| 0x0b (nanofrag) | 7 or 8 |
| 0x0c,0x1f,0x20 (epulsar) | 6 |
| anything else | never resisted |

`Resistance` keyword values in `src/Roles.cpp` are 2 `resists laser`, 4 `resists explosives`,
6 `resists epulsar`, 8 `resists small arms`; 1, 5 and 7 have no recovered keyword, and 8 acts as a
partial wildcard for four of the seven classes but **not** for classes 2, 4 or 6.

**Splash is charged as weapon type 0x21**, so only `resists explosives` (4) reduces it.

`DamageScalePlayerTeam` @ 0x006a7f28 and `DamageScaleOtherTeam` @ 0x006a7f2c are recomputed by
`RecomputeDamageScales` @ 0x004fc880, called once from `LoadLevel`:

```
DamageScalePlayerTeam = DamageScalePlayerByDifficulty[GameDifficulty]   // 0x006a7f08: 0.4 0.4 0.8 1.0
DamageScaleOtherTeam  = DamageScaleOtherByDifficulty[GameDifficulty]    // 0x006a7f18: 0.85 x4
// then, by level index:
idx >= 14 -> other *= 0.6667 ;  9..13 -> *= 0.7143 ;  7..8 -> *= 0.7692 ;  6 -> *= 0.8333
idx >=  5 -> player += (idx - 5) * (0.02 on difficulty 2/3, a larger step otherwise)
```

**Step 3 - shields and armour**, `MobileActor::ApplyDamage` @ 0x00535ac0, slot 68, `RET 0xc`,
`(float damage, bool use_defences, int attacker_team)`:

```
if (IsGodMode && TeamSlots[victim->team_id].+0x6a) return          // god mode
if (damage == 0) return

if (damage < 0) {                      // a heal
    heal = -damage
    // shields and armour are SKIPPED entirely
} else if (use_defences) {
    shield = *GetShieldValue()                       // slot 19 -> CharacterActor+0x2d0
    if (shield > 0) {
        if (damage > shield) { damage -= shield; ApplyShieldDamage(shield) }   // slot 21
        else                 { ApplyShieldDamage(damage); damage = 0 }
    }
    if (damage > 0) {
        ApplyArmorDamage(1.0f)                       // slot 20 - a FIXED one unit of wear
        armor = *GetArmorValue()                     // slot 18 -> Actor+0xf0
        damage = (damage > armor) ? damage - armor : 0
    }
}

if (damage > 0) strength (+0xf4) -= damage           // or += for a heal
if (strength > 0) strength = min(strength, character->strength)     // Character+0xa8 cap
else {
    strength = 0
    if (role->sever_points.count (+0xb0)) broadcast 0x8b (16 bytes, unreliable, gore)
    if (GameMode == Deathmatch && attacker_team != -1 && attacker_team != victim->team_id
        && role->name is one of {"", "gunlok", "maskelyn", "frend", "elint"})
        broadcast 0x9b {0x9b, attacker_team} (8 bytes, reliable)
    is_moving = false ; slot 89
}
```

Three things worth spelling out because they are counter-intuitive:

- **Armour is subtracted per hit, not as a percentage**, and it is a flat threshold: `armor >=
  damage` absorbs the whole hit. Shields are a pool that is drained; armour is a constant.
- **`ApplyArmorDamage` is always called with 1.0** - a damaging hit costs one unit of armour
  regardless of how big it was. In `CharacterActor` (slots 20/21) that destroys one armour /
  shield piece and broadcasts 0xa6/0xa8 and 0xa6/0xa7.
- **Healing bypasses defences.** A negative `damage` is negated and added straight to `strength`.
  That is exactly how the repair beam works: `repair_arm.gsh` sets `damage -1` on
  `Prj_RepairBeam`, so every impact adds `1 * damage_multiplier * DamageScale*` health, ignores
  shields and armour, and can never frag. The `-1` reaches `ScaleDamageForResistance`, which
  early-returns on `damage <= 0` - so **a heal is not scaled by difficulty and not resisted**.

`Actor::ApplyDamage` @ 0x0052f3b0 (the base, used by pickups, blockers, barrels, track objects) is
a **separate, simpler** implementation, not a superset: no god mode, **no shields**, armour is
tested as an absorb threshold the same way, and at zero strength it calls `Frag` (slot 64)
directly if `role->destructibility` (+0xa8) is non-NULL. It ignores its third argument entirely
(the `RET 0xc` still pops it).

### 7.2 Friendly fire

Damage is applied when

```
IsFriendlyFireEnabled()                                  // 0x00512a30, global IsFriendlyFireOn
|| victim_team == 0 || victim_team == 2
|| shooter_team == 0 || shooter_team == 2
|| (GameMode != SinglePlayer && GameMode != Cooperative && victim_team != shooter_team)
```

i.e. in single player / co-op a shot from team T hits an actor of team T only when one of the two
teams is 0 or 2, or friendly fire is on. **Team 2 (the player squad) always damages itself.** The
same expression guards both the direct hit (0x00543c86) and the splash (0x00543c86 again, per
victim).

### 7.3 Splash - no falloff

`ProjectileActor::OnPrePhysics`, from 0x00543a20:

```
if (projectile->blast_damage (+0x10) != 0) {
    d = this->damage_scale * blast_damage        // computed ONCE
    for each actor:
        if (actor == the directly hit actor) continue
        if (!actor->IsAlive()) continue
        if (projectile->blast_range_squared (+0x18) > dist2(actor->position, impact)) {
            <friendly-fire gate>
            ScaleDamageForResistance(d, ..., weapon_type = 0x21)
            actor->ApplyDamage(d, 1, this->team_id)
        }
}
```

`d` is loaded before the loop and never recomputed - **every actor inside the radius takes the
same damage**, there is no distance falloff. The comparison is strict (`blast_range_squared >
dist2`) and the direct victim is excluded, so nothing is double-damaged.

A broadcast 0x37 (0x14 bytes) `{id, strength, shield, armor}` follows each application; the
reliable flag depends on `BandwidthUse` and on whether the victim's team is player-controlled.

### 7.4 The `0x200` dissociate flag

After a direct hit, if `this->flags & 0x200` and the victim's team is not player-controlled, the
victim's `Dissociate` (slot 67) is called. That is the `interface arm` / hacking effect riding on
the projectile rather than on the damage.

---

## 8. Vulnerabilities fire on the ATTACK, not on the damage

This contradicts the natural reading of `role_system_notes.md` §10, which describes the data but
not when it is consumed. The evaluation is inside `CharacterActor::Update`, *before* the shot
(0x0053ff.. region, decompiled around the `attack_target` vulnerability walk), and nothing in
`ApplyDamage`, `OnPrePhysics` or `Frag` touches a `Vulnerability`.

Per frame while attacking, the shooter walks **the target's** `Actor::vulnerabilities` (+0x10) and
matches:

```
vuln->role      == shooter->entity                      // Vulnerability+0x00
&& (vuln->vuln_role == NULL || vuln->vuln_role == the projectile Role)   // +0x04
```

On the first match it latches `Actor+0x100 = 1`, `Actor+0x104 = now`,
`Actor+0x108 = now + vuln->delay` (+0x08, in seconds at this point) on the **shooter** and
broadcasts `0x51` (16 bytes). When `now >= Actor+0x108` it re-walks and dispatches on
`vuln->type` (+0x14):

| type | effect |
|---|---|
| 0 `Shutdown` | `target->+0x34 = 0`; if slot 14 (`IsMoving`) stop the movement; if slot 7 (`IsAttacking`) call slot 98 `StopAttacking` |
| 1 `Confusion` | SP/Coop: slot 78 `SetTarget(now, now + vuln->duration)`; MP: slot 80 `ChangeOwnerAndTeam(now, deadline, shooter_team)` |
| 2 `Destroy` | slot 64 `Frag` on the target, then stop |
| 4 `Script` | broadcast `0x62` (8 bytes) with the target id, then `QueueScriptExecution(vuln->script)` under `GL_Scripts`, `free` the string and null the field |

`VulnerabilityType::Charm` (3) has **no case in this switch** - `Confusion` (1) is the one that
carries the team change. Whether 3 is handled elsewhere is not established.

Note the consequence for `src/ScriptQueue.cpp`: `Vulnerability::script` is consumed here, on the
**main** thread inside slot 70, and the field is freed and nulled after the first fire - a
vulnerability's script runs at most once.

---

## 9. Barrels and destructible cover

"Several hits then explode" is just `strength` depletion: each hit runs through `ApplyDamage`,
and at `strength <= 0` `Actor::ApplyDamage` calls `Frag` (slot 64) provided
`role->destructibility` is set. Nothing counts hits.

`Actor::Frag` @ 0x0052e220 switches on `Destructibility::kind` (+0x04). **Case 3** is the
`frag data` variant (`role_subobjects_notes.md`: base 0x8 / `FragData` 0x24 /
`ReplaceDestructibility` 0x10):

```
this->flags |= 0x10                                  // gore
replace_role = FragData+0x0c
if (FragData+0x20 /*blast damage*/ != 0 && FragData+0x1c /*blast range*/ > 0) {
    r2 = FragData+0x1c * FragData+0x1c               // SQUARED HERE, at use
    for each actor: if (IsAlive() && !(flags & 0x10) && dist2 < r2) {
        ScaleDamageForResistance(FragData+0x20, victim resistance, ..., weapon_type)
        victim->ApplyDamage(...)                     // 0x0052e637
    }
}
... GetRoleByName("frag_projectile") + SpawnProjectileActor per debris piece ...
```

So there **are** two splash implementations - `ProjectileActor::OnPrePhysics` for a projectile's
`blast range`/`blast damage`, and `Actor::Frag` for a `frag data` section's - but they share
`ScaleDamageForResistance` and slot 68, and both are flat (no falloff). The difference that
matters for content authoring:

| section | field | stored | squared where |
|---|---|---|---|
| `projectile` | `blast range` 0x28 | `Projectile+0x14` metres | `ToProjectile` -> `+0x18` |
| `projectile` | `max range` 0x2d | `Projectile+0x0c` **already squared by the author** | never |
| `frag data` | `blast range` 0x28 | `FragData+0x1c` metres | inside `Frag`, per call |

Both spellings of `blast range` are therefore metres; only `max range` is the odd one out. That
the shipped `frag data` sections (`blobarrelfrag` 5/25, `oildrumfrag` 4/20, `minefrag` 5/30,
`technobox` 7/2500) are the only users of `blast range` is a data fact, not a code restriction -
a `projectile` with a blast works fine and is what §7.3 handles.

---

## 10. The flamethrower's `blast 5`

`flamethrower.gsh:40` and `:80` write a bare `blast 5 // proximity damage`. **There is no `blast`
keyword in the binary**: the only field-name strings are `'blast damage'` @ 0x00663944 and
`'blast range'` @ 0x00663954, and a full scan of defined strings finds no other token containing
"blast".

`Prj_FlameThrower` therefore ends up with `blast range 5` (from the *next* line, which is a valid
keyword) and `blast damage` at its section default. `ProjectileActor::OnPrePhysics` gates the
whole splash on `blast_damage != 0`, so **the flamethrower has no proximity damage**; the line was
meant to say `blast damage 5`.

The keyword looks like a feature that was cut rather than a typo: `interface_device.gsh:41` and
`repair_arm.gsh:38` both write `blast 0 // proximity damage (not yet supported)`, in the authors'
own words. Four shipped `projectile` sections use it and all four are inert.

What is *not* established is whether the line produces a parse error or a discarded warning.
`defaults.gsh` includes `flamethrower.gsh` and every level loads, and a GLS syntax error poisons
`LoadGLS` for the whole process (`CLAUDE.md`), so empirically it must be tolerated - but that is an
inference from "the game works", not a measurement. `gls.try_parse` on those two lines would
settle it in one REPL call.

---

## 11. Reference: addresses

| Address | Signature | What |
|---|---|---|
| 0x0053d8d0 | `__thiscall void(CharacterActor*, u32 lo, u32 hi, float now)` slot 70 | the whole character fire path |
| 0x0054b000 | slot 70 | turret fire path |
| 0x00540c60 | `__thiscall void(CharacterActor*, Actor *target, int range, char mode, char close_range)` `RET 0x10` slot 96 | attack an actor |
| 0x00540a10 | same with `Vec3*` slot 97 | attack a position |
| 0x00540f20 | slot 98 | stop attacking, broadcast 0x44 |
| 0x00541ed0 | slot 94 | replace the `Weapon`, broadcast 0x83 |
| 0x00541b90 | slot 99 | set ammo type, broadcast 0x82 |
| 0x00541c90 | `__fastcall float*(float *out, Vec3 *delta, float *speed, char *gravity, char *low_arc)` `RET 0xc` | **`SolveLaunchPitch`** |
| 0x0045d4a0 | `__fastcall int(float *r0, float *r1, float *a, float *b, float *c)` `RET 0xc` | **`SolveQuadraticNormalized`** |
| 0x00541800 | `__fastcall int(CharacterActor*)` | **`CharacterActor_SelectAmmo`** |
| 0x00541700 | `__fastcall int(CharacterActor*)` | **`CharacterActor_ConsumeRoundAndReload`** |
| 0x005423c0 | `__fastcall void(CharacterActor*)` | **`CharacterActor_RefreshCanFire`** |
| 0x004b1da0 | `__fastcall void(Weapon*, int)` | **`SetWeaponAmmoType`** |
| 0x00536b20 | `__thiscall void(MobileActor*, int)` | **`MobileActor_RecomputeAimSpread`** |
| 0x00536ba0 | `__thiscall` | equip an inventory item (module effects, `Role::limit` test) |
| 0x00503bd0 | `__fastcall` `RET 0x14` | `SpawnProjectileActor` (script/effect side) |
| 0x00542410 | `RET 0x24` | `ProjectileActor::Ctor` |
| 0x00542ae0 | `__thiscall void(ProjectileActor*, +3)` `RET 0xc` slot 55 | `ProjectileActor::OnPrePhysics` - impact, damage, splash |
| 0x0052f3b0 | `__thiscall bool(Actor*, float, bool, int)` `RET 0xc` slot 68 | `Actor::ApplyDamage` |
| 0x00535ac0 | same slot 68 | `MobileActor::ApplyDamage` |
| 0x0052e220 | `__thiscall void(Actor*)` slot 64 | `Actor::Frag` |
| 0x004fc740 | `__fastcall float*(float*, char, float, int, float, int)` `RET 0x10` | **`ScaleDamageForResistance`** |
| 0x004fc880 | `__stdcall void()` | **`RecomputeDamageScales`** |
| 0x00512a30 | `bool()` | **`IsFriendlyFireEnabled`** |

Globals:

| Address | Name | Value / meaning |
|---|---|---|
| 0x006a8688 | `GravityAcceleration` | 9.81, used by `SolveLaunchPitch` |
| 0x006a8250 | `ProjectileGravity` | 9.81, a second copy read only by `SpawnProjectileActor` |
| 0x00666234 | `CloseRangeSpeedFactor` | 0.65 |
| 0x006a7f28 / 0x006a7f2c | `DamageScalePlayerTeam` / `DamageScaleOtherTeam` | recomputed per level |
| 0x006a7f08 / 0x006a7f18 | `DamageScalePlayerByDifficulty` / `DamageScaleOtherByDifficulty` | `float[4]` |
| 0x007b5d40 / 0x007b5ec0 | `AmmoInfos` / `AmmoTable` | `AmmoInfo[19]` / `Ammo*[19*34]` |
| 0x007b9cf8 / 0x007b9d00 | `MissionShotsFired` / `...Team2` | incremented on the fire path |
| 0x007b9cfc / 0x007b9d04 | `MissionShotsHit` / `...Team2` | incremented in `OnPrePhysics` |

Struct corrections against `src/Actors.h` (all now applied in the DB):

| Field | Was | Is |
|---|---|---|
| `Weapon+0x00` | `field field0x0` | `Role *projectile_role` |
| `Weapon+0x04` | `float range` | `float muzzle_speed` |
| `Weapon+0x0c` | `ammo_rate` | `shot_interval` (`Ammo::round_time` x ticks/s) |
| `Weapon+0x10` | `ammo_param` | `reload_interval` |
| `Weapon+0x18` | "threshold" | `magazine_size` |
| `Weapon+0x1c` | "threshold" | `salvo_size` (rounds per shot) |
| `CharacterActor+0x2bc` | `aim_direction` | the **muzzle world position** (for characters too, not just turrets) |
| `CharacterActor+0x2ec` | `weapon_range` | `muzzle_speed` |
| `CharacterActor+0x2f4` | `int` gun pitch | `float gun_pitch_bam`, written from `SolveLaunchPitch` |
| `CharacterActor+0x304` | "cannot-fire gate" | `rounds_in_magazine` |
| `MobileActor+0x1b8` | unnamed | `aim_level` (module count) |
| `Actor+0x74` | unnamed | `consecutive_misses` |
| `Role+0x54` `limit` | no reader recorded | weapon/module exclusion mask, tested against inventory item `+0x30` |

---

## 12. Uncertain / not established

- **The L / V / delta target indicators.** Not found. `DrawTargetInfoPanel` @ 0x004a86c0 is the
  *recon* panel (`GL_RECON_DAMAGE_FACTOR`, `GL_RECON_HITS_REQUIRED`, `GL_RECON_CURRENT_WEAPON`) and
  is a different feature: it runs the victim's role through `ScaleDamageForResistance` to display
  a damage factor and a hits-required count. The nearest thing to an in-range predicate that the
  *engine* uses is §5's `dist2 <= projectile->max_range`, and detection is
  `Character::sight_range_squared` (+0x2c); whether the glyphs are those two tests was not
  measured.
- **"Greater height gives some weapons greater range" is not reproduced by the range gate.** That
  gate is a 3D sphere, so height costs horizontal reach. Two candidate mechanisms, neither
  confirmed: (a) the ballistic quadratic has real roots at a longer horizontal distance when
  firing downhill, so a shot that would otherwise be refused becomes solvable; (b) the elevation
  clamp in §3.3 bites less. Distinguishing them needs a run in the game.
- **`SolveLaunchPitch` ignores `SolveQuadraticNormalized`'s return value.** When the discriminant
  is negative the two root slots are never written (the early `return 0` precedes every store) and
  the caller reads uninitialised stack from an `_eh_vector_constructor_iterator_` with an empty
  element constructor. Reported here as a defect, but the *consequence* in play (a wild pitch on an
  unreachable ballistic target) has not been observed.
- **`GameDifficulty` index -> difficulty name** is not established, so `0.4 / 0.4 / 0.8 / 1.0`
  cannot yet be read as "easy..hard".
- **`VulnerabilityType::Charm` (3)** has no case in the slot-70 dispatch. Either it is handled
  somewhere else or it is dead; not checked.
- **Inventory item layout** was only read where combat needs it: `+0x00` kind (4 weapon,
  6 ammo, 3 shield, 7 module, 9 armour), `+0x20` ammo type / module subtype, `+0x2c` count,
  `+0x30` the mask tested against `Role::limit`, `+0x18` the module magnitude. No struct was
  defined.
- **`CharacterActor+0x2a4`** (`weapon_type` in the DB) was not shown to be read anywhere on the
  fire path; the weapon type the code actually uses is `Weapon+0x08`. Left alone.
- **`CharacterActor+0x2f0`** is presumably the gun yaw twin of `+0x2f4` but no writer was traced;
  left as `int field_0x2f0`.
- **`Ammo::firing_speed`** keeps its GLS-keyword name in `src/Roles.h` and in the DB, but it is the
  muzzle speed. A DB field comment now says so; renaming it would desync the mirror, so the repo
  mirror is the place to decide.
- **`flamethrower.gsh`'s `blast 5`** - see §10; the parse outcome (error vs discarded warning) is
  inferred, not measured.
