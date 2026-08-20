# Navigation, pathfinding and movement

How a Gunlok unit gets from A to B: the graph it searches, the search itself, what it does
with the result, and how that becomes a per-tick position and heading.

Everything below is **measured in the Ghidra DB** unless a claim is explicitly marked
*inferred*. Companion notes: `level_loading_notes.md` §5.5 (how the nav polygons are built —
**and see §9 here, which corrects two of its claims**), `actor_vtable_notes.md`,
`role_subobjects_notes.md` (the `Character` fields the mover reads).

## 1. The one-sentence version

There is **no navmesh, no hierarchy and no path cache**. The level's own triangles are the
graph; `FindNavPath` @ 0x0052b680 is a plain **A\* over polygon adjacency** with squared
Euclidean distance for both cost and heuristic, capped at 200–500 expansions; it is re-run
from scratch every tick for the actor's next few waypoints, its output is used mostly for its
*length*, and the actual steering is a straight-line chase of one point with a turning-circle
clamp.

## 2. The graph

### 2.1 Nodes are the level's triangles, and nothing else

`Map+0x88` / `Map+0x8c` — named `sections` / `num_sections` in the DB and in
`level_loading_notes.md` — are the **`NavPolygon *` array and its count**. This is measured, not
inferred: `LoadOrBuildSectionAdjacency` @ 0x0044fef0 reads a neighbour index list out of the
`.map` sidecar and does

```
piVar13 = TheMap->sections[i];                       /* 0x00450103 */
ReadFile(hFile, piVar13 + 5, 4, ...);                /* +0x14 == NavPolygon::flags     */
...
(**(code **)(*piVar13 + 0x5c))(TheMap->sections[j]); /* slot 23 == NavPoly_AddNeighbour */
```

`Map->sections[i]` is therefore an object with a `flags` word at +0x14 and an `AddNeighbour`
slot at +0x5c — a nav polygon. **"Section" is a misnomer**; see §9.1.

### 2.2 `NavPolygon` — corrected 0x40-byte layout

Ctor `NavPolygon_Ctor` @ 0x0048dbb0, vtable 0x00663e60 (27 slots).

| Off | Field | Evidence |
|-----|-------|----------|
| 0x00 | vptr | 0x00663e60 |
| 0x08 | `Vec3 normal` | ctor builds a Vec3 at `param_1+2`; `NavPoly_ProjectPointOntoPlane` divides by +0x0c |
| 0x14 | `uint flags` | read/written by the `.map` sidecar; tested against the agent mask by `FindNavPath` |
| 0x18 | `NavPolygon **neighbours` | `NavPoly_AddNeighbour` writes `[+0x18][ [+0x24]++ ]` |
| 0x1c | `NavPolygon *came_from` | **A\* scratch**, see §3.4 |
| 0x20 | unidentified | the ctor does not zero it |
| 0x24 | `uint num_neighbours` | loop bound in every neighbour walk |
| 0x28,0x2c,0x30 | `float *v0,*v1,*v2` | `NavPoly_ContainsPointXZ` walks the edge pairs (0x30,0x28), (0x28,0x2c), (0x2c,0x30) |
| 0x34 | `Vec3 centre` | slot 4 is `LEA EAX,[ECX+0x34]; RET`; slot 5 stores 3 dwords there (`RET 0xc`) |

**This corrects `level_loading_notes.md` §5.5**, which places `Vertex *v[3]` at 0x20 and does
not mention the adjacency array at all. 0x20 cannot hold `v[1]`: `NavPoly_ContainsPointXZ`
@ 0x004902a0 reads its vertices from 0x28/0x2c/0x30, and 0x18/0x24 are provably the neighbour
array and its count.

**There is a sibling class.** A second 27-slot vtable at **0x00663ecc** has a parallel
implementation of every slot (0x00493170/0x00493f30, 0x004915b0/0x00491ac0,
0x00491fd0/0x00492130, `PolygonAdjacencyTest` 0x0048ecf0 / 0x0048f580 …), and its slot 4 is
`LEA EAX,[ECX+0x38]` rather than `+0x34` — so its object is 4 bytes larger in the vertex
region. A **quad** nav polygon is the obvious reading (ctor pair 0x0048dbb0 / 0x0048dc50, both
153 bytes) but this is *inferred*; see §10.

### 2.3 Adjacency

Built by `BuildPolygonAdjacencyGrid` @ 0x0048aa00 and cached in the `.map` sidecar. Slot 20
`PolygonAdjacencyTest` @ 0x0048ecf0 is the builder's pair test; slot 19
`NavPoly_SharesEdgeWith` @ 0x00495460 is its runtime twin — it fetches both polygons' vertex
pointer arrays (slots 7 and 6) and returns true on the **second matching vertex pointer**,
i.e. a shared edge. Comparison is by pointer, so welding decides adjacency.

### 2.4 The spatial index is a lookup structure, not a pathfinding level

`Map+0x34` is a **three-level indexed grid**, and it is now measured rather than assumed:

```
cell = (*(*(*(Map+0x34) + i*4) + j*4)) + k*0xc          /* 0x0048cff0 */
cell = { NavPolygon **polys, uint count, ... }          /* 0xc bytes  */
```

`Map::MapGrid_WorldToCell` @ 0x004922b0 turns a `Vec3` into `(i, j, k)`. `Map+0x7c/0x80/0x84` are the per-axis plane
coordinate arrays and `Map+0x68` bounds the middle (vertical) axis.

**This settles the two-level question in the negative.** `FindNavPath` touches
`Map->sections[]` not at all and the grid not at all — it walks only `NavPolygon::neighbours`.
The grid exists solely so a `Vec3` can be turned into a polygon. There is one graph, and it
is flat.

### 2.5 Point → polygon

Two near-identical `__thiscall(Map *, Vec3 *) -> NavPolygon *`, both `RET 0x4`:

- **`Map::FindNavPolygonUnder` @ 0x0048cf50** — used by `Goto`,
  `GetNavigationTarget`, `RebuildWaypointRoute`, the console teleport commands, `EvaluateTriggers`.
- **`FindNavPolygonAt` @ 0x0048d380** — used by `CreateNavAgent`, `MoveNavAgent`,
  `ApplyUpdateMessage`.

Both take the grid cell for the point, call slot 10 (`NavPoly_ProjectPointOntoPlane`
@ 0x00492cd0) on each candidate — which runs the XZ containment test (slot 13) and then solves
the plane for Y — keep the candidate whose surface Y is nearest the query Y **and whose normal
has `y < 0`** (RIF is Y-down, so that is "faces up"), then walk the middle grid axis down and
then up until the vertical bound is passed.

Neither is a leaf of the other; they are two compilations of the same routine differing in one
zero-guard on the descent index (`TEST EDI,EDI` at 0x0048d54d, absent in the other). The repo's
inconsistent naming of the pair (`level_loading_notes.md` vs `actor_vtable_notes.md`) reflects
a real duplication in the binary, not an analysis error.

**Neither tests `flags & 0x100`.** A blocker polygon is still returned by a point lookup; the
walkability test lives in the search and in the callers (§3.3, §5.2).

## 3. The search — `FindNavPath` @ 0x0052b680

```
void __stdcall FindNavPath(NavPolygon *start, NavPolygon *goal,
                           List<NavPolygon*> *out, uint traversalMask);
```

**Ends `RET 0x10` at 0x0052be46 — callee-clean, four stack arguments, no register arguments.**
No caller does `ADD ESP,0x10` after the call (checked at all three sites: 0x0053c132,
0x0053c456, 0x005398bc), which is the confirming half of the test.

### 3.1 It is A\*

The open and closed sets are two binary trees of **0x20-byte nodes**:

| Off | Field |
|-----|-------|
| 0x00 | `NavPolygon *poly` |
| 0x04 | `float g` — cost so far |
| 0x08 | `float h` — heuristic |
| 0x0c, 0x10 | child links |
| 0x14 | parent link |
| 0x18, 0x1c | rebalance counters |

and the ordering predicate is `NavSearchNodeWorseThan` @ 0x0052c980, seven instructions:

```
MOVSS XMM1,[ECX+0x8]   ; a.h
MOVSS XMM0,[EDX+0x8]   ; b.h
ADDSS XMM1,[ECX+0x4]   ; a.h + a.g
ADDSS XMM0,[EDX+0x4]   ; b.h + b.g
COMISS XMM0,XMM1
SETA AL
```

**f = g + h.** That is A\*, not Dijkstra and not greedy best-first.

### 3.2 Cost and heuristic are both *squared* distance

Both are the squared Euclidean distance between polygon **centres** (slot 4):

