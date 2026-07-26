# Gunlok Role System - Reverse Engineering Notes

A **Role** is a template/definition for a spawnable game entity: its shape, AI class,
combat stats, pickup behaviour, vulnerabilities and cosmetic data. Roles are the output
of parsing a GLS `role` section; spawning a role produces a live `Actor`. The binary
calls them "entities" internally (the hash is the *entity* table, ids come from
`next_entity_id`).

Companion docs: `gls_system_notes.md` (the parser and per-field GLS rules),
`actor_vtable_notes.md` (the Actor subclasses a role spawns into),
`level_loading_notes.md` (how placed roles are spawned at level load),
`src/Roles.cpp` (the GkPlus C++ mirror of this layout).

## 1. Lifecycle at a glance

```
GLS "role {...}" ──ParseRole──> ParsedRole (0x1b68) ──ToRole──> Role (0xc0) ──hash──> roles table
                                                                    │
                                              Role:spawn / placed-object loop
                                                                    ▼
                                             SpawnRole ──CreateActor(role->ai)──> Actor subclass
```

| Function | Addr | Role |
|----------|------|------|
| `ParseRole` / `DoParseRole` | 0x00482bb0 / 0x0047cb90 | build the parsed `role` object (0x1b68), define field rules |
| `ToRole` | 0x0047cc20 (thunk 0x0047cc10) | parsed -> live `Role`; see §3 |
| `CreateRole` | 0x004add90 | allocate+default a `Role`, assign id, insert in hash |
| `RoleDtor` | 0x004ada50 | free everything a role owns (not the role itself) |
| `DestroyRoles` | 0x004ae250 | tear down the whole hash at level teardown |
| `GetRoleByName` | 0x004ae030 | linear scan of all buckets by `name` (`__mbsicmp`) |
| `GetRoleById` | 0x004ae0d0 | hash lookup by `id & bucket_mask` |
| `GetAmmoPickupRole` | 0x004ae110 | first role with `character->weapon == w` and pickup-type 6 |
| `GetWeaponPickupRole` | 0x004ae1b0 | first role with `character->weapon == w` and pickup-type 4 |
| `GetPickupType` | 0x004ae340 | `round(character->aggression*10)`, else 2 |
| `SpawnRole` | 0x00503710 | executor-side spawn wrapper (Role:spawn calls this) |
| `CreateActor` | 0x00510760 | `role->ai` -> Actor subclass dispatch |

## 2. The roles ("entity") hash table

Located at **0x007b48f0**. GkPlus (`src/Roles.cpp`) models it as `struct Roles`; the
Ghidra DB names the members as four contiguous globals:

| Offset | GkPlus | Ghidra global | Meaning |
|--------|--------|---------------|---------|
| 0x007b48f0 | `num_roles` | `num_entities` | live role count |
| 0x007b48f4 | `num_buckets` | `num_entity_lists` | bucket count (power of two) |
| 0x007b48f8 | `bucket_mask` | `entity_bucket_mask` | `num_buckets - 1` |
| 0x007b48fc | `buckets` | `entity_lists` | `RoleList*[]` bucket array |
| 0x007b48d4 | - | `next_entity_id` | monotonic id source (separate global, reset to 0 in `LoadLevel`) |

Buckets chain 8-byte nodes: `RoleList { Role* entity; RoleList* next; }`. Insertion is
head-first into bucket `id & bucket_mask`.

