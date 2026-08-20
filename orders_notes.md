# Player Order System

The per-character command queue, standing orders, formations, selection, Active Pause and the
key bindings that drive them. Everything here was read out of `gl.exe` in Ghidra; a claim that
was *inferred* rather than read off an instruction says so. Companion notes:
`actor_vtable_notes.md` (slot numbering), `directplay_protocol_notes.md` (the wire ids this
section corrects), `input_notes.md` (how a keystroke reaches `HandleGameKeyAction`),
`threading_model_notes.md` (why every order push runs on the executor thread).

**The one sentence version**: a player order is a **0x28-byte `PendingOrder` record on a
per-actor FIFO** (`MobileActor+0x1f0`), and *nothing on the client ever pushes one* — the client
sends a **command message to the server**, and the executor thread builds the record. Order
issuing, cancelling and clearing are therefore all wire protocol, and single player is just the
loopback case.

---

## 1. Key bindings

### 1.1 The record and the registry

`RegisterAllKeyBindings` @ **0x004effc0** makes exactly **71** calls to

```
RegisterKeyBinding(KeyBinding *bind /*ECX*/, const char *name /*EDX*/,
                   int dik, int mask, int category)      // 0x004f7360, RET 0xc
```

- `KeyBinding` is **8 bytes**: `{int dik; int modifier_mask;}` (`MOV [EDI],EAX` / `MOV [EDI+4],EAX`
  at 0x004f737b/0x004f7380). The bindings live in a flat region 0x007b7274..0x007b74c7.
- `name` is `GetResourceString(&LocalizedStrings, strid)` — the action names are in
  `glres<lang>.dll`, not in the exe.
- The call appends a **0x14-byte node** (vtable 0x0066789c, `{vptr, prev, next, KeyBinding*, char*}`)
  to `KeyBindingCategories[category]` @ **0x007b74f0**, stride 0x10 (a `List<T>` header).

### 1.2 Modifier matching — settles the `menu_system_notes.md` contradiction

Every test in the dispatcher is literally

```
CMP  ESI, dword ptr [bind]            ; ESI = event->dik      (KeyEvent+0x10)
MOV  ECX, EDI                          ; EDI = event->modifiers (KeyEvent+0x14)
CALL 0x004e3e60                        ; KeyModifiersForBindingMatch: AND ECX,0xfffffffe
CMP  EAX, dword ptr [bind+4]           ; must equal the mask EXACTLY
```

So **bit 0 is discarded and the remaining bits must match exactly**. Only masks `0` and `2` occur
in the shipped defaults, and mask 2 is the one carried by `Select all characters` (CTRL+A),
`Quit to the desktop` (CTRL+Q) and the four CTRL+arrow camera actions — so **`2` is CTRL**.
`menu_system_notes.md`'s "bit 1 = CTRL, bit 2 = ALT" is **wrong** and should be corrected;
`KeyModifierState`'s `MOD_Ctrl = 2` is right. All three bits are now pinned at their writer,
`HandleKeyMessage` @ 0x004e3f20: DIK `0x2a` LSHIFT -> bit **0x1** (0x004e3fa5), DIK `0x1d` LCTRL
-> bit **0x2**, DIK `0x38` LALT -> bit **0x4**. No shipped binding uses 0x1 or 0x4, which is why
only mask 0 and mask 2 appear in `GLkeys.cfg`.

### 1.3 The action table

`WriteGLKeys` @ 0x004fa670 walks `KeyBindingCategories` in order, so `<Gunlok>\scripts\GLkeys.cfg`
*is* the table. Category order and sizes, measured from the `category` argument of all 71
registrations and confirmed against the on-disk file:

| # | Category | Menu | Count |
|---|---|---|---|
| 0 | Camera | 28 | 11 |
| 1 | Mines | 29 | 18 |
| 2 | Character | 30 | 22 |
| 3 | Gameplay | 31 | 7 |
| 4 | ActivePause | 32 | 2 |
| 5 | Recon | 33 | 1 |
| 6 | Formations (really *standing orders*) | 34 | 8 |
| 7 | Music | 35 | 2 |

The order-layer half, with the DIK the binding is registered with and the function
`HandleGameKeyAction` @ **0x0046f700** calls on a match:

| Action | DIK | Key | Mask | Handler |
|---|---|---|---|---|
| **ActivePause (cat 4)** |
| `Active Pause` | 57 (0x39) | SPACE | 0 | `ToggleActivePause` 0x0046f4a0 |
| `Delete last queued command` | 211 (0xd3) | DELETE | 0 | `CancelLastOrderForSelection` 0x0049f250 |
| **Formations (cat 6)** |
| `Standing Orders Menu` | 49 (0x31) | N | 0 | `OpenStandingOrdersMenu` 0x004a12e0 |
| `Cluster formation` | 38 (0x26) | L | 0 | inline: `TeamSlots[team].formation = 0` @0x0046fd27 |
| `Side by side` | 48 (0x30) | B | 0 | inline: `= 1` @0x0046fd5d |
| `Single file` | 23 (0x17) | I | 0 | inline: `= 2` @0x0046fd93 |
| `Hold ground` | 35 (0x23) | H | 0 | `ApplyStandingOrderToSelection(3)` |
| `Always fire` | 21 (0x15) | Y | 0 | `ApplyStandingOrderToSelection(4)` |
| `Return fire` | 18 (0x12) | E | 0 | `ApplyStandingOrderToSelection(5)` |
| `Never fire` | 47 (0x2f) | V | 0 | `ApplyStandingOrderToSelection(6)` |
| **Character (cat 2), order-related** |
| `Select all characters` | 30 (0x1e) | A | **2** | `SelectAllCharacters` 0x004972a0 |
| `Deselect or activate menu` | 1 | ESC | 0 | `DeselectOrActivateMenu` 0x00496f70 |
| `Select next character and centre` | 15 | TAB | 0 | `SelectNextCharacter` + camera centre 0x00487bf0 |
| `Select the next character` | 27 (0x1b) | `]` | 0 | `SelectNextCharacter` 0x00497080 |
| `Select the previous character` | 26 (0x1a) | `[` | 0 | `SelectPrevCharacter` 0x00497160 |
| `Crouch / conceal` | 46 (0x2e) | C | 0 | `CrouchToggleSelection` 0x0049f1d0 |
| `Stop` | 31 (0x1f) | S | 0 | `StopSelection` 0x0049f130 |
| `Upgrade Screen` | 22 (0x16) | U | 0 | 0x0049f350 |
| `Command Wheel` | 17 (0x11) | W | 0 | `OpenCommandWheel` 0x004a0fa0 |
| `Ammo Menu` | 30 (0x1e) | A | 0 | `OpenAmmoMenu` 0x004a11d0 |
| `Attack ground` | 34 (0x22) | G | 0 | `BeginAttackGroundTargeting` 0x004a13f0 |
| `Select Gunlok`…`Maskelyn` | 2..6 | 1..5 | 0 | 0x0056eb60 then `ActivateUnitAndResume` 0x0046ef50 |
| `Select Ammo type 1..5` | 7..11 | 6,7,8,9,0 | 0 | (no direct call in the dispatcher; see §9) |
| **Recon (cat 5)** |
| `Toggle Recon Mode on/off` | 28 (0x1c) | ENTER | 0 | 0x004976d0 |

The manual's Active Pause = Pause key and cancel-last = CTRL+`\` are **both wrong for the shipped
binary**: they are SPACE and DELETE. The manual's "Formations"/"Engagement rules" wording is also
not the engine's — the strings are the eight names above, all in one category.

