# Gunlok Actor VTable - Reverse Engineering Notes

Slot numbers are only meaningful **within a branch of the hierarchy**. `Actor` owns slots 0-82;
`MobileActor` adds 83-94 and `CharacterActor` 95-99 on top of those, but `PickupActor` and
`ProjectileActor` add *their own* slots 83-85 / 83-84 that have nothing to do with MobileActor's.
Never compare a slot index across branches.

## Class Hierarchy (object size / vtable address / vtable slot count)

```
Actor (0x120) vtbl @ 0x00667e30 [83]
 +-- MobileActor (0x230) vtbl @ 0x00667f7c [95]
 |    +-- CharacterActor (0x308) vtbl @ 0x006680f8 [100]
 |    |    +-- CentibodyActor (0x310) vtbl @ 0x00668be0 [100]
 |    |    |    +-- CentipedeActor (0x310) vtbl @ 0x00668d70 [100]
 |    |    +-- PopupActor (0x310) vtbl @ 0x00668f00 [100]
 |    |         +-- TurretActor (0x320) vtbl @ 0x00669090 [105]
 |    +-- NodeActor (0x278) vtbl @ 0x00668a64 [95]
 |    +-- PresidentActor (0x240) vtbl @ 0x00669380 [96]
 +-- PickupActor (0x150) vtbl @ 0x006683dc [86]
 +-- TrackObjectActor (0x1b8) vtbl @ 0x00668534 [83]
 +-- TumbleweedActor (0x120) vtbl @ 0x00668680 [83]
 +-- BackgroundCreatureActor (0x120) vtbl @ 0x006687cc [83]
 |    +-- FlyingBackgroundCreatureActor (0x120) vtbl @ 0x00668918 [83]
 +-- BlockerActor (0x130) vtbl @ 0x00669234 [83]
 +-- ProjectileActor (0x178) vtbl @ 0x00668288 [85]
```

Every size is confirmed three ways: slot 35 `GetSize` returns it, slot 0's
`scalar_deleting_destructor` passes it to `Dealloc_`, and the Ghidra struct has that length.

> **`ProjectileActor` is 0x178, not 0x120.** Earlier revisions of this file and of `CLAUDE.md` said
> 0x120, which cannot be right: its own extension slots read `+0x150` and write `+0x168..0x174`.
>
> **`PresidentActor`'s vtable is 96 slots, not 84.** It is the last vtable in `.rdata`, so the
> "ends at the next vtable" shortcut under-counts it; it actually runs to 0x00669500, where the
> string pool starts. The DB's `PresidentActorVtbl` (len 384) already had this right.

**`ProjectileActor` was called `UnknownActor`** until the evidence below settled what it is. The
rename has been applied throughout: the class namespace and its 14 member functions, the
`ProjectileActor` / `ProjectileActorVtbl` types, the vtable label at 0x00668288, the two slot
funcdefs, and slot 38's `IsProjectile`. Older notes and any external write-ups will still say
`UnknownActor`.

## Constructors and destructors

Slot 0 is `scalar_deleting_destructor`, a thunk that calls the real destructor body and then
conditionally `Dealloc_`s. Every destructor body used to be a `FUN_`/mis-demangled symbol; they are
now named `<Class>::Destructor`.

| Class | Ctor | Dtor body | slot 0 thunk |
|---|---|---|---|
| Actor | 0x0052d1f0 | 0x0052d9c0 | 0x0054e2d0 |
| MobileActor | 0x005324b0 | 0x00532b00 | 0x0054e330 |
| CharacterActor | 0x0053c700 | 0x0053ca70 | 0x0054e510 |
| CentibodyActor | 0x00549c40 | 0x00549c80 | 0x0054e390 |
| CentipedeActor | 0x00549d10 | 0x00549d40 | 0x0054e3c0 |
| PopupActor | 0x0054a8a0 | 0x0054a8e0 | 0x0054e480 |
| TurretActor | 0x0054aed0 | 0x0054af20 | 0x0054e5a0 |
| NodeActor | 0x00549640 | 0x00549870 | 0x0054e420 |
| PresidentActor | 0x0054d3d0 | 0x0054d450 | 0x0054e4b0 |
| PickupActor | 0x00545fd0 | 0x005460c0 | 0x0054e450 |
| TrackObjectActor | 0x00547270 | 0x005473c0 | 0x0054e540 |
| TumbleweedActor | 0x00549400 | 0x00549500 | 0x0054e570 |
| BackgroundCreatureActor | 0x00549510 | 0x005495e0 | 0x0054e300 |
| FlyingBackgroundCreatureActor | 0x00549600 | 0x00549630 | 0x0054e3f0 |
| BlockerActor | 0x0054c940 | 0x0054c9f0 | 0x0054e360 |
| ProjectileActor | 0x00542410 | 0x00542670 | 0x0054e4e0 |

