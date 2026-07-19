# Gunlok Actor VTable - Reverse Engineering Notes

## Class Hierarchy (with sizes and vtable addresses)

```
Actor (0x120 = 288 bytes) vtbl @ 0x00667e30
 +-- MobileActor (0x230 = 560 bytes) vtbl @ 0x00667f7c
 |    +-- CharacterActor (0x308 = 776 bytes) vtbl @ 0x006680f8
 |    |    +-- CentibodyActor (0x310 = 784 bytes) vtbl @ 0x00668be0
 |    |    |    +-- CentipedeActor (0x310 = 784 bytes) vtbl @ 0x00668d70
 |    |    +-- PopupActor (0x310 = 784 bytes) vtbl @ 0x00668f00
 |    |         +-- TurretActor (0x320 = 800 bytes) vtbl @ 0x00669090
 |    +-- NodeActor (0x278 = 632 bytes) vtbl @ 0x00668a64
 |    +-- PresidentActor (0x240 = 576 bytes) vtbl @ 0x00669380
 +-- PickupActor (0x150 = 336 bytes) vtbl @ 0x006683dc
 +-- TrackObjectActor (0x1b8 = 440 bytes) vtbl @ 0x00668534
 +-- TumbleweedActor (0x120 = 288 bytes) vtbl @ 0x00668680
 +-- BackgroundCreatureActor (0x120 = 288 bytes) vtbl @ 0x006687cc
 |    +-- FlyingBackgroundCreatureActor (0x120 = 288 bytes) vtbl @ 0x00668918
 +-- BlockerActor (0x130 = 304 bytes) vtbl @ 0x00669234
 +-- UnknownActor (0x120 = 288 bytes) vtbl @ 0x00668288
```

## Constructors

| Class | Constructor | Address |
|---|---|---|
| Actor | Actor::Ctor | 0x0052d1f0 |
| MobileActor | MobileActor::Ctor | 0x005324b0 |
| CharacterActor | CharacterActor::Ctor | 0x0053c700 |
| PickupActor | PickupActor::Ctor | 0x00545fd0 |
| TrackObjectActor | TrackObjectActor::Ctor | 0x00547270 |
| TumbleweedActor | TumbleweedActor::Ctor | 0x00549400 |
| BackgroundCreatureActor | BackgroundCreatureActorCtor | 0x00549510 |
| FlyingBackgroundCreatureActor | FlyingBackgroundCreatureActorCtor | 0x00549600 |
| NodeActor | NodeCtor | 0x00549640 |
| CentibodyActor | CentibodyActor::Ctor | 0x00549c40 |
| CentipedeActor | CentipedeCtor | 0x00549d10 |
| PopupActor | PopupActor::Ctor | 0x0054a8a0 |
| TurretActor | TurretActor::Ctor | 0x0054aed0 |
| BlockerActor | BlockerActor::Ctor | 0x0054c940 |
| PresidentActor | PresidentCtor | 0x0054d3d0 |
| UnknownActor | UnknownActor::Ctor | 0x00542410 |

## Destructor

- Actor::~Actor (body) @ 0x0052d9c0
- Slot 0 is the scalar_deleting_destructor wrapper

## Actor Factory

`CreateActor_` @ 0x00510760 - dispatches based on `role->ai` enum to construct the right subclass.

## Actor Storage

Actors are stored in a hash map at `actors` @ 0x007ba0d8 (type `Actors`), keyed by actor ID.
- `GetActorById` @ 0x0044e0b0 - looks up actor by ID via hash bucket.
- `num_actors` @ 0x007b9ffc - running count incremented in CreateActor.

## VTable Layout (83 entries)

