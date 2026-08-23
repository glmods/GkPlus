#pragma once

#include "Field.h"
#include "HashTable.h"
#include "List.h"
#include "Math.h"
#include "Memory.h"
#include "Vulnerability.h"

#include <cstdint>

namespace gk {
struct Hierarchy;
struct Shape;
struct Role;

// The Role's AI class (Role.ai @ 0x7c). Selects the spawned Actor subclass in the game's
// CreateActor @ 0x00510760. Values verified against the game's AIType enum. Lowercase
// names are available via AITypeName / AITypeFromName below. See role_system_notes.md /
// role_subobjects_notes.md.
enum class AIType : int {
  Bot,
  Scavenger,
  Mine,
  Minebot,
  Reserved,
  Blocker,
  Waiting,
  Pathfinder,
  TrackObject,
  Tumbleweed,
  Pickup,
  BackgroundCreature,
  FlyingBackgroundCreature,
  Centipede,
  Centibody,
  Node,
  NodeWaiting,
  Swarm,
  Popup,
  President,
  Turret,
  Count,
};

/// The GLS `character` sub-object of a Role, 0xb8 bytes: perception, movement,
/// combat and the AI type that decides which Actor subclass the role spawns.
/// Built by `ToCharacter` @ 0x0047db80, which is also where the unit
/// conversions live; field by field in `role_subobjects_notes.md`.
///
/// Several fields are 16.16 fixed point stored in an `int` rather than floats;
/// see the comment on each.
struct Character {
  // 16.16 fixed point, and an **int** - not a float. ToCharacter @ 0x0047db80
  // stores it with FISTP and re-reads its own stored integer with FILD; every
  // reader in the binary does FILD then multiplies by 1/65536 (DAT_00652190):
  // MobileActor::Ctor @ 0x005325e1 (raw dword copy to MobileActor+0x178),
  // Inventory::AddItemFromRole @ 0x004e47b9, MobileActor::DropItem @ 0x00538452,
  // MobileActor::ReceiveObject @ 0x00539256, AiThink_Scavenger @ 0x00455c53.
  // There are no float readers. Typing it `float` here silently made MakeRole
  // write the IEEE bit pattern of the fixed-point value, which the game FILDs as
  // a nine-digit integer - see the note in MakeRole.cpp.
  int walking_speed;
  float turning_speed;
  float aim;
  float angular_scan_rate;
  float scan_delay;
  float scan_acceptance_angle;
  float mine_laying_time;
  bool latch_trigger;
  bool alertable;
  uint8_t field0x1e; // padding
  uint8_t field0x1f; // padding
  int generation_limit;
  float sight_angle;
  float sight_range;
  float sight_range_squared;
  float hearing_range;
  float hearing_range_squared;
  float alert_radius;
  float aggression;
  float gun_yaw_angle;
  float elevation_angle;
  // GLS `radius`/`height` pre-multiplied by `size`. A GLS value of 0 is not
  // "zero-sized": ToRole substitutes derived_radius / derived_height, i.e. the
  // geometry's own bounding box, so leaving them out is how a role gets its
  // collision extents from its model.
  float radius_times_size;
  float height_times_size;
  float size;
  float damage_multiplier;
  float shot_speed_multiplier;
  float target_cycle_delay;
  float alarm_delay;
  float weapon_cycle_time;
  float weapon_cycle_time2;
  int max_weapon;
  int max_ammo;
  int max_module;
  float initial_first_person_range;
  float maximum_first_person_range;
  bool can_turn;
  bool draw_vision_cone;
  bool draw_hearing_range;
  uint8_t field0x83; // padding
  // Refcounted, not pool-owned: CharacterDtor releases both through the shared
  // hierarchy release @ 0x00594b10.
  Hierarchy *customisation_hierarchy;
  Hierarchy *shadow_hierarchy;
  int blob_shadow;
  char *description; // NOT owned: ToCharacter takes it from GetResourceString
  // Derived geometry, NOT runtime scratch, and not set by ToCharacter - `ToRole`
  // fills all three after converting the character, from the role's hierarchy or
  // shape bounding box (or {1,1,1} when it has neither).
  float derived_radius; // 0x94 max(bbox.x, bbox.z) * 0.5
  float derived_height; // 0x98 bbox.y
  // 0x9c only when the hierarchy has more than one node: a value read out of
  // hierarchy+0x90 +0x14, multiplied by `size`. Zeroed by ToRole otherwise.
  int derived_hier_extent;
  // Normalized texture UVs, NOT integers: ToCharacter stores the parsed 0..1024
  // integer divided by 1024.0 with MOVSS (0x0047df06 / 0x0047df21). They were
  // typed `int` here and in the DB, which is what made that store decompile as a
  // nonsensical `(int)(x * 0.0009765625)`.
  float status_window_v;
  float status_window_u;
  float strength;
  int weapon;
  int secondary_weapon;
  bool always_cpu_controlled;
  uint8_t field0xb5;
  uint8_t field0xb6;
  uint8_t field0xb7;
};
static_assert(sizeof(Character) == 0xb8);

/// The GLS `light` sub-object of a Role, 0x1c bytes, the size `RoleDtor` and
/// `ProjectileDtor` pool-free, which is what pins it.
///
/// The engine's lights author a **black specular** and a diffuse well above 1
/// (`vulkan_renderer_notes.md` §4.48), so the three `specular_*` channels are
/// usually zero even on a light that visibly highlights.
struct Light {
  float red;
  float green;
  float blue;
  float specular_red;
  float specular_green;
  float specular_blue;
  float range;
};
static_assert(sizeof(Light) == 0x1c); // the size RoleDtor/ProjectileDtor pool-free

/// The GLS `projectile` sub-object of a Role, 0x20 bytes. Every shot in the
/// game builds an actor from one of these; there is no hitscan anywhere
/// (`combat_notes.md`).
struct Projectile { // GLS 'projectile'; see role_subobjects_notes.md
  bool gravity;         // 0x00 GLS 0x33
  uint8_t field0x1;     // padding
  uint8_t field0x2;     // padding
  uint8_t field0x3;     // padding
  float damage;         // 0x04 GLS 0x2a (negative heals)
  int sound;            // 0x08 GLS 0x20
  float max_range;      // 0x0c GLS 0x2d
  float blast_damage;   // 0x10 GLS 0x2c
  float blast_range;    // 0x14 GLS 0x28
  float blast_range_squared; // 0x18 = blast_range^2 (cached)
  // 0x1c GLS 0x1f (ToLight); ProjectileDtor @ 0x004adcc0 pool-frees it.
  pool_unique_ptr<Light> hit_light;
};
static_assert(sizeof(Projectile) == 0x20);

// ParticleGenerator::type, and the index into the 13-entry ParticleTypeInfos table
// (0x007c1964) that supplies every per-type default. Explosion is what the console
// parser returns for an unrecognised name.
//
// Two sources, agreeing where they overlap: the console keyword table in
// GetParticleIDFromName @ 0x0044c340, and the GLS lexer, read out with gls::ProbeKeywords
// (see gls_system_notes.md). The probe independently reproduced smoke/steam/fire/shot/
// explosion/big explosion/sparks and supplied Corona and LaserTrail, which the console
// table does not know. Only id 8 is still unnamed.
enum class ParticleType : int {
  Smoke = 0,
  Steam = 1,
  Snow = 2,
  Fire = 3,
  Shot = 4,
  Explosion = 5,
  BigExplosion = 6,
  Corona = 7,      // GLS `corona`; absent from the console table
  Trail = 9,
  LaserTrail = 10, // GLS `laser trail`; absent from the console table
  Rain = 11,
  Sparks = 12,
};

// Role::action_on_death, GLS field 0x5d. Recovered with gls::ProbeKeywords; 0 is the
// section default and has no keyword, so it means "unspecified" rather than a behaviour.
enum class ActionOnDeath : int {
  Unspecified = 0,
  MustDrop = 1,    // GLS `must drop`
  MustNotDrop = 2, // GLS `must not drop`
};

// Role::resistance, GLS field 0x4e. Recovered with gls::ProbeKeywords. Note the values
// step by 2 rather than being bit flags - 1, 3, 5 and 7 are unaccounted for, so this is
// NOT a mask and the four keywords are not combinable.
enum class Resistance : int {
  None = 0,
  Laser = 2,       // GLS `resists laser`
  Explosives = 4,  // GLS `resists explosives`
  Epulsar = 6,     // GLS `resists epulsar`
  SmallArms = 8,   // GLS `resists small arms`
};

// One particle animation channel, 0x18 bytes. The record starts at the *lead* dword, not
// at the vec4: ParticleEmitter_Ctor (0x00580510) ingests channels A and B with a single
// `MOVUPS xmm0, [tmpl+0x2c]` / `[tmpl+0x44]` plus a trailing MOVQ. `trail` is 2 from both
// constructors, but that same function tests bit 1 (zero v[3]) and bit 0 (zero byte 3 of
// `lead`) of channel A's copy, so it reads as a flags word rather than the keyframe count
// it was previously assumed to be.
struct PGenChannel {
  int lead;
  float v[4];
  unsigned trail;
};
static_assert(sizeof(PGenChannel) == 0x18);

// See role_subobjects_notes.md §3. Only ~15 fields come from GLS; the rest is emitter
// template state that ToParticleGenerator merely default-initialises, so its meaning comes
// from the consumer (ParticleEmitter_Ctor) and from ParticleTypeInfos[type]. No owned heap
// pointers.
struct ParticleGenerator {
  ParticleType type;    // 0x00 GLS 'type' 0x41 (0..12); indexes ParticleTypeInfos
  // Blend/render mode. ToParticleGenerator seeds it to 5, which ParticleEmitter_Ctor reads
  // as "take the default from ParticleTypeInfos[type]+0x20" - it is not an inert tag.
  int kind;             // 0x04
  int field0x8;         // 0x08 copied from parsed+0x1b60
  int field0xc;         // 0x0c copied from parsed+0x1b64 (not the lifespan; see 0xd0)
  float rate;           // 0x10 GLS 'rate' 0x43
  Vec3 coords;          // 0x14 GLS x/y/z 0x44-46
  Vec3 field0x20;       // 0x20 -> emitter+0xdc..0xe4
  PGenChannel colour;   // 0x2c v = GLS red/green/blue/alpha 0x21-0x24; -> emitter+0xa0
  PGenChannel channel_b;// 0x44 -> emitter+0xb8
  // Gate for attaching a **dynamic light** to the emitter. When set,
  // ParticleEmitter_Ctor calls SceneLightSet_AddDynamicLight @ 0x0057a040, which
  // builds a 0x6c-byte light record through its receiver's vtable slot 5.
  //
  // **It is a `__thiscall`, `RET 0x1c`, and the receiver is not one of these
  // arguments.** All six call sites load `ECX = [0x007c18cc]` - the scene
  // LightSet, the same object src/World.cpp's LightSet_SetEmissiveColour targets.
  // The seven stack arguments are
  // (&channel_c, &channel_d, &field0xb8, 5.5f, field0xa8, field0xac, field0xb0),
  // so binding this as a seven-argument free function would pass `channel_c`
  // where the callee wants `this` and then make a virtual call through it. This
  // comment described it as "the channel_c/channel_d feature" and omitted the
  // receiver entirely.
  //
  // ToParticleGenerator zeroes it (with 0x5d, in one word store), so GLS-built generators
  // never take that path - which is also why the three channels below stay uninitialised.
  bool use_channel_cd;  // 0x5c
  bool field0x5d;       // 0x5d -> emitter+0xd1
  short field0x5e;
  PGenChannel channel_c;// 0x60 lead not written by ToParticleGenerator
  PGenChannel channel_d;// 0x78 ditto
  PGenChannel channel_e;// 0x90 ditto
  float field0xa8;
  field field0xac;
  float field0xb0;      // 0xb0 default 4.0
  bool generate_generators; // 0xb4 GLS 0x68 -> emitter+0xd2
  bool field0xb5;       // 0xb5 -> emitter+0xd0; not written by ToParticleGenerator
  short field0xb6;
  Vec3 field0xb8;       // 0xb8 ctor'd/dtor'd; passed by address to SceneLightSet_AddDynamicLight
  float start_scale;    // 0xc4 GLS 'start scale' 0x64
  float end_scale;      // 0xc8 GLS 'end scale' 0x65
  float spin;           // 0xcc GLS 'spin' 0x66
  field lifespan_ticks; // 0xd0 GLS 'particle TTL' 0x67; 0 = use ParticleTypeInfos[type] TTL
};
static_assert(offsetof(ParticleGenerator, colour) == 0x2c);
static_assert(offsetof(ParticleGenerator, use_channel_cd) == 0x5c);
static_assert(offsetof(ParticleGenerator, channel_c) == 0x60);
static_assert(offsetof(ParticleGenerator, channel_e) == 0x90);
static_assert(offsetof(ParticleGenerator, generate_generators) == 0xb4);
static_assert(offsetof(ParticleGenerator, start_scale) == 0xc4);
static_assert(sizeof(ParticleGenerator) == 0xd4);

// Death-behaviour records. Three variants share a dtor-only base vtable; the death/frag
// handler (Frag @ 0x0052e220) dispatches on `tag` at +0x04. See role_subobjects_notes.md.
// The tag doubles as the explode/splatter type for the base variant.
enum class DestructibilityKind : int {
  Explode = 0,
  Splatter = 1,
  FragData = 3,
  ReplaceDestructibility = 4,
};

/// What happens when a Role's actor dies: the base variant of a three-way
/// family, 0x8 bytes, carrying nothing but the tag. `Frag` @ 0x0052e220
/// dispatches on that tag, so the variant is chosen by value and not by
/// vtable, so read `tag` before casting to FragData or ReplaceDestructibility.
struct Destructibility {
  virtual ~Destructibility() = 0;
  DestructibilityKind tag; // Explode/Splatter for this base variant; else FragData/ReplaceDestructibility
};

/// The `DestructibilityKind::FragData` variant, 0x24 bytes: break into pieces,
/// optionally spawning a replacement Role in place.
struct FragData { // GLS `frag data`
  virtual ~FragData() = 0;
  DestructibilityKind tag; // = FragData
  // Borrowed: both point at Roles living in the entity hash, which owns them.
  Role *role;         // GLS 'role' 0x60 - fragment pieces
  Role *replace_role; // GLS 'replace role' 0x61 - actor spawned in place at death
  pool_string remove; // GLS 'remove' 0x62
  int scale;          // GLS 'scale' 0x63
  bool replace;       // GLS 'replace' 0x69
  bool symmetric;     // GLS 'symmetric' 0x6a
  uint8_t pad[2];
  float blast_range;  // GLS 'blast range' 0x28
  float blast_damage; // GLS 'blast damage' 0x2c
};
static_assert(sizeof(FragData) == 0x24);

/// The `DestructibilityKind::ReplaceDestructibility` variant, 0x10 bytes:
/// "run a script when the object dies".
struct ReplaceDestructibility {
  virtual ~ReplaceDestructibility() = 0;
  DestructibilityKind tag; // = ReplaceDestructibility
  // A **script file name**, not an identifier, despite the GLS keyword. Its only
  // reader is Frag @ 0x0052e220 `case 4`, which hands it to QueueScriptExecution
  // and then reads `replace` - so this variant is "run a script when the object
  // dies". Was modelled as `name` after the GLS keyword until that case block was
  // read; ToReplaceDestructibility @ 0x0047eaa0 strdups it from GLS field 0x00.
  pool_string script; // GLS 'name' 0x00
  bool replace;       // GLS 'replace' 0x69
  uint8_t pad[3];
};
static_assert(sizeof(ReplaceDestructibility) == 0x10);

// GLS `ammo`, filled by ToAmmo @ 0x0047d740. Both strings are strdup'd onto the
// game pool; `role` is borrowed from the entity hash.
struct Ammo {
  float round_time;    // 0x00 GLS 0x14
  float reload_time;   // 0x04 GLS 0x15
  int life_timer;      // 0x08 GLS 0x16
  int magazine_size;   // 0x0c GLS 0x12
  int sound;           // 0x10 GLS 0x20
  int salvo_size;      // 0x14 GLS 0x13
  pool_string file;    // 0x18 GLS 0x01
  pool_string name;    // 0x1c GLS 0x00
  // GLS field 0x0b, which the master table calls `projectile` - for an `ammo`
  // section it is converted with ToRole, so this really is a Role.
  Role *role;          // 0x20
  float firing_speed;  // 0x24 GLS 0x2e
};
static_assert(sizeof(Ammo) == 0x28);

// One entry of AmmoInfos, filled by ToAmmoInfo @ 0x0047d8f0. The three ints are
// GL_RESOURCE_IDs, not strings.
struct AmmoInfo {
  Hierarchy *hierarchy; // 0x00 set when GLS `shape` 0x05 named a hierarchy
  Shape *shape;         // 0x04 ... or a shape; never both
  int ammo_name;        // 0x08 GLS 0x1c
  int description;      // 0x0c GLS 0x1a
  int max_per_slot;     // 0x10 GLS 0x1d
};
static_assert(sizeof(AmmoInfo) == 0x14);

// Ammo type / weapon type bounds, straight off ParseAmmo's declared max_values:
// `ammo type` 0x17 tops out at 19 and `weapon type` 0x18 at 33.
inline constexpr int AmmoTypeCount = 19; // the stride of the Ammo table below
inline constexpr int MaxAmmoType = 19;
inline constexpr int MaxWeaponType = 33;

/// What a Role looks like and reads as when it is sitting in an inventory: the
/// held model, its shape, and the localized description. Assembled by `ToRole`
/// and torn down by the dtor @ 0x004add40.
struct InventoryInfo {
  // Refcounted, not pool-owned: the InventoryInfo dtor @ 0x004add40 releases
  // these through the hierarchy/shape release functions (0x00594b10 / 0x00599110).
  Hierarchy *hierarchy;
  Shape *shape;
  // NOT owned: ToRole assigns both from GetResourceString.
  char *description;
  char *pickup_name;
  float pickup_radius;
  int action_on_death;
};
static_assert(sizeof(InventoryInfo) == 0x18); // the size RoleDtor pool-frees

// The pool_unique_ptr members below are exactly what RoleDtor @ 0x004ada50
// releases; every other pointer here is refcounted, borrowed or (in one case)
// leaked, as noted per field.
struct Role {
  pool_string name;
  // NOT owned: ToRole fills all four from GetResourceString, so they point into
  // the localized string table in the active glres*.dll.
  char *recon_name;
  char *recon_ai_short;
  int recon_ai_number;
  char *recon_ai_long;
  char *recon_ai_long2;
  // Refcounted, not pool-owned (releases @ 0x00599110 / 0x00594b10).
  Shape *shape;
  Hierarchy *hierarchy;
  // ParticleGenDtor @ 0x004af190 is called with the "free" flag, so both are
  // pool-owned despite going through a scalar-deleting dtor.
  pool_unique_ptr<ParticleGenerator> pgen;
  pool_unique_ptr<ParticleGenerator> pgen2;
  pool_string meta_sound;
  // Resolved positions of the hotspot / alternate-hotspot nodes within the role's
  // hierarchy (see HierarchyResolveNamedPointPos @ 0x00594890). Zero for shape/pgen roles.
  Vec3 hotspot_point;           // 0x2c
  Vec3 alternate_hotspot_point; // 0x38
  pool_string hotspot;           // 0x44 hotspot node name (strdup'd by ToRole)
  pool_string alternate_hotspot; // 0x48 alternate hotspot node name
  // Counts of present hierarchy nodes in slot ranges 26..30 / 21..25 (ToRole).
  int num_hier_nodes_26_30;     // 0x4c
  int num_hier_nodes_21_25;     // 0x50
  int limit;
  pool_unique_ptr<Light> light;
  pool_unique_ptr<Projectile> projectile;
  pool_unique_ptr<Character> character;
  pool_unique_ptr<InventoryInfo> inventory_info;
  // Entries are added outside ToRole (by the vulnerability-processing path).
  // The sentinel is pool_alloc'd but never released - RoleDtor drains the nodes
  // and leaves the head behind - so it is deliberately not a pool_unique_ptr.
  VulnerabilityList vulnerabilities; // 0x68
  bool alpha_fogging : 1;
  bool per_vertex_fogging : 1;
  bool no_lighting : 1;
  bool reflective : 1;
  bool destination_selectable : 1;
  bool destroy_after_collection : 1;
  bool hit_test_ignore : 1;
  bool frag_control : 1;
  bool moves_on_lifts : 1;
  bool status_display : 1;
  AIType ai;
  // The four `interface beam` GLS fields are copied verbatim into the delay /
  // type / script / duration of the synthesised elint-vs-interface_beam
  // Vulnerability by AddInterfaceBeamVulnerability @ 0x00510fe0.
  int interface_beam_delay;                  // 0x80
  VulnerabilityType interface_beam_effect;   // 0x84
  // Allocated by ToRole but absent from RoleDtor's free list - leaked, so it is
  // owned in practice yet has no release to attach a pool_unique_ptr to.
  char *interface_beam_script;               // 0x88
  int interface_beam_duration;               // 0x8c
  int resistance;
  float resistance_factor;
  float armor;
  float shields;
  float recharge_rate;
  float alpha;
  // All three variants' scalar-deleting dtors end in a pool free (0x8 / 0x24 /
  // 0x10), so the virtual dispatch does not change who owns the storage.
  pool_unique_ptr<Destructibility> destructibility;
  // Body-part/attachment name strings, split from the "sever point" GLS field
  // on ','. Uses trigger-style nodes, so RoleDtor cleans it with
  // TriggerList::DeleteTriggers. As with `vulnerabilities`, the head is never freed.
  List<pool_string> sever_points; // 0xac entries are pool-freed by RoleDtor
  int id;
};
static_assert(offsetof(Role, hotspot) == 0x44);
static_assert(offsetof(Role, ai) == 0x7c);
static_assert(offsetof(Role, id) == 0xbc);
static_assert(offsetof(Role, vulnerabilities) == 0x68);
static_assert(sizeof(Role) == 0xc0);

// The entity hash @ 0x007b48f0. Same field layout as the AvP hash table but with
// no vtable: nothing takes the table's address (no instruction in .text so much as
// mentions 0x007b48ec), every operation is inlined into CreateRole /
// GetRoleByName / GetRoleById / DestroyRoles as direct global accesses.
using Roles = HashTableBase<Role *>;
using RoleNode = Roles::Node;
static_assert(sizeof(Roles) == 0x10);
static_assert(sizeof(RoleNode) == 0x8);
// Standard-layout without the vtable, so these cost no warning - and they are what
// distinguishes this table from the Actors one, whose fields all sit 4 higher.
static_assert(offsetof(Roles, n_entries) == 0x00);
static_assert(offsetof(Roles, table_size_mask) == 0x08);
static_assert(offsetof(Roles, chains) == 0x0c);

// --- Native API over the game's role table ----------------------------------

// The global role/entity hash @ 0x007b48f0. Iterate with begin()/end().
Roles *GetRolesTable();

/// The Role registered under \p name, or nullptr. The name is the GLS
/// identifier; the table owns the Role, so the pointer is borrowed and stays
/// valid until `DestroyRoles`.
Role *GetRoleByName(const char *name); // 0x004ae030
/// The Role with \p id, or nullptr. Borrowed, as above.
Role *GetRoleById(int id);             // 0x004ae0d0

// SpawnRole @ 0x00503710 -> spawned actor id.
int SpawnRole(int team_id, Role *role, Vec3 *position, Vec4 *orientation,
              int owner_id);

// Lowercase name for an AIType (e.g. AIType::TrackObject -> "track_object"), or
// nullptr if out of range; and the inverse (AIType::Count if the name is unknown).
const char *AITypeName(AIType type);
/// The AIType \p name stands for, or AIType::Count when the name is unknown.
AIType AITypeFromName(const char *name);

// --- GLS enum keyword tables --------------------------------------------------
//
// The keywords for these fields are compiled into the parser's flex DFA, not
// stored as strings, so none of this could be read out of the binary. Every entry
// below was recovered by handing the parser a one-field section per keyword and
// reading the stored integer back (gls::ProbeKeywords, see gls_system_notes.md).
//
// Two independent checks passed on the way: `ai` reproduced all 21 values of
// AIType in order, and destructibility `type` reproduced DestructibilityKind.
//
// Names are the GLS spelling, spaces and all - `gk::gls` matches them with '_'
// and ' ' treated as the same character, so a script may write either.
struct EnumEntry {
  const char *name;
  int value;
};

// GLS `weapon type` (an `ammo` section, bounded 0..33) and `weapon` /
// `secondary weapon` (a `character` section, where ParseCharacter declares NO
// upper bound). Same field id 0x18 and the same numbering as far as it goes, so
// one table serves all three - but a character's is a wider *inventory item*
// space whose entries above 33 (the ammo/gadget names: `audio cloak`,
// `lock decoder`, `terrain scanner`, ...) are not yet recovered.
//
// 10 and 15 are unaccounted for - no shipped script names them - and 33 is the
// section default meaning "none", which has no keyword: `weapon type none` is a
// syntax error, and the default is reachable only by omitting the field.
const EnumEntry *WeaponTypeNames(size_t *count);
/// The GLS keyword for a weapon type, or nullptr when \p value is not one of
/// the recovered entries, which includes 10, 15 and everything above 33.
const char *WeaponTypeName(int value);   // nullptr when unknown
/// The weapon type \p name stands for, or -1 when the name is unknown.
int WeaponTypeFromName(const char *name); // -1 when unknown

// GLS `ammo type` (0x17). Complete: 0..18, bounded 0..19 by the constructor.
const EnumEntry *AmmoTypeNames(size_t *count);
/// The GLS keyword for an ammo type, or nullptr when \p value is not one.
const char *AmmoTypeName(int value);
/// The ammo type \p name stands for, or -1 when the name is unknown.
int AmmoTypeFromName(const char *name);

// The two small role enums above, by name.
const char *ActionOnDeathName(ActionOnDeath value);
/// The ActionOnDeath \p name stands for, or ActionOnDeath::Unspecified when
/// the name is unknown. An unknown name is not distinguishable from the
/// keyword for Unspecified.
ActionOnDeath ActionOnDeathFromName(const char *name); // Unspecified when unknown
/// The GLS keyword for a Resistance value.
const char *ResistanceName(Resistance value);
/// The Resistance \p name stands for, or Resistance::None when unknown.
Resistance ResistanceFromName(const char *name); // None when unknown
// GLS `type` in a pgenerator (0x41), bounded 0..12 - the 13 ParticleTypeInfos.
const char *ParticleTypeName(ParticleType value);
/// The ParticleType \p name stands for, or ParticleType::Explosion when
/// unknown, which is also the keyword `explosion`'s own value.
ParticleType ParticleTypeFromName(const char *name); // Explosion when unknown

// AmmoInfos @ 0x007b5d40, indexed by ammo type.
AmmoInfo *GetAmmoInfos();
// The Ammo* table @ 0x007b5ec0, indexed `ammo_type + weapon_type * AmmoTypeCount`.
//
// It sits 0x180 bytes past AmmoInfos, which is only 0x17c bytes of AmmoInfo[19] -
// so an ammo type of exactly 19 (which ParseAmmo's max_values permits) writes an
// AmmoInfo that laps into the first slots of this table. No shipped script does,
// but that is the game's bound, not a safe one.
Ammo **GetAmmoTable();
/// The Ammo entry for one (\p ammo_type, \p weapon_type) pair, i.e. the game's own
/// compatibility test, since an incompatible pair has no entry. Borrowed;
/// nullptr for an empty slot or an index outside the table.
Ammo *GetAmmo(int ammo_type, int weapon_type);
} // namespace gk