`PickupActor::Destructor` was named `Actor::Ctor006683dc` - a *destructor* labelled as a
constructor. `BlockerActor::Destructor` carried a bogus demangled
`Concurrency::call<unsigned int, std::function<...>>::~call<...>` symbol. Neither name meant
anything; both are examples of the "existing names are not evidence" rule.

**And the pattern is not a one-off.** The *client* counterpart, `BlockerUnit::~BlockerUnit`
@ 0x004cd620, carried the **same** stray `Concurrency::call<...>::~call<...>` symbol until the
client `Unit` tree was named (`rendering_notes.md` §5.1) - so a bogus demangled template on a
blocker destructor has now been seen in both trees, and "no name at all" is a better prior than a
plausible-looking library one.

## Actor Factory / Storage

`CreateActor` @ 0x00510760 dispatches on `role->ai` to the right subclass. Actors live in the hash
map `actors` @ 0x007ba0d8; `GetActorById` @ 0x0044e0b0; `num_actors` @ 0x007b9ffc.

## Base `Actor` VTable (83 slots)

Five slot names in this table were wrong and have been corrected in the DB. The evidence is
recorded as a plate comment on each function.

| Old name | New name | Why |
|---|---|---|
| `IsSpecialType_0` (7) | `IsAttacking` | CharacterActor's override is `MOV AL,[ECX+0x2d4]` = `is_attacking` |
| `IsSpecialType_1` (8) | `IsConcealed` | MobileActor's override is `MOV AL,[ECX+0x186]` = `is_concealed` - camouflage in water or a scrap pile, which every AI acquisition loop skips on. Was `IsMine`, which described nothing: a mine is an `ai mine` role, not this flag |
| `OnUpdate` (9) | `SetConcealed` | base is `RET 0x4` - it *takes* an argument; MobileActor's stores the byte into `+0x186`. It is the setter paired with slot 8, not a tick callback |
| `GetSecondaryWeapon` (15) | `GetAttackTarget` | CharacterActor's override returns `+0x2d8` = `attack_target`, not a weapon |
| `OnPostUpdate` (54) | `SetField0x188` | base is `RET 0x4`; MobileActor's stores the byte into `+0x188`, pairing with slot 30's getter |
| `GetMovementState` (16) | `GetInventory` | returns MobileActor `+0x194`, which the ctor null-inits, `SetTeamId` `malloc(0x44)`s into, `EquipItemInSlot`/`UseInventoryItem`/`ReceiveObject` read and the destructor frees - a container pointer, not a state enum |
| `HasInventory` (32) | `HasPendingOrders` | tests MobileActor `+0x1f4`, the **order-queue** count, not inventory |

### Slots 0-35

| Slot | Addr | Name | Base behaviour |
|---|---|---|---|
| 0 | 0x0054e2d0 | `scalar_deleting_destructor` | destroy, optionally free 0x120 |
| 1 | 0x005301a0 | `OnCreate` | no-op |
| 2 | 0x0052dbc0 | `SetHealth` | set health, broadcast 0x55 |
| 3 | 0x0054eab0 | `GetHealth` | out-param from `+0xf8` |
| 4 | 0x0054ed10 | `GetCenterCoords` | position with Y raised by half height |
| 5 | 0x0054ec10 | `GetStrengthRatio` | current / max strength |
| 6 | 0x0054f100 | `IsAlive` | `!is_dead` (`+0x115`) |
| 7 | 0x0054f130 | `IsAttacking` | false |
| 8 | 0x0054f170 | `IsConcealed` | false |
| 9 | 0x0054e890 | `SetConcealed(bool)` | `RET 4`, discards |
| 10 | 0x0054ea30 | `GetCharacterData` | `entity->character` |
| 11 | 0x0054ef80 | `GetWeapon` | NULL |
| 12 | 0x0054eb30 | `GetField0x118` | `+0x118` |
| 13 | 0x0054f0c0 | `IsEnabled` | true |
| 14 | 0x0054f1e0 | `IsMoving` | false |
| 15 | 0x0054ef30 | `GetAttackTarget` | NULL |
| 16 | 0x0054eb10 | `GetInventory` | NULL |
| 17 | 0x0054e650 | `HasCustomisationHierarchy` | false |
| 18 | 0x0054e8c0 | `GetArmorValue` | `+0xf0` |
| 19 | 0x0054ebd0 | `GetShieldValue` | global 0.0 |
| 20 | 0x0054f280 | `ApplyArmorDamage` | no-op |
| 21 | 0x0054f290 | `ApplyShieldDamage` | no-op |
| 22 | 0x0054eb60 | `GetAmmoCount` | 100 |
| 23 | 0x0054ebb0 | `GetField0x304` | 0 |
| 24 | 0x0054eb70 | `GetNavAgent` | NULL - MobileActor returns the actor's `NavAgent` (nav-mesh movement/collision agent, `+0x200`; see `src/Actors.cpp`) |
| 25 | 0x0054ead0 | `GetField0x18c` | 0 - MobileActor's returns a **team-slot index** |
| 26 | 0x0054f3f0 | `SetField0x18c` | no-op (ignores its arg) |
| 27 | 0x0054f370 | `Stub27` | no-op, never overridden anywhere |
| 28 | 0x0054f360 | `SetArmorValue` | `+0xf0` |
| 29 | 0x0054f440 | `SetShieldValue` | no-op |
| 30 | 0x0054f510 | `GetField0x188` | false |
| 31 | 0x0054eaf0 | `GetHotspot` | NULL |
| 32 | 0x0054f0e0 | `HasPendingOrders` | false |
| 33 | 0x0054e680 | `SetTeamId` | `this->team_id = arg` |
| 34 | 0x0054ef10 | `GetInventoryListPtr` | NULL - MobileActor returns `&this[0x19c]` |
| 35 | 0x0054e930 | `GetSize` | 0x120 |