### Group 1: Core Lifecycle (Slots 0-9)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 0 | 0x00 | 0x0054e2d0 | `scalar_deleting_destructor` | Destroys actor, optionally frees 0x120 bytes |
| 1 | 0x04 | 0x005301a0 | `OnCreate` | No-op stub; post-construction callback |
| 2 | 0x08 | 0x0052dbc0 | `SetHealth` | Sets health float + broadcasts event 0x55 |
| 3 | 0x0C | 0x0054eab0 | `GetHealth` | Writes health float to out-param |
| 4 | 0x10 | 0x0054ed10 | `GetCenterCoords` | Returns position with Y offset by half height |
| 5 | 0x14 | 0x0054ec10 | `GetStrengthRatio` | Returns current_strength / max_strength |
| 6 | 0x18 | 0x0054f100 | `IsAlive` | Returns true if byte at 0x115 is zero |
| 7 | 0x1C | 0x0054f130 | `IsSpecialType_0` | Returns false; subclass override |
| 8 | 0x20 | 0x0054f170 | `IsSpecialType_1` | Returns false; subclass override |
| 9 | 0x24 | 0x0054e890 | `OnUpdate` | No-op stub; per-tick update callback |

### Group 2: Character Data & Combat Stats (Slots 10-23)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 10 | 0x28 | 0x0054ea30 | `GetCharacterData` | Returns entity->character (MobileActor overrides) |
| 11 | 0x2C | 0x0054ef80 | `GetWeaponData` | Returns 0; CharacterActor returns weapon |
| 12 | 0x30 | 0x0054eb30 | `GetField0x118` | Returns field at 0x118 |
| 13 | 0x34 | 0x0054f0c0 | `IsEnabled` | Returns true (1); default active state |
| 14 | 0x38 | 0x0054f1e0 | `IsMoving` | Returns false; MobileActor checks field 0x184 |
| 15 | 0x3C | 0x0054ef30 | `GetSecondaryWeaponData` | Returns 0; CharacterActor returns field 0x2d8 |
| 16 | 0x40 | 0x0054eb10 | `GetMovementState` | Returns 0; MobileActor returns field 0x194 |
| 17 | 0x44 | 0x0054e650 | `HasCustomisationHierarchy` | Returns false; MobileActor checks character data |
| 18 | 0x48 | 0x0054e8c0 | `GetArmorValue` | Returns armor_value float (confirmed by console cmd) |
| 19 | 0x4C | 0x0054ebd0 | `GetShieldValue` | Returns global default 0.0; CharacterActor: field 0x2d0 |
| 20 | 0x50 | 0x0054f280 | `ApplyArmorDamage` | No-op; CharacterActor iterates armor pieces (type 9) |
| 21 | 0x54 | 0x0054f290 | `ApplyShieldDamage` | No-op; CharacterActor iterates shield pieces (type 3) |
| 22 | 0x58 | 0x0054eb60 | `GetAmmoCount` | Returns 100; CharacterActor returns weapon ammo |
| 23 | 0x5C | 0x0054ebb0 | `GetField0x304` | Returns 0; CharacterActor returns field 0x304 |

### Group 3: AI & Movement (Slots 24-30)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 24 | 0x60 | 0x0054eb70 | `GetAIController` | Returns 0; MobileActor returns field 0x200 |
| 25 | 0x64 | 0x0054ead0 | `GetField0x18c` | Returns 0; MobileActor returns field 0x18c |
| 26 | 0x68 | 0x0054f3f0 | `SetField0x18c` | No-op; MobileActor sets field 0x18c |
| 27 | 0x6C | 0x0054f370 | `stub_slot27` | No-op; never overridden |
| 28 | 0x70 | 0x0054f360 | `SetArmorValue` | Sets armor_value float (confirmed by console cmd) |
| 29 | 0x74 | 0x0054f440 | `SetShieldValue` | No-op; CharacterActor sets field 0x2d0 |
| 30 | 0x78 | 0x0054f510 | `GetField0x188` | Returns false; MobileActor reads byte at 0x188 |

