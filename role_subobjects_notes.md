# Gunlok Role Sub-Objects - Reverse Engineering Notes

The four heap objects a `Role` points at: **Character** (combat/AI stats), **Projectile**
(weapon effect), **ParticleGenerator** (emitter) and **Destructibility** (death
behaviour, a small 3-variant polymorphic family). Each is produced from the parsed GLS
by a `toGameObject`-style converter and owned by the parent `Role`.

Companion: `role_system_notes.md` (the `Role` itself), `gls_system_notes.md` (the parser
and per-field GLS rules), `src/Roles.cpp` (the C++ mirrors).

| Sub-object | Role field | Size | Converter | Dtor |
|------------|-----------|------|-----------|------|
| Character | 0x60 | 0xb8 | `ToCharacter` @ 0x0047db80 | `CharacterDtor` @ 0x004adce0 |
| Projectile | 0x5c | 0x20 | `ToProjectile` @ 0x0047e4e0 | `ProjectileDtor` @ 0x004adcc0 |
| ParticleGenerator | 0x20/0x24 | 0xd4 | `ToParticleGenerator` @ 0x0047c750 | `ParticleGenDtor` @ 0x004af190 |
| Light | 0x58 | 0x1c | `ToLight` @ 0x0047e220 | (freed inline) |
| Destructibility | 0xa8 | 0x8 / 0x24 / 0x10 | see §4 | see §4 |

All converters share the pattern: `if (!isValidDeep(parsed)) return NULL;` then `malloc`
+ field copy. **They do not null-check their own result**, and `ToRole` writes through a
NULL returned by `ToCharacter` - fill every required GLS field (see `role_system_notes.md`
§8). Values are read from `parsed + 0x238 + id*8` (the 8-byte union).

## 1. Character (0xb8)

The AI/combat stat block. Every field maps 1:1 to a GLS `character` field; the converter
applies the unit conversions below. The full GLS field table (ids, ranges, defaults) is in
`gls_system_notes.md`; here is the live layout with the transforms `ToCharacter` applies.

| Off | Field | GLS id | Transform |
|-----|-------|--------|-----------|
| 0x00 | walking_speed | 0x0c | `*65536` (16.16); if turning>0 & size>0, re-scaled `(v/size)*65536` |
| 0x04 | turning_speed | 0x0d | `*4096` (revolutions/sec -> units) |
| 0x08 | aim | 0x34 | deg -> 4096-step units (`*4096/360`) |
| 0x0c | angular_scan_rate | 0x10 | `/360*4096` |
| 0x10 | scan_delay | 0x0e | - |
| 0x14 | scan_acceptance_angle | 0x0f | `/360*4096` |
| 0x18 | mine_laying_time | 0x11 | - |
| 0x1c | latch_trigger | 0x7d | bool |
| 0x1d | alertable | 0x7c | bool |
| 0x1e-0x1f | *(padding)* | - | - |
| 0x20 | generation_limit | 0x7b | - |
| 0x24 | sight_angle | 0x35 | deg -> units |
| 0x28 | sight_range | 0x38 | - |
| 0x2c | sight_range_squared | - | `sight_range^2`, cached |
| 0x30 | hearing_range | 0x39 | - |
| 0x34 | hearing_range_squared | - | `hearing_range^2`, cached |
| 0x38 | alert_radius | 0x3a | - |
| 0x3c | aggression | 0x3b | - (**overloaded**: `round(aggression*10)` is the pickup type, see `role_system_notes.md` §7) |
| 0x40 | gun_yaw_angle | 0x36 | deg -> units |
| 0x44 | elevation_angle | 0x37 | deg -> units |
| 0x48 | radius_times_size | 0x6b | `radius * size`, pre-multiplied |
| 0x4c | height_times_size | 0x6c | `height * size`, pre-multiplied |
| 0x50 | size | 0x6d | - |
| 0x54 | damage_multiplier | 0x2b | - |
| 0x58 | shot_speed_multiplier | 0x2e | - |
| 0x5c | target_cycle_delay | 0x2f | - |
| 0x60 | alarm_delay | 0x32 | - (read by `Actor::Ctor`) |
| 0x64 | weapon_cycle_time | 0x30 | - |
| 0x68 | weapon_cycle_time2 | 0x31 | - |
| 0x6c | max_weapon | 0x83 | - |
| 0x70 | max_ammo | 0x84 | - |
| 0x74 | max_module | 0x85 | - |
| 0x78 | initial_first_person_range | 0x86 | - |
| 0x7c | maximum_first_person_range | 0x87 | clamped up to `initial_first_person_range` |
| 0x80 | can_turn | 0x3c | bool |
| 0x81 | draw_vision_cone | 0x3d | bool |
| 0x82 | draw_hearing_range | 0x3e | bool |
| 0x83 | *(padding)* | - | - |
| 0x84 | customisation_hierarchy | 0x76 | `ToHierarchy` (**owned**) |
| 0x88 | shadow_hierarchy | 0x77 | `ToHierarchy` (**owned**) |
| 0x8c | blob_shadow | 0x5a | - |
| 0x90 | description | 0x1a | `GetResourceString` |
| 0x94 | derived_radius | - | **set by `ToRole`, not `ToCharacter`** — `max(bbox.x, bbox.z) * 0.5` |
| 0x98 | derived_height | - | **set by `ToRole`** — `bbox.y` |
| 0x9c | derived_hier_extent | - | **set by `ToRole`** — hierarchy-node extent `* size`, else 0 |
| 0xa0 | status_window_v | 0x40 | `*(1/1024)` UV — a **float**, see below |
| 0xa4 | status_window_u | 0x3f | `*(1/1024)` UV — a **float**, see below |
| 0xa8 | strength | 0x29 | - (hit points; read by `Actor::Ctor`) |
| 0xac | weapon | 0x18 | - (`33` = none; drives the Actor-subclass choice in `CreateActor`) |
| 0xb0 | secondary_weapon | 0x19 | - |
| 0xb4 | always_cpu_controlled | 0x74 | bool |
| 0xb5-0xb7 | *(padding)* | - | - |