### Slots 36-50 - type checks (no RTTI in this binary)

All return false in `Actor`. **All fifteen are now accounted for** - the previous "three unknown
type-check slots" note was stale.

| Slot | Addr | Name | Overridden by |
|---|---|---|---|
| 36 | 0x0054e6c0 | `IsMobile` | MobileActor |
| 37 | 0x0054e800 | `IsCharacter` | CharacterActor |
| 38 | 0x0054e7e0 | `IsProjectile` | **ProjectileActor** |
| 39 | 0x0054e820 | `IsTrackObject` | **TrackObjectActor** |
| 40 | 0x0054e760 | `IsNode` | NodeActor |
| 41 | 0x0054e720 | `IsCentipede` | CentipedeActor |
| 42 | 0x0054e700 | `IsCentibody` | CentibodyActor |
| 43 | 0x0054e6a0 | `IsBackgroundCreature` | BackgroundCreatureActor |
| 44 | 0x0054e740 | `IsFlyingBackgroundCreature` | FlyingBackgroundCreatureActor |
| 45 | 0x0054e780 | `IsPickup` | PickupActor |
| 46 | 0x0054e840 | `IsTumbleweed` | TumbleweedActor |
| 47 | 0x0054e7a0 | `IsPopup` | PopupActor |
| 48 | 0x0054e6e0 | `IsBlocker` | **BlockerActor** |
| 49 | 0x0054e7c0 | `IsPresident` | PresidentActor |
| 50 | 0x0054e860 | `IsTurret` | TurretActor |

Only the class that *introduces* the override appears here; its descendants inherit it. That is why
`TurretActor` is not listed against 47 even though `IsPopup()` is true for a turret.

### Slots 51-82

