# Inventory / Equipment (Upgrade screen)

How Gunlok carries, equips, gives and drops items. Recovered from `gl.exe` in Ghidra, cross-checked
against the shipped `.gsh` headers in `<Gunlok>\scripts` and against `save_system_notes.md`'s
serialised item record.

Everything below is **measured** from the binary unless a claim is explicitly flagged *inferred*.
Section 11 lists what is still open.

Two containers, and confusing them costs an offset on every field:

- **`Inventory`** (0x44 bytes, `MobileActor+0x194`) — everything the character is *carrying*.
- **`MobileActor+0x19c`** — a separate `List<EquipSlot*>` of what is *equipped*. `Actor` slot 34
  `GetInventoryListPtr` returns the address of that field, not of the `Inventory`.

---

## 1. `Inventory` — the carried-items container

`Inventory` is allocated exactly once per actor, in **`MobileActor::SetTeamId` @ 0x00533430**, and
only when the actor moves onto a **player-controlled team** (`TeamSlots[team].player_controlled != 0`, `TeamSlot+0x6a`) and
`Actor+0x3c` is set. A background creature never gets one, which is why `Actor::GetInventory`
(slot 16, @ 0x0054eb10) returns `NULL` on the base class.

```
this->inventory = Inventory::Ctor(pool_alloc(0x44), role->character->weapon, 0xf, role);
```

| fn | address | signature |
|---|---|---|
| `Inventory::Ctor` | 0x004e4550 | `__thiscall(void *this, int base_weapon, int capacity, Role *role)`, ends `RET 0xc` |
| `Inventory::Dtor` | 0x004e46a0 | `__thiscall(Inventory *)` — **no `RET` of its own**, it tail-jumps to the sentinel destructor at 0x004e66e0 |
| `Inventory::AddItem` | 0x004e49a0 | `__thiscall(Inventory *, InventoryItem *)`, `RET 0x4` |
| `Inventory::AddItemFromRole` | 0x004e4790 | `__thiscall(Inventory *, PickupClass, Role *, int item_id, int value)`, `RET 0x10` |
| `Inventory::AddAmmoItem` | 0x004e4900 | `__thiscall(Inventory *, int ammo_type, int item_id)`, `RET 0x8` |
| `Inventory::AddAmmo` | 0x004e53e0 | `__thiscall(Inventory *, int ammo_type, uint count, int item_id)`, `RET 0xc` |
| `Inventory::RemoveItem` | 0x004e5290 | `__thiscall(Inventory *, InventoryItem *)`, `RET 0x4` |
| `Inventory::FindItemById` | 0x004e5200 | `__thiscall(Inventory *, int item_id) -> InventoryItem *`, `RET 0x4` |
| `Inventory::GetFreeCapacity` | 0x004e59f0 | `__thiscall(Inventory *, PickupClass, int ammo_or_weapon) -> int` |
| `Inventory::GetFreeCapacityForRole` | 0x004e59c0 | `__thiscall(Inventory *, PickupClass, Role *)` |
| `Inventory::CountWeapons` / `CountAmmoStacks` / `CountModules` | 0x004e5930 / 0x004e5960 / 0x004e5990 | `__thiscall(Inventory *) -> int` |
| `Inventory::NextWeapon` / `NextAmmo` / `NextModule` | 0x004e56f0 / 0x004e5740 / 0x004e5790 | `__stdcall(InventoryCursor *) -> bool`, ends `RET 0x4` |
| `Inventory::PrevWeapon` / `PrevAmmo` / `PrevModule` | 0x004e5810 / 0x004e5860 / 0x004e58b0 | same |
| `Inventory::PlaceItemModel` | 0x004e4a70 | `__thiscall(Inventory *, InventoryItem *, int row, int column)`, `RET 0xc` |
| `Inventory::BuildDisplayGrid` | 0x004e4fd0 | `__fastcall(Inventory *)` |
| `Inventory::ClearDisplayGrid` | 0x004e50e0 | `__fastcall(Inventory *)` |

### 1.1 Layout

The list is the **pointer-anchored** shape (`+0x1c` holds a pointer to a heap `List_Member_Base`
sentinel of 0xc bytes with vtable `DAT_0066731c`), like the console lists, not the embedded
`List<T>` of `src/List.h`.

```
+0x00  int    field0x00            zeroed by the ctor; no writer found
+0x04  int    base_weapon          role->character->weapon at construction
+0x08  int    capacity             ctor arg; 0xf at the one call site
+0x0c  int    max_weapon           Character+0x6c  (GLS `max weapon`)
+0x10  int    max_ammo             Character+0x70  (GLS `max ammo`)
+0x14  int    max_module           Character+0x74  (GLS `max module`)
+0x18  int    field0x18            zeroed by the ctor
+0x1c  node*  items_sentinel   \
+0x20  int    items_count       |  List<InventoryItem*>
+0x24  void*  items_cache       |
+0x28  bool   items_cache_valid /
+0x2c  node*  module_cursor    \   cursor pair {node, &items_sentinel}
+0x30  void** module_cursor_list/
+0x34  node*  weapon_cursor    \
+0x38  void** weapon_cursor_list/
+0x3c  node*  ammo_cursor      \
+0x40  void** ammo_cursor_list /
```

**The three cursors are the three tabs of the Upgrade screen**, and each has its own filter
predicate — weapons are `pickup_class == 4`, ammunition `== 6`, modules "anything that is not 4, 6
or 2". `Inventory::BuildDisplayGrid` walks weapon/ammo/module in that order into columns 0/1/2,
three rows each: the screen is a 3x3 grid of **real 3D models**, parked in
`InventoryGridNodes` @ 0x007b3ed0 (9 scene-node pointers; the item pointer is stashed at node+0xa8).

`Character::max_weapon`/`max_ammo`/`max_module` are copied in at construction, so a character's
category caps are frozen at the moment it joins a player team.

### 1.2 The bare-hands item

