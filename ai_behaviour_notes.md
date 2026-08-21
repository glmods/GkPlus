# Gunlok AI: perception, alert states and the vision cone

What the shipped manual calls the "vision cone" and "audio-scan circle", recovered from the
binary. Companions: `actor_vtable_notes.md` (the Actor hierarchy), `role_subobjects_notes.md`
(the `Character` stat block every number here comes from), `gls_system_notes.md` (the GLS
fields), `threading_model_notes.md` (which thread runs what).

Everything below is measured against `gl.exe` unless a line says otherwise. Section 11 lists
what is **not** established.

---

## 1. Where the AI lives

The AI is a **separate module** occupying roughly `0x0044f300`-`0x0045c800`, driven entirely
from the **executor thread**. It is not the Actor vtable: `Actor` carries a plain function
pointer, `ai_think` @ `Actor+0x34`, installed once from `role->ai`.

| Address | Name | Notes |
|---|---|---|
| 0x00450550 | `Actor_SetAiBehaviour` | `__fastcall`, `RET 0x8`. Installs `ai_think` (`Actor+0x34`) and, for `AIType::Mine` only, the on-placed hook at `Actor+0x38`, from `AIType` |
| 0x0044f560 | `AiExecutorTick` | `__stdcall(uint time_lo, int time_hi)`, `RET 0x8`. Only caller is `ExecutorThreadProc` @ 0x00509050 |
| 0x00457f30 | `RunAiForTeamGroup` | `__thiscall`, `RET 0x8`. Walks one `TeamActorLists[]` entry |
| 0x00451220 | **`AiThink_Bot`** | `__thiscall void(Actor*, uint time_lo, int time_hi)`, `RET 0x8`, **0x4009 (16,393) bytes**, one contiguous range `[0x00451220, 0x00455228]` — see §2.1 |

`Actor_SetAiBehaviour` dispatches through a 21-entry byte table at `0x0045069c` and a 10-entry
jump table at `0x00450674`. The full map (`AIType` -> think function):

| AIType | `ai_think` (`Actor+0x34`, per tick) | `Actor+0x38` (on-placed hook, once) |
|---|---|---|
| 0 Bot, 13 Centipede, 19 President | `AiThink_Bot` 0x00451220 — **only if vtable slot 11 `GetWeapon()` != 0**, otherwise NULL | — |
| 1 Scavenger | `AiThink_Scavenger` 0x004556e0 | — |
| 2 Mine | `AiThink_Mine` 0x004552a0 | `Mine_OnDeployed` 0x0045a640 |
| 3 Minebot | `AiThink_Minebot` 0x00456c50 | — |
| 6 Waiting, 14 Centibody | `AiThink_Waiting` 0x00456bb0, and `has_ai` (`Actor+0x3c`) **cleared** | — |
| 7 Pathfinder | `AiThink_Pathfinder` 0x004556d0 (a 2-instruction tail-jump to 0x0053a1d0) | — |
| 15 Node | `AiThink_Node` 0x0045a850 | — |
| 16 NodeWaiting, 18 Popup, 20 Turret | `AiThink_Waiting` 0x00456bb0, `has_ai` **set** | — |
| 17 Swarm | `AiThink_Swarm` 0x0045b620 | — |
| 4 Reserved, 5 Blocker, 8 TrackObject, 9 Tumbleweed, 10 Pickup, 11/12 BackgroundCreature | NULL, `has_ai` cleared | — |

`Actor+0x38` is **not** a second think proc. Every `AIType` installs its per-tick proc into
`Actor+0x34` (nine `MOV dword ptr [ESI + 0x34],imm` sites in `Actor_SetAiBehaviour`), and the
mine's is already `AiThink_Mine`; the single `MOV dword ptr [ESI + 0x38],0x45a640` @ 0x00450610 is
the only write to `+0x38` in the binary. That field is called **once**, from vtable slot 51
(`Actor::InitPositionAndTiming`) @ 0x0052dd18, under a null guard and right after the nav-poly
lookup — plus once more from `Actor_FixupAfterLoad` @ 0x00531860 when a savegame is restored, and
nowhere else in the image. And `ExecutorActorTick` @ 0x0052fad0 reads neither `+0x34` nor `+0x38`, calling only
vtable displacements 0x18, 0x90, 0xdc and 0x118. `Mine_OnDeployed`'s body matches: it switches on
the mine sub-kind (`Character+0xac`), each arm arming a deadline or announcing once, and nothing in
it loops or re-arms.

`AiThink_Turret` @ 0x00455de0 is a tenth think proc that this table does not contain: it is not in
`Actor_SetAiBehaviour`'s dispatch at all. A turret starts on `AiThink_Waiting` (AIType 20) and
`TurretActor::Update` swaps `Actor+0x34` to `AiThink_Turret` @ 0x0054b0b6, gated on
`TurretActor+0x191`.

`Actor_SetAiBehaviour` also seeds `ai_state = 3`, `aggression` (`Actor+0x60`, default
`0x3f333333` = 0.7 when slot 10 `GetCharacterData()` is NULL), and clears `+0x74`/`+0x78`.

**`AiExecutorTick`** does three things per executor tick:

1. builds a temporary list of every actor whose `role->character->always_cpu_controlled`
   (`Character+0xb4`) is set **and** whose `TeamSlots[team].player_controlled` (`+0x6a`) is set, and calls
   `actor->ai_think(time_lo, time_hi)` on each;
2. walks `AiTeamGroupList` @ `0x006af83c`, calling `RunAiForTeamGroup`, which walks
   `TeamActorLists[idx]` and calls `ai_think` on every live actor with a non-NULL one;
3. every 10th tick (`DAT_006afffc % 10 == 0`) ages `AiStimulusList` (§6).

`TeamActorLists` @ **0x007ba038** is an array of `List<Actor*>` headers, stride 0x10, indexed
by team slot. It is the iteration root for every enemy scan in this file.

`AiThink_Bot` is the only think function that runs the perception/alert machine. `Centipede`
and `President` share it; a bot with no weapon gets no AI at all.

---

## 2. The state fields

Two separate integers, both on `Actor`, and they are **not** the same thing.

### 2.1 `Actor::ai_state` @ +0x54 — what the actor is doing

| Value | Meaning | Written at |
|---|---|---|
| 0 | **alerted** — an alarm named a position, head for `alert_position` | 0x00454a15 / 0x00454fff (alarm propagation, on the *neighbour*); 0x00452b69 (handler 0) |
| 1 | **engaging** — has an `attack_target` | 0x004534bf (behaviour handler 1) |
| 3 | idle / hold | `Actor_SetAiBehaviour` @ 0x00450567; console `SET ACTIVITY STOP` @ 0x00445a70; and four sites inside `AiThink_Bot` — 0x00453856 (handler 5), 0x00453fc8 (handler 6, on expiry), 0x0045443d (handler 2, unreachable — see below), 0x00454579 (handler 8, empty waypoint list) |
| 4 | **investigating** a stimulus | 0x00452422 |
| 6 | **reacquiring** — the target was lost; paired with `alert_state == 1` for a 17 s window (§2.2, §11) | **0x00453e32** (handler 6); tested at 0x0045459e / 0x004545c5 |
| 7 | **investigating an object** — the retained object is `Actor+0x40` | `AiBeginInvestigate` @ 0x0045e090; 0x0045442b (handler 2, unreachable); tested by `AiThink_Minebot` @ 0x0045719a and `AiThink_Bot` @ 0x004513e5 |
| 8 | patrol / move order | console `SET ACTIVITY PATROL` @ 0x00445a39 and `GOTO` @ 0x0043ee05; 0x004524cb / 0x0045273b / 0x00452b15 (handler 3, the random wander) |

`AiThink_Bot` chooses a behaviour index 0..8 and tail-dispatches through the jump table at
**0x00455250** (`{0x452b2d, 0x453486, 0x45441c, 0x4524b8, 0x453d23, 0x45384d, 0x453e2b,
0x454588, 0x45455a}`) after remapping it through `ai_state` (0x00452492): `ai_state` 0/6 forces
index 6 unless the index was 5; 1 forces index 1 when `GetAttackTarget()` is non-NULL, else 6;
4 forces index 4; 8 forces index 8 when the index was 3 (the default).

**Which indices can actually be produced.** Enumerating all 7 predecessors of the dispatch
preamble at 0x004524ab: index 1 from 0x00451c8a (`MOV EAX,0x1`, an attack target is present) and
index **0** from 0x00451c9d (`XOR EAX,EAX`, a stimulus actor is present) — both of which reach the
dispatch **bypassing the `ai_state` remap entirely**; 8 from 0x0045245d; 6 from 0x0045246f; 1 from
0x0045248e; 4 from 0x0045249a; and the fallthrough at 0x004524a5, where `EAX = [EBP-0x164]`, a
local with exactly two writers in the whole function (`0x00451411 = 3`, `0x004522a3 = 5`). The
reachable set is therefore **{0,1,3,4,5,6,8}**: **indices 2 and 7 have no producer**. Index 7 is
0x00454588, the common tail, which is reached by 43 direct jumps regardless, so the only genuinely
dead body is **case 2** (0x0045441c, 318 bytes).