Angle unit: the game uses a 4096-step circle (sin/cos tables are 0x1000 entries).
`CharacterDtor` frees only `customisation_hierarchy` (0x84) and `shadow_hierarchy` (0x88).

### What `blob_shadow` (0x8c) actually selects

It picks between **two sprite quads on the same texture**, both built at startup by
`CreateBlobShadowSprites` @ 0x0054f900 (renamed this round from `CreateAlphaJunkSprites`) as
`pool_alloc(0x98)` objects sampling `AlphaJunkTexture` @ 0x007ba1a4 = `units\alpha junk.rim`. They
differ **only in UV rect**:

| value | shape global | u range | v range | shape |
|---|---|---|---|---|
| 0 | `BlobShadowShapeDefault` @ 0x007ba198 | 0.4395 .. 0.4971 | 0.5059 .. 0.5498 | 0.0576 x 0.0439 — **wider than tall** |
| 1 | `BlobShadowShapeSpider` @ 0x007ba19c | 0.375 .. 0.4385 | 0.5596 .. 0.6230 | 0.0635 x 0.0635 — **square** |

The selection happens in `Unit_BuildShadowNode` @ 0x005525b0 at 0x005539f2, walking
`Unit+0xb8` (`Role *`) -> `Role+0x60` (`Character *`) -> `Character+0x8c`. Two things there are worth
knowing:

- **A missing role or character falls back to value 0**, but a value that is neither 0 nor 1 produces
  **no shadow renderable at all** (`SUB EAX,0x1 / JNZ 0x005540ba` bails out). The GLS range
  `0..1` is therefore load-bearing, not decorative.
- **It is only consulted when `ShadowQuality` @ 0x006abdf4 == 1.** Quality 0 bails immediately;
  qualities 2 and 3 take the stencil/projected path and never look at this field. So on most settings
  the field has no effect whatsoever.

The developers' own word for value 1 is **`spider`** — 10 occurrences of `blob shadow spider` across
`archore.gsh`, `mplay_archore.gsh`, `pres_arrow.gsh` and `walking_mine.gsh`. `pres_arrow.gsh` has a
commented-out `shadow hierarchy Hcy_archore_shadow` immediately above one of them, which corroborates
that the blob is the cheap alternative to `shadow_hierarchy` (0x88) rather than an addition to it.

### 0x94/0x98/0x9c are derived geometry, not scratch

Earlier revisions of this file called them "runtime AI scratch, not set by the converter".
Half right: `ToCharacter` does not set them — **`ToRole` does**, right after converting the
character (0x0047dec0..0x0047dfd8). It builds a bounding-box size vector from whichever
geometry the role has:

| Role has | Size vector |
|----------|-------------|
| a hierarchy with `+0x58` set | component-wise `hier[0x68..0x74]` differences |
| a hierarchy with `+0x58` clear | `{1, 1, 1}` |
| a shape | component-wise `shape[0x48..0x54]` differences |
| neither | `{1, 1, 1}` |

then stores `derived_radius = max(size.x, size.z) * 0.5` (`FLOAT_006520a0`) and
`derived_height = size.y`, and — only when the hierarchy has more than one node —
`derived_hier_extent = hier[0x90][0x14] * character->size`.

**The consequence is a real gameplay rule, not bookkeeping:** immediately afterwards,

```c
if (character->radius_times_size == 0.0f) character->radius_times_size = derived_radius;
if (character->height_times_size == 0.0f) character->height_times_size = derived_height;
```

so a GLS `radius`/`height` of 0 — which is the *default*, and what almost every shipped
`character` block leaves it at — does not mean "zero-sized". It means "take the collision
extents from the model". Any native construction path that skips this produces characters
with no radius and no height.

### The three constants, and reproducing the conversion exactly

`ToCharacter`'s conversion constants, read out of `.rdata` rather than inferred:

| Address | Value | Used for |
|---------|-------|----------|
| 0x00663ca0 | 360.0 | degrees per turn |
| 0x00663cb0 | 4096.0 | BAM units per turn |
| 0x00663cc0 | 65536.0 | the 16.16 fixed point `walking_speed` is stored in |
| 0x00663c40 | 1/1024 | the status-window UV scale |
| 0x00652190 | 1/65536 | undoes the fixed point for the walking-speed re-scale |

Four details that a casual reimplementation gets wrong, all of them reproduced in
`gk::MakeCharacter` (`src/MakeRole.cpp`):

- **The degrees→BAM conversion has two association orders.** `scan acceptance angle`
  and `angular scan rate` compute `(deg / 360) * 4096`; `aim`, `sight angle`,
  `gun yaw angle` and `elevation angle` compute `(deg * 4096) / 360`. Same value
  mathematically, not always the same float.
- **`turning_speed` has no `/360`** — the GLS field is revolutions per second, so it is
  a bare `* 4096`.
- **`walking_speed` is rounded twice.** It is first `round(cycles * 65536)` stored as a
  float; then, if `turning_speed > 0 && size > 0`, that *already-rounded integer* is
  taken back to float via `* 1/65536`, divided by `size`, re-scaled by 65536 and rounded
  again. Rounding is `FISTP` under the default control word, i.e. nearest-**even**, not
  truncation and not `+0.5`.
- **`status_window_u`/`v` are floats, not ints.** The stores at 0x0047df06 and 0x0047df21
  are `MOVSS`. Both were typed `int` here and in the Ghidra DB, which is exactly the
  mistyped-field trap in CLAUDE.md: it made the store decompile as a meaningless
  `(int)(x * 0.0009765625)` and hid that these are normalized texture UVs.

### Section-constructor defaults are extractable

`ParseCharacter` @ 0x004821b0 writes each field's default straight into
`parsed_values[id]`, so the whole default table can be read from the binary instead of
transcribed. The stores are SSE and two encodings matter: `MOVSD` writes one slot from a
`.rdata` double, while `MOVAPS`/`MOVUPS` writes **two adjacent slots** from a 16-byte
constant — miss that and half the table comes out attached to the wrong ids. Booleans and
integers are separate `MOV byte/dword ptr [reg+disp], imm` stores. All 44 of
`ParseCharacter`'s defaults recovered this way agree with the table in
`gls_system_notes.md`, which is the first independent check that table has had.

## 2. Projectile (0x20)

The per-shot effect. Small and fully mapped.

| Off | Field | GLS id | Notes |
|-----|-------|--------|-------|
| 0x00 | gravity | 0x33 | bool |
| 0x01-0x03 | *(padding)* | - | - |
| 0x04 | damage | 0x2a | negative heals |
| 0x08 | sound | 0x20 | default 104 |
| 0x0c | max_range | 0x2d | default 196.0 |
| 0x10 | blast_damage | 0x2c | AoE damage |
| 0x14 | blast_range | 0x28 | AoE radius |
| 0x18 | blast_range_squared | - | `blast_range^2`, cached |
| 0x1c | hit_light | 0x1f | `ToLight` (**owned**, 0x1c) |