---

## 2. The order record

`PendingOrder` — **0x28 bytes**, `pool_alloc(0x28)` at every push site, freed with
`free_sized(rec, 0x28)`.

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0x00 | 4 | `kind` | `OrderKind` (§3) |
| 0x04 | 4 | `arg_a` | kind 0/6: **target actor id**; kind 2: float priority; kinds 3/4: `-1` |
| 0x08 | 4 | `arg_b` | kinds 5..10: the primary argument (item / slot / ammo id) |
| 0x0c | 4 | `arg_c` | kind 5 only |
| 0x10 | 12 | `position` | `Vec3` — kind 1 attack point, kind 2 destination position, kind 3 freeze position |
| 0x1c | 4 | `target_object` | kind 2: destination map-section/node object. Explicitly 0 for kind 3 |
| 0x20 | 1 | `flag` | kinds 0/1: the "queued" byte derived from the wire id |
| 0x24 | 4 | `arg_d` | kind 6 only |

**`+0x04` is an actor *id*, not an `Actor *`** — measured: the kind-6 handler does
`MOV ECX,[rec+0x04]; CALL GetActorById` at 0x00535195, and the kind-0 handler does the same at
0x0053dbd1. That also makes the record savegame-safe (`Read/WriteActorFixups` @ 0x00530900 /
0x00531cf0 serialise the queue).

`0x10..0x1b` is constructed and destroyed through `eh_vector_{constructor,destructor}_iterator`
with `count=3, size=4, ctor=0x0044cc60, dtor=0x0044d3e0` — and **both of those are empty**
(`MOV EAX,ECX; RET` and `RET`). It is a plain `Vec3`; MSVC just emits the iterators because the
type is a class. The dead space is `0x21..0x23` plus whichever of `arg_a..arg_d` a given kind does
not set — **those are left as raw pool memory**, so `position` on a kind-0 record is garbage and
must not be read.

### 2.1 The queue itself

`MobileActor+0x1f0` is a `List<PendingOrder*>` in the **pointer-anchored** form:
`{sentinel* @0x1f0, count @0x1f4, cache @0x1f8, cache_valid @0x1fc}`. Node is
`List_Member<T>` = `{vptr 0x00666214, prev+0x4, next+0x8, data+0xc}`.

| Operation | Address | Form |
|---|---|---|
| append at tail | `List_AppendTail` **0x004d02a0** | `__thiscall(List*, T**)`, `RET 0x4` |
| prepend at head | `List_PrependHead` **0x0054f7d0** | `__thiscall(List*, T**)`, `RET 0x4` |
| `HasPendingOrders` (slot 32) | 0x0054f0f0 | `CMP [ECX+0x1f4],0` |
| pop front + broadcast **0x86** | `MobileActor::PopFrontOrder` **0x00538700** | `__thiscall`, bare `RET` |
| remove tail | `MobileActor::CancelLastOrder` **0x00539030** | `__thiscall`, bare `RET` |
| clear all | `MobileActor::ClearOrderQueue` **0x00538830** | `__thiscall`, bare `RET`; also sets `Actor+0x13c = 1` |

`MobileActor::ClearOrderQueue` **is** the queue drain the brief asked about, but only in the "throw everything
away" sense — it pops and pool-frees every record and executes nothing. The *executor* is slot 70
(§4).

---

## 3. `OrderKind`

Recovered from the constant each pusher stores at `rec+0x00` and from the two dispatchers.
Values 0..10, no gaps.

| Kind | Name | Pusher | Consumer |
|---|---|---|---|
| 0 | `AttackTarget` | `CharacterActor::QueueOrderTarget` 0x00541a20, `RET 0x8` | CharacterActor slot 70 → slot 96 `AttackTarget` |
| 1 | `AttackPosition` | `CharacterActor::QueueOrderPosition` 0x00541ac0, `RET 0x10` | CharacterActor slot 70 → slot 97 `AttackPosition` |
| 2 | `GotoObject` | `MobileActor::QueueGotoObjectOrder` 0x00538b20, `RET 0x8` | MobileActor slot 70 → `GotoObject` 0x005394d0 |
| 3 | `HoldMarker` | `MobileActor::PushHoldMarkerOrder` 0x00538a30, bare `RET` — **prepended** | none: the jump table maps kind 3 to the *exit* label, so it blocks the queue |
| 4 | `CrouchToggle` | `MobileActor::QueueCrouchOrder` 0x00538be0, bare `RET` | MobileActor slot 70 → slot 83 `ToggleCrouchAndCamouflage` |
| 5 | `Interact` | `MobileActor::QueueInteractOrder` 0x00538e80, `RET 0x8` | MobileActor slot 70 → `EquipItemInSlot(arg_c, arg_b, 1)` @ 0x00536ba0 |
| 6 | `GiveItem` | `MobileActor::QueueGiveItemOrder` 0x00538f80, `RET 0xc` | MobileActor slot 70 → `GetActorById(arg_a)`, `this+0x40 = target` (refcounted), `+0x44 = arg_b`, `+0x124 = arg_d` |
| 7 | `Equip` | `MobileActor::QueueEquipOrder` 0x00538ca0, `RET 0x4` | MobileActor slot 70 → `UseInventoryItem(arg_b, 1)` @ 0x005370d0 |
| 8 | `Drop` | `MobileActor::QueueDropOrder` 0x00538d40, `RET 0x4` | MobileActor slot 70 → `MobileActor::DropItem(arg_b)` @ 0x00538240 |
| 9 | `UseItem` | `MobileActor::QueueUseItemOrder` 0x00538de0, `RET 0x4` | MobileActor slot 70 → walk `inventory_list` for `entry+0x24 == arg_b`, `MobileActor::UnequipSlot(entry)` @ 0x00536ec0, broadcast **0x80** |
| 10 | `SetAmmoType` | `CharacterActor::QueueOrderKind10` 0x00541980, `RET 0x4` | CharacterActor slot 70 → slot 99 `SetAmmoType(arg_b)` |

Names for 5/6/7 are taken from the *immediate* console/wire twin that reaches the same callee
(§8) — **and that provenance has now cost one name, so treat the other three as open.** Kind 6 was
called `Board` on `directplay_protocol_notes.md`'s reading of command 0x17 ("board / attach
(escort)"); the consumer is `GiveItemTo`, so it is **`GiveItem`**, and both it and its client twin
`Unit_SendGiveItem` @ 0x004c0d50 have been renamed. (Breadcrumb: outside write-ups still say
`Board` / `Unit_SendBoard` / `QueueBoardOrder`.)

**Kinds 5, 7 and 9 may be misnamed on exactly the same provenance — recorded as an open doubt, not
acted on.** Each name comes from the wire twin, and each arm's *callee* says something else:

| kind | current name | what its arm actually calls |
|---|---|---|
| 5 | `Interact` | `EquipItemInSlot(arg_c, arg_b, 1)` @ 0x00536ba0 |
| 7 | `Equip` | `UseInventoryItem(arg_b, 1)` @ 0x005370d0 |
| 9 | `UseItem` | walk `inventory_list`, `UnequipSlot(entry)` @ 0x00536ec0, broadcast 0x80 |

The three look **rotated by one** relative to their callees, which is suggestive and is *not*
evidence. **UNMEASURED — do not rename on the strength of this table.** Settling it needs the
0x13/0x14/0x19-family wire arms read against these three callees, which is what would say whether the
wire names or the callees are the mislabelled side. Kind 8 is **`Drop`**: its consumer is
`MobileActor::DropItem` @ 0x00538240 and its client producer is `Unit::Unit_SendDropItem`
@ 0x004c0cf0, whose 12-byte payload is `{id, actor_id, item_id}` with the id chosen by
`CMP byte [EBP+0xc],0` — 0x14 queued, 0x13 immediate.