**Case 2's deadness is now CONFIRMED, and the caveat above is discharged.** It used to rest on the
predecessor enumeration being complete for the current instruction map, which is itself derived from
the four jump tables below — not circular, but one level of inference deep. Three independent routes
now settle it, and note that **this verdict is not a `.text` sweep**: the dispatch index is a stack
local, not a parameter, so unlike the `alert_state` and `IsWithinElevationLimit` verdicts it does not
depend on any other function's disassembly.

- **Route A** — machine-level constant propagation over all seven flows into 0x004524ab. Six are
  direct `JMP`s that set `EAX` themselves and therefore **bypass the `CMP EAX,0x8 / JA` bound check**
  at 0x004524a2 (`MOV EAX,0x1` @ 0x00451c8f, `XOR EAX,EAX` @ 0x00451c9f, `MOV EAX,0x8` @ 0x00452462,
  `MOV EAX,0x6` @ 0x00452474, `MOV EAX,0x1` @ 0x00452493, `MOV EAX,0x4` @ 0x0045249a); the seventh is
  the fallthrough at 0x0045249c, which loads the index local. That local has exactly **five**
  appearances in the whole function — two writes (`= 3` @ 0x00451411, `= 5` @ 0x004522a3) and three
  reads — and **its address is never taken**, so no call can write it. Value set {0,1,3,4,5,6,8}; no 2.
  The uninitialized-garbage hole is closed too: the block containing the `= 3` init **dominates** the
  dispatch (removing it leaves 19 of 477 blocks reachable, none of them the dispatch).
- **Route B** — 0x0045441c has exactly two references, the jump-table slot @ 0x00455258 and the
  computed jump @ 0x004524b1, and the instruction immediately preceding it is an unconditional `JMP`
  (0x00454417 `JMP 0x00454588`), so there is no fallthrough either.
- **Route C** — the decompiler's own dataflow shows eight assignments to the index variable, values
  3, 1, 0, 5, 6, 1, 4, 8, matching the disassembly one for one and containing no 2.

The trap check passes cleanly: `AiThink_Bot`'s body [0x00451220, 0x00455228] contains **0
non-instruction bytes** (re-measured), all four indirect jumps carry `COMPUTED_JUMP` references,
three carry manual switch overrides, and the decompiler emits no `Could not recover jumptable` and
no `Treating indirect jump as call`.

**Dead only in `AiThink_Bot`.** The `+0x78` / `+0x64` mechanism itself is live: `AiThink_Scavenger`
@ 0x00455a66 / 0x00455a71 sets the same pair from a scanned object's coords. All four `Actor+0x78`
immediate-store sites binary-wide are 0x00450596 (`Actor_SetAiBehaviour`, `= 0`), 0x0045445a (the
dead one, `= 1`), 0x00455a66 (`AiThink_Scavenger`, `= 1`) and 0x00459168 (`FUN_00457f80`, `= 0`).

### 2.1.1 The nine behaviour handlers

All nine were read; the bodies below are measured, not inferred from the writes.

| idx | entry | bytes | what it does |
|---|---|---|---|
| 0 | 0x00452b2d | 1,729 | React to a newly perceived actor. Stops movement (`StopAndBroadcast`, `SetMoveState`), zeroes `+0x74`; copies the stimulus actor's position into `alert_position` (+0x64..+0x6c), clears `alert_flag` (+0x70) and bit 0x80 of `+0x7c`, sets `ai_state = 0`; then a random 4-way pick |
| 1 | 0x00453486 | 325 | Engage the attack target. Same stop-movement preamble; copies the *target's* position into `alert_position`; sets `ai_state = 1`; vtable slot `+0xf8` gate; resets the miss counter `+0x74` if positive; then a random 4-way pick |
| 2 | 0x0045441c | 318 | **Flinch-and-investigate**, reacting to the remembered position at `Actor+0x64`. Guarded on `+0x78`; if a remembered position exists and a per-tick local flag is set it sets `ai_state = 7` (investigate) @ 0x0045442b; else if `+0x70` is set it holds (`ai_state = 3`); else it sets `+0x78 = 1` @ 0x0045445a and issues vtable slot 88 `goto` to **`2*position - Actor+0x64`** — i.e. retreat directly away from the remembered position by the same distance. **CONFIRMED DEAD: no producer for index 2** (see above) |
| 3 | 0x004524b8 | 4,880 incl. shared tail | Idle / hold -> **random wander**. A `GetGameTimeSeconds` deadline on `+0x140`/`+0x144`; `ClearPatrolPoints`; `SinTable` @ 0x007f5f78 and `CosTable` @ 0x007faf78 (both indexed `int & 0xfff`) build a direction `(cos, 0, sin)`, scaled by **20.0** and added to the current position; `FindNavPolygonUnder` + a walkability test; two `PushRouteWaypoint` from either `+0xa0` or `goto_target` `+0x1dc` depending on `is_moving` `+0x184`; sets `ai_state = 8` at three sites |
| 4 | 0x00453d23 | 264 | Stimulus investigation; a 3-float delta and a 9.0 constant. Falls into case 6 at 0x00453d2a |
| 5 | 0x0045384d | 1,238 | Copies a *local* `Vec3` into `alert_position`, clears `alert_flag`, `StopAndBroadcast`; sets `ai_state = 3` |
| 6 | 0x00453e2b | 1,521 | The reacquire handler — `alert_state = 1`, the 17 s window, updates 0x63/0x62 (§11) |
| 7 | 0x00454588 | 3,233 | The common tail / alarm state machine (`alert_state` 2/3/4, updates 0x60/0x63, §4). Reached from everywhere |
| 8 | 0x0045455a | 3,267 incl. tail | Vtable `+0xf8` gate; if the waypoint list `+0x208` is empty sets `ai_state = 3`; falls into the tail |

**`SinTable` / `CosTable`: the pairing is CORRECT — but both types were wrong, and `CosTable` is
not a table at all.** This paragraph used to say the pairing was an unverified pre-existing DB name.
It is now measured, from the C++ static initializer `InitMathLookupTables` @ 0x0058f310 (was
`FUN_0058f310`, registered from 0x0043abe0 with `this = SinTable`) that fills both at runtime:

- **`SinTable` @ 0x007f5f78 is `float[0x1400]` = 5120 entries, not 4096.** `SinTable[j] =
  sinf(j * 2*pi/4096)` for `j` in 0..0xfff, and entries 0x1000..0x13ff are a **wrap-around copy** of
  0..0x3ff, so a read of `(i & 0xfff) + up to 0x400` stays in bounds. 0x1400 elements ends *exactly*
  at `CosTable`.
- **`CosTable` @ 0x007faf78 is a single `float *`, not an array.** Its one and only writer stores
  `SinTable + 0x1000` bytes = `&SinTable[1024]` — a quarter turn, so indexing it yields
  `sin(x + pi/2) == cos(x)`. Declaring it `float[4096]` was a live defect: `CosTable[0]` read out of
  the database gave the *pointer bits*, and the bogus 16 KB array swallowed the acos table below.
- It has **zero write references** in the reference manager, which is exactly what a computed
  `*(int *)(param_1 + 0x5000) = ...` store looks like — that absence was never evidence of no writer.

The discriminator is the **read shape**, and both appear side by side at the same angle with the
same mask in this very handler:

```
0045298a  AND EDI,0xfff
00452995  MOV EDI,dword ptr [EDI*0x4 + 0x7f5f78]   ; direct  -> SinTable IS an array
004529ab  MOV ESI,dword ptr [0x007faf78]           ; load the POINTER
004529c2  MOV ESI,dword ptr [ESI + EAX*0x4]        ; index through it
```

All 108 references to 0x007faf78 in `.text` are of the second shape; not one indexes it directly.
The use site builds `(cos, 0, sin)` — X from cosine, Z from sine, the standard heading-from-+X
convention — so there is no swap at the use site either.

**Two more from the same initializer.** 0x007faf7c is a `float[4097]` **`ACosToTurnsTable`** (entry
*k* covers cosine `-1 + k/2048` and holds `acosf(v) * 2048/pi`), 0x4004 bytes ending exactly at
`RsqrtMantissaTable` @ 0x007fef80; and `DAT_007f5f74` is the float 4096.0, now
**`TurnUnitsPerRevolution`**. **The angle unit throughout this binary is 4096 units per
revolution.**

### 2.1.2 The four jump tables

`AiThink_Bot` switches four times, and all four entry counts are **CONFIRMED by a perfect
tiling**: recursive descent from the function entry, resolving exactly these four tables at exactly
these counts, covers 16,393 of 16,393 bytes with zero gaps and zero overlaps and terminates exactly
where the table data begins. One entry more or fewer anywhere breaks the tiling.

| table | site | entries | switches on |
|---|---|---|---|
| 0x0045522c | 0x0045244b | 9 | `ai_state` (`Actor+0x54`) -> behaviour-index remap |
| 0x00455250 | 0x004524b1 | 9 | the behaviour dispatch (§2.1.1) |
| 0x00455274 | 0x00452fdb | 4 | `(per-thread RNG word >> 1) & 3` — the random pick inside handler 0 |
| 0x00455284 | 0x0045359a | 4 | the same idiom inside handler 1 |