### Group 4: Inventory & Misc (Slots 31-35)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 31 | 0x7C | 0x0054eaf0 | `GetHotspot` | Returns 0 (null) |
| 32 | 0x80 | 0x0054f0e0 | `HasInventory` | Returns false; MobileActor checks list size |
| 33 | 0x84 | 0x0054e680 | `SetTeamId` | Sets this->team_id |
| 34 | 0x88 | 0x0054ef10 | `GetInventoryListPtr` | Returns 0; MobileActor returns &inventory_list |
| 35 | 0x8C | 0x0054e930 | `GetStructSize` | Returns 0x120 (each subclass returns its own size) |

### Group 5: Type Checks (Slots 36-50)

All return false (0) in Actor base class, overridden to true (1) by the corresponding subclass.

| Slot | Offset | Base Addr | Suggested Name | Overridden By |
|------|--------|-----------|----------------|---------------|
| 36 | 0x90 | 0x0054e6c0 | `IsMobile` | MobileActor |
| 37 | 0x94 | 0x0054e800 | `IsCharacter` | CharacterActor |
| 38 | 0x98 | 0x0054e7e0 | `IsUnknownType38` | (never overridden in known classes) |
| 39 | 0x9C | 0x0054e820 | `IsUnknownType39` | (never overridden in known classes) |
| 40 | 0xA0 | 0x0054e760 | `IsNode` | NodeActor |
| 41 | 0xA4 | 0x0054e720 | `IsCentipede` | CentipedeActor |
| 42 | 0xA8 | 0x0054e700 | `IsCentibody` | CentibodyActor (and CentipedeActor) |
| 43 | 0xAC | 0x0054e6a0 | `IsBackgroundCreature` | BackgroundCreatureActor, FlyingBgCreatureActor |
| 44 | 0xB0 | 0x0054e740 | `IsFlyingBackgroundCreature` | FlyingBackgroundCreatureActor |
| 45 | 0xB4 | 0x0054e780 | `IsPickup` | PickupActor |
| 46 | 0xB8 | 0x0054e840 | `IsTumbleweed` | TumbleweedActor |
| 47 | 0xBC | 0x0054e7a0 | `IsPopupType` | PopupActor, TurretActor |
| 48 | 0xC0 | 0x0054e6e0 | `IsUnknownType48` | (never overridden; mislabeled "IsPopup" in Ghidra) |
| 49 | 0xC4 | 0x0054e7c0 | `IsPresident` | PresidentActor |
| 50 | 0xC8 | 0x0054e860 | `IsTurret` | TurretActor |

### Group 6: Position & Physics (Slots 51-58)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 51 | 0xCC | 0x0052dcc0 | `InitPositionAndTiming` | Sets timing fields + spatial lookup |
| 52 | 0xD0 | 0x0052de60 | `ReleaseAttachment` | Ref-counted release of field 0x40 |
| 53 | 0xD4 | 0x0052ded0 | `SetPositionAndOrientation` | Sets coords + quaternion + timing |
| 54 | 0xD8 | 0x0054f4f0 | `OnPostUpdate` | No-op stub |
| 55 | 0xDC | 0x0054f270 | `OnPrePhysics` | No-op stub |
| 56 | 0xE0 | 0x0054efa0 | `OnCollisionResponse` | No-op stub |
| 57 | 0xE4 | 0x0052e0a0 | `Raycast` | Ray intersection test against shape/model |
| 58 | 0xE8 | 0x0052df50 | `SweepTest` | Extended ray/sweep test |

### Group 7: Damage & Health (Slots 59, 64, 68-69)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 59 | 0xEC | 0x0054f4e0 | `OnDamageReceived` | No-op stub |
| 64 | 0x100 | 0x0052e220 | `Frag` | Destruction: scoring, splash damage, debris spawn |
| 68 | 0x110 | 0x0052f3b0 | `ApplyDamage` | Damage/heal logic, triggers Frag at 0 HP |
| 69 | 0x114 | 0x0054f2a0 | `OnHealthChanged` | No-op stub |