### 3.1 Correction to `src/Actors.h`

Slot 86 `QueueOrderPosition` ends `RET 0x10` and the four stack dwords are **not**
`(Vec3*, int, char, int)`. The prologue is

```
00541b31  MOVQ  XMM0, qword ptr [EBP + 0x8]     ; args 1..2
00541b36  MOVQ  qword ptr [ESI + 0x10], XMM0
00541b3b  MOV   EAX, dword ptr [EBP + 0x10]     ; arg 3
00541b3e  MOV   dword ptr [ESI + 0x18], EAX
00541b41  MOV   AL,  byte ptr [EBP + 0x14]      ; arg 4
00541b44  MOV   byte ptr [ESI + 0x20], AL
```

and the epilogue destroys the **by-value** parameter at `[EBP+8]` with the same 3×4 vector
destructor. So the real signature is `__thiscall void QueueOrderPosition(Vec3 pos /*by value,
12 bytes*/, char flag)` — 12 + 4 = 0x10. Slot 90 `AddWaypoint` also ends `RET 0x10`; whether it
is the same shape was **not** re-checked.

`MobileActor`'s own slots 85/86/87 (0x0054e5d0 / 0x0054e5f0 / 0x0054e5e0) are **stubs** — three
instructions each apart from 86, which exists only to destroy its by-value argument. Only
`CharacterActor` overrides them with real bodies, so kinds 0, 1 and 10 can only ever exist on a
`CharacterActor`.

---

## 4. The executor: vtable slot 70

Slot 70 is the per-tick `Update`; `__thiscall`, `RET 0xc` in every override (three stack args, the
third a float that both dispatchers forward as a priority). There are **two** independent order
dispatchers and neither calls the other.

### 4.1 `MobileActor` slot 70 @ **0x00533720** (9,066 bytes) — kinds 2..9

Dispatch at 0x00535104:

```
EAX = order_queue.sentinel->next->data
EAX = rec->kind - 2
if (EAX > 7) skip
JMP dword ptr [EAX*4 + 0x00535aa0]
```

Jump table 0x00535aa0, eight entries: `2→0x00535125, 3→0x0053533c (the exit label),
4→0x00535172, 5→0x00535181, 6→0x00535195, 7→0x00535329, 8→0x00535204, 9→0x00535213`.
Every arm falls through to `PopFrontOrder` at 0x00535337.

Re-verified arm by arm, with the payoff and whether the order is actually dequeued. **Kind 3 is the
odd one and the table's bound-fail target both**, and because it sits *after* `PopFrontOrder`
@ 0x00535335 the order is never dequeued — which is the mechanism behind "HoldMarker blocks the
queue":

| kind | arm | payoff | dequeued? |
|---|---|---|---|
| 2 | 0x00535125 | slot 63 test; false → `GotoObject`, `+0x44 = arg_a`; true → slot 83 | conditional |
| 3 | 0x0053533c | **none** — the default / bound-fail target, sitting *after* `PopFrontOrder` | **no** |
| 4 | 0x00535172 | slot 83 on `this` | yes |
| 5 | 0x00535181 | `EquipItemInSlot(arg_c, arg_b, 1)` | yes |
| 6 | 0x00535195 | `GetActorById(arg_a)`, `GotoObject`, then `held_actor` / `pending_action_id` / `pending_action_amount` | yes |
| 7 | 0x00535329 | `UseInventoryItem(arg_b, 1)` | yes |
| 8 | 0x00535204 | `DropItem(arg_b)` | yes |
| 9 | 0x00535213 | slot 34 walk, `UnequipSlot(entry)`, broadcast **0x80** | yes |

One thing this settles about kind 6: **`QueueGiveItemOrder` @ 0x00538f80 writes no field of `this`
at all** — it only allocates the record and appends it. So the `+0x40`/`+0x44`/`+0x124` writes in
§3's table belong to the **Consumer** column, i.e. to tick time in this arm, which is where that
table already put them. Only the *name* `Board` was ever wrong.

Gates, all immediately above the dispatch and all `JNZ skip`:

| Test | Address |
|---|---|
| `move_state (+0x1bc) == 0` | 0x005350c4 |
| `+0x120 == 0` | 0x005350d1 |
| `!IsActivePauseOn()` | 0x005350dd |
| `waypoint_active (+0x1c4) == 0` | 0x005350ea |
| `+0x12c == 0` | 0x005350f6 |

### 4.2 `CharacterActor` slot 70 @ **0x0053d8d0** (12,532 bytes) — kinds 0, 1, 10

Dispatch at 0x0053db72, an if-ladder rather than a table:
`SUB EAX,0 / JZ` → kind 0, `SUB EAX,1 / JZ` → kind 1, `SUB EAX,9 / JZ` → kind 10.

- kind 0 → `GetActorById(rec->arg_a)`, then slot 96 `AttackTarget(target, arg3, 0, rec->flag)`
- kind 1 → slot 97 `AttackPosition(&rec->position, arg3, 0, rec->flag)`
- kind 10 → slot 99 `SetAmmoType(rec->arg_b)`

then `PopFrontOrder` at 0x0053dbf4. Gates: `move_state == 0` (0x0053db3b), slot 7 false,
`!IsActivePauseOn()` (0x0053db59), `DAT_007b3de6 == 0`.

A second site at 0x0053dcc3 **drains the whole queue** (`while (count) PopFrontOrder()`) on the
panic/flee path — and only when `hold_ground` is clear (0x0053dca9). So "Hold ground" also
suppresses a character throwing its orders away when it panics.

---

## 5. Why Attack-then-Go is simultaneous and Go-then-Attack is sequential

**Measured, and it is not the queue.** Every arm of both dispatchers pops its record in the same
tick it executes it, so no order ever "occupies" the queue. The sequencing comes entirely from the
`move_state (+0x1bc) == 0` gate at the top of both dispatchers:

- An **attack** order writes resident state on `CharacterActor` (`is_attacking` +0x2d4,
  `attack_target` +0x2d8, `attack_position` +0x2dc) and pops. `move_state` is untouched **when the
  target is already in range**, so the next tick pulls the following order and the character walks
  while firing.
- A **go** order (kind 2) calls `GotoObject` @ 0x005394d0, which starts a traversal and leaves
  `move_state != 0` for its whole duration. The gate then blocks every further dispatch until the
  move completes, so a queued attack waits.

The asymmetry is therefore a property of *which state each order leaves behind*, not of ordering.
One caveat, measured: slot 96 `AttackTarget` @ 0x00540c60 itself calls slot 88 `Goto` at
0x00540d36 when the target is out of range — in that case the attack order *does* set
`move_state` and the queue blocks like a move. So "attack is non-blocking" holds only for a target
already within weapon range.

---

## 6. Standing orders

### 6.1 The dispatcher

`MobileActor::SetStandingOrder` @ **0x00538690**, `__thiscall`, `RET 0x4`, jump table
0x005386dc (7 entries, codes 0..6). Its client-side mirror is `Unit_SendStandingOrder`
@ **0x004bdd60** (`RET 0x4`), which sends command **`0x1b`**, size 0xc, `{0x1b, unit_id, code}`
and then applies the same three arms locally.

