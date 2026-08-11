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
`KeyModifierState`'s `MOD_Ctrl = 2` is right. What bits 0 and 2 mean is *not established* — no
shipped binding uses them.

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

`FUN_00538830` **is** the queue drain the brief asked about, but only in the "throw everything
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
| 4 | `CrouchToggle` | `MobileActor::QueueCrouchOrder` 0x00538be0, bare `RET` | MobileActor slot 70 → slot 83 `UpdateMineDetectionAndBounds` |
| 5 | `Interact` | `MobileActor::QueueInteractOrder` 0x00538e80, `RET 0x8` | MobileActor slot 70 → `FUN_00536ba0(arg_c, arg_b, 1)` |
| 6 | `Board` | `MobileActor::QueueBoardOrder` 0x00538f80, `RET 0xc` | MobileActor slot 70 → `GetActorById(arg_a)`, `this+0x40 = target` (refcounted), `+0x44 = arg_b`, `+0x124 = arg_d` |
| 7 | `Equip` | `MobileActor::QueueEquipOrder` 0x00538ca0, `RET 0x4` | MobileActor slot 70 → `FUN_005370d0(arg_b, 1)` |
| 8 | *(unnamed)* | `MobileActor::QueueOrderKind8` 0x00538d40, `RET 0x4` | MobileActor slot 70 → `FUN_00538240(arg_b)` |
| 9 | `UseItem` | `MobileActor::QueueUseItemOrder` 0x00538de0, `RET 0x4` | MobileActor slot 70 → walk `inventory_list` for `entry+0x24 == arg_b`, `FUN_00536ec0(entry)`, broadcast **0x80** |
| 10 | `SetAmmoType` | `CharacterActor::QueueOrderKind10` 0x00541980, `RET 0x4` | CharacterActor slot 70 → slot 99 `SetAmmoType(arg_b)` |

Names for 5/6/7/8 are taken from the *immediate* console/wire twin that reaches the same callee
(§8); `Board` for 6 follows `directplay_protocol_notes.md`'s reading of command 0x17
("board / attach (escort)") and is the least certain of them. Kind 8 has no name at all —
`FUN_00538240` is 1096 bytes and was not read.

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
`TeamSlots[DAT_006a58e0].formation` inline in `HandleGameKeyAction` (0x0046fd27 / 0x0046fd5d /
0x0046fd93), where `DAT_006a58e0` is the local player's team index. That means a formation change
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

The move path is offset-free end to end: `IssueOrderToSelection` @ 0x0049f010 hands **the identical
`Vec3 *`** to every selected unit (the pushed pointer is loop-invariant across the selection walk),
`Unit` slot 75 @ 0x004c3c90 copies it verbatim into the 0x18-byte command, and the executor arm
copies it verbatim into `CharacterActor+0x2dc`. No per-member index, no perpendicular or trailing
basis, no spacing constant, no unit-radius term.

**So there is also no leader or anchor** — every member of the selection is given the clicked world
point directly, and the spread visible in play is the nav agent's own collision avoidance
converging on a shared goal. The formation setting is a HUD indicator and a script-visible flag,
nothing more.

Sweep coverage, so this is re-checkable: 158 references to `TeamSlots`, 110 `IMUL reg, …, 0xc4`
slot-pointer sites, and all 141 instructions in `.text` carrying a `0xb0` displacement — of which
exactly 8 are in the two-register TeamSlot form. The rebased idiom (`ADD reg,[0x007b3ec4]` then
`[reg+disp]`) reaches displacements 0x64, 0x6a, 0x70, 0x74, 0x7c, 0x84, 0x94, 0x9c, 0xa0, 0xac —
**no 0xb0**. No site anywhere pushes or stores a formed TeamSlot pointer, so no callee can reach
`+0xb0` off a parameter. `FUN_0053a450` / `FUN_0053a640` / `FUN_004a17b0` / `FUN_004a17e0` are
excluded by that sweep and are not worth reading for this.

---

## 8. Replication — orders are wire messages, always