- `g' = g + |neighbour.centre - current.centre|²`
- `h  = |goal.centre - neighbour.centre|²` (`NavPolyCentreDistSquared` @ 0x0052cfc0 computes
  the start node's initial `h` the same way)

No square root is taken anywhere in the search. **Squared distance is not a metric**, so `h`
is not admissible and the accumulated `g` penalises one long step more than two short ones of
the same total length. The result is therefore **not guaranteed optimal**, and the search is
biased towards routes made of many small triangles. This is a property of the shipped code,
not an approximation in this write-up.

### 3.3 Rejection tests per neighbour

```
if ((neighbour->flags & traversalMask) != 0)  skip;      /* 0x0052b96e */
if (IsCornerCutBlocked(...))                  skip;
```

- The mask comes from the **agent**, so walkability is **per-agent** (§7).
- `CollectBlockedNeighbours` @ 0x0052cf10 runs once per expansion, filling
  `BlockedNeighbourArray` / `BlockedNeighbourCount` @ 0x007b9fa8 / 0x007b9fa0 with the current
  polygon's neighbours that *fail* the mask. `IsCornerCutBlocked` @ 0x0052cf50 then rejects a
  candidate that does not share an edge with the current polygon when some blocked neighbour
  shares an edge with both — i.e. **no cutting the corner past an obstacle**.

### 3.4 `came_from` lives on the shared polygon

The parent pointer is written into **`NavPolygon+0x1c`**, not into the search node. `piVar5[7]
== 0` is the "unvisited" test, `piVar5[7] = current` records the parent, and reconstruction
walks `param_1 = param_1[7]`. The two loops in the epilogue exist only to write 0 back into
`+0x1c` for every polygon in both trees.

Consequences, both *inferred* from that fact rather than separately measured:

- The search is **not re-entrant** and two concurrent searches on one map would corrupt each
  other. Both game threads exist (`threading_model_notes.md`); the callers are executor-side.
- An abandoned search that skipped its epilogue would leave stale parents behind.

### 3.5 Budget — 200 to 500 expansions

`PathSearchNodeBudget` @ **0x007b9f98** (was `SectionAdjacencyFlag`, a wrong name — it is not a
flag and has nothing to do with section adjacency beyond where it is initialised).

`LoadOrBuildSectionAdjacency` sets it from a timing measurement:

```
0x0044ff69  CMP EAX,0xc8
0x0044ff6e  JNC 0x0044ff7c
0x0044ff70  MOV [0x007b9f98],0xc8       ; floor 200
0x0044ff7c  MOV ECX,0x1f4
0x0044ff81  CMP EAX,ECX
0x0044ff83  CMOVA EAX,ECX               ; ceiling 500
0x0044ff86  MOV [0x007b9f98],EAX
```

So the budget is **machine-speed scaled, clamped to [200, 500] node expansions**, fixed for the
level. It is decremented once per pop; on exhaustion the function does **not** fail — it falls
through to reconstruction from the closed tree's root, emitting a **partial path**.

**The search is not time-sliced.** It runs to completion (or to budget) inside one call, on the
calling thread, with no resumable state. The budget is the only thing bounding it.

### 3.6 The second entry point — `FindNavPathWithinRadius` @ 0x0052c100

```
void __stdcall FindNavPathWithinRadius(NavPolygon *start, Vec3 *goalPos,
        List<NavPolygon*> *out, uint traversalMask, float goalRadiusSq);
```

Ends **`RET 0x14`** — callee-clean, five stack arguments. (So the two searches differ in
arity but not in convention.)

Verified to be the **same algorithm**, not a lookalike: it calls the same comparator
`NavSearchNodeWorseThan` @ 0x0052c980 (at 0x0052c315 and 0x0052c758), the same
`CollectBlockedNeighbours` @ 0x0052cf10 and `IsCornerCutBlocked` @ 0x0052cf50, the same tree
helpers (0x0052ca60 / 0x0052cb90 / 0x0052d190), and reads the same `PathSearchNodeBudget` at
0x0052c139. It differs by succeeding on any polygon **within `goalRadiusSq` of `goalPos`**
rather than on an exact goal polygon, and by one extra helper at 0x0052d0b0.

Its sole caller is **vtable slot 56 @ 0x00539930** — see §6.1.

## 4. Path post-processing: there is none

**No funnel, no string-pulling, no smoothing.** `FindNavPath` emits a `List<NavPolygon*>` of
0x10-byte `List_Member` nodes (vtable 0x00652084) and nothing else touches it.

`MobileActor::RebuildWaypointRoute` @ 0x0053c270, called from the tick,
uses the result almost entirely for its **length**:

```
budget = (move_state == 5) ? 20 : 8;
if (pathLength < budget + waypointIndex) accept this waypoint;
```

i.e. "is this waypoint reachable in few enough polygons" — a reachability filter, not a route.
Waypoints that pass are consumed off the front of the list; the polygons of the accepted path
are turned into actor waypoints one-for-one by `MobileActor::PushRouteWaypoint` @ 0x0053a640,
which **pushes a 0x18-byte record onto the *front*** of the
route list (+0x204). Push-front, not append: the body is AvP's `add_entry_start` —
`new->prev = sentinel; new->next = sentinel->next; sentinel->next->prev = new;
sentinel->next = new` — and it then refreshes the cursor at +0x224. That is what makes the A\*
backtrack, which is produced goal-first, come out in travel order. The record is `pool_alloc(0x18)`
and the node `pool_alloc(0x10)` (`List_Member<Waypoint*>`, vptr `DAT_00652188`).
So the unit walks **polygon centre to polygon centre**, with no corner optimisation at all.

The record is the `Waypoint` struct, and **both of its trailing dwords have readers** - this file
previously said they were written and never read:

| Off | Type | Meaning |
|---|---|---|
| +0x00 | `Vec3` | `pos` |
| +0x0c | `int` | `keep_on_arrival`. Read at 0x0053a2cf by `MobileActor::AdvanceWaypointRoute` @ 0x0053a1d0: **zero** pops the node and `free_sized(rec, 0x18)` through `MobileActor::PopCurrentWaypoint` @ 0x0053a340, **non-zero** advances the cursor and keeps the record. Every writer in the shipped binary passes literal 0 - including the console path, which pushes an immediate 0 at 0x0044c86c - so the retain branch is a **dead branch, not dead code**. (Do not mistake 0x0053a1fd `CMP [ECX+0xc],0` for this reader; that is the `List_Member`'s own data-pointer null check.) |
| +0x10 | `float` | `wait_time`, the dwell in seconds. Read at 0x00452696 by `AiThink_Bot`, off the **patrol** list, to set the dwell deadline `MobileActor+0x140` latched by `+0x144`. |
| +0x14 | - | **never written by any of the three allocators.** `pool_alloc(0x18)` with no `memset`, and both savegame paths move the full 0x18 bytes (`PUSH 0x18` @ 0x0053210d in `WriteActorFixups`, @ 0x00531060 in `ReadActorFixups`), so four bytes of uninitialised pool memory reach every `.sav` holding a waypoint. Nothing reads it; the only consequence is that saves are not byte-for-byte reproducible. |

`MobileActor` carries **two** waypoint lists, and they are not interchangeable: **+0x204** is the
route this function pushes (count +0x208, cache +0x20c/+0x210, cursor +0x224), and **+0x214** is
the **patrol-point** list - which is what it is *for*, a question this file and the DB both used to
leave open. It is filled by `MobileActor::AppendPatrolPoint` @ 0x0053a830 (was
`AppendAuthoredWaypoint`) and drained by `MobileActor::ClearPatrolPoints` @ 0x0053a550. What settles
it: `ADD WAYPOINT` and `ADD PATROLPOINT` are **one implementation**
(`CommandAddWaypointOrPatrolPoint` @ 0x0044c760, reached by a `JMP` from either registration with
only a `CL` flag between them), that flag is slot 90's `is_patrol_point` argument, and it is exactly
what gates the append to +0x214. `AiThink_Bot` then walks the list at 0x0045264b, picks the nearest
point by position and reads its `wait_time` to dwell there.
`MobileActor+0x228` holds `&this->+0x204`, i.e. a pointer back to the route list's own header.

Two other tests in the same function:

- a waypoint is rejected outright when `(poly->flags & 0x200040) != 0`;
- the search is called with `agent->traversal_flags_full | 0x200040`;
- a trailing prune drops nodes whose slot-4/`Vec3_FlatDistanceSquared` (0x0054efb0) value is below
  `DAT_006521c4` = 1.5.

### 4.1 The player's numbered green waypoint line

That line is **not** a pathfinding artefact. It is `MobileActor::waypoints` (+0x204) — the
player's own queued move orders — rendered by `DrawInGameOverlay` @ 0x00565920, the in-game
order/cursor overlay
called from `RunInGameFrame` @ 0x0046e6c0 (it `sprintf`s, calls `Font_QueueText` and reads
`MobileActor+0x204` three times). Player waypoints enter through **slot 90
`MobileActor::AddWaypoint` @ 0x0053a760**, which builds the same 0x18-byte record.

The engine reuses one list for "orders the player gave" and "polygons the pathfinder chose",
which is why `RebuildWaypointRoute` both reads and rewrites it.

## 5. Per-tick steering

### 5.1 Speed and turn radius — `UpdateSpeedAndTurnRadius` @ 0x0053bb80

```
this->+0x17c = (float)this->walking_speed * (1/65536) * character[+0x9c];
if (character->turning_speed != 0)
    t = this->+0x17c / (character->turning_speed * 0.0015339808);
this->+0x180 = t * t;
```

- `DAT_00652190` = **1/65536** — confirms `MobileActor::walking_speed` (+0x178) is 16.16 fixed
  point, as `MakeRole.h` says.
- `_DAT_0066622c` = 0.0015339808 = **2π/4096** — confirms `Character::turning_speed` (+0x04) is
  in **BAM per second** (4096 BAM = one turn), and converts it to radians/second.
- So **`MobileActor+0x17c` is linear speed in world units per second** and **`MobileActor+0x180`
  is the square of the turning radius** (`v / ω`). Both fields were `field0x17c` / unnamed in
  `src/Actors.h`.

### 5.2 The per-tick delta

In `MobileActor::Update` @ 0x00533720 (slot 70 — see §9.3), at 0x00534215:

```
dt   = Int64ToDouble(now - last) * threadTickRate   ; 0x007c07e4 main / 0x007c07b4 executor
step = this->+0x17c * dt
```

so the per-tick translation is **speed × dt**, taken at the *calling thread's* clock rate — the
same per-thread-clock hazard `MakeRole.h` documents for particle TTL.

### 5.3 Arrival tolerance

At 0x00534246, on the **XZ plane only** (Y is ignored):

```
d2 = (goto_target.x - pos.x)² + (goto_target.z - pos.z)²
if (DAT_00652198 > d2) { arrived; is_moving (+0x184) = 0; }
```

`DAT_00652198` = **0.2**, so the arrival tolerance is **√0.2 ≈ 0.447 world units**.

A second, much looser tolerance lives in `MobileActor::GetNavigationTarget` (slot 91):
`FLOAT_00664328` = **20.0** squared, i.e. **≈ 4.47 units** — inside that distance the unit
steers straight at the point instead of at the next polygon.

### 5.4 The turn clamp — and the manual's "circle a slow turner" claim

At 0x005344b3:

```
XMM0 = this->+0x180        ; turn radius squared
ADDSS XMM0,XMM0            ; 2r²
... fast reciprocal via the table at 0x007fff80 ...
XMM0 = (1/(2r²)) * d2      ; d2 = squared distance to the target
XMM1 = 1.0 - XMM0          ; DAT_006521b8 = 1.0
COMISS XMM1, -1.0          ; NoHitFraction @ 0x00652220 = -1.0
JZ/JBE -> the target is unreachable this turn
XMM1 *= 2048.0             ; DAT_006520b0; 2048 BAM = half a turn
```

`1 - d²/(2r²)` is exactly `cos θ` for a chord `d` on a circle of radius `r`. So the clamp is
**"is the required heading change achievable on my turning circle"**, evaluated per tick, with
the result carried in BAM.

This makes the manual's claim about Gunlok and Hark circling slow-turning enemies a **direct
consequence of the code**, not marketing: a low `turning_speed` gives a large `r`, so `d²/(2r²)`
stays small, `cos θ` stays near 1, and the unit can only bend its heading slightly per tick.
It is *inferred* that this is what the manual is describing — no comparison of actual unit
`turning_speed` values was made here.

### 5.5 `Map::MoveNavAgent` @ 0x00472e30

The 4164-byte sweep underneath all of it.
`bool __thiscall(Map *this, Vec3 *pos, NavAgent *agent, uint clock_lo)`, `RET 0xc`.

**The 3rd argument is a `uint` game-clock tick, not a `float until_time`** - this file's own §5.5
was where that `float` came from, and the DB believed it. `dt` is an **integer** subtract that is
then `FILD`-converted:

```
00472edb  MOV  ECX,[EBP+0x10]      ; arg3
00472ee9  SUB  ECX,[EDI]           ; minus NavAgent+0x00 update_time, an integer already
00472ef3  FILD [EBP-0xac]          ; the difference is FILD'd, so it was not a float
00472f08  DIVSS XMM0,<that dt>     ; ... and then used as the divisor
```

so it sets `agent->velocity = (dest - agent->position) / (float)(clock_lo - agent->update_time)`,
stores `agent->update_time = clock_lo` (@ 0x00473b91, @ 0x00473e07), integrates across polygon
boundaries, and re-snaps `agent->nav_poly` with `FindNavPolygonAt` / `FindNavPolygonUnder` as it
crosses. `NavAgent+0x00` was already `uint update_time` in the DB, which is the consistency check.
All six call sites pass their own `[EBP+0x8]`, i.e. `ExecutorActorTick`'s `clock_lo`. Called from
`MobileActor::Update`, `TurretActor::Update` and three 0x004bxxxx/0x004cxxxx callers.

The tick's **unit** is still open: `Clock+0x0c` is ticks-per-second (a rate, written only by
`MulDiv` inside `Clock::Calibrate` @ 0x005718b0) and `Clock+0x18` is the running accumulator read
through `AccumThreadClock64` @ 0x0044df20, so it is knowable, but it affects neither the type nor
the name.

### 5.6 The nav grid's two scale vectors, and what neither of them is

`Map+0x58` and `Map+0x70` are both per-axis float triples that `MapGrid_WorldToCell` @ 0x004922b0
only ever uses **multiplied together**, which made it look as though one of them was `1/cell_size`
and the other some unit conversion. **Neither is.** `Map::BuildNavGrid` @ 0x0048a0a0 (was
`FUN_0048a0a0`) settles it in eleven instructions:

```
0048a390  MOVSS XMM1,[0x006521b8]      ; = 0x3f800000 = 1.0f
0048a39e  DIVSS XMM0,[EBP-0x1c]        ; extent.x = bb_max.x - bb_min.x
0048a3ba  MOV   [EDI+0x58],EAX         ; +0x58 = 1.0f / extent.x   (inv_grid_extent)
0048a3d8  MOV   [EDI+0x64],ECX         ; +0x64 = nx  (int, grid_cells_i)
0048a3e4  FILD  [EBP-0x2c]             ; (float)nx
0048a3f7  MOV   [EDI+0x70],EAX         ; +0x70 = (float)nx         (grid_cells_f)
```

so `+0x58` is `1/extent`, `+0x70` is `(float)cell_count`, and their product is `1/cell_size`.
`MapGrid_WorldToCell` using only the product is therefore **arithmetically redundant, not a missing
distinction**. Independent confirmation comes from the same function's plane loops, which divide the
extent **by** `+0x70` to obtain the cell size (`DIVSS XMM3,[EDI+0x70]` @ 0x0048a5d8) before emitting
`plane[i] = origin + i*cell_size`. `Map+0x4c` is the AABB max (`grid_bounds_max`), stored beside
`grid_origin` at `+0x40`.

The grid is built by `Map::RebuildNavGrid` @ 0x0048a700 (was `FUN_0048a700`,
`void __thiscall(Map *this, uint nx, uint ny, uint nz)`), which min/max-reduces the level geometry
into an AABB seeded with +/-FLT_MAX and delegates to `BuildNavGrid`; its sole caller is
`LoadOrBuildSectionAdjacency` @ 0x0044fef0.

## 6. Dynamic collision and blockers

### 6.1 Slot 56 is `PathToTarget` — "path to this actor", and nothing to do with collision

> `MobileActor::PathToTarget` @ 0x00539930 is **vtable slot 56** (`MobileActorVtbl` 0x00667f7c +
> 0xe0, reached as `CALL [reg+0xe0]`), it ends **`RET 0x8`** at 0x00539adc, and its arguments are
> `(Actor *target, float stopRangeSq)` — an **actor** and a range. It is the pathfind-and-move
> entry point, and the caller of `FindNavPathWithinRadius` @ 0x0052c100: there is no collision
> manifold, impulse or contact normal anywhere in the body. The base `Actor` installs a bare
> `RET 0x8` stub at 0x0054efa0 whose empty body carries no meaning at all.

`__thiscall void (MobileActor *this, Actor *target, float stopRangeSq)`:

1. **early-out when `this->nav_agent->nav_poly` (+0x14) is null** — no occupied polygon, no
   path. This is what makes `NavAgent+0x14` load-bearing rather than incidental;
2. if `stopRangeSq > FLOAT_00664310`, shrink it: `r' = (√r² − K)²` via `SqrtMantissaTable`, so
   the unit stops *short* of the target;
3. target position from `target->vtbl[0x10]` (slot 4);
4. `FindNavPathWithinRadius(agent->nav_poly, targetPos, &list, agent->traversal_flags_full, r'²)`
   — note it passes the mask **without** the `| 0x200040` that `RebuildWaypointRoute` adds;
5. `SetMoveState(this, 0)`;
6. walks the returned list, pushing each polygon through slot 90 `MobileActor::AddWaypoint`;
   `is_moving = false` and the waypoint cursor is reset.

So this is the **approach-a-target** routine: a full A\* to a point near another actor,
converted straight into the waypoint list. Passing `agent->traversal_flags_full` here is
independent evidence for §7.

Per the sibling lane, it is gated by the **hold-ground byte `MobileActor+0x1b0`** (default 1,
set by `MobileActor::Ctor` @ 0x005324b0 at 0x0053267b), which gates *only* this approach call
and never the fire branch — i.e. hold-ground stops a unit walking to its target, not shooting
at it. Not re-measured here.

### 6.2 Actor-vs-actor avoidance: none found

**No velocity-obstacle, separation, or local-avoidance term exists anywhere in this layer.**
Avoidance, such as it is, is the corner-cut rejection in the search (§3.3) plus the fact that
`RebuildWaypointRoute` re-runs the whole search every tick, so a route around a newly blocked
polygon appears on the next tick for free.

**No stuck/repath timer was found either.** If one exists it is in the order/AI layer above
this, which the brief assigned elsewhere — see §10.

### 6.3 Destructible cover does reach slot 82 — confirmed

`BlockerActor::ActivateInWorld` @ 0x0054d310 (slot 82) drains the actor's `blocked_polys` list
and clears bit 0x100 on each polygon's `+0x14` (`*puVar1 &= 0xfffffeff`), whole body under
`TheMap->lock`. Slot 82 is invoked from four sites:

| Site | Address |
|------|---------|
| `Frag` | 0x0052e39f |
| `Delete` | 0x0052f111 |
| `MobileActor::Update` | 0x0053535a |
| `Die` | 0x0053a13f |

`Frag` and `Die` are the destruction paths, so **destroying cover does un-block its polygons**.
This closes the "pending confirmation" in `actor_vtable_notes.md`.

The flags word is shared mutable state on the polygon, so this is genuinely an in-place edit of
the graph rather than an overlay — as `actor_vtable_notes.md` already said.

## 7. Walkability is per-agent at query time

Yes, and this **qualifies `level_loading_notes.md` §5.5**, which measures the construction side
only.

- `CreateNavAgent` @ 0x00472b80 derives the mask from the collision shape: base `0x40100`;
  `|0x400` when `shape[+0x08] > FLOAT_00652878`; `|0x80` when `shape[+0x08] > DAT_006521a4`;
  `|0x200` when `shape[+0x04] > FLOAT_0065287c`.
- `NavAgent+0x2c` = `mask & 0xfffff97f` (clears 0x80|0x200|0x400).
- `NavAgent+0x30` = `mask | 0x100000` — **the unmasked value**, so the two differ by more than
  the 0x100000 bit (see §9.4).
- `FindNavPath` rejects a neighbour on `(neighbour->flags & traversalMask) != 0`, with
  `traversalMask` = `NavAgent+0x30` (plus `0x200040` from `RebuildWaypointRoute`).

So a polygon is walkable **for this agent**: the 45°/authored 0x100 bit is only one bit of a
mask, and a wide or tall unit is excluded from polygons a small one can use. §5.5's
"walkable iff `(flags & 0x100) == 0 && normal.y < 0`" is correct for **construction and for the
adjacency builder**, and is only the global floor at runtime.

Which bit means what beyond 0x100 is **not established** — the mask bits are set from shape
size but nothing was found that *writes* 0x80/0x200/0x400/0x100000 into a polygon's flags.

## 8. Call chain summary

```
MobileActor::Update (slot 70) @ 0x00533720          per tick, executor side
 ├─ MobileActor::RebuildWaypointRoute @ 0x0053c270
 │    ├─ FindNavPolygonUnder @ 0x0048cf50           waypoint -> polygon
 │    ├─ FindNavPath @ 0x0052b680                   A*, budget 200..500
 │    │    ├─ CollectBlockedNeighbours @ 0x0052cf10
 │    │    ├─ IsCornerCutBlocked @ 0x0052cf50
 │    │    └─ NavSearchNodeWorseThan @ 0x0052c980   f = g + h
 │    └─ MobileActor::PushRouteWaypoint @ 0x0053a640   (push-FRONT onto +0x204)
 ├─ UpdateSpeedAndTurnRadius @ 0x0053bb80 (via 0x00539ed0, broadcast 0x5f)
 ├─ step = +0x17c * dt ; arrival test vs 0.2 ; turn clamp vs +0x180
 └─ MoveNavAgent @ 0x00472e30
      ├─ FindNavPolygonAt @ 0x0048d380
      └─ FindNavPolygonUnder @ 0x0048cf50

MobileActor::PathToTarget (slot 56) @ 0x00539930      "path to this actor", see 6.1
 ├─ early-out on nav_agent->nav_poly == 0
 └─ FindNavPathWithinRadius @ 0x0052c100 -> slot 90 AddWaypoint per polygon

MobileActor::Goto (slot 88) @ 0x00539450
 └─ FindNavPolygonUnder -> MobileActor+0x1c0 (see 9.2), SetMoveState(2)
```

## 9. Corrections to the repo

### 9.1 `Map::sections` is the nav-polygon array

`level_loading_notes.md` treats `Map+0x88/0x8c` as "sections" with the `.map` sidecar holding a
"per-section adjacency graph", and lists 0x24..0x88 as unmapped. Measured (§2.1, §2.4):
`sections[]` **is** `NavPolygon *[]`, the `.map` sidecar is the **polygon** adjacency graph plus
each polygon's `flags` word, and `Map+0x34` is the point-lookup grid. No rename was made — the
name is load-bearing in three files — but both fields now carry a DB comment saying what they
are.

### 9.2 `MobileActor+0x1c0` is a destination polygon, not a path handle

`src/Actors.h` calls it `dest_node` (comment: "was `path_handle`"). `MobileActor::Goto`
@ 0x00539450 stores `FindNavPolygonUnder(TheMap, coords)` into it. It is a **`NavPolygon *` for
the goal**. Both names are misleading; `goto_polygon` would be right.

### 9.3 Slot 70 is `MobileActor::Update` @ 0x00533720

`__thiscall void(MobileActor *, +3 stack args)`, **`RET 0xc`** at 0x00535a87, 0x236a bytes;
slot 70 = `MobileActorVtbl` 0x00667f7c + 0x118. `NodeActor` shares it; `CharacterActor`
overrides at 0x0053d8d0 (0x30f4 bytes, also `RET 0xc`), `TurretActor` at 0x0054b000,
`PresidentActor` at 0x0054d4c0. Driven from `ExecutorActorTick` @ 0x0052fad0 ← `ExecutorThreadProc` @ 0x0050b2f8,
so **executor-side** — which is what makes §3.4's non-re-entrancy tractable (arity, `RET` form
and the thread route are a sibling lane's measurement, reproduced here for the address and
size only).