| Code | Name (GLkeys) | Effect (server @0x00538690) | Effect (client @0x004bdd60) |
|---|---|---|---|
| 0 | `Cluster formation` | `TeamSlots[this->team_id].formation (+0xb0) = 0` | `TeamSlots[Unit+0xb4].+0xb0 = 0` |
| 1 | `Side by side` | `… = 1` | `… = 1` |
| 2 | `Single file` | `… = 2` | `… = 2` |
| 3 | `Hold ground` | **toggles** the byte at `this+0x1b0` | toggles `Unit+0x1c8` |
| 4 | `Always fire` | `this+0x1ac = 4` | `Unit+0x1c4 = 4` |
| 5 | `Return fire` | `this+0x1ac = 5` | `Unit+0x1c4 = 5` |
| 6 | `Never fire` | `this+0x1ac = 6` | `Unit+0x1c4 = 6` |

Note the fire policy stores **4/5/6**, not 0/1/2 — the enum is shared with the formations, and the
three formation codes never reach the actor at all.

`MobileActor+0x1ac` is `standing_fire_policy`; `MobileActor+0x1b0` is `hold_ground`. Both are
serialised by `Read/WriteActorFixups`. Defaults: `MobileActor::Ctor` @0x005324b0 writes
`hold_ground = 1`; `MobileActor::SetTeamId` @0x00533430 then writes `standing_fire_policy = 6`
(**Never fire**) and `hold_ground = 0` at 0x005334b2/0x005334bc.

### 6.2 Who reads the fire policy

Two sites, and only two:

- **Proactive acquisition** — `MobileActor` slot 70 @ **0x005338ac**: `CMP [EDI+0x1ac], 4; JNZ`.
  Everything below that (the enemy scan and the target selection that follows) runs **only under
  Always fire**.
- **Retaliation** — `ProjectileActor::OnPrePhysics` @ **0x005442ae**: `CMP [ESI+0x1ac], 6; JZ skip`,
  where `ESI` is a nearby `MobileActor` and `EBX` is the shooter. **Never fire** is the only value
  that suppresses it.

`Return fire` (5) is therefore the *absence* of both extremes — it retaliates (not 6) but does not
seek (not 4). **There is no "was shot by" record**: the shooter is `EBX`, a live argument of the
projectile's own physics step, so retaliation is computed at flight time and nothing is stored.
The premise in the brief that Return fire needs such a record is **refuted**.

`hold_ground` is read at 0x00533dbd (in slot 70, immediately before `CALL [vtbl+0xe0]`, i.e. it
suppresses *closing on* an acquired target — the actor still fires, it just does not advance),
at 0x005443b6 in `ProjectileActor::OnPrePhysics` (same, for the retaliation path) and at
0x0053dca9 (the panic drain, §4.2).

### 6.3 Applying to the selection

`ApplyStandingOrderToSelection` @ **0x004a2010** — code in **ECX**, bare `RET`, no stack args.
Walks `SelectedUnits` and calls `Unit_SendStandingOrder(unit, code)` for every entry whose vslot 36
(`IsMobile`) is true. On an empty selection: `PlayUiSound(0x28)` and on-screen string id 0x2b18.

Only codes 3..6 come through here. The three **formation** keys bypass it entirely and write
`TeamSlots[LocalPlayerTeam].formation` inline in `HandleGameKeyAction` (0x0046fd27 / 0x0046fd5d /
0x0046fd93), where `LocalPlayerTeam` @ 0x006a58e0 is the local player's team index. That means a formation change
from the keyboard is **local only** — it does not go through command 0x1b and does not replicate.

---

## 7. Formations

The formation is one `int` per team: `TeamSlots[team] + 0xb0` (`TeamSlots` @ 0x007b3ec4, stride
0xc4), values 0 Cluster / 1 Side by side / 2 Single file.

**There is no per-member offset computation, and this is settled — do not go looking for it.**
An exhaustive dedicated sweep found `TeamSlot+0xb0` has exactly **three** readers, and none of
them is movement, AI, pathing or order-issuing code:

| Reader | What it does with the value |
|---|---|
| `HudItem_DrawByKind` @ 0x0055fbd0, read @ 0x00560af0 | picks one of three adjacent 33/512-wide U-spans in a sprite atlas — the formation **icon** |
| `WaitCond_FriendliesUseAbreastFormation` @ 0x00570840, read @ 0x00570858 | `== 1`; a `WAIT FOR` script predicate, table entry @ 0x0066a40c |
| `0x004d3280` (vtable 0x006664c4 slot 17), read @ 0x004d32d5 | the order menu's "is this order currently in effect?" query |

The ground-click path is offset-free end to end: `IssueGroundTargetOrderToSelection` @ 0x0049f010
(renamed — see §8.4) hands **the identical `Vec3 *`** to every selected unit (the pushed pointer is
loop-invariant across the selection walk), `Unit` slot 75 @ 0x004c3c90 copies it verbatim into the
0x18-byte command, and the executor arm copies it verbatim into `CharacterActor+0x2dc`. No per-member
index, no perpendicular or trailing basis, no spacing constant, no unit-radius term.

One branch of that function is **not** a movement order at all: with `DecoyTargetPending` set it
dispatches slot 80 `Unit_SendThrowDecoy` instead of slot 75, and with `queued` also set it
dispatches nothing. See §8.3.

**So there is also no leader or anchor** — every member of the selection is given the clicked world
point directly, and the spread visible in play is the nav agent's own collision avoidance
converging on a shared goal. The formation setting is a HUD indicator and a script-visible flag,
nothing more.

Sweep coverage, so this is re-checkable: 158 references to `TeamSlots`, 110 `IMUL reg, …, 0xc4`
slot-pointer sites, and all 141 instructions in `.text` carrying a `0xb0` displacement — of which
exactly 8 are in the two-register TeamSlot form. The rebased idiom (`ADD reg,[0x007b3ec4]` then
`[reg+disp]`) reaches displacements 0x64, 0x6a, 0x70, 0x74, 0x7c, 0x84, 0x94, 0x9c, 0xa0, 0xac —
**no 0xb0**. No site anywhere pushes or stores a formed TeamSlot pointer, so no callee can reach
`+0xb0` off a parameter. `ClearRouteWaypoints` / `PushRouteWaypoint` / `ExitFlareMode` / `EnterFlareMode` are
excluded by that sweep and are not worth reading for this.

One thing the waypoint side *did* settle, since it bears on whether any of this could be formation
state: the waypoint record's `+0x0c` is **`keep_on_arrival`**, and **no waypoint the shipped game
creates is ever retained on arrival.** All four `PushRouteWaypoint` call sites (0x005398ea,
0x0053c633, 0x00452ae4, 0x00452b0d) and both `AppendPatrolPoint` sites (0x0045489a, 0x00454e8f)
`PUSH 0x0`; only slot 90 `AddWaypoint` forwards a caller-supplied value, and it is reachable only
through the client/executor vtables, i.e. from the wire. The reader at 0x0053a2cf pops and frees on
zero and retains on non-zero, so the retain path exists and is unreachable in practice — a dead
*branch*, not dead code. Full write-up, including the record's uninitialised `+0x14`, in
`game_defects_notes.md` §19.

---

## 8. Replication — orders are wire messages, always

**Every** `PendingOrder` push in the binary is reached from `ExecutorThreadProc` @ 0x00509050
(the client→server command switch at 0x005092a4) or from `ApplyUpdateMessage`. There is no local
push path. The client-side producers are client `Unit`-tree vtable slots, and **their owners are two
different classes** — the earlier reading, "table base 0x00664ac8", filed both groups under a
descendant, which is the shallowest-owner error CLAUDE.md warns about. Measured ownership
(`rendering_notes.md` §5.1):