Both random picks are `switch (rand() >> 1 & 3)` whose signed-modulo sign fixup is **dead code**:
the `SHR` always leaves the value non-negative. (The RNG they draw from is `RngAi_State` @ 0x006a3130 —
one of **fourteen** per-translation-unit instances of BSD `random()` TYPE_3, none of which is
privileged and none of which is ever re-seeded; see `threading_model_notes.md`. This was once
recorded as "not the named one", implying there was a named one to differ from.)

**This is why the function was recorded as 0x1f39 bytes.** 8,400 bytes sat outside the function
body behind these tables — 8,075 of them undefined bytes, plus 325 bytes of already-disassembled
instructions attached to no function. The table at 0x00455274 was invisible to every earlier sweep
because *its own dispatch instruction* was inside the undefined bytes of handler 0: it only became
findable once table 0x00455250 had been resolved, so the recovery took two passes.

### 2.2 `Actor::alert_state` @ +0x80 — the alarm, and the cone colour

This is the field the manual's colours come from. It is mirrored to the client `Unit+0x24` by
network updates 0x60/0x61/0x62/0x63 (§5).

**The whole state machine is replicated, not just the field.** `Unit+0x24` mirrors
`Actor+0x80` and `Unit+0x88` mirrors `Actor+0x90`, and the executor sets state *and* deadline
immediately before each broadcast, so both sides can be read against each other. Confirmed pairings,
with the client arms:

| update | executor sites | sets | client arm | client effect |
|---|---|---|---|---|
| 0x60 | 0x0045473e, 0x00454d25 | `alert_state = 4`, no deadline | 0x005004e2 | `Unit+0x24 = 4`, `PlaySoundOnObject(0x24)` |
| 0x61 | 0x00454854, 0x00454e3b | `alert_state = 2`, `now + 1*rate` | 0x00500515 | `Unit+0x24 = 2`, same 1 s |
| 0x62 | 0x00454055 | `alert_state = 0`, `ai_state = 3` | 0x005005bc | `Unit+0x24 = 0`, `PlaySoundOnObject(0x25)` if still alerted |
| 0x63 | 0x00453ed6, 0x00453f46 | `alert_state = 1`, `ai_state = 6`, `now + 17*rate` | 0x00500664 | `Unit+0x24 = 1`, **same 17 s deadline** |

The 17-second state-1 window is therefore confirmed on **both** sides. Note the two 0x63 broadcast
sites share **one** `alert_state = 1` write, at 0x00453e3f — the second site is the
already-in-state-1 path, which re-broadcasts without rewriting the field (`JZ` @ 0x00453e39).
`Unit+0x24` and `Unit+0x88` are still `undefined` in the DB and the mirror inference is behavioural,
so those two client field names are PROPOSED.

| Value | Meaning | Cone colour |
|---|---|---|
| 0 | no alarm | **green** |
| 1 | reacquiring (17 s window) — written by behaviour handler 6 @ 0x00453e3f, see §11 | **orange ramp** |
| 2 | broadcasting the alarm (1 s window) | **blue** |
| 3 | alarm spent | **red** |
| 4 | alarm pending (`alarm delay` counting down) | **red** |

Supporting fields, all on `Actor`:

| Off | Name | Meaning |
|---|---|---|
| 0x30 | `alarm_delay` | seconds, straight from the GLS `alarm delay` field (`Character+0x60`), copied by `Actor::Ctor` |
| 0x34 | `ai_think` | the think function |
| 0x38 | `on_placed_hook` | called once from vtable slot 51, not per tick; only `AIType::Mine` installs one (`Mine_OnDeployed`) |
| 0x3c | `has_ai` | bool |
| 0x48 | `alarm_deadline` | `__int64` game clock; 4 -> 2 fires here |
| 0x54 | `ai_state` | §2.1 |
| 0x58 | `ai_next_think_time` | `__int64`; `PostAiStimulus` back-dates it to wake a listener |
| 0x64 | `alert_position` | `Vec3f` — **the "last known position"** the alarm names |
| 0x70 | `alert_flag` | byte copied alongside `alert_position` |
| 0x80 | `alert_state` | §2.2 |
| 0x90 | `alert_timer_end` | `__int64`; end of the state-2 (1 s) or state-1 (17 s) window |

`Actor+0x74` is **not** part of this — per an independent measurement it is a consecutive-miss
counter on the shooter (zeroed by `AttackTarget` @ 0x00540e72, tested `== 2` at 0x005400cc).

---

### 2.3 `AiBeginInvestigate` @ 0x0045e050 — how state 7 is entered

`__fastcall void(Actor *self /*ECX*/, Actor *object /*EDX*/, float seconds)`, `RET 0x4`. Both
callers are in `AiThink_Minebot` (0x00456f7c, and 0x004571b0 where the same object is propagated
to a second actor that is `ai_type == 3` and not already in state 7). The body, instruction by
instruction: `ai_state = 7` (0x0045e090); broadcast **0xb6**, 24 bytes unreliable, carrying the
actor's own position, a radius of 20.0 and a packed `kind = 1, life = 3 s` (§7.2); then
`MobileActor::GotoObject(self, object->+0x118, seconds)` @ 0x005394d0; then a refcount handover
into `Actor+0x40` — release the old attachment through slot 0, retain the new one with
`INC dword ptr [ESI+0x4]`. So `Actor+0x40` in state 7 is the object being investigated, and
`Actor+0x04` is a reference count.

**Defect — now CONFIRMED, and it belongs in `game_defects_notes.md` §16.** The 17-line tail that
computes the state's deadline into `+0x90/+0x94` contains **no clock read at all**: no
`ReadScaledClock64`, no `Clock::ReadScaled32` @ 0x00571b60. It reads the tick rate (`0x007c07e0`, or
`0x007c07b0` when the caller is the executor thread), scales `seconds` by it, then
`0045e1c6 MOV EDI,[0x007c07dc] / ADD EDI,EAX / MOV [EBX+0x90],EDI`. Every sibling reads the clock
first — `Decoy_Dismiss` @ 0x00450f60 (`clock + 60*ticks_per_second`), `Mine_OnDeployed`
(`now + 10*ticks_per_second`), `PostAiStimulus` (`ReadScaledClock64(&GameTimeClock)`).

**`0x007c07dc` is settled: it is ticks-per-second, a rate** (`MainClock+0x0c`; see §2.5). So the
deadline is `rate + seconds*rate`, with **no `now` term at all**.

**But the consequence is neither "expires instantly" nor "never expires", and the earlier phrasing
above was wrong to reach for the first.** The `ai_state == 7` handler in `AiThink_Minebot`
@ 0x00456f86 compares the deadline against `now` as a signed 64-bit value, and its **expired branch
@ 0x00456fa6 does not transition out — it re-arms to `now + rate*1`** (0x00456fb3-0x00456fd2) and
yields `remaining = rate` via `SUB ECX,EAX` @ 0x00456fde. So the first state-7 tick always takes the
expired branch and **the investigate window is silently replaced by ~1 second, re-armed**. The
`seconds` argument still reaches `GotoObject` @ 0x0045e168, so **movement is unaffected — only the
timer is**, which is exactly why this was never noticed in play and why an in-game observation would
*not* have settled it.

Nor is it uptime-dependent: `Clock+0x18`, the accumulator, is zeroed **once** at static init by
`Clock_Ctor` @ 0x00571830 and never reset (`LoadLevel`'s `MainClock -> ExecutorClock` copy carries
the total across), so a level load guarantees the bogus deadline is in the past. **Uniformly wrong.**

Two other things this function does that matter elsewhere: it **retains its target in
`Actor+0x40` `goto_actor`** (so that field is not an "attachment" — it is the actor being pursued,
retained just after the `GotoObject` that set it; `AiThink_Swarm` @ 0x0045b8b4 does
the same for `ai_state = 8`), and `ai_state = 8` itself is written by `AiThink_Swarm` @ 0x0045b88a,
which §2.1's enumeration did not cover.

### 2.4 `MobileActor+0x214` is the patrol-point list, and patrol dwell is `+0x140`/`+0x144`

This closes a standing gap: handler 3's line in §2.1.1 already cited `+0x140`/`+0x144` without
knowing what they were.

`AiThink_Bot` picks the nearest entry of the **patrol-point list at `MobileActor+0x214`** (walked at
0x0045264b) and reads that record's `+0x10` `float wait_time` @ 0x00452696, then arms
`patrol_wait_until = GetGameTimeSeconds() + wait_time` into **`MobileActor+0x140`** @ 0x004526e1,
latched by the bool **`patrol_waiting` @ MobileActor+0x144** (set 0x004526b5/0x004526ca, cleared on
expiry 0x004526f2). Both names are PROPOSED; the behaviour is measured.

