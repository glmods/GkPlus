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
| 0x00450550 | `Actor_SetAiBehaviour` | `__thiscall`, `RET 0x8`. Installs `ai_think`/`ai_think_secondary` from `AIType` |
| 0x0044f560 | `AiExecutorTick` | `__stdcall(uint time_lo, int time_hi)`, `RET 0x8`. Only caller is `ExecutorThreadProc` @ 0x00509050 |
| 0x00457f30 | `RunAiForTeamGroup` | `__thiscall`, `RET 0x8`. Walks one `TeamActorLists[]` entry |
| 0x00451220 | **`AiThink_Bot`** | `__thiscall void(Actor*, uint time_lo, int time_hi)`, `RET 0x8`, 0x1f39 bytes |

`Actor_SetAiBehaviour` dispatches through a 21-entry byte table at `0x0045069c` and a 10-entry
jump table at `0x00450674`. The full map (`AIType` -> think function):

| AIType | `ai_think` | `ai_think_secondary` |
|---|---|---|
| 0 Bot, 13 Centipede, 19 President | `AiThink_Bot` 0x00451220 — **only if vtable slot 11 `GetWeapon()` != 0**, otherwise NULL | — |
| 1 Scavenger | `AiThink_Scavenger` 0x004556e0 | — |
| 2 Mine | `AiThink_Mine` 0x004552a0 | `AiThink_MineSecondary` 0x0045a640 |
| 3 Minebot | `AiThink_Minebot` 0x00456c50 | — |
| 6 Waiting, 14 Centibody | `AiThink_Waiting` 0x00456bb0, and `has_ai` (`Actor+0x3c`) **cleared** | — |
| 7 Pathfinder | `AiThink_Pathfinder` 0x004556d0 (a 2-instruction tail-jump to 0x0053a1d0) | — |
| 15 Node | `AiThink_Node` 0x0045a850 | — |
| 16 NodeWaiting, 18 Popup, 20 Turret | `AiThink_Waiting` 0x00456bb0, `has_ai` **set** | — |
| 17 Swarm | `AiThink_Swarm` 0x0045b620 | — |
| 4 Reserved, 5 Blocker, 8 TrackObject, 9 Tumbleweed, 10 Pickup, 11/12 BackgroundCreature | NULL, `has_ai` cleared | — |

`Actor_SetAiBehaviour` also seeds `ai_state = 3`, `aggression` (`Actor+0x60`, default
`0x3f333333` = 0.7 when slot 10 `GetCharacterData()` is NULL), and clears `+0x74`/`+0x78`.

**`AiExecutorTick`** does three things per executor tick:

1. builds a temporary list of every actor whose `role->character->always_cpu_controlled`
   (`Character+0xb4`) is set **and** whose `TeamSlots[team]+0x6a` is set, and calls
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
| 0 | **alerted** — an alarm named a position, head for `alert_position` | 0x00454a15 / 0x00454fff (alarm propagation, on the *neighbour*) |
| 1 | **engaging** — has an `attack_target` | 0x004534bf (behaviour handler 1) |
| 3 | idle / hold | `Actor_SetAiBehaviour` @ 0x00450567; console `SET ACTIVITY STOP` @ 0x00445a70 |
| 4 | **investigating** a stimulus | 0x00452422 |
| 7 | (unidentified; only tested, at 0x004513e5) | — |
| 8 | patrol / move order | console `SET ACTIVITY PATROL` @ 0x00445a39 and `GOTO` @ 0x0043ee05 |
| 6 | (tested at 0x0045459e / 0x004545c5, never written in this module) | — |

`AiThink_Bot` chooses a behaviour index 0..8 and tail-dispatches through the jump table at
**0x00455250** (`{0x452b2d, 0x453486, 0x45441c, 0x4524b8, 0x453d23, 0x45384d, 0x453e2b,
0x454588, 0x45455a}`) after remapping it through `ai_state` (0x00452492): `ai_state` 0/6 forces
index 6 unless the index was 5; 1 forces index 1 when `GetAttackTarget()` is non-NULL, else 6;
4 forces index 4; 8 forces index 8 when the index was 3 (the default).