### Group 8: State Queries (Slots 60-63, 75)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 60 | 0xF0 | 0x0054f1a0 | `IsTargetable` | Returns false |
| 61 | 0xF4 | 0x0054f250 | `IsVisible` | Returns field 0x10d |
| 62 | 0xF8 | 0x0054f200 | `IsInteractable` | Returns false |
| 63 | 0xFC | 0x0054f150 | `CanBePickedUp` | Returns false |
| 75 | 0x12C | 0x0054f230 | `HasCustomAnimation` | Returns false |

### Group 9: Actor Deletion & Association (Slots 65-67)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 65 | 0x104 | 0x0052f0d0 | `DeleteActor` | Full deletion + cleanup + network msg 0x49 |
| 66 | 0x108 | 0x0054e640 | `Associate` | No-op stub (DummyAssociate) |
| 67 | 0x10C | 0x0054e880 | `Dissociate` | No-op stub |

### Group 10: Sync & Animation (Slots 70-77)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 70 | 0x118 | 0x0052f8a0 | `SyncPositionAndBroadcast` | Update model pos + network msg 0x6f |
| 71 | 0x11C | 0x005300e0 | `PlayAnimation` | Plays animation on 3D model |
| 72 | 0x120 | 0x00530120 | `BlendAnimation` | Animation blending |
| 73 | 0x124 | 0x00530150 | `PlayAnimationEx` | Extended animation playback |
| 74 | 0x128 | 0x00530180 | `SetAnimationState` | Animation state control |
| 76 | 0x130 | 0x0054f4d0 | `OnAnimationComplete` | No-op stub |
| 77 | 0x134 | 0x0054f350 | `OnAnimationEvent` | No-op stub |

### Group 11: Targeting & Ownership (Slots 78-82)

| Slot | Offset | Base Addr | Suggested Name | Base Behavior |
|------|--------|-----------|----------------|---------------|
| 78 | 0x138 | 0x00530270 | `SetTarget` | Sets target fields + network msg 0x56 |
| 79 | 0x13C | 0x00530390 | `ClearTarget` | Clears target + network msg 0x57 |
| 80 | 0x140 | 0x00530470 | `ChangeOwnerAndTeam` | Reassigns owner/team + network msgs 0x58/0x50 |
| 81 | 0x144 | 0x00530650 | `ReleaseFromOwner` | Releases from owner + neutralize + msgs 0x59/0x50 |
| 82 | 0x148 | 0x005308d0 | `ActivateInWorld` | Re-registers in spatial/team structures |

## Network Message Types Used

| Code | Used In | Description |
|------|---------|-------------|
| 0x49 | DeleteActor (slot 65) | Actor deletion |
| 0x50 | ChangeOwnerAndTeam/ReleaseFromOwner (slots 80-81) | Team change |
| 0x55 | SetHealth (slot 2) | Health update |
| 0x56 | SetTarget (slot 78) | Target assignment |
| 0x57 | ClearTarget (slot 79) | Target cleared |
| 0x58 | ChangeOwnerAndTeam (slot 80) | Owner change |
| 0x59 | ReleaseFromOwner (slot 81) | Owner release |
| 0x6b | Frag (slot 64) | Destruction event |
| 0x6f | SyncPositionAndBroadcast (slot 70) | Position sync |
| 0xA6 | ApplyArmorDamage (slot 20) | Armor piece destroyed |
| 0xA7 | ApplyShieldDamage (slot 21) | Shield partial damage |
| 0xA8 | ApplyArmorDamage (slot 20) | Armor partial damage |

## Subclass-Specific VTable Entries

### MobileActorVtbl (380 bytes = 95 slots)

Extends ActorVtbl with 12 additional entries for movement, navigation, and equipment.