* slots **100-104** (`Unit_SendInteract`, `Unit_SendEquip`, `Unit_SendDropItem`, `Unit_SendGiveItem`,
  `Unit_SendUseItem`) are **added by `MobileUnit`, table base 0x0066491c** — not by
  `CharacterUnit`. `MobileUnit` also adds 92 `Unit_IsCrouched` and 93
  `Unit_SetCrouchedAndConcealed`, and overrides 33 `Unit_SetTeamWithInventory`,
  55 `Unit_Dissociate`, 57 `Unit_UpdateMovement`, 73 `Unit_SendStop`,
  77 `Unit_SendCrouchToggle`, 83 `Unit_SendSetAmmoType` and 91 `LeaveWorld`. These take a
  trailing `bool queued` and choose between an immediate id and `id + 1`, as the disassembly below
  shows;
* slots **74, 75, 76, 78, 79 and 80** are **`CharacterUnit`'s overrides of base slots**, table base
  0x00664ac8, which also **adds** extension slots 108 `Unit_SetAttackTarget`,
  109 `Unit_SetAttackPosition`, 110 and 111. They are 0x004c3da0, `Unit` slot 75 @0x004c3c90,
  0x004c3e40, `Unit_SendAttackTarget`, `Unit_SendAttackPositionQueued`, and 0x004c4040 — which is **not**
  a sixth movement variant but `Unit_SendThrowDecoy` — see §8.3. The other five take **no
  `queued` argument**. They
  read `TEST byte ptr [KeyModifierState @0x007b6ddc],0x2` — the **CTRL** bit — and their second id
  is `id + 2`, not `id + 1`. That second id does not mean "queued" at all: it means
  **`close_range`**, which is where `Actor` slots 96/97's computed `0x41 + close_range` /
  `0x3f + close_range` broadcast ids come from, and which also scales `Unit+0x2c4` (engagement
  range) by 0.65.

```
004c0bd0  CMP  byte ptr [EBP + 0x10], 0     ; the `queued` argument
004c0bdf  JZ   immediate
004c0be3  MOV  dword ptr [EBP + -0x14], 0x10   ; queued id
004c0bea  CALL 0x004be540                       ; + build the on-screen order marker
...
004c0bf1  MOV  dword ptr [EBP + -0x14], 0xf     ; immediate id
```

| Immediate | Queued | Kind | Client producer (`Unit` slot) | Server handler |
|---|---|---|---|---|
| `0x1e` | `0x20` | 0 `AttackTarget` | `Unit_SendAttackTarget` 0x004c3fc0 (slot 0x138) | vslot 87 `QueueOrderTarget` @0x00509e7d |
| `0x1d` | `0x1f` | 1 `AttackPosition` | `Unit_SendAttackPositionQueued` 0x004c3e90 (slot 0x13c) | vslot 86 `QueueOrderPosition` @0x00509e48 |
| — | `0x08`, `0x21` | 2 `GotoObject` | (map-section move; `0x21` is the multi-actor batch) | `QueueGotoObjectOrder` @0x00509463/0x00509492/0x00509906 |
| — | `0x26` push / `0x25` pop | 3 `HoldMarker` | `ActivateUnitAndResume` 0x0046ef50 | `PushHoldMarkerOrder` @0x0050a10c / `ReleaseHoldMarkerOrder` @0x0050a0b0 |
| `0x1c` | `0x22` | 4 `CrouchToggle` | `Unit_SendCrouchToggle` 0x004c09d0 (slot 0x134) | `QueueCrouchOrder` @0x00509dc9 |
| `0x0f` | `0x10` | 5 `Interact` | `Unit_SendInteract` 0x004c0bc0 (slot 0x190) | `QueueInteractOrder` @0x0050a21c |
| `0x17` | `0x18` | 6 `GiveItem` | `Unit_SendGiveItem` 0x004c0d50 (slot 0x19c) | `QueueGiveItemOrder` @0x0050a2e8 |
| `0x11` | `0x12` | 7 `Equip` | `Unit_SendEquip` 0x004c0c90 (slot 0x194) | `QueueEquipOrder` @0x0050a33a |
| `0x13` | `0x14` | 8 `Drop` | `Unit::Unit_SendDropItem` 0x004c0cf0 (slot 0x198) | `QueueDropOrder` @0x0050a38a |
| `0x15` | `0x16` | 9 `UseItem` | `Unit_SendUseItem` 0x004c0dc0 (slot 0x1a0) | `QueueUseItemOrder` @0x0050a4af |
| `0x19` | `0x1a` | 10 `SetAmmoType` | `Unit_SendSetAmmoType` 0x004c0c30 (slot 0x14c) | vslot 85 `QueueOrderKind10` @0x0050a52c |
| — | `0x1b` | — | `Unit_SendStandingOrder` 0x004bdd60 | `SetStandingOrder` @0x0050a4c2 |
| — | `0x23` | — | `Unit_SendCancelLastOrder` 0x004be6f0 | `CancelLastOrder` @0x0050a015 |
| — | `0x09`, `0x0a`/`0x0c`, `0x0b`/`0x0d` | — | — | each calls `ClearOrderQueue` first |

Server→client updates the order layer emits: **0x86** (a record was popped, from `PopFrontOrder`),
**0x66** (a `HoldMarker` was released), **0x65** (team-wide activate, tail of case 0x26),
**0x80** (a `UseItem` order consumed an inventory entry).

### 8.1 Corrections to `directplay_protocol_notes.md` §6

Six rows of the existing table describe the wrong thing. All six are measured here:

| id | Existing note | Actual |
|---|---|---|
| `0x19` | "set movement state" | select ammo type, immediate (`Unit_SendSetAmmoType`) |
| `0x1a` | "set health (`SetHealth` vcall), arg1=`f32`" | select ammo type, **queued** → vslot 85, kind 10 |
| `0x1c` | "destroy actor" | crouch/conceal toggle, immediate → slot 83 |
| `0x1d`/`0x1f` | "query health into buffer" | attack ground → vslot 86, kind 1 |
| `0x1e`/`0x20` | "query center coords into buffer" | attack target → vslot 87, kind 0 |
| `0x25`/`0x26` | "team command -> re-broadcasts `0x65`" | `0x26` pushes a `HoldMarker` on **every other** team member (walk of the per-team list at `0x007ba038 + team*0x10`) *then* broadcasts 0x65; `0x25` releases one |

Also missing from that table entirely: `0x10`, `0x12`, `0x14`, `0x16`, `0x18`, `0x21`, `0x22`.

### 8.2 A probable shipped bug

In the `0x1d`/`0x1f` case block the "queued" byte is computed as

```
00509e2b  CMP   EDI, 0x20        ; EDI is the raw command id
00509e33  SETZ  CL
```

`0x20` is not one of that block's ids — the neighbouring `0x1e`/`0x20` block uses the identical
two instructions, where the test *is* correct. So `PendingOrder::flag` is always 0 for an
attack-ground order — and the consequence *is* now known: that fourth argument is
**`close_range`**, so a queued attack-ground order can never be a close-range one. On the
immediate path the same argument is supplied correctly as `(command_id == 0x0c)` at 0x00509eab,
with the third argument a literal 0.