The authoring side is the console command **`ADD PATROLPOINT <x> <y> <z> [seconds]`**: the optional
fourth token is that `wait_time`, parsed with a 0.0 default by `ConsoleParseFloat` @ 0x004d6860 and
pushed as slot 90's fourth argument. `CommandAddPatrolPoint` @ 0x00442140 and `CommandAddWaypoint`
@ 0x0043e860 share one body (0x0044c760) and differ **only** in the `CL` they pass — `1` versus `0` —
which is precisely the flag that selects `AppendPatrolPoint` (the `+0x214` list) over the route list
at `+0x204`. So a *waypoint* and a *patrol point* are the same record in two different lists, and
only the patrol list has a dwell consumer.

### 2.5 The `Clock` facts, because this area has now turned on them twice

Both §2.3's defect and §4's deadlines depend on telling two globals apart, and confusing them
re-opens §16 from scratch:

- **`Clock+0x0c` is a RATE — ticks per second.** `ClockTicksPerSecond` @ 0x007c07dc for `MainClock`
  @ 0x007c07d0, `ClockTicksPerSecondExecutor` @ 0x007c07ac for `ExecutorClock` @ 0x007c07a0. Its
  **only** writer is `MulDiv(delta_ticks, 1000, elapsed_ms)` inside `Clock::Calibrate` @ 0x00571931,
  called from `WinMain` @ 0x0046b934 and `LoadLevel` @ 0x004e1420 **only** — never per frame. 133
  reads, zero writes.
- **`Clock+0x18` is the running accumulator**, read via `AccumThreadClock64` @ 0x0044df20 (wrapped by
  `Clock::ReadScaled32` @ 0x00571b60 and `Clock::ReadScaledClock64` @ 0x00571bb0). `Clock_Ctor`
  @ 0x00571830 zeroes it once, at static init; `Clock_Dtor` @ 0x005718a0 is a bare `RET`.
- **The correct deadline idiom is `now + rate*N`**, where `now` is the think proc's own
  `now_lo`/`now_hi` parameters. Worked examples: `AiThink_Bot` case 6 @ 0x00453e55 (`rate*17 + now`),
  `Mine_OnDeployed` @ 0x0045a75f, `AiThink_Minebot` @ 0x00456fb3, `Decoy_Dismiss` @ 0x00450fd8.
- `Clock::ReadScaled32` / `ReadScaledClock64` are **mistyped in the DB** as taking `Clock *`: the
  `Decoy_Dismiss` call site passes `MOV ECX,0x6aaab8` (`GameTimeClock`) and the bodies do a 64x64
  multiply against `[this]`/`[this+4]`, which is not a `Clock` field pair's meaning. Flagged because
  it misleads.

## 3. Perception: the vision "cone" is a **sweeping ray**, not a cone

This is the single most surprising result. Gunlok never tests "is the target inside a cone".
It sweeps a single ray back and forth across the arc and detects a target on the tick where
the ray **crosses** it.

### 3.1 The sweep

`CharacterActor` carries the sweep state (offsets verified, struct size unchanged at 0x308):

| Off | Name | Meaning |
|---|---|---|
| 0x25c | `next_scan_restart_time` | game-time (s) of the next sweep restart. **Was named `weapon_cycle_offset` in the DB and had nothing to do with the weapon** |
| 0x260 | `scan_arc_valid` | true while a sweep is running |
| 0x264 | `scan_angle` | current sweep bearing, BAM (4096/turn), relative to facing |
| 0x268 | `scan_last_update_time` | previous integration step |
| 0x26c | `scan_eye_pos` | `Vec3f` ray origin = `coords + AiEyeOffset` |
| 0x278 | `scan_ray_end` | `Vec3f` ray end = origin + dir(`scan_angle`) * `sight_range`, clipped to the first wall |
| 0x284 | `scan_sweep_reversed` | 0 = climbing to `+sight_angle`, 1 = falling back to `-sight_angle` |
| 0x28c | `ai_scan_context` | the scratch object every perception call takes in ECX |

**Restart** — `CharacterActor::Update` @ 0x0053d986:

```
if (now > next_scan_restart_time) {
    next_scan_restart_time = now
                           + sight_angle * 4.0 / character->angular_scan_rate   // 0x006521d0 = 4.0
                           + character->scan_delay;
    broadcast update 0x71;
    CharacterActor_RestartScanArc(now);          // 0x0053d0e0
}
```

`CharacterActor_RestartScanArc` sets `scan_angle = -sight_angle` (sign flip via
`DAT_00652210 = -0.0`), `scan_sweep_reversed = 0`, `scan_arc_valid = 1`, and precomputes the
eye/end pair. The `4.0` is exactly the there-and-back travel: `-A -> +A -> -A` is `4A` of arc.

**Integration** — `AdvanceScanArcAndFilterCandidates` @ 0x00459930, per tick:

```
dt = now - scan_last_update_time;  scan_last_update_time = now;
if (!scan_sweep_reversed) {
    scan_angle += dt * character->angular_scan_rate;         // Character+0x0c
    if (scan_angle > sight_angle) {                          // MobileActor+0x174
        scan_angle = 2.0*sight_angle - scan_angle;           // FLOAT_006520a8 = 2.0, reflect
        if (scan_angle <= -sight_angle) { scan_arc_valid = 0; return; }
        scan_sweep_reversed = 1;
    }
} else {
    scan_angle -= dt * character->angular_scan_rate;
    if (scan_angle <= -sight_angle) { scan_angle = -sight_angle; scan_arc_valid = 0; }
}
```

So `sight angle` is confirmed a **half-angle** (the arc is `±sight_angle`), and
`angular scan rate` is what makes the sweep take time. A sweep costs
`4 * sight_angle / angular_scan_rate` seconds, then the actor idles `scan_delay` seconds
before the next one.

### 3.2 The detection test

`CollectDetectableEnemies` @ 0x00459440 (`__thiscall`, `RET 0x8`) runs the whole pipeline over
a scan-context scratch object `{int n; int cap; pair<float,Actor*> *arr; List memo; ...;
Actor *self @ +0x1c}`:

1. **gather** — every actor on a team other than mine, alive, `!vtbl[8]`, whose `ai_think` is
   not `AiThink_Mine`, with `dist² < min(sight_range², *inout_best)`. `sight_range` is
   `MobileActor+0x168`; the squaring is done here, not read from `Character+0x2c`.
2. **`AdvanceScanArcAndFilterCandidates`** — the sweep above, then per candidate:
   - drop unless `dot2D(candidate - eye, ray) > 0` — a **XZ-plane** forward half-plane test
     (`local_2c*local_20 + local_24*local_18`, x and z only; elevation is not tested);
   - `side = cross2D(candidate - eye - ray, ray) < 0` (against `DAT_0065218c = 0.0`);
   - look the candidate up in the per-actor **memo list** (`ctx+0x0c`, nodes
     `{int actor_id; int side; bool seen}`) and **drop it if the recorded side is unchanged**.
     A candidate seen for the first time is therefore always dropped.
   - unseen memo nodes are freed at the end; the function returns `n != 0`.
3. **`CullCandidatesBeyondScanRayHit`** @ 0x0045a2d0 — raycasts `scan_eye_pos -> scan_ray_end`
   through `IsWorldSegmentClear` @ 0x00463460; on a hit `scan_ray_end` is clamped to the hit point and every
   candidate at or beyond that squared distance is removed. This is "a wall blocks the cone".
4. **`CullCandidatesWithoutLineOfSight`** @ 0x0045a480 — per candidate, raycasts
   `candidate->coords + AiEyeOffset` -> `self->coords + AiEyeOffset` with `IsWorldSegmentClear` and
   removes it when blocked.
5. nearest survivor -> `*out`, `*inout_best`.

**`AiEyeOffset` @ 0x006affec is `Vec3f {0, -1, 0}`**, built by the CRT static ctor
`StaticInit_AiEyeOffset` @ 0x00436b80. Gunlok's +Y points down (the navmesh rule is `normal.y < 0` for walkable), so
this is one metre **up** — the eye height for every ray above.

### 3.2.1 The acquisition call site, and the reacquire range

`AiThink_Bot` @ **0x00451ba2**:

```
if (TeamSlots[this->team_id].+0x6b != 0) skip acquisition entirely;
range = (this->alert_state == 1) ? 4.0f * sight_range²      // 0x00451bc2
                                 : 1.0f;                    // 0x00451be1
if (!FindNearestVisibleEnemy(range, &out, &best))           // 0x00451c42
    if (this->scan_arc_valid)                               // CharacterActor+0x260
        CollectDetectableEnemies(&out, &best);              // 0x00451c6d
```

**`FindNearestVisibleEnemy` @ 0x004591e0** (`__thiscall`, `RET 0xc`) is range plus LOS only —
**no cone at all** — and it skips **team 0** as well as the actor's own team. Its range is
**1.0** — one square metre — in every state but one, so outside that state it fires only for an
enemy standing essentially on top of the actor, and **the swept ray is what actually detects
anything**. The `4.0 * sight_range²` branch **is** reachable: `alert_state == 1` is written by
behaviour handler 6 (§2.1.1, §11), so a bot that has just lost its target spends the 17-second
reacquire window with a range-and-LOS acquisition four times its own sight range and no cone
constraint at all. That is the mechanic the manual describes as reacquiring, and it is live.

Both take the scan context in ECX from **`CharacterActor+0x28c`**.

### 3.2.2 A third acquisition shape: the minebot's omnidirectional hearing radius