| Slot | Addr | Name | Base behaviour |
|---|---|---|---|
| 51 | 0x0052dcc0 | `InitPositionAndTiming` | timing fields + spatial lookup, caches the nav poly into `+0x118` |
| 52 | 0x0052de60 | `ReleaseAttachment` | ref-counted release of `+0x40` |
| 53 | 0x0052ded0 | `SetPositionAndOrientation` | coords + quaternion + timing |
| 54 | 0x0054f4f0 | `SetField0x188(bool)` | `RET 4`, discards |
| 55 | 0x0054f270 | `OnPrePhysics` | no-op |
| 56 | 0x0054efa0 | `PathToTarget` | `RET 0x8` stub, arguments discarded; `MobileActor::PathToTarget` @ 0x00539930 is the real body - it calls `FindNavPathWithinRadius` @ 0x0052c100 and pushes the result through slot 90 (`navigation_notes.md` §5.4) |
| 57 | 0x0052e0a0 | `Raycast` | ray/shape intersection |
| 58 | 0x0052df50 | `SweepTest` | swept intersection |
| 59 | 0x0054f4e0 | `OnDamageReceived` | no-op - see PickupActor, where the slot is really `SetPickupType` |
| 60 | 0x0054f1a0 | `IsTargetable` | false |
| 61 | 0x0054f250 | `IsVisible` | `+0x10d` |
| 62 | 0x0054f200 | `IsInteractable` | false |
| 63 | 0x0054f150 | `IsCrouched` | false. Was `CanBePickedUp`, refuted by the override column: `PickupActor` carries *this* base (`XOR AL,AL`), so the one class that can be picked up always answered false. Only MobileActor and below override it, returning `+0x187` |
| 64 | 0x0052e220 | `Frag` | scoring, splash, debris |
| 65 | 0x0052f0d0 | `Delete` | sets `is_dead`, cleanup, broadcast 0x49 |
| 66 | 0x0054e640 | `Associate` | no-op |
| 67 | 0x0054e880 | `Dissociate` | no-op |
| 68 | 0x0052f3b0 | `ApplyDamage` | damage/heal, fragging at 0 |
| 69 | 0x0054f2a0 | `OnHealthChanged` | no-op |
| 70 | 0x0052f8a0 | `Update` | the per-tick entry point, invoked by `ExecutorActorTick` @ 0x0052fad0 through `vtbl+0x118`. The base body ages a timer, stores the tick clock into `+0xc8`/`+0xd0` and the seconds into `+0xd8`, pushes the transform to `anim_object`, and broadcasts 0x6f **only** when `position_set` is set |
| 71 | 0x005300e0 | `PlayAnimation` | |
| 72 | 0x00530120 | `BlendAnimation` | |
| 73 | 0x00530150 | `PlayAnimationEx` | arg 7 is the address of a completion flag |
| 74 | 0x00530180 | `SetAnimationState` | operates on `anim_object` (`+0xe0`) |
| 75 | 0x0054f230 | `HasCustomAnimation` | false |
| 76 | 0x0054f4d0 | `OnAnimationComplete` | no-op |
| 77 | 0x0054f350 | `OnAnimationEvent` | no-op |
| 78 | 0x00530270 | `SetTarget` | broadcast 0x56 |
| 79 | 0x00530390 | `ClearTarget` | broadcast 0x57 |
| 80 | 0x00530470 | `ChangeOwnerAndTeam` | broadcasts 0x58 + 0x50 |
| 81 | 0x00530650 | `ReleaseFromOwner` | broadcasts 0x59 + 0x50 |
| 82 | 0x005308d0 | `ActivateInWorld` | re-register in spatial/team structures, set flag 0x200 |

> Slot 70 is the per-tick update, and it is `Update` in all ten of its distinct bodies. What each
> one does with the tick differs: in `MobileActor` (0x00533720, 0x236a bytes, 46 distinct callees)
> it is mode dispatch, enemy acquisition, movement integration, nav stepping, order dispatch and
> the deployable state machine; in `CharacterActor` (0x0053d8d0) the attack/weapon tick; in
> `TurretActor` the firing solution; in `TrackObjectActor` the spline motion; in `ProjectileActor`
> the flight step, which broadcasts nothing of its own. The *position sync* is a behaviour of the
> `Actor` base body only - and even there it is conditional on `position_set`.
>
> Slot 82 has the same problem in reverse: `MobileActor`'s override disposes of the inventory on
> death and `BlockerActor`'s *un*-blocks nav polygons. Both then chain to the base, which does
> activate. Treat the name as describing the base only.

## Actor fields pinned by the slot bodies

Fields whose meaning was settled by reading the code that touches them, not by their position.

| Off | Type | Meaning | What pins it |
|---|---|---|---|
| 0x04 | `int` | **reference count** | the retain/release pair in `AiBeginInvestigate` @ 0x0045e050: `INC dword ptr [ESI+0x4]` to retain, and on release decrement then call slot 0 with the scalar-delete flag when it reaches zero |
| 0x24 | `float` | deadline in seconds firing **slot 79** (`ClearTarget`); 0.0 means disabled | `AiThink_Waiting` @ 0x00456bed - `MOVSS XMM0,[ESI+0x24]`, compared in seconds against the scaled clock, then `CALL [vtbl+0x13c]` |
| 0x2c | `float` | deadline in seconds firing **slot 81** (`ReleaseFromOwner`); 0.0 means disabled | the same shape @ 0x00456c21 -> `CALL [vtbl+0x144]` |
| 0x38 | `void (*)(Actor *, uint, int)` | the **on-placed hook**, called once from slot 51 (`InitPositionAndTiming`) @ 0x0052dd18 under a null guard, immediately after the nav-poly lookup. It is *not* a think proc - every `AIType` installs its per-tick proc into `+0x34`, and `ExecutorActorTick` reads neither field | `Actor_SetAiBehaviour` @ 0x00450610 is the only writer, and only for `AIType::Mine` (`Mine_OnDeployed` @ 0x0045a640) |
| 0xd8 | `float` | game time in seconds of the current tick | written by the slot-70 base body @ 0x0052f91a from its third argument |
| 0xdc | `float` | the `+0xd8` value as of the last state broadcast | @ 0x0052fa66, and by all four `MobileActor` motion broadcasters, each of which ends `+0xdc = +0xd8` |

## Override matrix

