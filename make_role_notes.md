# Building game objects natively and from script

The `ToXxx` converters re-expressed over description structs (`src/MakeRole`), the `make`
namespace that exposes them to JS, and the three things only the real GLS parser can answer.
`gls_system_notes.md` is the parser itself; `role_system_notes.md` and
`role_subobjects_notes.md` are the objects being built.

### Native constructors (`src/MakeRole.h/cpp`)

The `ToXxx` converters re-expressed over plain description structs, so a game object can be
built with no `ParsedThing` in sight. One `Make*` per GLS section type that produces an
object — thirteen of them, covering every converter except `ToMap` (which `CustomLevel`
drives) and `ParseGLDirs` (which sets game directories rather than building anything):

| GLS section | Converter | Native |
|---|---|---|
| shape / hierarchy | `ToShape` / `ToHierarchy` | `MakeShape` / `MakeHierarchy` — pure `.rif` lookups |
| light | `ToLight` | `MakeLight` |
| projectile | `ToProjectile` | `MakeProjectile` |
| pgenerator | `ToParticleGenerator` | `MakeParticleGenerator` |
| character | `ToCharacter` | `MakeCharacter` |
| destructibility / frag data / *replace* | slot-8 trio | `MakeDestructibility` / `MakeFragData` / `MakeReplaceDestructibility` |
| role | `ToRole` | `MakeRole` |
| ammo / ammo info | `ToAmmo` / `ToAmmoInfo` | `MakeAmmo` / `MakeAmmoInfo` — write the global tables |
| camera track | `ToCameraTrack` | `MakeCameraTrack` — needs a loaded level, and both `name` and `file` |

**Descriptions are in `.gls` units** (degrees, seconds, metres, cycles/sec), because that is
what a `ParsedThing` holds: `CheckValue` only range-checks and stores, and every conversion
lives in the converter. Each `Desc`'s defaults are its section constructor's own, read out of
the `.rdata` constants rather than transcribed.

Five conversions carry real risk and are the reason this is RE work rather than field copying:

- **Angles are BAM** — 4096 to a turn, what indexes the sin/cos tables — and `ToCharacter`
  uses **two association orders** for the same conversion (`(d/360)*4096` for scan angles,
  `(d*4096)/360` for aim/sight/yaw/elevation). Same value mathematically, not always the same
  float, so `MakeRole.h` exposes both.
- **`walking_speed` is a 16.16 fixed-point `int`, rounded twice** — not a float; every reader
  `FILD`s it and there are no float readers. `round(cycles * 65536)`, then if
  the character turns, that *already-rounded integer* goes back to float, divides by `size`,
  re-scales and rounds again. Rounding is `FISTP` under the default control word: nearest-even.
- **A GLS `radius`/`height` of 0 means "use the model's bounding box"**, and `ToRole` — not
  `ToCharacter` — computes it. Skipping it yields characters with no collision extents.
- **GLS units are not uniform, and `max range` is the trap.** It is authored **pre-squared**
  (metres², default 196.0 = 14²) and compared against a squared distance, while `sight range`,
  `hearing range` and `blast range` are plain metres — `ToProjectile` squares the last one on the
  way in. Two shipped roles get this wrong, so the retail game has weapons with a seventh of
  their intended reach.
- **`Role::flags` packs ten booleans** in a fixed bit order, and `alpha_fogging` forces
  `per_vertex_fogging` off rather than reporting a conflict.
- **Particle TTL is converted at the *calling thread's* clock rate** — client and executor
  keep separate ones.

Two things deliberately not reproduced: `ToRole`'s leak on the beam-script error path
(`MakeRole` refuses up front instead), and the converters' habit of leaving allocations
partly uninitialised — everything here zeroes first, which is also required because several
mirrors carry `pool_unique_ptr` members that cannot start from garbage.

### Building game objects from script (`src/JsMake.cpp`, the `make` namespace)