The swept ray (§3.1) and the reacquire range above are not the only two shapes. **`AiThink_Minebot`
@ 0x00456c50 acquires on the `hearing` radius, with no bearing or facing test anywhere.** Its loop A
(0x00456e20-0x00456f64, gated on `ai_state != 7`) walks `TeamActorLists` @ 0x007ba038 over all
`NumTeamSlots`, and its filters are only:

1. skip the minebot's own `team_id` (0x00456e20);
2. **skip team 0 outright** (0x00456e2c) — a flat hostility test, so it consults neither
   `TeamSlot+0x6b` `not_enemy_source` (as `AiThink_Bot` does @ 0x00451ba7) nor slot 8;
3. slot 6 `IsAlive` (0x00456e52);
4. `dist² < character->hearing_range_squared` — `Character+0x34`, fetched via slot 10
   `GetCharacterData` @ 0x00456de8 and compared `COMISS` @ 0x00456f09;
5. nearest-so-far (0x00456f1e).

An accepted actor goes to `AiBeginInvestigate` @ 0x00456f7c, so the minebot **walks to it and
detonates on it**. Unlike `AiThink_Swarm` it has no `ai_think == AiThink_Mine` skip, so **a minebot
will acquire and walk onto another team's deployed mine**.

The roles are `Rol_Walking_Mine` (identifier `"minebot"`) and `Rol_Mini_Minebot`
(`"mini_minebot"`) — the *roles*, not the characters `Chr_Walking_Mine` / `Chr_Mini_Minebot`, which
also back the `ai swarm` `Rol_Smartbot` / `Rol_mini_Smartbot`. And `Chr_Walking_Mine` authors
`sight range 15` and `hearing range 15` **equal**, so for this family the sight/hearing distinction
is unobservable in play.

See `stealth_and_fog_notes.md` §4 for the concealment consequence, which is the reason the modality
matters: this is a **hearing** bypass, and hearing looks exempt by design rather than broken here.

### 3.3 Consequences for the shipped data

- `sight angle 0` (13 shipped uses) makes `scan_angle` start and end at 0, so
  `scan_arc_valid` is cleared on the first integration step: **the swept-ray path never
  detects anything**. Such a character sees only through `FindNearestVisibleEnemy` and hears
  through §6.
- `hearing range 0` (27 uses) makes the stimulus test `d² - r² <= 0` — only a stimulus whose
  own radius reaches the listener wakes it.

---

## 4. The alarm state machine

All of it is in `AiThink_Bot`, in the tail shared by the behaviour handlers (entered from
0x00454588 and its duplicate at 0x004545b5). **It only runs while `ai_state` is 0 or 1**
(alerted or engaging); any other `ai_state` clears `alarm_deadline` and skips the block.

```
                     ai_state becomes 0 or 1, alert_state == 0, alarm_deadline == 0
alert_state 0  ──────────────────────────────────────────────────────────────►  4
     ▲                    alarm_deadline = now + ticks_per_sec * alarm_delay
     │                    broadcast 0x60                              (0x004546d6)
     │
     │  update 0x62                       now >= alarm_deadline
     │  (client only)     4  ──────────────────────────────────────────────►  2
     │                       alert_timer_end = now + 1 s; broadcast 0x61  (0x004547a0)
     │                                                                   │
     │                       ┌───────────────────────────────────────────┘
     │                       ▼  every tick for 1 second: propagate (§5)
     │                    now >= alert_timer_end
     └──────────────────  2 ────────────────────────────────────────────►  3
                             alarm_deadline = 0, no broadcast          (0x00454aa8)
```

- **`alarm delay` is the delay before the alarm goes out**, in seconds, converted at the
  calling thread's tick rate: `MULSS xmm0, [EBX+0x30]` at 0x004546a0 against
  `ClockTicksPerSecond` / `...Executor` selected by `GetCurrentThreadId() == ExecutingThread`.
  It is *not* the reacquire timeout the brief guessed at.
- The **1 second** at 0x004547c1 is `ticks_per_sec` exactly; the **17 seconds** in the state-1 arm
  is `0x11 * ticks_per_sec` (0x00454b4d: `SHL ECX,4; ADD ECX,EDX`). That arm was recorded here as
  dead while `alert_state == 1` was believed unwritten; it is not (§11), and handler 6 arms the
  same window at 0x00453e71.
- Entering state 2 also calls `MobileActor::AppendPatrolPoint(this->coords, 0, 0)` @ 0x0053a830 when `MobileActor+0x218` is 0. (Renamed from `AppendAuthoredWaypoint`; `MobileActor+0x214` is the **patrol-point list** — see §2.4.)

---

## 5. Alert propagation

`AiThink_Bot` @ **0x004548a7**, once per tick while `alert_state == 2`:

```
remaining      = (float)(alert_timer_end_lo - now_lo) * seconds_per_tick;
radius_squared = (remaining / 1.0f) * alert_radius * alert_radius;   // alert_radius = MobileActor+0x170
for (Actor *a : TeamActorLists[this->team_id])            // OWN team only
    if (dist2(this->coords, a->coords) < radius_squared) {
        if (a->IsAlive() && a->ai_type == Bot
                         && a->ai_state != 0 && a->ai_state != 1
                         && a->alert_state == 0) {
            a->ai_state       = 0;                        // alerted
            a->alert_position = this->alert_position;     // the last known enemy position
            a->alert_flag     = this->alert_flag;
            a->flags(+0x7c)  &= ~0x80;
        } else if (a->IsNode() /* vtbl slot 40 */ && a->+0x250) {
            a->+0x251 = 1;                                // trip an alarm node
        }
    }
```

- **Team filtering is by list, not by test**: only `TeamActorLists[my team]` is walked, so the
  alarm never crosses teams.
- Only `AIType::Bot` actors are alerted. An actor already in `ai_state` 0 or 1, or already
  running its own alarm, is skipped.
- **The radius shrinks to zero over the one-second window** (`remaining` falls 1.0 -> 0.0), so
  the first tick of the alarm does essentially all the work and the ring collapses after.
  The `/ 1.0f` is a literal `FILD 1` at 0x004548d6.
- The `IsNode` fallback writes the same byte (`Actor+0x251`) the `ALERT NODE` console command
  writes (`CommandAlertNode` @ 0x0044bb90).

### 5.1 Replication and the client mirror

| Update | Sent from | Client effect (`ApplyUpdateMessage`) |
|---|---|---|
| 0x60 | `AiThink_Bot` 0x004546e0 | 0x005004e2: `Unit+0x24 = 4`, plays sound 0x24 |
| 0x61 | `AiThink_Bot` 0x004547c3 | 0x00500515: `Unit+0x24 = 2`, `Unit+0x88 = now + 1 s` |
| 0x62 | `CharacterActor::Update` 0x0053f7ee, carrying the **attack target's** id; and `AiThink_Bot` 0x00454055, when the reacquire window expires (§11) | 0x005005bc: `Unit+0x24 = 0`, plays sound 0x25 / 0x4e |
| 0x63 | `AiThink_Bot` 0x00454b82 / 0x0045511c, and 0x00453ed6 / 0x00453f46 on **entering** `alert_state = 1` (§11) | 0x00500664: `Unit+0x24 = 1`, `Unit+0x88 = now + 17 s` |

All four are 8 bytes `{id, actor_id}`, unreliable — at the three handler-6 sites the id pairs with
`[EBX+0xc]`, and a by-value `Vec3` goes to `BroadcastToPlayers` alongside the payload rather than
inside it. **That header/payload split is now CONFIRMED** — it was previously inferred from the call
shape, and has since been read out of `BroadcastToPlayers`' own body: the `Vec3f` is a **spatial
relevance filter**, compared against `FloatZero` @ 0x007f5f40 and, on the single-player loopback
path, ignored entirely; only `EDX` bytes ever cross the wire. See
`directplay_protocol_notes.md` §4.2. One consequence worth knowing here: the same id can reach
different audiences, because two of the three `AiThink_Bot` 0x63 sites pass different coords —
0x00453ed6 passes **the actor's own position**, so that broadcast is proximity-culled, while
0x00453f46 (0x63) and 0x00454055 (0x62) pass **zeroes** and reach every player. The client also
makes the 2 -> 3 transition itself (§7), so state 3 needs no message.

---

## 6. Noise events

`AiStimulusList` @ **0x006af824** is a `List<>` of 0x30-byte records:

| Off | Meaning |
|---|---|
| 0x00 | `Vec3f` current position |
| 0x0c | `Vec3f` original position |
| 0x18 | radius **squared** |
| 0x1c | int "kind"/loudness — `AiExecutorTick` rewrites it to `0x19` while the owner `IsMoving()`, else 0 |
| 0x20 | `__int64` expiry (game clock) |
| 0x28 | `Actor *` owner, refcounted |