The constructor pushes one `InventoryItem` before anything else: all pointers NULL, `weapon = 0x21`
(**33 = "none"**, matching `ParseAmmo`'s declared max for `weapon type`), `role_id = 0x7fffffff`,
`value = 0`. It is a module-category entry (class 0) and it is what `Inventory::base_weapon`
restores to when an arm slot is emptied.

**There is no other seeding.** `InventoryInfo` does not declare a starting inventory (see §4), and
nothing in `ToRole` builds one. A squad member's starting kit comes from the level's own
`GIVE`/`GIVE AND EQUIP` console commands and from `ApplyTeamCarryOverState` @ 0x004da4a0, which
replays the previous level's items through `Inventory::AddItemFromRole` and
`MobileActor::EquipItemInSlot`.

---

## 2. `InventoryItem` — 0x38 bytes

Size is **measured twice**: `Inventory::Dtor` does `free_sized(item, 0x38)`, and
`save_system_notes.md` already records the savegame's per-item blob as `u8[0x38]`.

```
+0x00  PickupClass  pickup_class     round(role->character->aggression * 10)
+0x04  void*        hierarchy        InventoryInfo::hierarchy, else Role::hierarchy   (refcounted)
+0x08  void*        shape            InventoryInfo::shape,     else Role::shape       (refcounted)
+0x0c  Renderable*  display_node     built lazily by Inventory::PlaceItemModel; refcount at +0xa0
+0x10  void*        field0x10        second refcounted object; released by the dtor. Purpose unknown
+0x14  char*        description      GetResourceString(InventoryInfo::description | AmmoInfo::description)
+0x18  int          variant          round(pickupRole->character->walking_speed / 65536)
+0x1c  int          role_id          Role::id, or NEGATIVE (-ammo_type) for an ammo item
+0x20  int          weapon           pickupRole->character->weapon (class-dependent, see below)
+0x24  int          id               unique, from NextInventoryItemId @ 0x007b6e48
+0x28  int          max_value        capacity   -- denominator of the screen's "10/20"
+0x2c  int          value            current    -- numerator
+0x30  uint         limit            deny mask, copied from the pickup Role::limit
+0x34  int          action_on_death  InventoryInfo::action_on_death (1 must drop / 2 must not drop)
```

### 2.1 The discriminator is `pickup_class`, and it is `aggression`

`Role::GetPickupType` @ 0x004ae340 — `__thiscall(Role *)`, bare `RET`, no stack args:

```
004ae350  MOV dword ptr [EBP-0x4],0xa
004ae357  FILD dword ptr [EBP-0x4]        ; 10.0
004ae360  MOVSS XMM0,dword ptr [EAX+0x3c] ; character->aggression
004ae365  MULSS XMM0,...
004ae36f  FLD  float ptr [EBP-0x4]
004ae372  FISTP dword ptr [EBP-0xc]       ; <-- FISTP
```

So it is **`FISTP` = round-to-nearest-even under the default control word**, not floor and not
truncate. `0.61*10 = 6.1 -> 6` and `0.6*10 = 6.0 -> 6` both work; the `.x1` nudge in the shipped
headers is belt-and-braces, not a requirement. A role with **no `character` returns 2**, which is
why class 2 reads as "nothing".

`role_system_notes.md` §7's "2 = default / non-pickup" is wrong in wording but right in mechanism:
2 is both the no-character fallback *and* the authored "nothing" pickup.

**Classes 1 and 8.** No `aggression 0.11` exists in the shipped headers, so class 1 is unused. Class
**8 does exist in code** — `MobileActor::EquipItemInSlot` accepts 3/7/8/9 into the body slots and
`UnequipSlot` reverses 7 and 8 identically — so 8 is a second body-slot class. No shipped header
authors it (*inferred*: it is a slot-type distinction the data never used).

### 2.2 `weapon` (+0x20) is a class-dependent discriminant, not a weapon id

| class | meaning of `+0x20` |
|---|---|
| 4 weapon | weapon type, into `WeaponTypes` (0..33, 33 = none) |
| 6 ammo | **ammo type**, index into `AmmoInfos[19]` @ 0x007b5d40 |
| 5 minelayer | mine sub-type 0..4 -> roles `"mine"`, `"remote_mine"`, `"timed_mine"`, `"decoy"`, `"EMP_mine"` |
| 7 / 8 body slot | module id: 3 = sight/terrain scanner, 6 = target imager, 14 = interface arm, … |
| 10 other | 4 = hologram decoy, 5 = audio cloak |

This corrects a natural reading of `src/Roles.cpp`: `Character::weapon` on a *pickup* role is not
always a `WeaponTypes` value. `Chr_Flares_Pickup` writes `weapon flares` and the engine uses that
integer as an `AmmoInfos` index. The recovered `WeaponTypes[]` table is therefore incomplete **and**
partly a different namespace, and the mine sub-types 0..4 are not `WeaponTypes` values at all.

### 2.3 `variant` (+0x18) — the `walking_speed`-as-payload consumer

The consumer the task asked for is `Inventory::AddItemFromRole` @ 0x004e4790, at 0x004e47c1:

```
004e47b9  MOV  EAX,dword ptr [EAX]          ; character->walking_speed  (an INT: 16.16 fixed point)
004e47c1  FILD dword ptr [EBP+0x14]
004e47cc  MULSS XMM0,dword ptr [0x00652190] ; 1.52587890625e-05 == 1/65536
004e47dc  FISTP dword ptr [EBP-0x8]
```

i.e. `variant = round(walking_speed_fixed / 65536)` — the plain integer the `.gsh` author typed.
`MobileActor::ReceiveObject` and `MobileActor::DropItem` run the identical expression, so the
quantity survives a drop/pickup round trip.

That single field is:

- the **quantity** in an ammo or minelayer pickup (`walking speed 50 // dummy for number of bullets`),
- the **magnitude** of a module (sight range delta, accuracy step),
- the **variant index** for a module family (`walking speed 1 // 1 = no radar`, `0 // with radar`,
  `2 // with radar and mine detector`).

Note `Character::walking_speed` is stored as a **16.16 fixed-point int**, which the Ghidra
`Character` struct still types as `float` — see §11.

### 2.4 The "10/20"

`DrawInventoryItemPanel` @ 0x004a7890, cases 3 / 6 / 9 (shield, ammo, armour): itoa of
`item+0x2c`, then the two-byte separator constant `DAT_006642a0`, then itoa of `item+0x28`. Class 5
(minelayer) prints `item+0x18` alone. So **`value` / `max_value`**, and the display is shared by
shields and armour, not just ammunition.

---

## 3. `EquipSlot` — 0x18 bytes, the equipped list

`MobileActor+0x19c` anchors a `List<EquipSlot*>`. `EquipSlot` is `pool_alloc(0x18)` in
`MobileActor::EquipItemInSlot` and `free_sized(slot, 0x18)` in `UnequipSlot`.

```
+0x00  void*        hierarchy      copy of item->hierarchy
+0x04  void*        shape          copy of item->shape
+0x08  InventoryItem* item
+0x0c  int          slot           0..12
+0x10  PickupClass  pickup_class   copy of item->pickup_class; the unequip switch reads THIS
+0x14  int          weapon         copy of item->weapon
```

---

## 4. `InventoryInfo` (`Role+0x64`) declares presentation, not a starting inventory

0x18 bytes, already in the DB: `{hierarchy, shape, description, pickup_name, pickup_radius,
action_on_death}` — GLS `inventory shape`, `description`, `pickup name`, `pickup radius`,
`action on death`.

`Inventory::AddItemFromRole` reads exactly four of them into the item, with a fallback:

```
item->hierarchy       = inv->hierarchy ? inv->hierarchy : role->hierarchy   (Role+0x1c)
item->shape           = inv->shape     ? inv->shape     : role->shape       (Role+0x18)
item->description     = inv->description                                    (InventoryInfo+0x08)
item->action_on_death = inv->action_on_death                                (InventoryInfo+0x14)
```

with `hierarchy = role->hierarchy, shape = role->shape, description = 0, action_on_death = 0` when
`Role::inventory_info` is NULL. **This is the same fallback the savegame loader performs** when it
re-resolves an item's pointers from `role_id` (`save_system_notes.md`, "Inventory"), which is an
independent confirmation of the field order.

`pickup_name` and `pickup_radius` are not read here — they belong to the world `PickupActor`.

**Nothing anywhere seeds a starting inventory from a role.** See §1.2.

---

## 5. The slot model

**Thirteen slots, 0..12.**

| slot | binding |
|---|---|
| 0 | hierarchy node **"Upper Arm Right"** |
| 1 | hierarchy node **"Upper Arm Left"** |
| 2..12 | hierarchy nodes **"slota" … "slotk"** |

The names are 11 NUL-terminated strings at **0x006672c4, stride 8** (`slota`..`slotk`; the last is
at 0x00667314). `MobileActor::EquipToFirstOpenSlot` @ 0x00536830 (MobileActor vtable **slot 84**)
copies eight of them (`slota`..`sloth`) onto the stack and asks the actor's *hierarchy* whether each
node exists (`Renderable_GetNodeWorldPosition(hierarchyInstance, "slotX", 0)` @ 0x0059d270), then picks the first that exists and is
not already occupied. Slot 0/1 come from `MobileActor::UnequipSlot`'s `case 0: case 1:` arm, which
detaches from `"Upper Arm Right"` / `"Upper Arm Left"`.

**So how many arm/body/head slots a character has is authored in its `.rif` model**, as dummy nodes,
not in the GLS. Ordering `slota`..`slotk` is what makes the search deterministic.

Range checks:

- `EquipItemInSlot` accepts `slot - 2 < 0xb`, i.e. **2..12**, for the "body" arm of its switch; the
  same 2..12 range is the `case` list in `UnequipSlot`. Exactly the eleven `slota`..`slotk` names.
- `EquipToFirstOpenSlot` only ever searches **2..9** (`slota`..`sloth`); slots 10..12 are reachable
  only through an explicit `EquipItemInSlot`.
- Only `pickup_class` 3 (shield), 7, 8 (body slot) and 9 (armour) may go into slots 2..12.

**Weapons are right-arm only, and it is hard-coded.** `MobileActor::UseInventoryItem`, class 4:

```
EquipItemInSlot(0, item_id, silent);   /* slot 0 == "Upper Arm Right" */
```

Nothing writes slot 1 from any dispatch path; the left arm is reachable only through the `0x0f`
network command, which carries an explicit slot number.

### 5.1 Eligibility — the red LED, and why only Frend gets the heavy guns

One mask test, in three places, always the same shape. The executor-side one - the gate that
actually refuses the equip - is inside **`MobileActor::EquipItemInSlot` @ 0x00536ba0**, at
0x00536c64:

```
00536c64  MOV  EAX,dword ptr [ESI+0xc0]   ; Actor::role
00536c6a  MOV  ECX,dword ptr [EAX+0x54]   ; Role::limit
00536c70  TEST dword ptr [EAX+0x30],ECX   ; InventoryItem::limit
```

**Usable iff `(item->limit & wearer->role->limit) == 0`.** It is a *deny* mask.

`Role::limit` is GLS field `limit` (id 0x78, `I`, default 0). On a squad-member role it is that
character's identity bit; on a pickup role it is the OR of the characters that may **not** use it.
Straight from the shipped headers:

| role | `limit` | |
|---|---|---|
| `Rol_GunLok` | 1 | |
| `Rol_Hark` | 2 | |
| `Rol_Frend` | 4 | |
| `Rol_Elint` | 8 | |
| `Rol_Maskelyn` | 16 | |
| `Rol_MissileLauncher_Pickup` | **27** = 1\|2\|8\|16 | denies everyone but Frend |
| `flamethrower` pickup | 26 = 2\|8\|16 | Gunlok and Frend |
| `Rol_InterfaceArm_Pickup` | **23** = 1\|2\|4\|16 | Elint only |

So the manual's "only Frend can use the heaviest weapons" is **a per-role bitmask, not a weight
class and not a name list**. It is authored per pickup.

The three sites: `MobileActor::EquipItemInSlot` @ 0x00536c70 (refuses, plays
`SendLocalizedMessageToOwner` @ 0x00508e70 / `SendSoundToOwner` @ 0x00508f60), `MobileActor::UseInventoryItem` @ 0x005371f8 (same), and
`DrawInventoryItemLamps` @ 0x004a697b + `DrawInventoryItemPanel` @ 0x004a7ef2 — the UI runs the
*identical* test to choose the lamp / material. That is the red LED.

### 5.2 The other two lamps

`DrawInventoryItemLamps` @ 0x004a68e0, one call per Upgrade-screen column:

- **green "in use"** — the item appears in the on-screen entity's equipment list
  (`slotRecord->item == gridNode->item`) -> `DrawItemLampInUse` @ 0x004a65e0.
- **red "cannot use"** — the mask test above -> `DrawItemLampCannotUse` @ 0x004a6820.
- **ammo back-light** — for a class-6 item,
  `AmmoTable[GetWeapon()->type * 19 + item->variant]` @ **0x007b5ec0** is non-NULL ->
  `DrawItemLampBacklight` @ 0x004a66a0.

**That confirms the compatibility key.** `weapon type` is *not* the key by itself: the key is the
`(ammo_type, weapon_type)` pair, resolved through the second ammo table at 0x007b5ec0 indexed
`ammo_type + weapon_type * 19` — exactly the indexing `src/Roles.h:497-506` already records. The
same expression gates equipping ammo in `UseInventoryItem` (case 6, at 0x0053725x), so the
back-light is not cosmetic: an unlit round cannot be loaded.

All three lamps pass the same vertex colour `0xccffffff`; the red/green distinction is in the UV
rect (constants at 0x006643a0..0x006643ac), i.e. one atlas.

---

## 6. Operations

| operation | address | signature |
|---|---|---|
| pick up | `MobileActor::ReceiveObject` @ **0x00539120** | `__thiscall(MobileActor *, ItemActor *, bool say)` |
| use / auto-equip | `MobileActor::UseInventoryItem` @ **0x005370d0** | `__thiscall(MobileActor *, int item_id, bool silent)` |
| equip into slot | `MobileActor::EquipItemInSlot` @ **0x00536ba0** | `__thiscall(MobileActor *, int slot, int item_id, bool silent)`, `RET 0x10` |
| auto-slot | `MobileActor::EquipToFirstOpenSlot` @ **0x00536830** | vtable slot 84, `__thiscall(MobileActor *, int item_id, bool silent)` |
| unequip | `MobileActor::UnequipSlot` @ **0x00536ec0** | `__thiscall(MobileActor *, EquipSlot *)`, `RET 0x4` |
| give (request + walk) | `MobileActor::Update` @ 0x00533720, block at **0x00533f2f** | pending state on the actor |
| give (transfer) | `MobileActor::GiveItemTo` @ **0x00537db0** | `__thiscall(MobileActor *, int item_id, int recipient_id, uint amount)` |
| give (move the record) | `MobileActor::TransferItemTo` @ **0x00538060** | `__thiscall(MobileActor *, InventoryItem *, Actor *to, int amount, bool whole, int new_id)` |
| drop | `MobileActor::DropItem` @ **0x00538240** | `__thiscall(MobileActor *, int item_id)`, `RET 0x4` |
| console cheats | `GiveObject` @ 0x0044ef60, `GiveAndEquip` @ 0x0044f110 | `__fastcall(bool say)` |

### 6.1 Pick up

`ReceiveObject` requires `this->inventory != NULL` and `ItemActor+0x120 == 0`. It asks
`Inventory::GetFreeCapacityForRole(pickup_class, role)`; **0 means full** and it broadcasts update
`0x78` and stops. Otherwise it takes `min(free, ItemActor+0x130)` — `ItemActor+0x130` is the world
pickup's remaining quantity, defaulting to `round(character->walking_speed / 65536)` when it is -1 —
calls `Inventory::AddItemFromRole`, broadcasts `0x76`/`0x77` (24 bytes), and either destroys the
world actor or decrements its quantity.

`GetFreeCapacity` returns **-1 for "unbounded"**, and every caller compares it *unsigned* against
the offered amount, so -1 never clamps. Per class: 4 -> room iff `CountWeapons() < max_weapon`;
6 -> room iff `CountAmmoStacks() < max_ammo`, else the sum of `max_value - value` over existing
stacks of that ammo type; anything else -> `CountModules() < max_module`. Class 2 is always -1.

### 6.2 Give, and the walk-to-receiver

The request is not an order-queue entry — it is **three fields of pending state on the giver**,
written by the executor's command `0x17` handler at 0x0050a22f:

```
Actor+0x40  = recipient Actor*   (addref'd; released by 0x004ad420)
Actor+0x44  = item id
MobileActor+0x124 = amount
```

and the handler first issues a move order toward the recipient (`MobileActor::GotoObject` @ 0x005394d0), which is the
"if too far, the giver walks to them first" behaviour. `MobileActor::Update` then polls every tick
(0x00533f26):

```
COMISS XMM2,XMM1        ; 16.0 vs |giver - recipient|^2
SETA   byte ptr [...]   ; strictly less than 4.0 world units
... CALL 0x00537db0     ; GiveItemTo(this, item_id, recipient->id, amount)
```

then releases the reference and resets the three fields to `0 / -1 / -1`.

`GiveItemTo` re-checks the *recipient's* free capacity (0 -> broadcast `0x78`), decides whole-item
vs split, broadcasts `0x81`, and calls `TransferItemTo`, which for a whole item **moves the same
`InventoryItem` object** (unequipping it first if it is in a slot) rather than copying it. Ammo is
the exception: it is re-added by count via `Inventory::AddAmmo` and the source record is freed.

### 6.3 Drop

`DropItem` unequips, resolves the world pickup role — `GetRoleById(item->role_id)` when `role_id >= 0`,
`GetAmmoPickupRole(item->weapon)` when it is negative, and **always role `"shield_pickup"` for class
3** — `SpawnRole`s it at the dropper's position and orientation with the dropper's id as owner,
writes the quantity into `ItemActor+0x130`, and broadcasts `0xa3` then `0x7d`.

### 6.4 Command Wheel ammo selection

Selecting an ammo type *is* equipping the ammo item: `UseInventoryItem` case 6 checks the
`(ammo, weapon)` pair against the table at 0x007b5ec0 and then calls **`CharacterActor::SetAmmoType`
@ 0x00541b90 (CharacterActor vtable slot 99)**. There is no separate ammo-selection path.

---

## 7. Module effects

Both module effects live behind `EquipItemInSlot`'s default arm, dispatched on **`item->weapon`**,
and both are reversed by `UnequipSlot` with a negated argument.

### 7.1 Sight / terrain scanner — `weapon == 3`

`MobileActor::ApplySightModule` @ **0x00536aa0**, `__thiscall(MobileActor *, float delta)`:

```
sight_range               (+0x168) += delta
initial_first_person_range(+0x16c) += delta * 5
+0x164                             += 1   (or -1, floored at 0)
```

`delta` is `item->variant`. **Additive** — two sight modules stack linearly, and `+0x164` is a count
of how many are fitted.

### 7.2 Target imager / optical tracker — `weapon == 6`

`MobileActor::ApplyAimModule` @ **0x00536b20**, `__thiscall(MobileActor *, int delta)`:

```
+0x1b8 += delta;  if (+0x1b8 < 0) +0x1b8 = 0        ; count of fitted aim modules
aim (+0x1b4) = role->character->aim                  ; Character+0x08, the GLS `aim`
for (i = 0; i < +0x1b8; ++i) aim *= 0.5              ; FLOAT_006520a0 == 0.5
```

**`aim` is a spread, so this is the accuracy upgrade.** Two properties matter:

- **Combining is multiplicative**, `aim = character->aim * 0.5^n` — each imager halves the spread.
- It is **recomputed from the `Character` base every time**, not accumulated, so removing one is
  exact and the value cannot drift.

This is the concrete mechanism behind the manual's "upgrades can often be combined". Slot
exclusivity is not what limits it: `Inventory::max_module` caps how many modules can be *carried*,
and the number of `slota`..`slotk` dummy nodes in the model caps how many can be *worn*.

### 7.3 Everything else

The other module ids in `Character::weapon` (`lock decoder`, `beacon tracker`, `hologram generator`,
`audio cloak`, `repair arm`, `interface arm`, `nanotech dismantler`, …) have **no effect in
`EquipItemInSlot`** — its default arm only handles 3 and 6. They work by being *asked about*:
`WaitCond_ElintHasInterfaceArmEquipped` @ 0x0056fc60 is the pattern —

```
weapon = actor->GetWeapon();       /* Actor vtable slot 11 */
return weapon && weapon->type == 14;   /* +0x08 == interface arm */
```

Two of them are consumables handled in `UseInventoryItem` class 10: `item->weapon == 4` is the
hologram decoy (picks a random enemy, sets a timer, broadcasts `0x9d`, consumes the item) and
`== 5` the audio cloak (`0x9e`).

---

## 8. Replication

All of it replicates. Inventory state is executor-owned; the client sends commands and receives
updates.

### 8.1 Commands (client -> server), from `ExecutorThreadProc`'s jump table

Recovered from the byte map at **0x0050bae0** (indexed `cmd - 4`) and the pointer table at
**0x0050ba3c**.

| cmd | size | handler | meaning |
|---|---|---|---|
| `0x0f` | 16 | 0x0050a1d7 | `EquipItemInSlot(actor, slot, item_id, 1)` |
| `0x10` | 16 | 0x0050a204 | **the queued twin of `0x0f`** — `QueueToggleEquipOrder` @ 0x00538e80, `PendingOrder` **kind 5**. It goes through the **toggle**, so it can push **kind 9 (unequip)** instead: the pusher counts queued kind 5 minus kind 9 for the same item and tail-calls the kind-9 pusher when the balance is 1 |
| `0x11` | 12 | 0x0050a2fb | `UseInventoryItem(actor, item_id, 1)` |
| `0x12` | 12 | 0x0050a325 | **the queued twin of `0x11`** — `QueueUseItemOrder` @ 0x00538ca0, `PendingOrder` **kind 7** |
| `0x13` | 12 | 0x0050a34d | `DropItem(actor, item_id)` |
| `0x14` | 12 | 0x0050a38a | **the queued twin of `0x13`** — `QueueDropOrder` @ 0x00538d40, `PendingOrder` **kind 8** |
| `0x15` | 12 | 0x0050a39d | **unequip** — walk `inventory_list` for `slot->item->id == arg`, `UnequipSlot`, then broadcast update `0x80` from 0x0050a478 |
| `0x16` | 12 | 0x0050a49a | **the queued twin of `0x15`** — `QueueUnequipOrder` @ 0x00538de0, `PendingOrder` **kind 9** |
| `0x17` | 24 | 0x0050a22f | **give item**, immediate `{giver, recipient, item_id, f32 game_time, amount}` — sets the pending give state and issues the walk |
| `0x18` | 24 | 0x0050a2b7 | **give item**, queued — same payload; calls `QueueGiveItemOrder` @ 0x00538f80 (at 0x0050a2e8) to build `PendingOrder` kind 6 |

**The odd/even pairing is what named the order kinds.** The odd id calls the callee **directly**
while the even id only enqueues, so each immediate arm above is a literal statement of what its
queued twin means — which is how `orders_notes.md` §3 settled that kinds 5, 7 and 9 are *equip into
a body slot*, *use item* and *unequip*, correcting a one-place rotation (`Interact` / `Equip` /
`UseItem`). Read that section before relying on any order-kind name written before it.

**This is now settled, and `directplay_protocol_notes.md` has been corrected to match.** It read
`0x17` as "board / attach (escort) — actor id + carrier + slot"; there is no board or attach command
anywhere in the executor table. The body at 0x0050a22f is unambiguously the give-item request (it
addrefs the recipient into `Actor+0x40`, stores the item id at `Actor+0x44` and the amount at
`MobileActor+0x124`, and issues a move order), and the client-side sender — read this round, having
been left unread when §11 was written — is `Unit::Unit_SendGiveItem` @ 0x004c0d50, renamed from
`Unit_SendBoard`. `RET 0x10` = 16 argument bytes = `(int item_id, int recipient_actor_id,
uint amount, bool queued)`, with `queued` selecting between the two ids exactly as the other `Unit`
slot 100-104 senders do. The other four ids in that table match the notes.

**The `?` field at wire `+0x10` is an `f32` game-time stamp**, not a payload field. The `0x17` arm
passes it straight to `GotoObject(giver, recipient->slot12(), now)` @ 0x005394d0 as the `now`
argument (`MOVSS XMM0,[EBP+0xc]` then `COMISS` against `MobileActor+0x1e8`), so the full 24-byte
layout is `{u32 id, u32 giver_id, u32 recipient_id, u32 item_id, f32 game_time, u32 amount}` — note
the wire order is not the sender's C argument order.

**What settles the give reading is the payoff site, not the arm.** `MobileActor::Update` @ 0x00533f5c,
gated on a squared distance `< 16.0` (within 4 world units of the recipient), calls `GiveItemTo`
@ 0x00537db0 at 0x00533f6d with `item_id = [EDI+0x44]`, `recipient_actor_id = [[EDI+0x40]+0xc]`,
`amount = [EDI+0x124]`, then clears the pending state (`+0x124 = -1`, `+0x44 = -1`, `+0x40 = 0`). The
queued form reaches the identical code: kind 6 pops at 0x00535195 and repeats the `0x17` arm
instruction-for-instruction.

`Actor+0x40` is the pending-give recipient, a refcounted `Actor *`. **`Actor+0x44` is not an item-id
field** — it carries the item id on this path, but it is a pending-order parameter reinterpreted per
order kind: at 0x00534fae it is passed to `GetActorById` (gated on `+0x40 == 0`), and the kind-2
`GotoObject` arm at 0x0053515d also writes it. Treat it as "order argument", not "item id".

### 8.2 Updates (server -> clients)

| id | size | producer | meaning |
|---|---|---|---|
| `0x76` / `0x77` | 24 | `ReceiveObject` | item picked up `{actor, class, role_id, item_id, amount}`; `0x76` when `say` |
| `0x78` | 12 | `ReceiveObject`, `GiveItemTo` | inventory full `{actor, class}` |
| `0x79` / `0x7a` | 16 | `EquipItemInSlot` | equipped `{actor, slot, item_id}`; `id = 0x79 + (play_ui_sound ^ 1)`, so **`0x79` is the audible form and `0x7a` the silent one** — both mean equip |
| `0x7b` / `0x7c` | 12 | `UseInventoryItem` | item used `{actor, item_id}` |
| `0x7d` | 12 | `UseInventoryItem`, `DropItem`, `CommandRemoveItem` | item removed `{actor, item_id}` |
| `0x81` | 24 | `GiveItemTo` | transfer `{giver, recipient, item_id, amount, new_item_id}` |
| `0xa3` | 12 | `DropItem` | world pickup quantity `{actor_id, quantity}` (unreliable) |
| `0x4f` | 25 | `UseInventoryItem` class 5 | mine laid |
| `0x9d` / `0x9e` | 20 / 16 | `UseInventoryItem` class 10 | hologram decoy / audio cloak |
| `0x98` | 32 | `ApplyTeamCarryOverState` | carry-over roster entry |

The client side is `ApplyUpdateMessage` @ 0x004fde70, which calls `Inventory::AddItemFromRole`,
`BuildDisplayGrid` and `ClearDisplayGrid` directly.

**The third parameter of `EquipItemInSlot` was named `silent`, and that name is inverted.** It is now
`play_ui_sound` in the Ghidra DB. The measurement is on the client: the shared arm for `0x79`/`0x7a`
at 0x00500107 pushes `1` when the id is `0x79` and `0` otherwise, and inside
`Unit_EquipItemInSlot` @ 0x004bcfb0 (the client mirror, renamed from `FUN_004bcfb0`) that flag gates
**nothing but three `PlayUiSound` calls** — ids `0x1b`, `0x23` and `0x1c`, each behind
`if (param_3 != 0)`. Everything else that function does happens either way: it walks the unit's
inventory for the item, re-applies the same `item->limit & role->limit` deny mask the executor
checks, `pool_alloc(0x18)`s the `EquipSlot`, links it into the equipment list, and for pickup classes
0/1/2 builds a 0xa8-byte `HudWidget` when the unit is on `LocalPlayerTeam`. Since
`id = 0x79 + (arg3 ^ 1)`, `arg3 == 1` produces `0x79` and therefore **plays** the sound — and the
wire command `0x0f`/`0x10` arm passes 1, which is right for a user interact action. So the argument
means "play the UI sound", and any reading of `(silent ^ 1) + 0x79` that infers `0x79` is the quiet
one has the two ids the wrong way round.

Note this pair is **not** equip-versus-unequip. `MobileActor::UnequipSlot` @ 0x00536ec0 broadcasts
nothing at all — no `BroadcastToPlayers` call anywhere in its body, verified both by its absence from
all 186 call sites of 0x00504bf0 and by a direct instruction scan — so **no update id carries an
unequip**. `directplay_protocol_notes.md` had update `0x72` as "unequip"; `0x72` is a bare
`PlayUiSound(0x28)` broadcast by `UseInventoryItem` @ 0x005370d0 (site 0x00537d36), carrying no actor
id at all, and that row has been corrected.

---

## 9. Savegame cross-check

`save_system_notes.md`'s inventory block reads, with the names now available:

```
u32  actorsWithInventory
u32  NextInventoryItemId
actorsWithInventory x {
    u32  actorId
    u32  itemCount          // written as (list count - 1)
    itemCount x u8[0x38]    // raw InventoryItem records
}
```

- `0x38` is `sizeof(InventoryItem)` ✓
- "field `+0x1c` is a role id (or a negative ammo id)" ✓ — `role_id`, and `AddAmmoItem` writes
  `-ammo_type` there
- "re-resolves hierarchy, shape and description from `Role::inventory_info`, falling back to the
  role's own" ✓ — the exact fallback in `AddItemFromRole`
- "Negative ids index `AmmoInfos`" ✓

The `count - 1` is the built-in bare-hands item (§1.2) being skipped: it is always the list head and
the constructor recreates it. *Inferred* — plausible from the ordering, not read out of `SaveGame`.

`ApplyTeamCarryOverState` @ 0x004da4a0 is the between-levels path and calls both
`Inventory::AddItemFromRole` and `MobileActor::EquipItemInSlot`, so equipment survives a level
change, not just the carried list.

---

## 10. C++ mirror sketch (`src/Inventory.h`)

```cpp
#pragma once
#include <cstdint>
#include "src/List.h"

namespace gk {

struct Role;
struct Character;
struct Hierarchy;
struct Shape;
struct Renderable;
struct MobileActor;

// round(Character::aggression * 10), Role::GetPickupType @ 0x004ae340 (FISTP,
// round-to-nearest-even). A role with no `character` yields Nothing.
enum class PickupClass : int32_t {
  Health    = 0,   // aggression 0
  Nothing   = 2,   // aggression 0.21, and the no-character fallback
  Shield    = 3,   // 0.31
  Weapon    = 4,   // 0.41
  MineLayer = 5,   // 0.51
  Ammo      = 6,   // 0.6 / 0.61
  BodySlot  = 7,   // 0.71
  BodySlot2 = 8,   // no shipped header authors this; the code treats it like 7
  Armour    = 9,   // 0.91
  Other     = 10,  // 1.00 / 1.01
};

// Equipment slot index. 0/1 are the arms; 2..12 are hierarchy dummy nodes
// "slota".."slotk" (0x006672c4, stride 8) and exist only if the .rif has them.
enum class EquipSlotIndex : int32_t {
  UpperArmRight = 0,
  UpperArmLeft  = 1,
  SlotA = 2, SlotB, SlotC, SlotD, SlotE, SlotF, SlotG, SlotH, SlotI, SlotJ, SlotK, // = 12
};

// One carried item. pool_alloc(0x38) in Inventory::AddItemFromRole @ 0x004e4790 and
// Inventory::AddAmmoItem @ 0x004e4900; free_sized(_, 0x38) in Inventory::Dtor.
// This is also the record the savegame writes verbatim.
struct InventoryItem {
  PickupClass pickup_class;   // 0x00
  Hierarchy *hierarchy;       // 0x04 InventoryInfo::hierarchy, else Role::hierarchy (refcounted)
  Shape *shape;               // 0x08 InventoryInfo::shape,     else Role::shape     (refcounted)
  Renderable *display_node;   // 0x0c built lazily by Inventory::PlaceItemModel; refcount at +0xa0
  void *field0x10;            // 0x10 refcounted; released by the dtor, purpose unknown
  char *description;          // 0x14 GetResourceString result; NOT owned
  int32_t variant;            // 0x18 round(pickup character->walking_speed / 65536)
  int32_t role_id;            // 0x1c Role::id, or -ammo_type for an ammo item
  int32_t weapon;             // 0x20 class-dependent: weapon / ammo type / mine sub-type / module
  int32_t id;                 // 0x24 from NextInventoryItemId @ 0x007b6e48
  int32_t max_value;          // 0x28 the "20" of "10/20"
  int32_t value;              // 0x2c the "10"
  uint32_t limit;             // 0x30 deny mask; usable iff (limit & wearer->role->limit) == 0
  int32_t action_on_death;    // 0x34 1 must drop / 2 must not drop
};
static_assert(sizeof(InventoryItem) == 0x38);
static_assert(offsetof(InventoryItem, variant)   == 0x18);
static_assert(offsetof(InventoryItem, role_id)   == 0x1c);
static_assert(offsetof(InventoryItem, max_value) == 0x28);
static_assert(offsetof(InventoryItem, limit)     == 0x30);

// One equipped item. pool_alloc(0x18) in MobileActor::EquipItemInSlot @ 0x00536ba0.
struct EquipSlot {
  Hierarchy *hierarchy;       // 0x00 copy of item->hierarchy
  Shape *shape;               // 0x04 copy of item->shape
  InventoryItem *item;        // 0x08 borrowed - the Inventory owns it
  EquipSlotIndex slot;        // 0x0c
  PickupClass pickup_class;   // 0x10 copy; the unequip switch reads THIS, not item->pickup_class
  int32_t weapon;             // 0x14 copy of item->weapon
};
static_assert(sizeof(EquipSlot) == 0x18);

// Cursor into Inventory::items. The list is the pointer-anchored shape: `list` points at
// the Inventory's own sentinel *field*, so *list is the sentinel node.
struct InventoryCursor {
  List_Member_Base<InventoryItem *> *node;  // 0x00
  List_Member_Base<InventoryItem *> **list; // 0x04 = &Inventory::items_sentinel
};
static_assert(sizeof(InventoryCursor) == 0x08);

// MobileActor+0x194. pool_alloc(0x44) in MobileActor::SetTeamId @ 0x00533430, and ONLY
// when the actor joins a player-controlled team. Actor slot 16 GetInventory returns it.
struct Inventory {
  int32_t field0x00;          // 0x00 zeroed by the ctor; no writer found
  int32_t base_weapon;        // 0x04 Character::weapon at construction; restored on unequip
  int32_t capacity;           // 0x08 ctor arg; 0xf at the one call site
  int32_t max_weapon;         // 0x0c Character+0x6c, GLS `max weapon`
  int32_t max_ammo;           // 0x10 Character+0x70, GLS `max ammo`
  int32_t max_module;         // 0x14 Character+0x74, GLS `max module`
  int32_t field0x18;          // 0x18 zeroed by the ctor

  // 0x1c..0x2b - pointer-anchored list, NOT src/List.h's embedded List<T>: the anchor is a
  // pointer to a heap 0xc-byte List_Member_Base sentinel (vtable 0x0066731c).
  List_Member_Base<InventoryItem *> *items_sentinel; // 0x1c
  int32_t items_count;        // 0x20
  void *items_cache;          // 0x24 flattened array; freed+nulled on mutation
  bool items_cache_valid;     // 0x28
  uint8_t pad0x29[3];

  InventoryCursor module_cursor; // 0x2c pickup_class not in {4, 6, 2}
  InventoryCursor weapon_cursor; // 0x34 pickup_class == 4
  InventoryCursor ammo_cursor;   // 0x3c pickup_class == 6
};
static_assert(sizeof(Inventory) == 0x44);
static_assert(offsetof(Inventory, max_weapon)     == 0x0c);
static_assert(offsetof(Inventory, items_sentinel) == 0x1c);
static_assert(offsetof(Inventory, module_cursor)  == 0x2c);
static_assert(offsetof(Inventory, weapon_cursor)  == 0x34);
static_assert(offsetof(Inventory, ammo_cursor)    == 0x3c);

// --- native API -----------------------------------------------------------------
// Container
Inventory *GetActorInventory(MobileActor *actor);              // Actor vtable slot 16
InventoryItem *InventoryFindItemById(Inventory *, int id);     // 0x004e5200
int InventoryFreeCapacity(Inventory *, PickupClass, int key);  // 0x004e59f0, -1 = unbounded
int InventoryCountWeapons(Inventory *);                        // 0x004e5930
int InventoryCountAmmoStacks(Inventory *);                     // 0x004e5960
int InventoryCountModules(Inventory *);                        // 0x004e5990
void InventoryAddItemFromRole(Inventory *, PickupClass, Role *, int id, int value); // 0x004e4790
void InventoryAddAmmo(Inventory *, int ammo_type, unsigned count, int id);          // 0x004e53e0
void InventoryRemoveItem(Inventory *, InventoryItem *);        // 0x004e5290

// Actor operations - all __thiscall on MobileActor
void EquipItemInSlot(MobileActor *, EquipSlotIndex, int item_id, bool silent);  // 0x00536ba0
void EquipToFirstOpenSlot(MobileActor *, int item_id, bool silent);             // 0x00536830 (slot 84)
void UnequipSlot(MobileActor *, EquipSlot *);                                   // 0x00536ec0
void UseInventoryItem(MobileActor *, int item_id, bool silent);                 // 0x005370d0
void GiveItemTo(MobileActor *, int item_id, int recipient_id, unsigned amount); // 0x00537db0
void DropItem(MobileActor *, int item_id);                                      // 0x00538240

// Eligibility, exactly as the engine spells it.
inline bool CanUseItem(const Role *wearer, const InventoryItem *item) {
  return (item->limit & wearer->limit) == 0;
}

}  // namespace gk
```

`Role::limit` must be `uint32_t` for `CanUseItem`; `src/Roles.h` should carry the dual-use comment
from the Ghidra plate.

---

## 11. What is still unknown

- **`InventoryItem+0x10`.** A second refcounted object (vtable at +0x00, refcount at +0x04) that the
  `Inventory` destructor releases. No writer was found in the functions read; it is never set by
  either item constructor. Settle it by scanning every writer of `[reg+0x10]` inside the inventory
  and Upgrade-screen modules.
- **`Inventory+0x00` and `+0x18`.** Zeroed by the constructor; no writer found. Likely dead.
- **`Inventory::capacity` (+0x08).** Always `0xf` at the one call site. No reader found — the three
  category caps are what actually limit the inventory. *Inferred*: vestigial.
- **`InventoryScreenEntity` @ 0x007b6e4c is not an `Actor`.** Its `Role *` is at **+0xb8**
  (0x004a6972: `MOV EAX,[EDX+0xb8]` then `[EAX+0x54]`) whereas `Actor::role` is at +0xc0 — and
  +0xb8 falls inside `Actor::orientation`. Yet its vtable exposes the Actor slots (`+0x2c`
  `GetWeapon`, `+0x88` `GetInventoryListPtr`, `+0x144`) and it is refcounted at +0x04 like an Actor.
  So it is a **client-side entity class parallel to `Actor`**, not an `Actor`. Which class was not
  established. Nothing in §5.1 depends on it — the executor-side test at
  0x00536c64, inside `MobileActor::EquipItemInSlot`, is on `Actor+0xc0` and is definitive.
- **Class 8.** Reachable in code, absent from all shipped headers. What distinguishes it from
  class 7 is not established.
- **`Character::walking_speed` is typed `float` in the Ghidra DB and in `src/Roles.h`, but every
  read in the inventory path is `FILD` on an int** (0x004e47c1, 0x005382xx, 0x00539xxx) —
  it is 16.16 fixed point, exactly as `CLAUDE.md` already says under `MakeRole`. Left as-is because
  `Character` is far outside this task's scope; correcting it is a one-line retype with wide
  decompilation fallout, and it should be done deliberately.
- **`status display` / `status window u` / `status window v`.** Not the item icon. Items on the
  Upgrade screen are drawn as **3D models** (`Inventory::PlaceItemModel` builds a `Renderable` from
  the item's hierarchy or shape and parks it in a 3x3 grid of scene nodes), so there is no icon
  atlas for items. `Character::status_window_u/v` @ +0xa0/+0xa4 belong to the **character portrait**,
  not the inventory. Their consumer was not traced.
- **Which lamp graphic is green and which red** was not read out of the UV constants at
  0x006643a0..0x006643ac; the *roles* of the three drawers are measured from their guards, the
  colours are taken from the manual.
- **`OpenUpgradeScreen` @ 0x004e5ad0** (2874 bytes) writes `InventoryScreenEntity` and
  `InventorySelectedItem` and references the `"slota"` table. Its body was not read, so what it
  does beyond opening the Upgrade screen is not established.
- ~~**`0x0050a22f` is command `0x17`** was derived from the byte map + pointer table, not from a
  client-side sender.~~ **Resolved.** The sender was read: `Unit::Unit_SendGiveItem` @ 0x004c0d50
  (renamed from `Unit_SendBoard`), `RET 0x10`, `queued` picking `0x17` versus `0x18`. The byte map
  and pointer table were also re-walked (`0x0050bae0` index `cmd - 4` -> slot 15 -> `0x0050ba3c`), and
  the executor arm, the queued order kind and the transfer site all agree. §8.1.

---

## 12. Corrections to existing notes

- **`directplay_protocol_notes.md`**, command table: `0x17` is **give item to another actor**
  (24 bytes `{giver, recipient, item_id, f32 game_time, amount}`), not "board / attach (escort)".
  §8.1 above. **Applied** — that file's row now reads give-item and carries the resolved layout, and
  the `?` field is the `f32` game-time stamp the arm hands to `GotoObject` as `now`. Confirmed from
  the executor side (arm 0x0050a22f), from the client sender (`Unit_SendGiveItem` @ 0x004c0d50) and
  from the payoff site (`GiveItemTo` @ 0x00537db0, called at 0x00533f6d). The DB names that carried
  the wrong reading are gone: `Unit_SendBoard` -> `Unit_SendGiveItem`, `QueueBoardOrder` @ 0x00538f80
  -> `QueueGiveItemOrder`. **There is no board or attach command anywhere in the executor table**, so
  `orders_notes.md`'s `PendingOrder` kind 6 is `GiveItem`, not `Board`.
- **`directplay_protocol_notes.md`**, update table: `0x72` was "unequip" and is `PlayUiSound`; the
  equip pair is `0x79`/`0x7a`, differing only in that sound. `MobileActor::UnequipSlot` @ 0x00536ec0
  broadcasts nothing. §8.2 above. **Applied.**
- **`role_system_notes.md` §7**: pickup class 2 is the authored *"nothing"* pickup **and** the
  no-`character` fallback of `Role::GetPickupType`; "default / non-pickup" understates it. The
  conversion is `FISTP` (round-to-nearest-even), not floor or truncate.
- **`Role::limit`** was an unannotated `int` in the Ghidra DB and is undocumented in
  `role_system_notes.md`. It is the item-eligibility bitmask; see §5.1.
- **`src/Roles.cpp`'s `WeaponTypes[]`** is not merely incomplete: `Character::weapon` on a pickup
  role is a **class-dependent discriminant** and for an ammo pickup it is an `AmmoInfos` index, not
  a weapon id. §2.2.
- `MobileActor+0x194` was described in the DB as "0x44-byte container object … was mis-named
  `movement_state`". It is the `Inventory`. `MobileActor+0x19c` is the *equipment* list and is a
  different container; the DB comment on it conflated `EquipSlot` fields with `InventoryItem`
  fields (`P+0x10` is the pickup class, `Q+0x24` the item id, `Q+0x2c` the current value).