Which slots each subclass replaces, relative to its immediate parent. Slots not listed are
inherited unchanged. Every leaf overrides slot 0 (`scalar_deleting_destructor`) and slot 35
(`GetSize`); those are omitted below since they are universal and mechanical.

| Class | Parent | Overrides (beyond 0/35) | New slots |
|---|---|---|---|
| MobileActor | Actor | 4,5,6,8,9,10,12,14,16,17,24,25,26,30,32,33,34,36,51,52,53,54,56,60,61,62,63,67,68,69,70,82 | 83-94 |
| CharacterActor | MobileActor | 7,11,15,19,20,21,22,23,29,31,33,37,51,52,65,70,85,86,87,94 | 95-99 |
| CentibodyActor | CharacterActor | 42,51,70,89 | - |
| CentipedeActor | CentibodyActor | 41,51,70 | - |
| PopupActor | CharacterActor | 47,51,70 | - |
| TurretActor | PopupActor | 50,70,80,81,96,97,98 | 100-104 |
| NodeActor | MobileActor | 40,51,90 | - |
| PresidentActor | MobileActor | 33,49,51,70 | 95 |
| ProjectileActor | Actor | 38,51,52,55,57,58,70,82 | 83-84 |
| PickupActor | Actor | 13,18,45,51,57,58,59,66,70,75 | 83-85 |
| TrackObjectActor | Actor | 39,51,52,64,70,76,77 | - |
| TumbleweedActor | Actor | 46 | - |
| BackgroundCreatureActor | Actor | 43,82 | - |
| FlyingBackgroundCreatureActor | BackgroundCreatureActor | 44 | - |
| BlockerActor | Actor | 48,51,82 | - |

Nine of the sixteen classes add **no** slots at all; `TumbleweedActor` and
`FlyingBackgroundCreatureActor` only override their type check. That is why their vtables are
typed as plain `ActorVtbl` in the DB.

## Subclass extension slots

### MobileActor, slots 83-94

| Slot | Addr | Name | Notes |
|---|---|---|---|
| 83 | 0x00536090 | `ToggleCrouchAndCamouflage` | flips `is_crouched` (`+0x187`, read at 0x00536105, written at 0x00536110), sets `is_concealed` (`+0x186`) at 0x005362a1/0x005362cb and clears it on standing up at 0x005362dc, tests nav-polygon bit **0x800** (`SHR EAX,0xb` @ 0x005362c6 - the water/camouflage-terrain bit, `stealth_and_fog_notes.md` §2), swaps the collision box between a standing and a halved box, and broadcasts `0x4c + 2*(!is_crouched)` (`LEA EAX,[EAX*0x2 + 0x4c]` @ 0x00536783). Gated on model node 0x13 existing. Nothing in it detects mines |
| 84 | 0x00536830 | `EquipToFirstOpenSlot` | first free `slota`..`sloth` (indices 2-9) from the `+0x19c` list |
| 85 | 0x0054e5d0 | `QueueOrderKind10` | was `OnPreThink`; appends a tag-10 order record |
| 86 | 0x0054e5f0 | `QueueOrderPosition` | was `OnPostThink`; appends a tag-1 record with a Vec3 |
| 87 | 0x0054e5e0 | `QueueOrderTarget` | was `OnPreUpdate`; appends a tag-0 record |
| 88 | 0x00539450 | `Goto` | move order, gated on `can_turn`, strength and priority |
| 89 | 0x0053a020 | `Die` | broadcasts 0x3d (destructible) or 0x48 (not), then slots 82 and 64 |
| 90 | 0x0053a760 | `AddWaypoint` | 0x18-byte record pushed onto the `+0x204` list |
| 91 | 0x0053b560 | `GetNavigationTarget` | fills a 0x24-byte descriptor |
| 92 | 0x0054eaa0 | `GetField0x198` | returns `+0x198`, a `Hierarchy*` override |
| 93 | 0x0053baa0 | `PlayActionAnimation` | gated on a busy flag and a timestamp |
| 94 | 0x0054e690 | `SetWeapon` | was `OnMobileDamageReceived`; CharacterActor's override frees the old 0x28-byte weapon and allocates a new one (0x21 = none), broadcasting 0x83 |

Slots 85/86/87 all `malloc(0x28)`, fill a tagged record and append it to the list header at
`+0x1f0`. `MobileActor::ClearOrderQueue` @ 0x00538830 drains that list and `Dealloc_(node->data, 0x28)`, which is
what proves the payload size. The queue is serialized by `Read`/`WriteActorFixups`, so pending
orders survive a save/load. Slot 32 tests its count.

### CharacterActor, slots 95-99