`ProjectileDtor` frees `hit_light`. `Light` (0x1c) = red/green/blue, specular
red/green/blue, range (all floats, GLS ids 0x21-0x28) - see `gls_system_notes.md`.

## 3. ParticleGenerator (0xd4)

The emitter template. Only ~15 fields come from GLS; the rest is runtime animation state
that `ToParticleGenerator` **default-initialises**. No owned heap pointers -
`ParticleGenDtor` only destructs the three embedded `Vec3` members.

### GLS-derived fields

| Off | Field | GLS id | Notes |
|-----|-------|--------|-------|
| 0x00 | type | 0x41 | emitter type 0..12 (shot/fire/smoke/explosion/...) |
| 0x04 | kind | - | constant 5 (object-kind tag) |
| 0x08, 0x0c | *(from parsed ext)* | - | copied from parsed+0x1b60/0x1b64 (pgen thing is 0x1b70, not 0x1b60) |
| 0x10 | rate | 0x43 | emission rate |
| 0x14 | coords | 0x44/0x45/0x46 | Vec3 emitter offset (x/y/z) |
| 0x30 | red | 0x21 | base colour (channel 0, see below) |
| 0x34 | green | 0x22 | |
| 0x38 | blue | 0x23 | |
| 0x3c | alpha | 0x24 | |
| 0xb4 | generate_generators | 0x68 | bool |
| 0xc4 | start_scale | 0x64 | default 1.0 |
| 0xc8 | end_scale | 0x65 | default 1.0 |
| 0xcc | spin | 0x66 | default 0 |
| 0xd0 | lifespan_ticks | 0x67 | `particle TTL` * current clock rate |

`type` is a `ParticleType` (0..12). The console keyword table in `GetParticleIDFromName`
@ 0x0044c340 names 10 of them: smoke 0, steam 1, snow 2, fire 3, shot 4, explosion 5,
bigexplosion 6, trail 9, rain 11, sparks 12. Ids 7, 8 and 10 exist but have no keyword.
Explosion doubles as the parser's fallback for an unrecognised name.

`kind` is **not** an inert "object-kind tag". `ToParticleGenerator` seeds it to 5, and
`ParticleEmitter_Ctor` reads 5 as *"take the blend/render mode from
`ParticleTypeInfos[type]+0x20`"*; any other value is used directly.

### Why the rest of the struct was unmapped

`ToParticleGenerator` writes constants (`1.0f`, `2`, `0`) into everything outside the GLS
table above, so **reading the converter can never reveal what those fields mean** - and
until now it was the only function in the Ghidra DB with the `ParticleGenerator` type
applied. The struct also carried no `static_assert` on the GkPlus side, so nothing was
pinning the layout either. The meaning lives entirely in two places downstream:

1. **`ParticleEmitter_Ctor` @ 0x00580510** - builds the live 0x104-byte emitter from the
   template. 15 call sites; the `ParticleTester` object (0x190) embeds a whole
   `ParticleGenerator` at +0xb8 and hands it straight to this function, which is the
   easiest way to see the template consumed end to end.
2. **`ParticleTypeInfos` @ 0x007c1964** - `InitParticleSystem` @ 0x005828f0 allocates
   `malloc(0xac8)` = array cookie + **13 x 0xd4** elements and fills each with
   `InitParticleTypeInfo` @ 0x0057d220 (a 13-case switch, ~12.8 KB of code). Every default
   the emitter falls back to - colour ramp, blend mode, min/max TTL, gravity, bounding
   extent - is in there. **Trap: 0xd4 is also `sizeof(ParticleGenerator)`, but this is a
   different type.** Its element ctor builds `Vec3`s at 0x2c/0x38/0x44/0x50/0x5c/0x80/
   0x8c/0xa4/0xb8/0xc4, which `ParticleGenerator` does not have. Size coincidence only.

### Runtime state: five 0x18-byte animation channels