**`PostAiStimulus` @ 0x0044f960** is the only poster: `__thiscall`, `RET 0x24` — ECX carries the
kind, and the nine stack dwords are `(Vec3 pos_a, Vec3 pos_b, __int64 expiry, float radius)`;
the radius is squared on the way in. After inserting the node it walks `DAT_007ba058` and, for
every actor that is alive, not attacking, and of `AIType` Bot or Scavenger, tests
`dist²(actor, pos_b) - radius² <= character->hearing_range_squared` (`Character+0x34`) and on
a hit sets `actor->ai_next_think_time = now` — i.e. **the noise wakes the listener's AI this
tick**; the actual reaction is `AiThink_Bot`'s stimulus scan.

Callers: **`ProjectileActor::OnPrePhysics` @ 0x00542ae0** (this is the explosion/impact case
the manual describes), `MineDetonate` @ 0x004507b0, `AiThink_Mine`, `AiThink_Minebot`, `AiThink_Swarm`.

`AiExecutorTick` ages the list every 10th tick: a node whose owner is alive has its position
refreshed from the owner and its kind set from `IsMoving()`; an ownerless or dead node is
freed once `expiry` passes.

`AiThink_Bot` reads the list at 0x00451cdb / 0x00451cf0. The acceptance range there is
`character->hearing_range_squared` while `alert_state == 0`, and
`max(hearing_range², sight_range²)` once an alarm is running (0x00452299) — **an alerted bot
hears further**. A stimulus inside range with the right bearing sets `ai_state = 4` at
0x00452422.

---

## 7. The cone renderer and the colour map

Drawn from `DrawOrderMenu` @ 0x00498610 (a 0x3c21-byte function that is far more than a menu).
It is called **every frame, unconditionally**, from `RunInGameFrame` @ 0x0046e8ca — the name is
misleading — and it walks the client Units table **twice**. The function immediately preceding it
in the same band, previously `FUN_00498140`, is **`UpdateCursorForMode`**, the per-frame
mouse-cursor selector, which `RunInGameFrame` also calls, at 0x0046ea01; it is documented in
`orders_notes.md` §10. The cones and circles are in the
*second* walk, and that whole walk is gated on one global that none of the rest of this section
mentions:

- **`ReconModeActive` @ 0x007b9ca1 must be non-zero.** Tested at 0x0049a063; a zero jumps clean
  over the second walk to the epilogue. The *value* test is measured and certain. The gloss that
  used to follow it — "**so no cone or range circle is ever drawn outside Recon Mode**" — is
  **polarity-dependent and is now probably backwards**: see §11, where the shipped tutorial text
  turns out to point the view cones out to the player *before* telling them to press ENTER. Treat
  this bullet as "the cones are drawn on the non-zero side of the byte" and nothing more until §11's
  question is settled.
  It is `.bss`, so it starts at 0, and the only way to set it is the key bound to
  "Toggle Recon Mode on/off" (default **ENTER**, DIK 28) -> `ToggleReconMode` @ 0x004976d0.
  **No console command reaches it**, and `LoadGame` @ 0x00505b28 restores it from a save.
  `EnterCutsceneMode` @ 0x00487f3f forces it on. That the flag means *recon* rather than *normal*
  was previously called settled by the GLS wait-condition table — the records at 0x0066a1ec
  (`NORMAL VIEW MODE`) and 0x0066a200 (`RECON VIEW MODE`) share the predicate 0x0056fe40 and differ
  only in `+0x10`, 0 and 1. **That is now recorded as the one unverified step in an open question**:
  it holds only if record `+0x10` is an *expected value*, which nobody has read the consumer of.
  See §11 — the polarity is genuinely open, with measurements on both sides, and the gate in this
  bullet is stated in terms of the byte's **value** so that it stands either way.

  Measured in the running game: pressing the bound key with **nothing selected does nothing**, and
  with a character selected it takes level02's start frame from 278 draws to 147 and moves the
  camera to the overhead green view. That second half is a measurement of this gate and is on the
  *supporting* side of §11's question — cones appear when the key press drives the byte to 1.
  The first half's extension "…and leaves `ReconModeActive` clear" is an **inference, not a
  measurement**, and the disassembly predicts the opposite; see below.

  **A selection is a further precondition, and it IS the selection test at 0x00497786** — an
  earlier version of this section concluded it was not, and that conclusion was wrong. The test
  covers only the `:= 0` direction, which is correct; what was missing is that the game is *already
  in* that direction's starting state whenever there is nothing selected. `RunInGameFrame` calls
  `UpdateSelectedUnitCamera` @ 0x00497ca0 only when the byte is 0 (`CMP` @ 0x0046e6d9, call
  @ 0x0046e733), and that function's **first test** after its SEH prologue is

  ```
  00497cca  CMP dword ptr [SelectedUnits count 0x007b46dc],0x0
  00497cd1  JZ  0x004980b5          ; -> CALL ToggleReconMode
  ```

  so with an empty selection the game drives the flag itself on the very first such frame. The
  player's key press then finds the byte already set, takes `ToggleReconMode`'s other branch, and
  the gate at 0x00497786 aborts the transition. That is the "nothing happens". It also means the
  byte is *not* left clear in that situation — re-measuring it live is the cheap confirmation.

  It also has **3 writers / 21 reads**, not one: both stores in
  `ToggleReconMode` (0x00497798 and 0x00497968) plus the `LoadGame` restore above.

  On the same path, `ToggleReconMode` sets `CursorMode` 5 on the `:= 0` branch and 0 on the other,
  and clears `MousePickingEnabled` @ 0x006ac628 on the `:= 0` branch — which is why the cursor is
  inert in the follow-unit view. The cursor dispatcher itself is
  `orders_notes.md` §10's `UpdateCursorForMode`.

`orders_notes.md` line 103 already listed that binding; nothing had connected it to the cone
renderer. Then two more gates:

- **`VisionConesEnabled` @ 0x007b4708** — a byte, console `VISION on|off`
  (`CommandVision` @ 0x00442e00 -> `SetVisionConesEnabled` @ 0x004a0eb0, getter 0x004a0ec0).
  Tested at 0x0049b327; everything below is skipped when clear.
- **`Unit+0x7c` = `draw_vision_cone`, `Unit+0x7d` = `draw_hearing_range`**, read at
  0x0049b355. Both are seeded in the Unit constructor `Unit::Unit_Ctor` @ 0x004b4620, at 0x004b48cd / 0x004b48eb
  from `Character+0x81` / `Character+0x82`, i.e. straight from the GLS `draw vision cone` and
  `draw hearing range` booleans, defaulting to 1 when the role has no character. The console
  commands `TURN VISION CONE {on|off} <actor>` and `TURN HEARING RANGE {on|off} <actor>`
  (`DoTurnVisionOrHearing` @ 0x0044ec00) write the same two bytes at run time.

With `draw_vision_cone` set, `switch (Unit+0x24)` at **0x0049b960**, jump table 0x0049c274:

| `Unit+0x24` | Handler | Colour (RGBA float4 in `.rdata`) |
|---|---|---|
| 0 | 0x0049baa3 | `{0.1, 1.0, 0.1, 0.95}` @ 0x006643c0 — **green** |
| 1 | 0x0049b973 | ramp, see below — **orange** |
| 2 | 0x0049baf6 | `{0.1, 0.1, 1.0, 0.95}` @ 0x006643e0 — **blue** |
| 3 | 0x0049ba79 | `{1.0, 0.1, 0.1, 0.95}` @ 0x006643b0 — **red** |
| 4 | 0x0049ba79 | same constant — **red** |

The state-1 ramp (0x0049b9db..0x0049ba0e), with `t = (Unit+0x88 - now)` seconds:

```
x = t / 17.0 * 0.6           // 0x00664324 = 17.0, 0x006521a4 = 0.6
r = x + 0.4                  // 0x00652874 = 0.4
g = 0.8 - x                  // 0x006642f4 = 0.8
b = 0.0                      a = 0.95
```

At `t = 17` that is `(1.0, 0.2, 0)` — red; it fades through orange to `(0.4, 0.8, 0)` as the
window runs out. Handler 2 also performs the client-side 2 -> 3 transition at 0x0049bbf3.

**Purple.** When `draw_vision_cone` is *false* but `draw_hearing_range` is true, control goes
to 0x0049bc3e instead, which draws the audio-scan circle with
`{0.7, 0.1, 0.6, 0.95}` @ 0x006643d0 — **purple** — and **ignores the alert state entirely**.
That is the manual's "alternative scanning technology you cannot hide from": there is no cone
to stay out of, only an omnidirectional circle. Exactly two shipped roles are in that form,
and both fit the description: **`Chr_Adversor`** (`adversor.gsh`: `sight angle 89`,
`sight range 30`, `hearing range 30`, `can turn no`, `draw vision cone no`,
`draw hearing range yes`) and **`Chr_Walking_Mine`**. Of the other 78 shipped characters that
set the flags, 75 set both to `no` and 3 set both to `yes`.

`DrawOrderMenu` also runs a second block at 0x0049b4c0-0x0049b684 which finds the nearest unit
(within `2500.0` @ 0x006a5b4c) whose vision or hearing is drawn — the HUD "you are being
scanned" indicator — and **`ShowRangeRingsToggle` @ 0x006a373e** at 0x0049b34e / 0x0049baca. That
byte is the key bound to "Toggle vision cones on/off" (default DIK 83, numpad `.`, binding
`KeyBinding_ToggleVisionCones` @ 0x007b74c0); it lives in `.data` and **its initial value is 1**,
so it is on until the player turns it off. The read at 0x0049b34e is not itself a gate — it only
clears `Unit+0x154` when the toggle is off; 0x0049baca and 0x0049bc72 are the gates.