| Slot | Addr | Name | Notes |
|---|---|---|---|
| 95 | 0x0054f430 | `SetField0x304` | setter for the cannot-fire gate |
| 96 | 0x00540c60 | `AttackTarget` | broadcast id is computed: `0x41 + close_range` |
| 97 | 0x00540a10 | `AttackPosition` | broadcast id is computed: `0x3f + close_range` |
| 98 | 0x00540f20 | `StopAttacking` | broadcast 0x44 |
| 99 | 0x00541b90 | `SetAmmoType` | was `ExecuteSpecialAbility`; sets the weapon's ammo type (ECX = `weapon_data`, EDX = ammo id), reselects ammo, broadcasts 0x82 |

### TurretActor, slots 100-104

| Slot | Addr | Name |
|---|---|---|
| 100 | 0x0054e8b0 | `SetTurretEnabled` (`+0x310`) |
| 101 | 0x0054ee40 | `GetTurretAimDirection` (`+0x2bc`) |
| 102 | 0x0054eb90 | `GetTurretTargetAngles` (`+0x318`/`+0x31c`) |
| 103 | 0x0054f190 | `IsTurretEnabled` |
| 104 | 0x0054f410 | `SetTurretTargetAngles` |

The three `TurretActor`-only fields are `turret_enabled` (byte `+0x310`), `target_angle_yaw`
(`+0x318`) and `target_angle_pitch` (`+0x31c`); slot 102 reads the yaw/pitch pair as a single
int64. The dword at `+0x314` is alignment padding to 8-align that pair — the constructor
(0x0054aed0) initialises `+0x310`/`+0x318`/`+0x31c` but skips it, and no Actor-family function
reads or writes it.

Curiously, `TurretActor::Update` (the firing solution) touches **none** of
`+0x310`, `+0x318` or `+0x31c`. The angles it actually integrates are `CharacterActor+0x2f0` and
`+0x2f4`, which are typed `int` in the DB but used as floats (gun yaw/pitch in 4096-unit brads).

### PresidentActor, slot 95

| Slot | Addr | Name |
|---|---|---|
| 95 | 0x0054f380 | `SetExitPosition(Vec3)` - writes `+0x230..0x23b`. Was `PresidentMethod` |

### ProjectileActor, slots 83-84

| Slot | Addr | Name |
|---|---|---|
| 83 | 0x0054eba0 | `GetProjectileFlags` - returns `+0x150`, used throughout as a **bitfield** (0x10 gore, 0x40 guided, 0x200 dissociate). The field was named `limit`, which is only the GLS keyword it is loaded from (`Role+0x54`), not its meaning |
| 84 | 0x0054f460 | `SetTargetPosition` - writes a Vec3 by value to `+0x168`, the guidance/arrival target |

### PickupActor, slots 83-85

| Slot | Addr | Name |
|---|---|---|
| 83 | 0x00546240 | `SetPickupEnabled` - stores the byte at `+0x120` and, when `+0x12c == 0`, schedules `+0x144 = GetGameTimeSeconds() + MPRespawnDelay * k` with `k` selected by `+0x140` (1 -> x2, 2 -> x1, 3 -> x0.5). Broadcasts 0x85 |
| 84 | 0x00546440 | `OnPickedUp` - first enforces the REQUIRES gate: if `required_item_name` (`+0x138`) is set, the pickup is denied unless the collector's inventory already holds a role of that name (`CountInventoryItemsWithRoleName` @ 0x004e5240 != 0), broadcasting 0x74 and, for a `"key"`-named requirement, the `GL_INMISSION_2` message (0xb3). Once past the gate: broadcasts 0x74, 0x75, 0x8c, 0x4f and, when `associated_script` is set, calls `QueueScriptExecution` (see `directplay_protocol_notes.md` §8.11) |
| 85 | 0x00546b20 | `SetRequiredItem` (was `SetField0x138`) - the `REQUIRES [name] [reqd name]` console command sink: frees and `strdup`s the required-item role name into `required_item_name` (`+0x138`). **Not** `associated_script`, which is `+0x134` (the `ASSOCIATE` script) |

`PickupActorVtbl` used to be an untyped `void *[86]`; it is now a proper struct like the others, so
its slots have real cross-references. An **undefined** vtable gives its slots no xrefs at all, which
reads as "this virtual has no callers" - check that a vtable is defined before drawing that
conclusion.

## Per-class findings

### ProjectileActor

Launch origin, velocity and launch time integrated against gravity, with a `Projectile` role
sub-object supplying damage, splash and radius. Slot 51 stamps the launch time *and* precomputes the
impact by raycasting every candidate actor; slot 55 (`OnPrePhysics`) is the actual physics step
(integrate, collide, damage, broadcast, self-delete); slot 70 dead-reckons or steers and does **no**
networking of its own beyond chaining to `Actor::Update`.