The old reading of this block - "five `{Vec4 of 1.0f, int count=2}` groups at 0x30, 0x48,
0x64, 0x7c, 0x94" - was **wrong about where the records start**, which is why the leading
dwords looked like padding. `ParticleEmitter_Ctor` ingests channel A with a single
`MOVUPS xmm0,[tmpl+0x2c]` + `MOVQ [tmpl+0x3c]`, and channel B from `+0x44`/`+0x54`;
`ToParticleGenerator` likewise stores `xmmword ptr [ESI+0x2c]` and `[ESI+0x44]`. So the
record is 0x18 bytes and **starts at the lead dword**:

```
struct PGenChannel { int lead; float v[4]; unsigned trail; };   // 0x18
```

| Off | Channel | Notes |
|-----|---------|-------|
| 0x2c | A - colour | `v` = GLS red/green/blue/alpha 0x21-0x24; -> emitter+0xa0 |
| 0x44 | B | -> emitter+0xb8 |
| 0x60 | C | lead never written by `ToParticleGenerator` |
| 0x78 | D | ditto |
| 0x90 | E | ditto |

with a 4-byte gap at 0x5c..0x5f between B and C holding two bools. `trail` is 2 from both
constructors, but `ParticleEmitter_Ctor` tests **bit 1** (zero `v[3]`, i.e. alpha) and
**bit 0** (zero byte 3 of `lead`) of channel A's copy when the blend mode is 1 - so it
reads as a flags word, not the keyframe count previously assumed. Only one consumer has
been checked, so treat that as measured-but-not-exhaustive.

Channels C and D are gated: `use_channel_cd` (0x5c) enables
`SceneLightSet_AddDynamicLight(&chanC, &chanD, &field0xb8, 5.5f, field0xa8, field0xac, field0xb0)`
@ 0x0057a040.
`ToParticleGenerator` zeroes that gate, which is why it can leave C/D/E's leads
uninitialised without it mattering - a latent hazard for any code that builds a generator
by hand, not a live bug.

### Template -> emitter field map (from `ParticleEmitter_Ctor`)

| Template | Emitter | Meaning |
|----------|---------|---------|
| 0x00 `type` | +0x00 | also indexes `ParticleTypeInfos` for every default |
| 0x04 `kind` | +0x04 | blend mode; 5 = take `ParticleTypeInfos[type]+0x20` |
| 0x08, 0x0c | +0x28, +0x2c | 0x0c also drives +0xd4 |
| 0x10 `rate` | +0xd8 | and +0x30 = tickrate / rate = **emission interval in ticks** |
| 0x14 Vec3 | +0x7c..+0x84 | then divided by the tick rate, so it is a **per-second rate**, not a static offset |
| 0x20 Vec3 | +0xdc..+0xe4 | |
| 0x5d, 0xb4, 0xb5 | +0xd1, +0xd2, +0xd0 | byte flags (0xb4 = `generate_generators`) |
| 0xc4/0xc8 | +0x3c/+0x40 | start/end scale |
| 0xcc `spin` | +0x9c | scaled by the tick rate |
| 0xd0 `lifespan_ticks` | +0x94/+0x98 | **0 = use `ParticleTypeInfos[type]`+0xb0/+0xb4 min/max TTL** |

The emitter also threads itself onto a per-type live-emitter list at
`ParticleTypeInfos[type]+0x28`, with the next pointer at emitter+0xfc.

### `ParticleTypeInfo` (0xd4) - the per-type default table

`ParticleTypeInfos` @ 0x007c1964 is `ParticleTypeInfo[13]`, indexed by `ParticleType`.
Reading the 13 filled instances side by side is what named it: a field holding 9.81 is
gravity, one holding 30 for rain and 9 for snow is fall speed, and so on.