| Slot | Offset | Address | Name | Description |
|------|--------|---------|------|-------------|
| 83 | 0x14C | 0x00536090 | `updateMineDetectionAndBounds` | Checks nearby mines/items, computes bounding box from skeleton/height, sends network msg 0x4C/0x4E |
| 84 | 0x150 | 0x00536830 | `equipToFirstOpenSlot` | Finds first empty equipment slot (slota-sloth, types 2-9) and equips item |
| 85 | 0x154 | 0x0054e5d0 | `onPreThink` | No-op stub for subclass override |
| 86 | 0x158 | 0x0054e5f0 | `onPostThink` | Cleanup stub with vector destructor |
| 87 | 0x15C | 0x0054e5e0 | `onPreUpdate` | No-op stub for subclass override |
| 88 | 0x160 | 0x00539450 | `gotoPosition` | Navigate to Vec3f target via pathfinding; checks can_turn/CanBePickedUp/strength |
| 89 | 0x164 | 0x0053a020 | `die` | Death handler: sends msg 0x48/0x3D, resets is_mine, calls ActivateInWorld/Frag |
| 90 | 0x168 | 0x0053a760 | `addWaypoint` | Allocates 0x18-byte waypoint, pushes to waypoint queue |
| 91 | 0x16C | 0x0053b560 | `getNavigationTarget` | Fills output struct with current nav target (actor, coords, pathfinding node) |
| 92 | 0x170 | 0x0054eaa0 | `getField0x198` | Getter for field_0x198 (state enum / AI mode) |
| 93 | 0x174 | 0x0053baa0 | `playActionAnimation` | Plays animation with condition checks (flag at 0x190+1, time threshold) |
| 94 | 0x178 | 0x0054e690 | `onMobileDamageReceived` | No-op stub for subclass damage reaction |

### CharacterActorVtbl (400 bytes = 100 slots)

Extends MobileActorVtbl with 5 entries for combat.

| Slot | Offset | Address | Name | Description |
|------|--------|---------|------|-------------|
| 95 | 0x17C | 0x0054f430 | `setField0x304` | Setter for field at 0x304 (AI target / character state) |
| 96 | 0x180 | 0x00540c60 | `attackTarget` | Attack an actor: stores ref-counted target at 0x2D8, sets weapon range, sends msg 0x41/0x42 |
| 97 | 0x184 | 0x00540a10 | `attackPosition` | Attack world position: stores coords at 0x2DC, sets weapon range, sends msg 0x3F/0x40 |
| 98 | 0x188 | 0x00540f20 | `stopAttacking` | Stop attack: release target ref, reset anim priorities, sends msg 0x44 |
| 99 | 0x18C | 0x00541b90 | `executeSpecialAbility` | Execute special ability, sends network msg 0x82 |

### TurretActorVtbl (420 bytes = 105 slots)

Extends CharacterActorVtbl with 5 entries for turret aiming.

| Slot | Offset | Address | Name | Description |
|------|--------|---------|------|-------------|
| 100 | 0x190 | 0x0054e8b0 | `setTurretEnabled` | Setter for enabled flag at 0x310 |
| 101 | 0x194 | 0x0054ee40 | `getTurretAimDirection` | Getter for turret aim Vec3f at 0x2BC |
| 102 | 0x198 | 0x0054eb90 | `getTurretTargetAngles` | Getter for yaw/pitch at 0x318/0x31C (no auto-function in Ghidra) |
| 103 | 0x19C | 0x0054f190 | `isTurretEnabled` | Getter for enabled flag at 0x310 (no auto-function in Ghidra) |
| 104 | 0x1A0 | 0x0054f410 | `setTurretTargetAngles` | Setter for yaw/pitch at 0x318/0x31C |

### NodeActorVtbl (380 bytes = 95 slots)

Same size as MobileActorVtbl, no extra entries beyond MobileActor's 12.

### UnknownActorVtbl (340 bytes = 85 slots)

Extends ActorVtbl with 2 entries.

| Slot | Offset | Address | Name | Description |
|------|--------|---------|------|-------------|
| 83 | 0x14C | 0x0054eba0 | `getField0x150` | Getter for int field at 0x150 |
| 84 | 0x150 | 0x0054f460 | `setField0x168` | Stores 12-byte value (int64 + int) at offset 0x168 |