- `GetRoleById` uses the hash directly (ids are the hash key).
- `GetRoleByName` / `GetAmmoPickupRole` / `GetWeaponPickupRole` **cannot** use the hash
  (names/weapons aren't the key) and walk every bucket - O(n).

Two hazards worth noting: names are compared case-insensitively (`__mbsicmp`), and
nothing dedupes on name, so two roles with the same identifier both live in the table
and `GetRoleByName` returns whichever hashed earlier.

## 3. The `Role` struct (0xc0 bytes)

Verified against `ToRole` (the only field-setter from script), `CreateRole` (defaults),
`RoleDtor` (ownership) and `Actor::Ctor` (consumption). "GLS id" is the parsed field id
(`parsed + 0x238 + id*8`); a blank id means the field is runtime-derived, not from script.

| Off | Field | Type | GLS id | Notes |
|-----|-------|------|--------|-------|
| 0x00 | `name` | char* | **0x47** | in-game name for `GetRoleByName`; from `identifier`, not `name` |
| 0x04 | `recon_name` | char* | 0x7e | `GetResourceString` (localized-string id) |
| 0x08 | `recon_ai_short` | char* | 0x7f | `GetResourceString` |
| 0x0c | `recon_ai_number` | int | 0x80 | default -1 |
| 0x10 | `recon_ai_long` | char* | 0x81 | `GetResourceString` |
| 0x14 | `recon_ai_long2` | char* | 0x82 | `GetResourceString` |
| 0x18 | `shape` | Shape* | 0x05 | `ToShape`, if the value is a `shape` |
| 0x1c | `hierarchy` | Hierarchy* | 0x05 | `ToHierarchy`, if the value is a `hierarchy` |
| 0x20 | `pgen` | ParticleGenerator* | 0x07 (or 0x05) | also set from 0x05 when that slot is a `pgenerator` |
| 0x24 | `pgen2` | ParticleGenerator* | 0x08 | |
| 0x28 | `meta_sound` | char* | 0x88 | `strdup` |
| 0x2c | `hotspot_point` | Vec3 | - | resolved position of the `hotspot` node in the hierarchy |
| 0x38 | `alternate_hotspot_point` | Vec3 | - | resolved position of the `alternate hotspot` node |
| 0x44 | `hotspot` | char* | - | hotspot node name (from the shape/hierarchy sub-field) |
| 0x48 | `alternate_hotspot` | char* | - | alternate hotspot node name |
| 0x4c | `num_hier_nodes_26_30` | int | - | count of present hierarchy nodes in slots 26..30 |
| 0x50 | `num_hier_nodes_21_25` | int | - | count of present hierarchy nodes in slots 21..25 |
| 0x54 | `limit` | int | 0x78 | |
| 0x58 | `light` | Light* | 0x1e | `ToLight` |
| 0x5c | `projectile` | Projectile* | 0x0b | `ToProjectile` |
| 0x60 | `character` | Character* | 0x0a | `ToCharacter` |
| 0x64 | `inventory_info` | InventoryInfo* | 0x06/0x1a/0x1b/0x5d/0x5e | lazily allocated, see §4 |
| 0x68 | `vulnerabilities` | VulnList* | - | list head; entries added outside `ToRole` |
| 0x6c | `num_vulnerabilities` | int | - | count |
| 0x70 | `vuln_cached_array` | void* | - | flattened-array cache, freed on edit |
| 0x74 | `vuln_cache_valid` | int | - | cache flag |
| 0x78 | `flags` | u16 | many | 10 bit-packed booleans, see §5 |
| 0x7c | `ai` | AIType(int) | 0x49 | selects the Actor subclass, see §6 |
| 0x80 | `interface_beam_delay` | int | 0x4a | default -1; `< 0` disables the vuln |
| 0x84 | `interface_beam_effect` | VulnerabilityType | 0x4b | default 1 (`Confusion`); `4` = `Script` (needs 0x4c) |
| 0x88 | `interface_beam_script` | char* | 0x4c | only if effect == 4 |
| 0x8c | `interface_beam_duration` | int | 0x4d | default -1 |
| 0x90 | `resistance` | int | 0x4e | |
| 0x94 | `resistance_factor` | float | 0x4f | |
| 0x98 | `armor_value` | float | 0x5b | |
| 0x9c | `shields` | float | 0x5c | |
| 0xa0 | `recharge_rate` | float | 0x5f | |
| 0xa4 | `alpha` | float | 0x24 | |
| 0xa8 | `destructibility` | Destructibility* | 0x59 | slot-8 converter |
| 0xac | `sever_points` | list head | 0x48 | `sever point` string split on `,` |
| 0xb0 | `num_sever_points` | int | 0x48 | one per token |
| 0xb4 | `sever_point_cached_array` | void* | - | freed+nulled on each insertion |
| 0xb8 | `sever_point_cache_valid` | int | - | cache flag |
| 0xbc | `id` | int | - | `next_entity_id++`, the hash key |

**Two embedded 16-byte list headers** follow the game's standard
`{sentinel, count, cached_array, cache_flag}` shape (same as TriggerList):

- `0x68..0x74` - **vulnerabilities**. Nodes are `VulnList` (0x10) holding
  `Vulnerability*` (0x1c); each vuln owns a `script` string. Populated by the
  vulnerability-processing path, not `ToRole`. See §10.
- `0xac..0xb8` - **sever points**. `ToRole` splits the `sever point` GLS string on
  `,`; each token becomes a node (`{vtbl=TriggerBase, char* name}`). Despite reusing the
  trigger node type (so `RoleDtor` cleans it with `TriggerList::DeleteTriggers`), this is
  a list of body-part/attachment names, not triggers.

### `hotspot_point` / `alternate_hotspot_point` (0x2c / 0x38)

Only written for **hierarchy** roles. `ToRole` `strdup`s the hotspot node name into
`hotspot` (0x44), then `HierarchyResolveNamedPointPos(hierarchy, &hotspot_point, name)`
(0x00594890) recursively walks the hierarchy for the node of that name and writes its
resolved position. `alternate_hotspot_point` is seeded from `hotspot_point` then
re-resolved from the alternate name. Shape/pgen roles leave all four (0x2c, 0x38, 0x4c,
0x50) at their zero defaults.

## 4. `InventoryInfo` (0x18) - the pickup sub-object

`Role.inventory_info` (0x64) is allocated lazily (only when a relevant field is present):

| Off | Field | GLS id | Notes |
|-----|-------|--------|-------|
| 0x00 | `hierarchy` | 0x06 | `ToHierarchy` if the `inventory shape` value is a hierarchy |
| 0x04 | `shape` | 0x06 | `ToShape` if it is a shape |
| 0x08 | `description` | 0x1a | `GetResourceString` |
| 0x0c | `pickup_name` | 0x1b | `GetResourceString` |
| 0x10 | `pickup_radius` | 0x5e | default 6.0 |
| 0x14 | `action_on_death` | 0x5d | |

**Latent bug:** the inline allocation path (taken when the `inventory shape` field 0x06
is present) initialises only 5 of the 6 fields and leaves `pickup_radius` (+0x10)
**uninitialised**; the `FUN_00483390` constructor path zeroes all six. A role that sets
`inventory shape` but not `pickup radius` therefore gets garbage pickup radius.

## 5. The `flags` word (0x78) - 10 packed booleans

Low byte at 0x78, high byte at 0x79. Each written with the `flags ^= (bit ^ flags) & mask`
idiom.

| Bit | Mask | Flag | GLS id |
|-----|------|------|--------|
| 0 | 0x001 | alpha_fogging | 0x56 |
| 1 | 0x002 | per_vertex_fogging | 0x58 |
| 2 | 0x004 | nolighting | 0x73 |
| 3 | 0x008 | reflective | 0x57 |
| 4 | 0x010 | destination_selectable | 0x6e |
| 5 | 0x020 | destroy_after_collection | 0x6f |
| 6 | 0x040 | hit_test_ignore | 0x72 |
| 7 | 0x080 | frag_control | 0x75 |
| 8 | 0x100 | moves_on_lifts | 0x70 |
| 9 | 0x200 | status_display | 0x71 |

**`alpha_fogging` wins over `per_vertex_fogging`:** bit 1 is forced to 0 whenever bit 0
is set, regardless of the GLS `per vertex fogging` value. `Actor::Ctor` reads bit 7
(`~flags >> 7`) when constructing the visual object.

## 6. Spawning: `ai` -> Actor subclass

`Role:spawn(team, pos, ori, owner_id)` -> `SpawnRole` (0x00503710): suspends the
executor, calls `CreateActor`, broadcasts **update 0x64** (48 bytes, reliable) so clients
mirror the spawn, runs `InitPositionAndTiming` + `SetCoords`, resumes. `owner_id != -1`
snaps the new actor onto the owner actor's hotspot.

`CreateActor` (0x00510760) dispatches on `role->ai` (executor-side classes; sizes match
`actor_vtable_notes.md`):

| Condition | Actor class | Size |
|-----------|-------------|------|
| `ai == Bot`, or `ai == default` with a character whose `weapon != 33` | CharacterActor | 0x308 |
| character with `weapon == 33`, or `ai == Mine` | MobileActor | 0x230 |
| no character, no projectile | plain Actor | 0x120 |
| **no character but HAS projectile** | **NULL - not spawned** | - |
| `ai == Blocker` | BlockerActor | 0x130 |
| `ai == TrackObject` | TrackObjectActor | 0x1b8 |
| `ai == Tumbleweed` | TumbleweedActor | 0x120 |
| `ai == Pickup` | PickupActor | 0x150 |
| `ai == BackgroundCreature` | BackgroundCreatureActor | 0x120 |
| `ai == FlyingBackgroundCreature` | FlyingBackgroundCreatureActor | 0x120 |
| `ai == Centipede` | CentipedeActor | 0x310 |
| `ai == Centibody` | CentibodyActor | 0x310 |
| `ai == Node` / `NodeWaiting` | NodeActor | 0x278 |
| `ai == Popup` | PopupActor | 0x310 |
| `ai == President` | PresidentActor | 0x240 |
| `ai == Turret` | TurretActor | 0x320 |

`Scavenger`, `Minebot`, `Reserved`, `Waiting`, `Pathfinder`, `Swarm` have no explicit
case and fall through `default` (so they become CharacterActor / MobileActor / plain
Actor depending on the character/projectile presence). Every spawn does `num_actors++`.

Note there is a **second, lighter actor factory** for the client/main thread,
`ClientSpawnActorForTeam` @ 0x004fce90, with different class sizes and its own id counter
(`DAT_007b68e4`); see `level_loading_notes.md`. On a listen host both run.

`Actor::Ctor` (0x0052d1f0) reads these Role fields when building the actor: `shape` /
`hierarchy` / `flags` bit7 (visual object), `armor_value`, `character->strength` /
`->alarm_delay` / `->size`, `interface_beam_delay` (folded into a starting-health
formula), and copies the `vulnerabilities` list. It also special-cases roles named
`blobarrel` / `oildrum` to health 0.

## 7. Pickup classification

`GetPickupType(role)` = `round(role->character->aggression * 10)`, or `2` if the role
has no character. `Character.aggression` is overloaded to carry the pickup type for
pickup roles. Observed:

- `4` -> weapon pickup (`GetWeaponPickupRole` matches this + `character->weapon`)
- `6` -> ammo pickup (`GetAmmoPickupRole` matches this + `character->weapon`)
- `2` -> default / non-pickup

## 8. Known defects in `ToRole` (relevant to the `gk::gls` builder)

1. **NULL write-through.** `ToRole` stores `ToCharacter`/`ToHierarchy` results and then
   dereferences them without a null check (`character->field... = 0`, hierarchy node
   walks). An incomplete sub-object (missing a required field) makes the converter return
   NULL and the game crashes. `gk::gls` gates on `isValidDeep` before converting for
   exactly this reason - fill every required field first.
2. **Leak on the beam-script error path.** If `interface beam effect == 4 (script)` but
   the script field is missing, `ToRole` prints a parse error and aborts *after*
   allocating the Role, skipping the cache store - the half-built Role leaks and a later
   call rebuilds.
3. **Uninitialised `pickup_radius`** in the inline InventoryInfo path (§4).

## 8.5 Building a `Role` without a `ParsedThing` (`src/MakeRole.cpp`)

`gk::MakeRole(const RoleDesc &)` is `ToRole` re-expressed over already-built sub-objects,
so a definition costs a few dozen bytes instead of a 0x1b60-byte parsed object. It calls
the game's own `CreateRole`, so the Role still lands in the entity hash with a fresh id.
Five things about `ToRole` that the rewrite had to reproduce and that are easy to miss:

- **GLS `shape` (0x05) is one field with three meanings**, dispatched on the *section type*
  of what it referenced: a `shape` fills `Role::shape`, a `pgenerator` fills `Role::pgen`
  (overwriting whatever field 0x07 put there), and anything else is treated as a hierarchy.
  Only the hierarchy branch resolves hotspots and counts nodes.
- **The hotspot names come from the hierarchy section, not the role.** `ToRole` reaches
  into `parsed[0x05]->parsed_values[0x03]` and `[0x04]`, strdups them, and resolves each to
  a point with `HierarchyResolveNamedPointPos` @ 0x00594890. `alternate_hotspot_point`
  starts as a *copy* of `hotspot_point`, so a hierarchy with only a `hotspot` gives both
  points the same value rather than leaving the second at the origin.
- **`InventoryInfo` is allocated lazily by six different fields** (0x06, 0x1a, 0x1b, 0x5d,
  0x5e and the shape branch), each re-checking whether one exists. Because `pickup radius`
  (0x5e) defaults to **6.0, not 0**, and the allocation is gated on `!= 0.0`, every role
  converted from GLS ends up with an `InventoryInfo` whether it is a pickup or not.
- **The character's collision extents are derived here, not in `ToCharacter`** - see
  `role_subobjects_notes.md` §1. A GLS `radius`/`height` of 0 means "use the model's
  bounding box".
- **A non-`TrackObject` role drops a chunk hanging off its shape at +0x8c** (release
  @ 0x00595c60, then a 0x18 pool free). Shared geometry only `TrackObject` keeps a private
  copy of.

`ParseRole`'s own defaults, recovered from its `.rdata` constants: `alpha` 1.0,
`ai` 4, `interface beam delay`/`duration` -1, `interface beam effect` 1 (`Confusion`),
`pickup radius` 6.0, `destroy after collection` and `status display` **true**, `recon ai
number` -1, `switch size` 100, everything else 0/false.

## 9. GkPlus surface (`src/Roles.cpp`, `require("gk.roles")`)

- `roles[id]` / `roles["name"]` -> `GetRoleById` / `GetRoleByName`, wrapped as a `Role`
  userdata; `pairs(roles)` walks all buckets.
- `Role` fields exposed: `id` (`->id`), `type` (`->ai`), `name`, `vulnerabilities`
  (table), and `spawn(team, pos, ori, owner_id)` -> `SpawnRole`.
- The C++ `Role`/`Character`/`Projectile`/`Light`/`ParticleGenerator`/`InventoryInfo`
  struct mirrors live in `src/Roles.cpp` with `static_assert`s on the key offsets and
  total size (0xc0).

## 10. Vulnerabilities

A `Vulnerability` (0x1c) says "when a **`role`** hits me with a **`vuln_role`**, apply
**`type`** after **`delay`**". The C++ mirror is `src/Vulnerability.cpp`.

| Off | Field | Type | Notes |
|-----|-------|------|-------|
| 0x00 | `entity` | Role* | the role that must deliver the hit (stock: `elint`) |
| 0x04 | `vuln_entity` | Role* | the role it must be delivered with (stock: `interface_beam`) |
| 0x08 | `delay` | int | ticks before it fires |
| 0x0c | `duration` | int | only read for `Confusion`/`Charm`, else -1 |
| 0x10 | `script` | char* | owned; NULL unless `type == Script` |
| 0x14 | `type` | VulnerabilityType | |
| 0x18 | `actor_scoped` | bool | byte-wide; 1 = attached to one Actor, 0 = inherited from a Role |

`VulnerabilityType`: `Shutdown` 0, `Confusion` 1, `Destroy` 2, `Charm` 3, `Script` 4 -
the keywords `SHUTDOWN` / `CONFUSE` / `DESTROY` / `CHARM` / *(script name)* that
`CommandVulnerability` (0x0044a600, console `VULNERABILITY`) parses. `Role::interface_beam_effect`
(0x84) holds the same enum.

Both `Role` (0x68) and `Actor` (0x10) carry a `List<Vulnerability*>` in the standard
16-byte `{sentinel, count, cached_array, cache_valid}` header; nodes are `VulnList`
(0x10 = `{vtbl, prev, next, vuln}`, vtbl `PTR_FUN_00652070`). **The sentinel is only 0xc
bytes** - `Actor::Ctor` mallocs 0xc for it - so it has no `vuln` field and must never be
dereferenced as a full node. Walk it the way the engine does, `for (cur = sentinel->next;
cur != sentinel; cur = cur->next)`.

Population paths:

- `Actor::Ctor` (0x0052d1f0) copies every node of `role->vulnerabilities` into the new
  actor's list. It copies the **pointer**, so a role's `Vulnerability` is shared by every
  actor spawned from it - editing one edits all of them.
- `AddInterfaceBeamVulnerability` (0x00510fe0, from `SpawnRole` /
  `ServerSpawnActorForTeam`) synthesises the stock elint-hack vuln from the role's four
  `interface beam` fields (delay<-0x80, duration<-0x8c, script<-0x88, type<-0x84), unless
  `interface_beam_delay < 0` or an elint/interface_beam vuln is already present.
- `CommandVulnerability` adds one by hand; given a role name it also fans the same
  `Vulnerability*` out to every live actor of that role.
- `ReadActorFixups` restores them on savegame load via `VulnList_AddEntryEnd` (0x0054f610).
  The insert helpers are `VulnList_AddEntryStart` (0x0044ea80) and that one; both take the
  list *header* in ECX and invalidate `cached_array`.

GkPlus exposes each entry as a `Vulnerability` userdata (`src/Vulnerability.h`) with
`role`, `vulnerability_role`, `delay`, `duration`, `script` (nil when absent), `type` and
`actor_scoped`, reachable as `Role.vulnerabilities` / `Actor.vulnerabilities`.