### 2.2 `Actor::alert_state` @ +0x80 — the alarm, and the cone colour

This is the field the manual's colours come from. It is mirrored to the client `Unit+0x24` by
network updates 0x60/0x61/0x62/0x63 (§5).

| Value | Meaning | Cone colour |
|---|---|---|
| 0 | no alarm | **green** |
| 1 | reacquiring (17 s window) — **never written server-side**, see §11 | **orange ramp** |
| 2 | broadcasting the alarm (1 s window) | **blue** |
| 3 | alarm spent | **red** |
| 4 | alarm pending (`alarm delay` counting down) | **red** |

Supporting fields, all on `Actor`:

| Off | Name | Meaning |
|---|---|---|
| 0x30 | `alarm_delay` | seconds, straight from the GLS `alarm delay` field (`Character+0x60`), copied by `Actor::Ctor` |
| 0x34 | `ai_think` | the think function |
| 0x38 | `ai_think_secondary` | only `AIType::Mine` |
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
   through `FUN_00463460`; on a hit `scan_ray_end` is clamped to the hit point and every
   candidate at or beyond that squared distance is removed. This is "a wall blocks the cone".
4. **`CullCandidatesWithoutLineOfSight`** @ 0x0045a480 — per candidate, raycasts
   `candidate->coords + AiEyeOffset` -> `self->coords + AiEyeOffset` with `FUN_00463460` and
   removes it when blocked.
5. nearest survivor -> `*out`, `*inout_best`.

**`AiEyeOffset` @ 0x006affec is `Vec3f {0, -1, 0}`**, built by the CRT static ctor
`FUN_00436b80`. Gunlok's +Y points down (the navmesh rule is `normal.y < 0` for walkable), so
this is one metre **up** — the eye height for every ray above.

### 3.2.1 The acquisition call site, and a second dead branch

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
**no cone at all** — and it skips **team 0** as well as the actor's own team. Because
`alert_state == 1` is unreachable (§11), its range in the shipped build is always
**1.0** — one square metre. So it fires only for an enemy standing essentially on top of the
actor, and **the swept ray is what actually detects anything**.

Both take the scan context in ECX from **`CharacterActor+0x28c`**.

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
- The **1 second** at 0x004547c1 is `ticks_per_sec` exactly; the **17 seconds** in the dead
  state-1 arm is `0x11 * ticks_per_sec` (0x00454b4d: `SHL ECX,4; ADD ECX,EDX`).
- Entering state 2 also calls `FUN_0053a830(this->coords, 0, 0)` when `MobileActor+0x218` is 0.

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
| 0x62 | `CharacterActor::Update` 0x0053f7ee, carrying the **attack target's** id | 0x005005bc: `Unit+0x24 = 0`, plays sound 0x25 / 0x4e |
| 0x63 | `AiThink_Bot` 0x00454b82 / 0x0045511c | 0x00500664: `Unit+0x24 = 1`, `Unit+0x88 = now + 17 s` |

All four are 8 bytes `{id, actor_id}`, unreliable. The client also makes the 2 -> 3 transition
itself (§7), so state 3 needs no message.

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
the manual describes), `FUN_004507b0`, `AiThink_Mine`, `AiThink_Minebot`, `AiThink_Swarm`.

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
misleading — and it walks the client Units table **twice**. The cones and circles are in the
*second* walk, and that whole walk is gated on one global that none of the rest of this section
mentions:

- **`ReconModeActive` @ 0x007b9ca1 must be non-zero.** Tested at 0x0049a063; a zero jumps clean
  over the second walk to the epilogue, so **no cone or range circle is ever drawn outside Recon
  Mode**. It is `.bss`, so it starts at 0, and the only way to set it is the key bound to
  "Toggle Recon Mode on/off" (default **ENTER**, DIK 28) -> `ToggleReconMode` @ 0x004976d0.
  **No console command reaches it**, and `LoadGame` @ 0x00505b28 restores it from a save.
  `EnterCutsceneMode` @ 0x00487f3f forces it on. That the flag means *recon* rather than *normal*
  is settled by the GLS wait-condition table: the records at 0x0066a1ec (`NORMAL VIEW MODE`) and
  0x0066a200 (`RECON VIEW MODE`) share the predicate 0x0056fe40 and differ only in the expected
  value, 0 and 1.

  Measured in the running game: pressing the bound key with **nothing selected does nothing**, and
  with a character selected it takes level02's start frame from 278 draws to 147 and moves the
  camera to the overhead green view. So a selection is a further precondition somewhere on
  `ToggleReconMode`'s path; where was not chased.