**Now confirmed from the jump table rather than by inspection of the ids.** Decoding
`ExecutorThreadProc`'s switch — byte index table @ 0x0050bae0 into targets @ 0x0050ba3c, index
`id - 4`, bounded `CMP EAX,0x39 / JA` — gives the arm at 0x00509e14 an id set of **exactly
{`0x1d`, `0x1f`}**, so `ZF` provably can never be set there. The full decode also confirms the
neighbours: 0x00509e5c ← {`0x1e`, `0x20`}, 0x00509e91 ← {`0x0a`, `0x0c`}, 0x00509fae ←
{`0x0b`, `0x0d`}. Cross-referenced as `game_defects_notes.md` §18. The general lesson is in
CLAUDE.md's Analysis Traps: an arm's compare against a constant means nothing until you know which
ids reach the arm.

### 8.3 Slot 80 is the decoy throw, not a movement order

`0x004c4040` was `Unit_SendUseAbilityAtPosition` here and in the slot tables; it is
**`Unit_SendThrowDecoy`**, `__thiscall void(Unit *, Vec3 *pos)`, `RET 0x4` @ 0x004c4113, owned by
`CharacterUnit`. It sends command **`0x2b`**, 24 bytes `{0x2b, unit_number, f32
GetGameTimeSeconds, Vec3 pos}`, and `directplay_protocol_notes.md` already documented `0x2b` as
"throw decoy at a position → `MobileActor::ThrowDecoy` @ 0x00541170", so both ends now agree.

**The gate is in the caller, not under the slot.** `IssueGroundTargetOrderToSelection` @ 0x0049f06f loads
`DecoyTargetPending` @ 0x007b3f52 (set when the player arms a decoy — `gadgets_notes.md` §2) and
picks:

| `queued` | `DecoyTargetPending` | dispatched |
|---|---|---|
| 0 | set | slot **80** `Unit_SendThrowDecoy` (`CALL [EAX+0x140]` @ 0x0049f0b3) |
| 0 | clear | slot 75 (`CALL [EAX+0x12c]` @ 0x0049f0a2) |
| 1 | set | **nothing at all** (`TEST AL,AL` / `JNZ 0x0049f0ce` @ 0x0049f0bb) |
| 1 | clear | slot 79 (`CALL [EAX+0x13c]` @ 0x0049f0c8) |

So a **queued** decoy click — Active Pause, or shift-click — is silently dropped: no command, no
error, and the flag stays armed. That is a game defect, and it is why §7's move path never sees the
decoy branch as a movement order.

### 8.4 None of these is a *move* order, and three names had to change

**No arm anywhere in the 41-slot command table calls `MobileActor::Goto` (slot 88), the actual move
primitive.** The four position/target arms pair up as (immediate, queued) x (position, target):

| ids | Actor slot | body |
|---|---|---|
| `0x0a` / `0x0c` | 97 | `CharacterActor::AttackPosition` — **immediate fire at a point** |
| `0x0b` / `0x0d` | 96 | `CharacterActor::AttackTarget` |
| `0x1d` / `0x1f` | 86 | `CharacterActor::QueueOrderPosition` — queue order kind 1 |
| `0x1e` / `0x20` | 87 | `CharacterActor::QueueOrderTarget` — queue order kind 0 |

That made three client-side names actively misleading, and all three are **renamed in the Ghidra DB**
(breadcrumbs left, because `directplay_protocol_notes.md` and other write-ups still use the old ones):

| was | now | why |
|---|---|---|
| `Unit_SendMoveOrder` @ 0x004c3c90 (slot 75) | **`Unit_SendAttackPositionImmediate`** | sends 0x0a/0x0c → slot 97, which clears `attack_target`, sets `is_attacking = 1`, breaks concealment and broadcasts 0x3f. It is an attack |
| `Unit_SendAttackPosition` @ 0x004c3e90 (slot 79) | **`Unit_SendAttackPositionQueued`** | sends 0x1d/0x1f → slot 86, the *queued* variant. The old name did not distinguish it from slot 75 |
| `IssueMoveOrderToSelection` @ 0x0049f010 | **`IssueGroundTargetOrderToSelection`** | its three dispatch targets are attack-position, queued-attack-position and throw-decoy; it ends by setting `Cursor_AttackGround` |

**`0x0c` is not a queued variant** — it is the **close-range lob**, chosen on
`KeyModifierState & 2` (LCTRL), and the client pre-computes the same
`muzzle_speed * CloseRangeSpeedFactor` the callee will. `Actor` slot 97 itself is **not** misnamed;
that question, left open in the DB plate on slot 75, is now closed.

### 8.5 The client's mouse dispatch: the interface handler matrix

This is where a ground click *comes from*, and it is what makes every `SetCursorMode` call a
statement about which handler set is live.

There are **three registry objects, one per mouse button**, selected by the `msg - 0x201` jump table
@ 0x00470c3c inside `HandleKeyPress3`: `LeftButtonInterface` @ 0x007b41f8,
`RightButtonInterface` @ 0x007b4498, `MiddleButtonInterface` @ 0x007b3f88. Each holds **three 6x7
handler matrices** — click `+0x40`, drag `+0xe8`, button-down `+0x190`, each `6*7*4 = 0xa8` bytes,
tiling to `0x238` (so `0x40 + 6*0x1c == 0xe8` is a coincidence, not a two-matrix layout):

```
cell = registry + base + target_class*0x1c + cursor_mode*4
```

**The 7-valued dimension (stride 4) is `CursorMode`; the 6-valued dimension (stride 0x1c) is the
cursor target class.** Measured from both dispatchers' index arithmetic
(`LEA ECX,[EAX*8]; SUB ECX,EAX; ADD ECX,ESI` = `col*7 + mode`, where `EAX` is
`ClassifyCursorTarget()` and `ESI` is `CursorMode` @ 0x007b3f78) and independently from the
registrars' loop bounds. Registration masks encode it as: **bits 0..6 = `CursorMode` rows,
bits 7..12 = target-class columns**, and a cell is written iff *both* its bits are set.

`CursorTargetClass` (the column, from `ClassifyCursorTarget` @ 0x004a77a0): 0 nothing under cursor,
1 world location only, 2 hostile unit, 3 a unit whose `TeamSlot+0x6b` `not_enemy_source` flag is set,
4 own/allied unit, 5 HUD element.