---

## 8. Turret traverse

Two `Character` fields, both BAM after `ToCharacter`'s `deg * 4096 / 360`:

- **`gun_yaw_angle`** (`Character+0x40`) is the maximum yaw the gun may cover **without turning
  the body**. `CharacterActor::Update` @ **0x0053f2b0**:

  ```
  if (character->can_turn == 0 && !this->is_moving)   // Character+0x80, MobileActor+0x184
      skip the turn entirely;
  if (character->gun_yaw_angle < required_yaw_delta)
      skip;                                          // cannot bring the gun to bear
  ... play turn animation 0xd (vtable slot 71) ...
  ```

  `TurretActor::Update` has its own copy at 0x0054b844 with the same shape. So
  **`can turn no` plus a `gun yaw angle` under 180° is exactly the "cannot shoot you from
  behind" turret**: it can neither traverse past the limit nor rotate the body to fix it,
  because the body turn is gated on already moving and an emplacement has
  `walking speed 0`.

- **`elevation_angle`** (`Character+0x44`) is tested by **`IsWithinElevationLimit`
  @ 0x005420a0** — `slot 10 GetCharacterData()`, then `|pitch delta| <= elevation_angle`,
  returning a bool. It has **two call sites**, 0x00452ff8 and 0x004535b7, both inside
  `AiThink_Bot`: `case 0` of each of the two random 4-way picks (§2.1.1, §2.1.2), whose `case 1` is
  `IsAnyEnemyWithinSightRange`. It was previously recorded as having no xrefs at all because the
  calling bytes were undefined.

The angles the turret actually integrates are `CharacterActor+0x2f0` / `+0x2f4` (see
`actor_vtable_notes.md`), not `TurretActor+0x318`/`+0x31c`.

---

## 9. Answering the manual, line by line

| Manual | Binary |
|---|---|
| Green = Patrol, undetected | `alert_state == 0`, `{0.1,1.0,0.1}` |
| Blue = Alarm, has detected one of your characters | `alert_state == 2`, `{0.1,0.1,1.0}` |
| "every other enemy within this signal's radius is also alerted" | §5, radius from `alert radius`, own team, `AIType::Bot` only |
| Red = has detected and is targeting | `alert_state` 3 **and** 4, both `{1.0,0.1,0.1}` |
| Orange = lost lock, reacquiring, a few seconds | `alert_state == 1`, a 17 s ramp — entered by behaviour handler 6 @ 0x00453e3f, which also broadcasts 0x63 (§11) |
| "may go to the target's last known position" | `Actor::alert_position` @ +0x64, propagated with the alarm |
| Purple = alternative scanning technology you cannot hide from | `draw vision cone no` + `draw hearing range yes` -> the omnidirectional circle at 0x0049bc3e, `{0.7,0.1,0.6}`; `Chr_Adversor` and `Chr_Walking_Mine` |
| Explosions make noise that attracts enemies | `PostAiStimulus` from `ProjectileActor::OnPrePhysics`, tested against `hearing range` (§6) |
| Some turrets cannot shoot you from behind | `gun yaw angle` + `can turn` (§8) |

---

## 10. The GLS fields, mapped

| GLS field | `Character` off | Consumed by |
|---|---|---|
| `sight angle` | 0x24 -> `MobileActor+0x174` | sweep bounds, `AdvanceScanArcAndFilterCandidates`; **half-angle**, and 0 disables the sweep |
| `sight range` | 0x28 -> `MobileActor+0x168` | squared in `CollectDetectableEnemies`; also the sweep ray length |
| `angular scan rate` | 0x0c | sweep speed (BAM/s) |
| `scan delay` | 0x10 | idle time between sweeps |
| `scan acceptance angle` | 0x14 | **not reached by any function in this file** |
| `hearing range` | 0x30, squared at 0x34 | `PostAiStimulus` wake test and `AiThink_Bot`'s stimulus scan |
| `alert radius` | 0x38 -> `MobileActor+0x170` | alarm propagation radius (§5) |
| `alarm delay` | 0x60 -> `Actor+0x30` | delay before the alarm goes out (§4) |
| `alertable` | 0x1d | **no reader found** in the AI module |
| `gun yaw angle` | 0x40 | yaw traverse limit (§8) |
| `elevation angle` | 0x44 | pitch limit, `IsWithinElevationLimit` |
| `can turn` | 0x80 | may the body turn to extend traverse (§8) |
| `draw vision cone` | 0x81 -> `Unit+0x7c` | renderer gate only |
| `draw hearing range` | 0x82 -> `Unit+0x7d` | renderer gate only |
| `always cpu controlled` | 0xb4 | `AiExecutorTick`'s membership test |

**`draw vision cone` and `draw hearing range` are not among the ten `Role::flags` bits** — they
are `Character` bools, and `src/Roles.h` already mirrors them correctly at 0x81/0x82. No gap.

---

## 11. Not established