### PickupActorVtbl (344 bytes = 86 slots)

Extends ActorVtbl with 3 entries. Bounded below by `PickupActorVtbl` @ `0x006683dc` and above by
`TrackObjectActorVtbl` @ `0x00668534`.

| Slot | Offset | Address | Name | Description |
|------|--------|---------|------|-------------|
| 83 | 0x14C | 0x00546240 | `SetPickupEnabled` | Stores the byte arg at +0x120; if +0x12c == 0, schedules a respawn deadline into +0x144 as `GetGameTimeSeconds()` + `MPRespawnDelay` scaled by the mode at +0x140 (1 -> x2, 2 -> x1, 3 -> x `FLOAT_006520a0`). Broadcasts update `0x85` (8 B, reliable) |
| 84 | 0x150 | 0x00546440 | `OnPickedUp` | Item collected by a `MobileActor`. Broadcasts up to 6 updates (`0x75`, `0x8c`, `0x4f` + 3 unresolved) and, when the item has an `associated_script` (+0x134), calls `QueueScriptExecution` on it — one of the seven paths that make every client run a `.gcs` (see `directplay_protocol_notes.md` §8.11) |
| 85 | 0x154 | 0x00546b20 | `SetField0x138` | Frees the existing `char*` at +0x138 and stores a `strdup` of the argument. **Not** `associated_script`, which is +0x134 — a separate string field, purpose not yet established |

> This vtable was **undefined data** in the Ghidra DB, so none of its slots had cross-references
> and `OnPickedUp` appeared to have zero callers of any kind — which is exactly why its
> host-vs-client thread affinity could not be settled statically. It is now defined as
> `pointer[86]`. Worth checking for the other subclass vtables before concluding "no callers".

**Still undocumented** in this file: the vtable extensions for `CentibodyActor`, `CentipedeActor`,
`PopupActor`, `TrackObjectActor`, `TumbleweedActor`, `BackgroundCreatureActor`,
`FlyingBackgroundCreatureActor`, `BlockerActor` and `PresidentActor`.

### Subclass-Specific Network Messages

| Code | Used In | Description |
|------|---------|-------------|
| 0x3D | die (slot 89) | Death notification (non-destructible) |
| 0x3F | attackPosition (slot 97) | Attack position (ranged) |
| 0x40 | attackPosition (slot 97) | Attack position (melee) |
| 0x41 | attackTarget (slot 96) | Attack target (ranged) |
| 0x42 | attackTarget (slot 96) | Attack target (melee) |
| 0x44 | stopAttacking (slot 98) | Stop attack |
| 0x48 | die (slot 89) | Death notification (destructible) |
| 0x4C | updateMineDetectionAndBounds (slot 83) | Bounds update |
| 0x4E | updateMineDetectionAndBounds (slot 83) | Mine detection update |
| 0x82 | executeSpecialAbility (slot 99) | Special ability activation |

## Key Observations

1. **No RTTI** - The binary has RTTI disabled. Instead, slots 36-50 implement
   manual type-checking via `IsXxx()` virtual methods (pre-RTTI C++ pattern).

2. **Slot 35 (`GetStructSize`)** returns the class's sizeof for each subclass.
   This enables polymorphic allocation/serialization without RTTI.

3. **Getter/Setter Pairs**: armor (18/28), shield (19/29), field 0x18c (25/26).

4. **Damage Pipeline**: ApplyDamage (68) -> ApplyArmorDamage (20) + ApplyShieldDamage (21)
   -> OnHealthChanged (69) -> if health <= 0: Frag (64) -> DeleteActor (65).

5. **Three unknown type check slots**: 38, 39, and 48 are never overridden in
   any of the 16 known subclasses. May correspond to cut content or types only
   present in specific builds.

6. **Ghidra mislabeling**: Slot 47 was labeled "IsPickup" but is actually
   overridden by PopupActor/TurretActor. The real IsPickup is slot 45.
   Slot 48 labeled "IsPopup" is never overridden by PopupActor.