| Off | Field | Notes |
|-----|-------|-------|
| 0x08 | `uv_u0` `uv_u1` `uv_v0` `uv_v1` | sprite rect in the `particles.rim` atlas, stored as `(pixel + 0.5) / 512`. The renderer (0x00582d10) takes `u1-u0` / `v1-v0` as the quad size. Smoke is a 58x54 sprite, snow 10x9, rain 8x14, trail a 23x211 strip. |
| 0x18 | `render_state` | `malloc(0x1bc)`. Its ctor/finalise (0x00469860 / 0x005a2460) are **generic** engine helpers shared with the shadow renderer - not particle-specific. |
| 0x20 | `blend_mode` | 0/1/2; what `ParticleEmitter_Ctor` uses when the template's `kind` is 5 |
| 0x24 | `type` | own index |
| 0x28 | `live_emitters` | head of the live-emitter list; next pointer at emitter+0xfc |
| 0x2c/0x44/0x50/0x5c | per-tick precompute | derived in the epilogue, see below |
| 0x68 | `alpha` | 128 smoke/steam/snow, 255 most, 0 rain/sparks; the update divides it by a step count to get a fade rate |
| 0x74/0x78 | `min_size` `max_size` | 0.35 smoke, 2.0 bigexplosion, 0.15 shot |
| 0x7c | `needs_state_change` | draw-batching hint, see below |
| 0x80 | `turbulence` | small per-axis drift (**inferred**); `ParticleEmitter_Ctor` ABSs it into the emitter's bounds |
| 0x8c | `velocity` | units/s. rain (0,30,0), snow (0,9,0), steam (0,-1,0), fire (0,-0.7,0) - **+y is down** |
| 0xa0 | `field0xa0` | bool gate tested by the update |
| 0xa4 | `field0xa4` | per-second Vec3, non-zero only for bigexplosion (2.5,-2.5,2.5) and sparks (3,-3,3); purpose unconfirmed |
| 0xb0/0xb4 | `min_ttl_16_16` `max_ttl_16_16` | lifetime in 16.16 fixed-point seconds; what a template with `lifespan_ticks == 0` falls back to |
| 0xb8 | `spawn_velocity_range` | per-axis initial velocity randomisation, u/s. The update multiplies a random vector by it and forces y to `-ABS`, so the stored sign is irrelevant. shot (5,5,5), explosion (4,-4,4), trail (6,-8,6). |
| 0xc4 | `gravity` | u/s^2: 9.81 for snow/explosion/rain/sparks, 6 for trail, 0 for smoke/steam/fire |
| 0xd0 | `ttl_seconds` | the update rounds `ttl_seconds * tickrate` into ticks |

0x00, 0x04, 0x6c, 0x70 and the `Vec3` at 0x38 are never written by `InitParticleTypeInfo`;
0x98/0x9c are written from per-case locals that have not been traced.

**The shared epilogue is all derived state** - do not read 0x2c/0x44/0x50/0x5c as
independent data:

```
turbulence_per_tick = turbulence / sqrt(tickrate)     // random-walk scaling
velocity_per_tick2  = velocity / tickrate^2
velocity_half_t2    = velocity_per_tick2 / 2          // the 1/2 a t^2 position term
gravity_per_tick2   = gravity  / tickrate^2
live_emitters       = NULL
```

It then sets `needs_state_change` unless this type's `render_state` is field-for-field
equal to **`ParticleTypeInfos[type - 1]`**'s (including the 0x30-byte per-stage array) -
a batching hint so adjacent types that share a material don't re-issue state. The compare
is guarded by `type != SMOKE`, so the `-1` index is never taken.

**Caveat on the `ParticleType` ids:** the table is 13 entries and the switch has 13 cases,
but ids 7, 8 and 10 have no console keyword. 7 and 8 share an identical sprite rect, and 9
(`trail`) and 10 have identical physics - so the unnamed ids are variants of their
neighbours, not unused slots.

**Next levers:** the per-tick update `ParticleEmitter_Update` @ 0x00581180 (single caller
`ParticleTypeInfo_UpdateEmitters` @ 0x00580460) and the
renderer `ParticleSystem_Render` @ 0x00582d10 (single caller `RenderSceneAndPresent`
@ 0x00574c50) are the only remaining consumers;
between them they should settle `turbulence`, `field0xa4`, `field0xa0` and the emitter's
own 0x104-byte layout. Both now decompile against named `ParticleTypeInfo` fields.

## 4. Destructibility - a 3-variant polymorphic family

`Role.destructibility` (0xa8) points at one of three record types. They share a
**dtor-only base vtable** (`DestructibilityDtor` @ 0x00483950 resets `vtbl`); dispatch is
**not** virtual - the death/fragment handler `Frag` @ 0x0052e220 switches on the `tag`
int at **+0x04**.

| tag | Variant | Size | Converter | GLS section |
|-----|---------|------|-----------|-------------|
| 0 / 1 | base Destructibility | 0x8 | `ToDestructibility` @ 0x0047e680 | `destructibility { type explode\|splatter }` |
| 3 | FragData | 0x24 | `ToFragData` @ 0x0047e890 | `frag data { ... }` |
| 4 | ReplaceDestructibility | 0x10 | `ToReplaceDestructibility` @ 0x0047eaa0 | the name+replace section |