Guided mode (`flags & 0x40`) blends the velocity direction toward the target over the first 20 ms.
Slots 57/58 (`Raycast`/`SweepTest`) return 0, opting the projectile out of being hit itself -
`PickupActor` does the same.

### TrackObjectActor - spline-driven moving geometry

Slot 76 (`OnAnimationComplete`) is really "install a path": it builds a Catmull-Rom coefficient
matrix from 4 control points. Slot 77 (`OnAnimationEvent`) starts a leg of motion, gathers riders,
closes doors and broadcasts `0xad` (forward) / `0xae` (reverse) with a trailing rider-id array. Slot
70 evaluates the spline and drags riders, track vertices and door sections under the map write lock.
Slot 64 (`Frag`) releases the riders and reopens the doors.

Its geometry pointer at `+0x130` comes from a list at **`Map+0x24`** - a datapoint inside `Map`'s
otherwise unmapped 0x24..0x88 region.

### BlockerActor - nav-mesh blockage

Slot 51 flood-fills from the floor polygon under the actor to every connected polygon whose centre
lies inside the blocker's world AABB, sets bit `0x100` ("not walkable") on each, and records them in
the list header at `+0x120` purely so slot 82 can clear the bit again. The whole body runs under
`TheMap->lock` taken for write.

`Map::FindNavPolygonUnder(Map*, Vec3*)` @ 0x0048cf50 resolves a point to a nav polygon and reaches into `Map+0x34` (a
three-level spatial grid), `Map+0x68` (extent) and `Map+0x7c/0x80/0x84` (grid plane coordinates) -
more of the unmapped `Map` region.

### CentibodyActor / CentipedeActor - the segmented worm

`CentipedeActor`'s slot 51 spawns five `centibody` roles, each one segment behind the last, and
links them through `+0x308` (a refcounted `Actor*`), broadcasting `0x8a` per link. Slot 70 is a
follow-the-leader chain solver: it frags itself when the chain is empty, releases dead segments, and
pulls live ones toward their leader. `CentibodyActor`'s own slot 70 is an empty no-op that
deliberately suppresses the CharacterActor tick.

### PopupActor / TurretActor

`+0x308` is `deployed` and `+0x309` `transition_in_progress` (the DB had a single `short`).
`PopupActor::Deploy(bool reverse)` @ 0x0054ac90 plays the popup animation with
`&this->deployed` as the completion flag, recomputes the collision bounds from model asset 0 or
0x13, and broadcasts `0x4a + reverse`. Retracted popups scan enemy teams against
`character->hearing_range_squared` and deploy on the first hit.

`TurretActor::Update` calls `Actor::Update` **directly**,
skipping PopupActor, CharacterActor and MobileActor - a deliberate three-level bypass.

### PresidentActor - the escort/VIP

Slot 33 picks a random `exita`..`exitd` aux object out of `MapAuxObjectList` (0x00739098), copies its
position into `+0x230`, and **unicasts** message `0xc7` (24 bytes, guaranteed) to the owning player
via `SendToPlayer` @ 0x00504ea0 - the only unicast in the whole Actor hierarchy. Slot 70 broadcasts
`0x9b` and deletes the actor when it gets within 5 units of the exit, and otherwise defects to
whichever team has the nearest actor (broadcast `0x50`, rate-limited by `+0x23c`).

## Network messages

Recovered by scanning every vtable slot implementation for the immediate stored just before a
`BroadcastToPlayers` (0x00504bf0) call. Single-candidate sites are reliable - the scan independently
reproduced every id this file previously documented. Sites where the id is *computed* rather than
stored as a literal are marked; a few resisted the scan entirely and are listed as "computed".

`BroadcastToPlayers(void *msg, int size, byte guaranteed, Vec3f cullOrigin)`; the first dword of
every message is the id and the second is almost always `Actor::id`.