Everything is installed in one place — `RegisterAllInterfaceHandlers` @ 0x0049c290, whose sole caller
is `BeginLevelSession` @ 0x004e25c7 — as 6 clears (mask `0x1fff`) plus **22** registrations, and
**registration order matters**: a later write overwrites an earlier cell, which is what makes
`FUN_004a4100` dead (registered for row 2 / column 1 at 0x0049c3bd, overwritten at 0x0049c421 by
`OnGroundTargetClick`'s all-columns registration for the same row). Two structural facts worth
keeping: **`CursorMode` 6 (`Suspended`) is never filled in any matrix of any registry**, and the only
configuration-dependent cells in the whole table are one drag pair keyed on `RightMouseScrollMode`
@ 0x007b9cbc (a GLkeys.cfg / Options setting) at 0x0049c348-0x0049c359.

**Flare mode is the worked example, end to end.** Entry is *not* a matrix cell — it is the
`GL_CONTROLS_FIREFLARE` key binding (default DIK_F) calling `EnterFlareMode` @ 0x004a17e0, which
arms each selected unit (`Unit+0x284 = 1`, prior loadout saved to `+0x27c`/`+0x280` with `0x21` as the
"nothing saved" sentinel) and ends with `SetCursorMode(GroundTarget)`. The matrix owns the other two
thirds: **commit** is `OnGroundTargetClick` @ 0x004a4420 (LMB click matrix, row `GroundTarget`, all
six columns) → `IssueGroundTargetOrderToSelection`, and **cancel** is `CancelGroundTargeting`
@ 0x004a44c0 (the RMB cell in the same row) → `ExitFlareMode`. In flare mode
`IssueGroundTargetOrderToSelection` additionally requires each unit's `+0x284`, so **only
flare-armed units respond at all**. See `gadgets_notes.md` §6 for why the *AI* half of the flare
feature is nonetheless dead.

---

## 9. Active Pause

`ActivePauseOn` @ **0x00738f78**, `int`. Read through `IsActivePauseOn` @ 0x0046a550
(`CMP [0x00738f78],1; SETZ AL`).

**What it gates — the complete list of `IsActivePauseOn` callers**: `MobileActor` slot 70
(0x005350dd), `CharacterActor` slot 70 (0x0053db59), `Unit_DrawWithTeamState` (0x004bf2ca, the
order markers), 0x0049ef10, 0x0049f010, 0x004a6280, 0x004c0a30, 0x00558550. It is **not** consulted
by the executor thread loop, by the clock, or by any AI or physics step. So the flag itself only
withholds **order dispatch** (plus some UI) — the manual's description is correct *as far as the
flag goes*.

But the toggle does more. `ToggleActivePause` @ **0x0046f4a0**, `__stdcall`, bare `RET`:

```
if (ActivePauseOn != 0)          goto OFF
if (IsMultiplayerActive())       goto OFF        ; so it can never turn ON in MP
ActivePauseOn = 1
DAT_00739088 = GetClockScale()                    ; 0x00505350
SetClockScale(0.0f)                               ; 0x00505380 with ECX = &local(0.0f)
return
OFF: ActivePauseOn = 0; SetClockScale(&DAT_00739088)
```

So **in single player Active Pause also sets the simulation clock scale to zero** — the world
really does stop, and the flag's job is only to stop new orders leaking out. In multiplayer the
key does nothing at all through this path; `EnableActivePause` / `DisableActivePause`
(0x0046a520 / 0x0046a4f0) tail-jump into the toggle **only when `GameMode == 0`**, and otherwise
set/clear the flag without touching the clock. Both of those are called from
`ApplyUpdateMessage` (0x004ffbe4 / 0x004ffc18), so Active Pause is a replicated state in
multiplayer, flag-only, with the world still running.

Three Gameplay keys (increase/decrease/reset game speed) are refused outright while the flag is
set — 0x00470121, 0x0047028a, 0x00470410.

### 9.1 How pressing 1-5 ends it

`ActivateUnitAndResume` @ **0x0046ef50**, `__thiscall(Unit *this)`, bare `RET`, bound to the five
character keys:

```
if (ActivePauseOn == 1) {
    SendToServer({0x26, unit->id}, 8);
    GameMode == 0 ? ToggleActivePause() : SendPauseToggleRequest();
} else if (HasPendingOrders() && IsMobile() && head order kind == 3) {
    SendToServer({0x25, unit->id}, 8);          // release the hold marker
} else {
    ClearSelection(); AddToSelection(this); optionally recentre the camera
}
```

Command `0x26`'s server arm (0x0050a0c3) walks the per-team actor list at
`0x007ba038 + team_id*0x10` and calls `PushHoldMarkerOrder` on **every member except the
commanding one**, then broadcasts 0x65. Since a kind-3 record is prepended and the MobileActor
jump table maps kind 3 to the exit label, those actors stop dequeuing while the activated one
runs its queue. Pressing the key again with the marker at the head sends `0x25`, which pops it.

That is the whole mechanism: Active Pause is "queue orders with time stopped"; activating a
character is "start time again, but freeze everybody else's queue".

---

## 10. Selection

**The selected set is a `HashTable<Unit*>` at `0x007b46d8`** — the vptr-carrying shape, laid out
exactly like `actors`:

| Address | Field |
|---|---|
| 0x007b46d8 | vptr |
| 0x007b46dc | `n_entries` |
| 0x007b46e0 | `num_buckets` |
| 0x007b46e4 | `mask` |
| 0x007b46e8 | `buckets` (`Node**`, node = `{d, next}`, payload first) |

| Function | Address | Notes |
|---|---|---|
| `AddToSelection(Unit*)` | **0x0049ed30** | `__thiscall`. Hash 0x004ce6d0, probe the chain, `PlayUiSound(0x54)` if new, `HashTable_Insert` 0x004a6dc0, take a reference at `Unit+0x04` |
| `ClearSelection()` | **0x004972f0** | For each entry with vslot 30 true, calls 0x004ad050 — which **sends command 0x3b** (position/orientation/waypoint commit). Then releases each reference and `HashTable_Clear` 0x004ad4a0 |
| `SelectAllCharacters()` | **0x004972a0** | Walks `ObjectList` @0x007b6928, adds every object whose vslot 71 is true. Does **not** clear first |
| `SelectNextCharacter()` | **0x00497080** | Finds the currently selected entry in `ObjectList`, scans forward (wrapping at the sentinel) for the next vslot-71 object, `ClearSelection` + `AddToSelection`, returns the `Unit*` |
| `SelectPrevCharacter()` | **0x00497160** | The mirror image |
| `GetSelectionCount()` | **0x0049f310** | `MOV EAX,[0x007b46dc]; RET` |
| `CancelLastOrderForSelection()` | **0x0049f250** | DELETE key: one command `0x23` per selected mobile |
| `StopSelection()` | **0x0049f130** | S key: `Unit` vslots 0x124 and 0x130 per entry |
| `CrouchToggleSelection()` | **0x0049f1d0** | C key: `Unit` vslot 0x134 = `Unit_SendCrouchToggle` |
| `DeselectOrActivateMenu()` | **0x00496f70** | ESC: sends command `0x35`, then `ClearSelection` |

`ObjectList` @0x007b6928 supplies the **ordering** for next/previous: it is the client's
`List<Object*>` of every game object, and the scan is a plain forward/backward walk with the
sentinel skipped, filtered by vslot 71. There is no separate "player character" array.

Every selection mutator opens with `CMP byte ptr [0x007b3f51],0` → `ExitFlareMode(CL=0)` @ 0x004a17b0, which is
presumably "close the open command wheel"; that was not confirmed. One step of that guess can now
be sharpened without being closed: the command-wheel mode **is** `CursorMode == 1`, set by
`OpenCommandWheel` @ 0x004a11bd (§10.1) — but that is a **different global**, a dword at
0x007b3f78, not the byte at 0x007b3f51. Do not conflate the two; 0x007b3f51's writers were not
read, so what that byte means remains unconfirmed.

### 10.1 The cursor-mode dispatcher

This belongs with selection rather than with the `PendingOrder` FIFO: it reads
`SelectedUnits.n_entries` through `GetSelectionCount` @ 0x0049f310 and iterates the selection table
@ 0x007b46d8 in its case 0, and its mode 2 is the mode `BeginAttackGroundTargeting` and
`EnterFlareMode` both enter — the family named at the end of §7. It touches **none** of the 11
order kinds.

- **`UpdateCursorForMode` @ 0x00498140** (was `FUN_00498140`), `void __cdecl(void)`, called every
  frame from `RunInGameFrame` at 0x0046ea01 and twice from `ToggleReconMode`. Picks one of ten
  cursor objects and applies it. **No bound check** on its index.
- **`CursorMode` @ 0x007b3f78** is an `int` with exactly **one writer** — `SetCursorMode`
  @ 0x004a28e0 (`__fastcall`, mode in ECX), storing at 0x004a2a57. The seven values are pinned by
  the ECX constant at each of that setter's 26 call sites, and independently by a **stride-7 index
  computation in four unrelated readers** (`ECX = EAX*7 + CursorMode`).

| value | name | set by | cursor chosen | confidence |
|---|---|---|---|---|
| 0 | `Normal` | reset from 14 sites | the full context-sensitive pick | CONFIRMED by its body |
| 1 | `CommandWheel` | `OpenCommandWheel` @ 0x004a11bd | "standard" (inert) | PROPOSED — empty body |
| 2 | `GroundTarget` | `BeginAttackGroundTargeting`, `EnterFlareMode`, `OnCommandWheelClick` | "attack ground" | CONFIRMED by its body |
| 3 | `InventoryScreen` | `FUN_0049f350` @ 0x0049f48c | "drop" / "pass" | PROPOSED-strong |
| 4 | `PauseMenu` | `ToggleInGamePauseMenu` @ 0x004a0e50 | "standard" (inert) | PROPOSED — empty body |
| 5 | `UnitFollow` | `ToggleReconMode` @ 0x004977bd | "standard" (inert) | PROPOSED — empty body |
| 6 | `Suspended` | `SuspendCursorMode` @ 0x004a0d70 (console `CommandCursor`, `CutsceneCamera_Enter`) | none — dispatches to the epilogue | CONFIRMED by its body |

The confidence column is load-bearing: modes 1, 4 and 5 have **empty** case bodies, so their names
come from their setters' names, one inference step beyond the rest.

`CommitPendingOrderTarget` in the row above is now **`OnCommandWheelClick`** @ 0x004a4250: it is the
LMB click handler registered for row `CursorMode::CommandWheel`, all six columns, and cases 0, 1 and
4 of its state machine are cursor-mode changes and cancel rather than order-target commits. (Its
former open question — whether cases 0-3 were dead — is settled: `PendingCommandWheelAction`
@ 0x007b4448 has six writers, five of them the command-wheel entry classes' vtable slots.)

**Mode 5's name `UnitFollow` is now itself suspect.** It is the only mode owning a *complete*
LMB+RMB x {button-down, click, drag} handler set in the matrices of §8.5, which reads much more like
a dedicated ability-aiming mode than a follow toggle. Not renamed — the evidence is structural, not a
measured engine word.

The addresses in the "set by" column are as recovered and are **not uniformly function entries** —
0x004a11bd and 0x004977bd sit inside `OpenCommandWheel` and `ToggleReconMode`, whose entries §11
and §10 record as 0x004a0fa0 and 0x004976d0, so those two are presumably the `SetCursorMode` call
site rather than the function. Not re-checked here; treat the pairing of name to value as the
measured part.

- The ten cursors are named by the developers themselves. `LoadCursorHierarchies` @ 0x0049c4d0
  builds each from `user interface/game_cursor.rif` by hierarchy name — `"unselected"`,
  `"standard"`, `"attack"`, `"attack ground"`, `"pick up"`, `"activate"`, `"drop"`, `"pass"`,
  `"cameralock"`, `"heal character"` — into the globals 0x007b470c..0x007b4730 (now `Cursor_*`).
- **`CurrentCursor` @ 0x007b4738** and **`CursorVariant` @ 0x007b4734** (0/1/2, a **target-valid**
  flag: `ApplyWin32CursorShape` @ 0x004a2140 — formerly named `SetCursor` — uses it to choose
  `IDC_NO` vs `IDC_CROSS` for the attack cursor).
- **Case 0 is the interesting one.** With an empty selection it uses `ActorUnderCursor`
  (0x007b68e8) -> "unselected"; otherwise a hostility test via `TeamSlots` (0x007b3ec4, stride
  0xc4, bytes +0x6a/+0x6b selected on `GameMode == 1`) plus `IsFriendlyFireEnabled`, and a
  `FogOfWar_SampleTotal` visibility test, yielding "activate", "pick up", or "attack" with variant
  1 (in range) or 0 (out of range) after comparing `|target − unit|²` against a per-unit threshold.
- What it is **not**: mode 6's body is the bare epilogue, which is what makes "suspended" mean *the
  cursor is left exactly as it was*. And the guard at the top of the function
  (`CMP byte [0x006a5b34],0` / `JZ`) **is never taken** — 0x006a5b34's image value is 1 and its only
  writer also writes 1. That global is deliberately left unnamed: six other references take its
  **address**, and its neighbours are the constant 2 and pointers to
  `"gunlok"`/`"elint"`/`"frend"`/`"maskelyn"`, so it is probably the head of a record rather than a
  flag. **PROPOSED.**

**No box/marquee select was found.** `ToggleReconMode` @ 0x004976d0 (the Recon Mode toggle
of line 103, and the master gate on the cone renderer; see `ai_behaviour_notes.md` §7) and
`UpdateSelectedUnitCamera` @ 0x00497ca0 (the **default** in-game camera update, the one that runs
while `ReconModeActive == 0`, not a cursor update) contain no rectangle test and no repeated `AddToSelection`. The only entry points into the table are the single-unit
click path, `SelectAllCharacters`, next/previous and the five character keys — which is consistent
with Gunlok being a four-character squad game rather than an RTS. Stated as **not found**, not as
proven absent.

---

## 11. What is still unknown

- **The formation geometry.** `TeamSlot+0xb0` has four writers and no reader was located. Neither
  the per-member offsets for Cluster / Side-by-side / Single file nor the anchor rule was found.
- ~~**Kind 6's exact meaning.**~~ **SETTLED: kind 6 is `GiveItem`**, and the three fields are named.
  `Actor+0x40` is `held_actor` (a general retained reference to another actor — "the actor I am moving
  toward or holding", *not* the pending-give recipient, and its savegame encoding `target->id + 1`
  is what settles the kind); `Actor+0x44` is `pending_action_id`, an id whose **namespace depends on
  the pending action** — an actor id for the kind-2 goto arm and the goto-object command, an item id
  for the kind-6 give arm; and `+0x124` is `pending_action_amount`, whose every measured use is the
  give amount. The latter two names are PROPOSED (the *overloading* on `+0x44` is confirmed); see
  their field comments in the Ghidra DB for the full writer/reader enumeration. Note `MobileActor` is
  a **flat** struct that redeclares the base layout rather than inheriting `Actor`, so the same three
  fields exist twice and both declarations now carry the names.
- **Kinds 5, 7 and 9.** Still named from their immediate wire twins, and now a recorded doubt rather
  than a footnote — see the table in §3. Their callees are `EquipItemInSlot` @ 0x00536ba0,
  `UseInventoryItem` @ 0x005370d0 and `UnequipSlot` @ 0x00536ec0 respectively, none of which matches
  its kind's name.
- **`PendingOrder::flag` for kinds other than 0/1.** On the slots 96/97 path it is `close_range`
  (§8.2); what the field means for the remaining kinds was not decoded.
- **Command Wheel / Standing Orders Menu contents.** `OpenCommandWheel` 0x004a0fa0,
  `OpenAmmoMenu` 0x004a11d0, `OpenStandingOrdersMenu` 0x004a12e0, `DrawOrderMenu` and
  `BeginAttackGroundTargeting` 0x004a13f0 were identified but not read, so which wheel entry maps
  to which order kind is unrecorded.
- **The `Select Ammo type 1..5` keys.** They are registered, but `HandleGameKeyAction`'s match
  block for them makes no call in the region scanned — the dispatch presumably happens through the
  Ammo Menu. Not established.
- **`Actor+0x13c`**, which `ClearOrderQueue` sets to 1, and `Actor+0x120`/`+0x12c`, which gate the
  MobileActor dispatcher. Not identified.
- **Box selection.** See §10 — absent from everything read, not proven absent.