Slot 70 is `Update` in every one of its ten distinct bodies, base included: the `Actor` body at
0x0052f8a0 is the one that does the position sync, and even there the 0x6f broadcast is
conditional on `position_set`. `MobileActor::Update` calls it as one of its 46 callees.

### 9.4 The `NavAgent` mirror is correct; one comment is not

`src/Actors.h:316-338` verifies field for field against `CreateNavAgent` @ 0x00472b80 —
`update_time` +0x00, `position` +0x04, `shape` +0x10, `nav_poly` +0x14, `radius` +0x18
(= `shape[+0x04] * 0.4`), `velocity` +0x1c, `gravity` +0x28, flags +0x2c/+0x30, 0x34 total.

The one error is the **comment** on `traversal_flags_full`: "the same bits with 0x100000 forced
on". `+0x2c` is `mask & 0xfffff97f` and `+0x30` is `mask | 0x100000` — both derived from the
*same unmasked* `mask`, so `+0x30` retains bits 0x80/0x200/0x400 that `+0x2c` clears. They are
not the same bits.

Also worth adding to that mirror: `MobileActor+0x17c` = speed (units/s) and `MobileActor+0x180`
= turn radius **squared**, both currently unnamed (§5.1).

### 9.5 `BlockerActor` is 0x130, not 0x140 — two independent measurements