| Id | Sent by | Meaning |
|---|---|---|
| 0x37 / 0x38 | ProjectileActor slot 55, Actor::Frag | projectile damage stat sync (0x38 when `flags & 0x10`) |
| 0x3b / 0x3d | MobileActor slot 70 | position sync; computed as `(arrived^1)*2 + 0x3b`, so 0x3b stopped / 0x3d moving |
| 0x3d / 0x48 | MobileActor slot 89 `Die` | death snapshot; 0x3d when the role is destructible, 0x48 when not |
| 0x3f / 0x40 | CharacterActor slot 97 | attack position; computed `0x3f + close_range` |
| 0x41 / 0x42 | CharacterActor slot 96 | attack target; computed `0x41 + close_range` |
| 0x43, 0x45, 0x46, 0x51, 0x62, 0x71 | CharacterActor slot 70 | attack/weapon tick |
| 0x44 | CharacterActor slot 98 | stop attacking |
| 0x46 | TurretActor slot 70 | turret fire (64 bytes, unreliable) |
| 0x47 | ProjectileActor slot 55 | projectile impact |
| 0x49 | Actor slot 65 | actor deleted |
| 0x4a / 0x4b | `PopupActor::Deploy` | computed `0x4a + reverse` |
| 0x4c / 0x4e | MobileActor slot 83 | crouch toggle; computed `0x4c + 2*(!is_crouched)`, so 0x4c now crouched / 0x4e now standing |
| 0x4f | MobileActor slot 70, PickupActor slot 84 | replicate animation |
| 0x50 | Actor slots 80/81, PresidentActor slot 70 | team change |
| 0x53, 0x54, 0x7e, 0x7f, 0x80, 0x8e | MobileActor slot 70 | mine laying, deployable counts, inventory removal |
| 0x55 | Actor slot 2 | health update |
| 0x56 / 0x57 | Actor slots 78/79 | target set / cleared |
| 0x58 / 0x59 | Actor slots 80/81 | owner change / release |
| 0x6b, 0xba | Actor slot 64 `Frag` | destruction events (0xba was previously undocumented) |
| 0x6f | Actor slot 70 | position sync (40 bytes, unreliable) |
| 0x74, 0x75, 0x8c | PickupActor slot 84 | item collected |
| 0x82 | CharacterActor slot 99 | ammo type changed |
| 0x83 | CharacterActor slot 94 | weapon changed |
| 0x84 | PickupActor slot 66 | script associated |
| 0x85 | PickupActor slot 83 | pickup enabled / respawn scheduled |
| 0x8a | CentipedeActor slot 51 | body segment linked |
| 0x8b | MobileActor slot 68 | gib; payload picks a random sever point |
| 0x96 | PickupActor slot 70 | pickup expired |
| 0x97 | MobileActor slot 67 | actor dissociated |
| 0x9b | MobileActor slot 68, PresidentActor slot 70 | deathmatch frag score / president extracted |
| 0xa6 | CharacterActor slots 20/21 | armor or shield piece destroyed |
| 0xa7 / 0xa8 | CharacterActor slots 21/20 | shield / armor partial damage |
| 0xad / 0xae | TrackObjectActor slot 77 | track leg start; computed `0xad + reverse` |
| 0xc7 | PresidentActor slot 33 | **unicast**, not broadcast - exit position |

`MobileActor::ApplyDamage`'s 0x9b is only sent in Deathmatch, when the killer is on another team,
and when the victim's role name is one of `hark`, `gunlok`, `maskelyn`, `frend`, `elint`.

## Key Observations

1. **No RTTI.** Slots 36-50 are the manual type-check mechanism. All fifteen are now mapped to a
   concrete class.
2. **Slot 35 `GetSize`** returns each class's `sizeof`, which is how the engine does polymorphic
   allocation without RTTI - and how the sizes above were confirmed.
3. **Getter/setter pairs:** armor 18/28, shield 19/29, `+0x18c` 25/26, `is_concealed` 8/9, `+0x188`
   30/54, `+0x304` 23/95. Two of those pairs were only discovered by noticing that the "callback"
   slots 9 and 54 are `RET 0x4`.
4. **Damage pipeline:** slot 68 -> slots 20/21 (armor/shield absorption) -> slot 69 -> at zero
   strength, slot 89 `Die` -> slot 82 -> slot 64 `Frag` -> slot 65 `Delete`.
5. **Deliberate base-class bypasses.** Several overrides skip intermediate classes and call
   `Actor::`'s implementation directly - `TurretActor` slot 70 (skips three levels),
   `CentipedeActor` slot 70 (calls `CharacterActor::`, skipping `CentibodyActor::`), `PopupActor`
   slot 70 on the retracted path. Do not assume an override chains to its immediate parent.
6. **Slot names describe the base implementation.** Slots 9, 15, 16, 32, 54, 59, 76, 77 and 82
   all mean something materially different in at least one subclass. The five worst were renamed;
   the rest are flagged above.

## Method-ownership convention

A slot implementation belongs to the **shallowest** class in the hierarchy whose vtable contains
that address. Ghidra's symbols originally violated this for 55 of the 249 distinct functions -
every one of them attributed to a *descendant* that merely inherits the slot (`MobileActor::IsMobile`
filed under `PresidentActor`, `CharacterActor::GetWeapon` under `TurretActor`, and so on). All 55
have been reparented, which as a side effect also fixed their `this` parameter type, since Ghidra
derives that from the parent class namespace.

When re-deriving this, note that the decompiler's C output prints a class prefix that does **not**
always agree with `FunctionManager.getParentNamespace()`. Query the namespace; do not read the
header line.