**Every** `PendingOrder` push in the binary is reached from `ExecutorThreadProc` @ 0x00509050
(the client→server command switch at 0x005092a4) or from `ApplyUpdateMessage`. There is no local
push path. The client-side producers are `Unit` vtable slots (table base **0x00664ac8**), and each
takes a trailing `bool queued` that selects between an immediate id and `id + 1`:

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
| `0x1d` | `0x1f` | 1 `AttackPosition` | `Unit_SendAttackPosition` 0x004c3e90 (slot 0x13c) | vslot 86 `QueueOrderPosition` @0x00509e48 |
| — | `0x08`, `0x21` | 2 `GotoObject` | (map-section move; `0x21` is the multi-actor batch) | `QueueGotoObjectOrder` @0x00509463/0x00509492/0x00509906 |
| — | `0x26` push / `0x25` pop | 3 `HoldMarker` | `ActivateUnitAndResume` 0x0046ef50 | `PushHoldMarkerOrder` @0x0050a10c / `ReleaseHoldMarkerOrder` @0x0050a0b0 |
| `0x1c` | `0x22` | 4 `CrouchToggle` | `Unit_SendCrouchToggle` 0x004c09d0 (slot 0x134) | `QueueCrouchOrder` @0x00509dc9 |
| `0x0f` | `0x10` | 5 `Interact` | `Unit_SendInteract` 0x004c0bc0 (slot 0x190) | `QueueInteractOrder` @0x0050a21c |
| `0x17` | `0x18` | 6 `Board` | `Unit_SendBoard` 0x004c0d50 | `QueueBoardOrder` @0x0050a2e8 |
| `0x11` | `0x12` | 7 `Equip` | `Unit_SendEquip` 0x004c0c90 (slot 0x194) | `QueueEquipOrder` @0x0050a33a |
| `0x13` | `0x14` | 8 — | `Unit_SendOrderKind8` 0x004c0cf0 | `QueueOrderKind8` @0x0050a38a |
| `0x15` | `0x16` | 9 `UseItem` | `Unit_SendUseItem` 0x004c0dc0 | `QueueUseItemOrder` @0x0050a4af |
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
attack-ground order. **Measured, but the consequence is not** — what slot 97 `AttackPosition`
does with its fourth argument was not read.

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
    GameMode == 0 ? ToggleActivePause() : FUN_0046f590();
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

Every selection mutator opens with `CMP byte ptr [0x007b3f51],0` → `FUN_004a17b0(CL=0)`, which is
presumably "close the open command wheel"; that was not confirmed.

**No box/marquee select was found.** `ToggleReconMode` @ 0x004976d0 (was `FUN_004976d0`, and was
described here as "the world-click order handler", which it is not — it is the Recon Mode toggle
of line 103, and the master gate on the cone renderer; see `ai_behaviour_notes.md` §7) and
`FUN_00497ca0` (the per-frame cursor update called from `RunInGameFrame`) contain no rectangle
test and no repeated `AddToSelection`. The only entry points into the table are the single-unit
click path, `SelectAllCharacters`, next/previous and the five character keys — which is consistent
with Gunlok being a four-character squad game rather than an RTS. Stated as **not found**, not as
proven absent.

---

## 11. What is still unknown

- **The formation geometry.** `TeamSlot+0xb0` has four writers and no reader was located. Neither
  the per-member offsets for Cluster / Side-by-side / Single file nor the anchor rule was found.
- **Kind 8.** `FUN_00538240` (1,096 bytes) was not read, so the order has no name.
- **Kind 6's exact meaning.** "Board / attach" is carried over from `directplay_protocol_notes.md`
  rather than measured; what `Actor+0x40`/`+0x44`/`+0x124` mean was not established.
- **Kinds 5 and 7.** Named from their immediate wire twins (`FUN_00536ba0`, `FUN_005370d0`), whose
  bodies were not read.
- **`PendingOrder::flag`.** Set from the wire id, consumed as the fourth argument of slots 96/97,
  never decoded. §8.2's suspected copy-paste bug hinges on it.
- **Modifier bits 0 and 2.** No shipped binding uses them; only mask 2 = CTRL is measured.
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