1. `CreateActor` @ 0x00510760 does `PUSH 0x130; CALL pool_alloc` at 0x00510bd7, immediately
   before `CALL 0x0054c940` (the `BlockerActor` ctor).
2. **`BlockerActor::GetSize`** (slot 35) @ 0x0054e960 is `MOV EAX,0x130 ; RET`. The slot is the
   cleanest size oracle in the tree: `Actor::GetSize` @ 0x0054e930 returns 0x120 and
   `MobileActor::GetSize` @ 0x0054e950 returns 0x230, both matching `src/Actors.h`.

`actor_vtable_notes.md` and `src/Actors.h` are right about `BlockerActor` — **and so is
`level_loading_notes.md`, which is not talking about the same class.** Its table sits under
`ClientSpawnActorForTeam` @ 0x004fce90 and its own header says the entries are "the *client*
mirror classes and differ from the executor-side sizes in `actor_vtable_notes.md`". So 0x130 is
the executor-side `BlockerActor` and 0x140 is its client `Unit` counterpart; there was never a
contradiction, and the "0x10–0x20 large" pattern across that table is just the client mirrors
being consistently larger. Both measurements above stand — they simply measure the executor side.

The general lesson, which is the reason to keep this section: **Gunlok has two parallel class
trees**, one per thread, and a size, an offset or a slot index is only comparable within one of
them. `role_system_notes.md`'s "MobileActor 0x230" and this table's "Mine 0x238" are both right —
and the 0x238 client class is `MobileUnit`, shared by `ai mine` and every unarmed character rather
than being mine-specific (`rendering_notes.md` §5.1).