- **`alert_state == 1` (orange) is reachable, and this entry used to say the opposite.** The
  earlier sweep of every write to `Actor+0x80` in `.text` found only `0` (`Actor::Ctor`
  0x0052d35a, `ReadActorFixups` 0x00530a06), `2`, `3` and `4`, and concluded that nothing writes 1
  — so neither the 17-second reacquire window nor update 0x63 nor the orange ramp could occur in
  the shipped build. **The write exists**; it sat in bytes that were undefined in the database, so
  the sweep could not see it. **Behaviour handler 6 @ 0x00453e2b** — one of the nine this section
  used to list as unread — does exactly what the manual describes:

  ```
  00453e2b  CMP dword ptr [EBX + 0x80],0x1      ; already reacquiring?
  00453e32  MOV dword ptr [EBX + 0x54],0x6      ; ai_state = 6
  00453e39  JZ  0x00453f57
  00453e3f  MOV dword ptr [EBX + 0x80],0x1      ; alert_state = 1
  00453e71  MOV dword ptr [EBX + 0x90],ECX      ; alert_timer_end = now + 17 s
  00453e9f  MOV dword ptr [EBP + -0x30],0x63    ; update id 0x63
  00453ed6  CALL 0x00504bf0                     ; BroadcastToPlayers
  ```

  and the expiry is further down the same handler: 0x00453fc8 sets `ai_state = 3`, 0x00453fed sets
  `alert_state = 0`, 0x00453ff7 loads update id **0x62**, 0x00454055 broadcasts. So **0x63 is
  emitted on entering the state and 0x62 on leaving it**, the client mirror at `Unit+0x24` and its
  17-second orange ramp (§7) both run, and the acquisition range at 0x00451bb3 really is
  `4.0*sight_range²` while reacquiring (§3.2.1).

  The lesson is worth more than the fact: **a sweep over `.text` is only as good as the
  disassembly.** 8,400 bytes of this one function were outside its body behind four unresolved
  jump tables (§2.1.2), and every claim of the form "nothing writes X" made over that region was
  a statement about the database rather than about the binary. Two other entries in this section
  fell to the same trap (`IsWithinElevationLimit`, and `ai_state` 6's missing writer).
- **"returns to its original coordinates" — a measured negative.** All nine behaviour handlers
  have now been read, and an inventory of every `[EBX + disp]` access across all 3,977 instructions
  of `AiThink_Bot` (45 distinct offsets) finds the only positional memory to be `alert_position`
  (+0x64..+0x6c, 5 writes / 3 reads), the actor's own position (+0xa0..+0xa8, read-only), and
  `+0x1dc` used as a route origin when `+0x184` is set. **No spawn or home field is read
  anywhere**, and the idle behaviour is not a return to a stored point but a bounded **random
  wander** — current position plus 20 units on a random bearing (handler 3, §2.1.1). `Actor+0x64`
  is the *alert* position; patrol points come from `ADD PATROLPOINT` (handler 0x00442140 ->
  `CommandAddWaypointOrPatrolPoint` @ 0x0044c760) into the `MobileActor` waypoint list at +0x204.

  **`MobileActor+0x1dc` is now identified, and it makes this negative STRONGER rather than
  weaker.** It is `goto_target`, the **current move destination** (it was `unk_coords`, and this
  paragraph used to call it the last unidentified positional field, "the only thing anyone might
  mistake for a home position"). `+0x184` is `is_moving`, and the `is_moving ? &+0x1dc : &+0xa0`
  idiom is a general "where am I heading" accessor, not wander-specific. It is written from the
  caller's destination by `SetMoveDestinationAndBroadcast` @ 0x005395b0 and read as the nav target
  by `GetNavigationTarget` @ 0x0053b5d2; the arrival test in `Update` @ 0x00534246 is the
  *horizontal* squared distance `(x-pos.x)^2 + (z-pos.z)^2`, which also pins the `Vec3` components
  (x at 0x1dc, z at 0x1e4, y skipped). **Definitively not a home or spawn position**:
  `BroadcastStopAtPosition` @ 0x00539d92 *overwrites it with the current position* on every stop,
  as `is_moving` clears — a home field is exactly what that cannot be. Distinct from `Actor+0x40`
  `goto_actor` and `MobileActor+0x1c0` `dest_node`. (The `[ECX+0x1dc]` writes in
  `ToggleCrouchAndCamouflage` @ 0x00536353 / 0x005363b0 are false positives — `ECX` there is
  `this->field_0xe0`, a different object with its own `Vec3` at `+0x1d4`.)
- **`scan acceptance angle` and `alertable`** have no reader in anything read here.
- ~~**`TeamSlots+0x6a` / `+0x6b`** gate which actors get AI at all, and their meaning is a separate
  open question with no writer found.~~ **SETTLED.** `+0x6a` is `player_controlled` — the team
  belongs to a **human player**, any player, not necessarily the local one — and `+0x6b` is
  `not_enemy_source`, set for team 0 alone. Both writers are in one per-team init loop,
  `FUN_00496420`; the team-index space is a three-way partition (player teams {1,3,4,5,...},
  team 0 neutral/scenery, team 2 the AI enemy team). So "which actors get AI at all" is exactly
  right, and it is the **complement** of `+0x6a`: `LoadLevel` @ 0x004e0d30 creates an AI team-group
  for every team *without* the flag. See `address_map.md` under `TeamSlots` for the derivation.
  Everything above that says "CPU team" is now measurement, not inference from usage.
- **`DAT_007ba058`**, the list `PostAiStimulus` walks to wake listeners, is not identified; it
  is a `List<Actor*>` but its membership rule was not read.
- ~~**The selection precondition on Recon Mode.**~~ **SETTLED — it is the selection gate at
  0x00497786 after all**, and the three "camera-transition deadlines" were never deadlines.

  The gate is `CMP dword ptr [0x007b46dc],0` / `JZ 0x00497bbd` at **0x00497786** and it does cover
  only the `ReconModeActive := 0` direction; the `:= 1` store at 0x00497968 is unconditional on its
  path. Both of those readings were right. What two prior sessions missed is that **the game puts
  itself in the `:= 0` direction's starting state** whenever the selection is empty:
  `RunInGameFrame` calls `UpdateSelectedUnitCamera` @ 0x00497ca0 only when the byte is 0 (`CMP`
  @ 0x0046e6d9, call @ 0x0046e733), and that function's first test is
  `CMP [0x007b46dc],0 / JZ 0x004980b5 → CALL ToggleReconMode`. So the flag is already set by the
  time the player presses the key, `ToggleReconMode` takes the leaving branch, and 0x00497786 aborts
  it. **Five of the six callers are `if (flag == 0) ToggleReconMode()`** —
  `UpdateSelectedUnitCamera` @ 0x004980b5, `MobileUnit::LeaveWorld` @ 0x004c10bf, `FUN_0049f350`
  @ 0x0049f471, `EnterCutsceneMode` @ 0x00487f3f, `HandleGameKeyAction` @ 0x0047009a — and only the
  key binding at 0x0046fa0f calls it unconditionally. **No caller gates on the selection.**

  Corollary: the §7 measurement's extension "and leaves `ReconModeActive` clear" is an **inference**
  and the disassembly predicts the opposite. Re-measuring the byte live is the cheap confirmation.

  **The three guards at 0x0049773d / 0x0049774e / 0x0049775f are MSVC thread-safe-static guards, not
  deadlines.** `[TLS+0x20]` is `_Init_thread_epoch`, not a tick: `ESI` comes from the TLS array
  (`MOV ESI,[EAX + ECX*4]`), and each `JG` target runs the out-of-line
  `_Init_thread_header` @ 0x005e459e / `_atexit(<empty RET stub>)` / `_Init_thread_footer`
  @ 0x005e4554 triple followed by a `CMP <guard>,-1` re-entrancy check (e.g. 0x00497bd8-0x00497c09).
  Both helpers are `__cdecl void(int *guard)` with 37 call sites each, always paired.
  **The real statics are the dword *before* each guard**, and each has exactly one write and one
  read in the whole binary:

  | static | guard | value | written | read |
  |---|---|---|---|---|
  | `ReconCameraSavedRoll` 0x007b48b8 | 0x007b48bc | `Camera_World.roll` 0x007b4dd4 | 0x00497924 | 0x00497a7c |
  | `ReconCameraSavedPitch` 0x007b48c0 | 0x007b48c4 | `Camera_World.pitch` 0x007b4dd8 | 0x0049792e | 0x00497a93 |
  | `ReconCameraSavedYaw` 0x007b48c8 | 0x007b48cc | `Camera_World.yaw` 0x007b4dd0 | 0x00497938 | 0x00497aa3 |

  All three are `float`. The `:= 0` branch snapshots them from `Camera_World`; the `:= 1` branch
  pushes them into the camera object at 0x007b4ba0. That is a carry-across, not a save/restore pair,
  and it is **not** evidence for either polarity.

- **The polarity of `ReconModeActive` @ 0x007b9ca1 is now itself the open question**, and it
  replaces the one above. §7 used to call it settled. The evidence is split:

  - **For the name as it stands (non-zero = recon view):** `RunInGameFrame`'s dispatch sends
    non-zero to `UpdateMouseEdgeScroll` + `UpdateReconCamera` and zero to
    `UpdateSelectedUnitCamera`; and the **vision-cone walk** in `DrawOrderMenu` is gated on
    **non-zero** (`CMP` @ 0x0049a063), which pairs with §7's in-game measurement that the key press
    is what makes the cones appear.
  - **For the inverted reading (0 = recon view):** `DrawOrderMenu` calls `DrawTargetInfoPanel`
    @ 0x004a86c0 — **39 `GL_RECON_*` resource ids, more than any other function in the binary** —
    only when the byte is **0** (`CMP` @ 0x004987a2 / `JNZ`, call @ 0x004987b0); and `CursorMode` 5,
    whose name `ReconView` is confirmed *from that panel*, is set on the `:= 0` branch at 0x004977c2,
    the binary's only `SetCursorMode(5)` site.

  So the cone view and the `GL_RECON_*` target panel sit on **opposite branches of the same byte**.

  **The shipped English string table moves the balance, and it moves it toward "inverted".** This is
  external evidence no prior pass had; all quotations are verbatim from `glreseng.dll`'s
  training-level text (extracted from the `RT_STRING` resource, not from the manual):

  1. *"Select Gunlok and press enter to go into Recon mode."* / *"You need to select Gunlok."* — a
     non-empty selection is a documented precondition for **entering**. In the code that gate gates
     the `:= 0` branch and only that branch. So `:= 0` is the entering direction.
  2. *"You are now in Recon mode. In Recon mode, you are not able to move … You can look around
     using the mouse. You can zoom in using the left mouse button. The right mouse button will zoom
     back out."* — the LMB/RMB zoom belongs to recon mode, and its handlers are `CursorMode` 5's,
     set on the `:= 0` branch. *"Not able to move"* also matches that branch clearing
     `MousePickingEnabled` @ 0x006ac628.
  3. The zoom is **integrated inside `UpdateSelectedUnitCamera`** (`ReconZoomFactor *= ReconZoomRate`
     @ 0x00497dca), which `RunInGameFrame` runs on the byte-`== 0` side. So the function that applies
     the recon zoom runs when the byte is 0, and `UpdateSelectedUnitCamera` /
     `UpdateReconCamera` look like they have their **names swapped**.
  4. *"The green triangle in front of the enemy is its view cone and the circles emitting from it are
     its audio scan range … Now press the enter key to go into Recon mode."* — the cones are pointed
     out to the player **before** entering recon mode. That reverses what was the strongest item on
     the other side, and it is why §7's cone gloss is now flagged.

  So four of the five items now line up on the inverted side, and the only datum left against it is
  the one unverified step in §7's wait-condition argument. **Still not renamed**, deliberately: what
  would settle it is reading `InstallWaitForCondition` and the per-frame evaluator to find the actual
  consumer of wait-condition record `+0x10` (record size 0x14, table `WaitForConditionTable`
  @ 0x0066a0c0, 28 entries). If that confirms the inversion then `ReconModeActive`,
  `UpdateReconCamera` @ 0x00484e40, `UpdateSelectedUnitCamera` @ 0x00497ca0 and §7's cone claim all
  have to change **together** — a coherent edit, not a one-name patch, which is the other reason not
  to start it piecemeal. Every claim written in this round is phrased in terms of the byte's *value*
  so that it survives either answer.

  What does **not** depend on the answer: `CursorMode` 5 is `ReconView` (the evidence is the
  39-`GL_RECON_*`-string panel plus the tutorial's own description of the LMB/RMB zoom), and the six
  `OnReconView*` handler names and their zoom directions are confirmed by that same text.
- `AiThink_Scavenger`, `AiThink_Mine`, `AiThink_Minebot`, `AiThink_Node`, `AiThink_Swarm` and
  `AiThink_Waiting` were identified and named but **not analysed**.
- The exact byte layout of the client `Unit` (`Unit::Unit_Ctor` @ 0x004b4620's product) beyond +0x24, +0x7c/+0x7d,
  +0x88 and +0x140..0x154 is not mapped here.