The `"gk"` module's `make` namespace is the native constructors above, exposed to JS. It is
how the shipped `.gsh` headers get re-implemented as `.mjs` modules — no `ParsedThing`, no
parser, and a definition costs a few dozen bytes rather than 0x1b60:

```js
const role = make.role({
  identifier: "bug",
  hierarchy: { rif: "units\\bug.rif", object: "bug", hotspot: "head" },
  character: { walking_speed: 1.5, strength: 1, aggression: 0.1, weapon: "enemy laser weak" },
  ai: "background creature",
  destructibility: { kind: "explode" },
});
```

Four things decide the shape:

- **One call builds a whole role.** A Character, Light, Projectile, pgen or Destructibility
  becomes *owned* by the Role (`RoleDtor` pool-frees all six), so handing the same one to two
  roles would double-free it at level teardown. Describing them inline makes that
  unrepresentable rather than merely discouraged. Shapes and hierarchies are the exception —
  the rif cache owns those — so `make.shape` / `make.hierarchy` hand back reusable handles.
- **`make.role` registers as it builds**, and `DestroyRoles` clears the hash between levels,
  so a header module exports *functions* and a level's `define` hook calls them once per load.
  There is no `register()` step and no conversion cache to reset.
- **Enum fields take keywords**, resolved through the tables recovered by probing (`ai`,
  `weapon`, `secondary weapon`, `ammo type`, `weapon type`, `action on death`, `resistance`,
  particle `type`). A field whose table is not recovered — `interface beam effect` — says so
  and takes the number.
- **Ranges are still checked against the game's own bounds.** `gls::FindField` reads
  `min_values`/`max_values` off the section constructor, so `make` reports the same limits
  `CheckValue` would have, without going through it.

GLS inheritance has no equivalent and needs none: `child : parent` becomes object spread,
which does the merge at authoring time instead of inside the game. `abstract` likewise — a
description is a plain object until something calls `make.role` on it.

### What only the parser can answer (`src/JsGls.cpp`, the `gls` namespace)

Everything else moved to `make`; `gls` keeps the three things a reimplementation cannot
provide, all of which run the real parser:

- **`gls.schema(section)`** — the field table each section constructor declares *about
  itself*: `field_types`, `field_names` (the GLS keyword), `field_satisfied` (false =
  required) and `min_values`/`max_values`. `gls::SectionFields` builds it by constructing a
  throwaway instance and reading it, so no hand-maintained table can drift from the binary.
- **`gls.probe(section, field, names)`** — what integer a GLS enum keyword stands for.
- **`gls.try_parse(source)`** — does this text parse, for bisecting one that does not.

All three inherit the parser's hazards: destructive global state (never during a level load)
and the poisoning described above.

**The enum keyword tables were recovered by asking the parser.** `ai bot` and friends are
compiled into the flex DFA — not stored as strings, absent from every shipped header — so
`gls.probe(section, field, names)` builds a one-field section per keyword in memory, parses it, and
reads `parsed_values[field]` straight back. `ai`, `weapon type`, `ammo type`,
`action on death` and `resistance` now accept names (tables in `src/Roles.cpp`); `type` still
takes a number, because that id is shared by `destructibility` (0..1) and `pgenerator`
(0..12) and the binding cannot tell which section it is being asked about.

Two independent checks passed: `ai` reproduced all 21 values of `AIType` in order, and
`destructibility type` reproduced `DestructibilityKind`. Full tables, declared bounds and
what is still unknown are in `gls_system_notes.md`.

**One parser fact that outranks the rest: a syntax error poisons `LoadGLS` for the whole
process.** It resets its error counter, `ParsedObjList` and the symbol tables on entry, but
not the file stack, and nothing afterwards recovers — a verbatim copy of a shipped section
fails identically. Anything making repeated `LoadGLS` calls (`gls.probe`, `gls.try_parse`,
a level's `includes`) has to treat the first failure as terminal.