### 9.6 `NavPolygon` vertex offset

§5.5's table places `Vertex *v[3]` at 0x20; it is **0x28** (§2.2).

### 9.7 Nothing contradicts the 45° claim

`BuildNavPolygons`' slope test and the `1e-5` epsilon at 0x00663f4c were not re-derived here and
nothing found contradicts them. §7 qualifies rather than refutes: 0x100 is one bit of a
per-agent mask at query time, and the point lookups do not test it at all.

## 10. What is still unknown

- **The second polygon class** (vtable 0x00663ecc). "Quad nav polygon" is inferred from the
  +0x38 vs +0x34 centre offset and the paired function sizes. Settling it needs the ctor at
  0x0048dc50 read for its vertex count and the allocation site found in `BuildNavPolygons`.
- **What the traversal mask bits mean.** 0x80 / 0x200 / 0x400 / 0x100000 / 0x200040 are set on
  the *agent* from shape size and tested against polygon flags, but no writer of those bits into
  a polygon's flags was found. Candidates: `OpenDoor2` @ 0x0048da20 and the door subsystem.

  **The door half of that is now closed on the client side.** The door/lift is
  `TrackObjectUnit` (vtable 0x00664f74, `ai track object`): its `Update` @ 0x004c7220 calls
  `OpenDoor2`, its slot 86 @ 0x004c8000 calls `TriggerCloseDoor`, and its `EnterWorld` @ 0x004c79f0
  binds to a placed map object by shape. Authoring-side confirmation:
  `<Gunlok>\scripts\level02.gls:329` is `role Rol_TOWERLIFTA` with `ai track object`, and line 395
  `Rol_hoversled` likewise. So `OpenDoor2`'s caller is identified; what is still unknown is whether
  it (or anything else) writes one of the *mask* bits into a polygon's flags. The one runtime
  polygon-flag writer found so far writes **0x100** and nothing else: `BlockerUnit::EnterWorld`
  @ 0x004cd680 / `BlockerUnit::Unblock` @ 0x004cdf10 (`level_loading_notes.md` §5.5).
- **`NavPolygon+0x20`** — not written by the ctor and not identified.
- **A stuck/repath timer.** None found in the movement layer; if it exists it is in the order/AI
  layer (`MobileActor::ClearOrderQueue` @ 0x00538830 and friends), which the brief assigned
  elsewhere.
- **Whether anything else calls slot 56.** Only the vtable entries were enumerated here; the
  hold-ground gate at `MobileActor+0x1b0` is a sibling lane's measurement, not re-derived.
- **`MobileActor::GetNavigationTarget` (slot 91) @ 0x0053b560 has no callers.** Eight vtable
  references and, by an exhaustive scan of every `[reg + 0x16c]` operand in `.text`, zero call
  sites in the Actor family. It reads as the intended "where do I steer next" API — current
  polygon, goal polygon, steer point, with the `IsNeighbour` shortcut and the 20.0² tolerance —
  and the live code does the equivalent inline. Treat it as **vestigial** until a computed call
  is found.
- **Whether the two search callers can overlap.** §3.4's non-re-entrancy matters only if they
  can; both look executor-side, but that was not traced.
- **The exact node-budget input.** The clamp to [200, 500] is measured; the timing expression
  feeding it (`* 0.002 * 1000.0` at 0x0044ff54) was not decoded.