### base Destructibility (0x8)

```
+0x00 vtbl (DestructibilityDtor)
+0x04 tag   // = GLS 'type' 0x41: 0 explode, 1 splatter
```

### FragData (0x24) - "shatter into pieces"

```
+0x00 vtbl (FragDataDtor)
+0x04 tag = 3
+0x08 Role* role          // GLS 'role' 0x60 - fragment pieces
+0x0c Role* replace_role   // GLS 'replace role' 0x61 - actor left in place at death
+0x10 char* remove         // GLS 'remove' 0x62 (owned)
+0x14 int   scale          // GLS 'scale' 0x63
+0x18 bool  replace        // GLS 'replace' 0x69
+0x19 bool  symmetric      // GLS 'symmetric' 0x6a
+0x1c float blast_range    // GLS 'blast range' 0x28
+0x20 float blast_damage   // GLS 'blast damage' 0x2c
```

`ToFragData` builds `role`/`replace_role` via `ToRole`. `FragDataDtor` frees `remove`.

### ReplaceDestructibility (0x10) - "run a script on death"

```
+0x00 vtbl (ReplaceDestructibilityDtor)
+0x04 tag = 4
+0x08 char* script   // GLS 'name' 0x00 (owned) - a .gcs FILE NAME, see below
+0x0c bool  replace  // GLS 'replace' 0x69
```

**+0x08 is a script file name, not an identifier**, and the section heading here used to say
"swap for another object" on the strength of the GLS keyword alone. Its only reader is `Frag`'s
`case 4` (below), which passes it to `QueueScriptExecution` - so the variant runs a `.gcs` when
the object dies. `ReplaceDestructibilityDtor` @ 0x00483d00 frees it; nothing else touches it.
Mirrored as `ReplaceDestructibility::script`.

### Runtime consumption (`Frag` @ 0x0052e220)

When an actor dies with a destructibility, `Frag` reads `destructibility->tag`:

- **default (0/1)** - simple explode/splatter effect.
- **case 3 (FragData)** - marks the actor (`field_0x7c |= 0x10`); if `blast_damage != 0`
  and `blast_range > 0`, applies area damage in `blast_range^2`; then `SpawnRole`s
  `replace_role` at the actor's position/orientation. If the replacement's hierarchy has
  node slot 3 present, additional attachment logic runs. It does **not** queue a script -
  `Frag` contains exactly one `QueueScriptExecution` call and it is in case 4.
- **case 4 (ReplaceDestructibility)** - `QueueScriptExecution(destructibility + 8)`, i.e. it
  puts the `script` field on the script queue, then reads `replace` at `+0x0c` into the same
  local that case 3 fills from `FragData::replace` (so the flag means the same thing in both).
  This is one of the seven callers of `QueueScriptExecution`; the full inventory is in
  `threading_model_notes.md`.

(tag `2` is unused by any shipped converter.)

## 5. GkPlus source

`src/Roles.cpp` mirrors all four: `Character` (0xb8), `Projectile` (0x20),
`ParticleGenerator` (0xd4, plus `PGenChannel` and the `ParticleType` enum),
`Destructibility` (`{vtbl, tag}`), `FragData` and `ReplaceDestructibility`. The
GLS-derived fields are named; runtime particle slots and struct padding keep `field0xNN`
names. `Character`, `PGenChannel`, `ParticleGenerator` (size + five channel offsets),
`FragData` and `ReplaceDestructibility` carry `static_assert`s; **`Projectile` still has
none**.

The destructibility `tag` fields are typed `enum class DestructibilityKind`
(`Explode=0, Splatter=1, FragData=3, ReplaceDestructibility=4`), mirrored in the Ghidra DB.
`Role.ai` is `enum class AIType` (Roles.h; §6 of `role_system_notes.md`). Fields whose
value mapping is not fully recovered (particle `type` 0..12, role `resistance` 0..9) are
left as `int` rather than encoded as a guessed enum. Note: the console command
`GetParticleIDFromName` @ 0x0044c340 gives a *partial* particle mapping (smoke=0, steam=1,
snow=2, fire=3, shot=4, explosion=5, bigexplosion=6, trail=9, rain=11, sparks=12) that may
differ from the GLS lexer's, so it was not promoted to an enum.