`orders_notes.md` line 103 already listed that binding; nothing had connected it to the cone
renderer. Then two more gates:

- **`VisionConesEnabled` @ 0x007b4708** — a byte, console `VISION on|off`
  (`CommandVision` @ 0x00442e00 -> `SetVisionConesEnabled` @ 0x004a0eb0, getter 0x004a0ec0).
  Tested at 0x0049b327; everything below is skipped when clear.
- **`Unit+0x7c` = `draw_vision_cone`, `Unit+0x7d` = `draw_hearing_range`**, read at
  0x0049b355. Both are seeded in the Unit constructor `FUN_004b4620` @ 0x004b48cd / 0x004b48eb
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
  returning a bool. No direct xref was found to this function (see §11).

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
| Orange = lost lock, reacquiring, a few seconds | `alert_state == 1`, a 17 s ramp — **and nothing ever enters that state** (§11) |
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

- **`alert_state == 1` (orange) is unreachable.** A sweep of every write to `Actor+0x80` in
  `.text` finds only `0` (`Actor::Ctor` 0x0052d35a, `ReadActorFixups` 0x00530a06), `2`, `3`
  and `4` (all six in `AiThink_Bot`). Nothing writes 1, so neither the 17-second reacquire
  window nor update 0x63 nor the orange ramp can occur in the shipped build. Either the manual
  documents a cut feature, or the entry point is data-driven in a way this sweep cannot see.
  What would settle it: a live `render`/REPL observation of a cone turning orange, or a
  savegame containing `alert_state == 1`.
  It has a second consequence beyond the colour: the acquisition range at 0x00451bb3 is
  `4.0*sight_range²` only in that state and `1.0` otherwise (§3.2.1).
- **"returns to its original coordinates."** No home-position field was found. `Actor+0x64`
  is the *alert* position, not a spawn point; patrol points come from `ADD PATROLPOINT`
  (handler 0x00442140 -> `FUN_0044c760`) into the `MobileActor` waypoint list at +0x204. The
  return-home behaviour, if it exists, is inside one of the nine behaviour handlers at
  0x00455250, which were not read.
- **`ai_state` 6 and 7** are tested but never written in this module; their meaning is unknown.
- **`scan acceptance angle` and `alertable`** have no reader in anything read here.
- **`TeamSlots+0x6a` / `+0x6b`** gate which actors get AI at all (`AiExecutorTick`,
  `AiThink_Bot`'s enemy scans). Their meaning is a separate open question — no writer has been
  found by any lane. Everything above that says "CPU team" is inference from usage, not
  measurement.
- **`DAT_007ba058`**, the list `PostAiStimulus` walks to wake listeners, is not identified; it
  is a `List<Actor*>` but its membership rule was not read.
- The **selection precondition on Recon Mode** is not located. Pressing the bound key with nothing
  selected leaves `ReconModeActive` clear (measured in the running game); which of
  `ToggleReconMode`'s six callers or which test inside it enforces that was not read.
- **`IsWithinElevationLimit` @ 0x005420a0 has no xrefs at all** — not a vtable slot in any of
  the five Actor vtables checked, and no call site. It may be dead.
- The **behaviour dispatch table at 0x00455250** (nine handlers) was not decompiled; the
  `ai_state` values in §2.1 come from the writes and the remap switch, not from reading what
  each handler does.
- `AiThink_Scavenger`, `AiThink_Mine`, `AiThink_Minebot`, `AiThink_Node`, `AiThink_Swarm` and
  `AiThink_Waiting` were identified and named but **not analysed**.
- The exact byte layout of the client `Unit` (0x004b4620's product) beyond +0x24, +0x7c/+0x7d,
  +0x88 and +0x140..0x154 is not mapped here.
