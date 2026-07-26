#pragma once

#include "Math.h"
#include "Roles.h"

#include <cstdint>

// Native constructors for the role sub-objects, replacing the ToXxx converters'
// dependency on a parsed GLS object.
//
// The game has no constructor that takes a description: CreateRole @ 0x004add90
// is 149 instructions of pool_alloc, null-the-pointers and hash-insert, and the
// only thing that turns a description into a filled Character / Light /
// Projectile / Role is the ToXxx family, whose parameter type is ParsedThing.
// These functions are those converters re-expressed over a plain struct, so a
// definition costs a few dozen bytes instead of a 0x1b60-byte ParsedThing and
// nothing has to go through CheckValue.
//
// The values are in .gls units throughout - degrees, seconds, metres, animation
// cycles per second - because that is what the parsed object holds: CheckValue
// only range-checks and stores, and every conversion happens here, in the
// converter. Each Desc's defaults are the section constructor's own, read out of
// its .rdata constants, so a default-constructed Desc is exactly what an empty
// `character { }` block would parse to.
//
// See role_subobjects_notes.md for the target layouts and gls_system_notes.md
// for the field tables these mirror.

namespace gk {
struct Hierarchy;

// --- character ----------------------------------------------------------------

// A GLS `character` section. Defaults from ParseCharacter @ 0x004821b0.
//
// The eight fields with no meaningful default are the ones GLS marks required;
// leaving them at zero is legal here (nothing validates) but produces a character
// that cannot see, hear, move or take damage.
struct CharacterDesc {
  // required in GLS
  double walking_speed = 0;  // 0x0c animation cycles per second
  double turning_speed = 0;  // 0x0d revolutions per second
  double strength = 0;       // 0x29 hit points
  double aim = 0;            // 0x34 degrees
  double sight_angle = 0;    // 0x35 degrees, 0..89
  double sight_range = 0;    // 0x38 metres
  double hearing_range = 0;  // 0x39 metres
  double aggression = 0;     // 0x3b 0..1.01

  // optional, with the section's own defaults
  double scan_delay = 0.5;             // 0x0e seconds
  double scan_acceptance_angle = 20.0; // 0x0f degrees, 0..360
  double angular_scan_rate = 50.0;     // 0x10 degrees per second
  double mine_laying_time = 8.0;       // 0x11 seconds
  double damage_multiplier = 1.0;      // 0x2b
  double shot_speed_multiplier = 1.0;  // 0x2e
  double target_cycle_time = 3.0;      // 0x2f seconds
  double weapon_cycle_time = 0;        // 0x30
  double weapon_cycle_time2 = 0;       // 0x31
  double alarm_delay = 0;              // 0x32
  double gun_yaw_angle = 60.0;         // 0x36 degrees, 0..180
  double elevation_angle = 80.0;       // 0x37 degrees, 0..180
  double alert_radius = 10.0;          // 0x3a
  double radius = 0;                   // 0x6b stored pre-multiplied by size
  double height = 0;                   // 0x6c stored pre-multiplied by size
  double size = 1.0;                   // 0x6d
  double initial_first_person_range = 5.0; // 0x86 5..100
  double maximum_first_person_range = 5.0; // 0x87

  int32_t weapon = 33;           // 0x18 33 = none
  int32_t secondary_weapon = 33; // 0x19
  int32_t description = 0;       // 0x1a a GL_RESOURCE_ID; 0 = none
  int32_t status_window_u = 0;   // 0x3f 0..1024, stored as u/1024
  int32_t status_window_v = 0;   // 0x40 0..1024, stored as v/1024
  int32_t blob_shadow = 0;       // 0x5a 0..1
  int32_t generation_limit = 5;  // 0x7b 1..10
  int32_t max_weapon = 0;        // 0x83
  int32_t max_ammo = 0;          // 0x84
  int32_t max_module = 0;        // 0x85

  bool can_turn = true;              // 0x3c
  bool draw_vision_cone = true;      // 0x3d
  bool draw_hearing_range = true;    // 0x3e
  bool always_cpu_controlled = false; // 0x74
  bool alertable = true;             // 0x7c
  bool latch_trigger = false;        // 0x7d

  // ToCharacter reaches these through ToHierarchy on a Custom field; here they
  // are already-converted objects, refcounted and borrowed exactly as the
  // converter leaves them (CharacterDtor releases both).
  Hierarchy *customisation_hierarchy = nullptr; // 0x76
  Hierarchy *shadow_hierarchy = nullptr;        // 0x77
};

// Builds a Character (0xb8) on the game's pool, applying every conversion
// ToCharacter @ 0x0047db80 applies. Never null unless the pool is exhausted.
Character *MakeCharacter(const CharacterDesc &desc);

// --- shape and hierarchy --------------------------------------------------------
//
// Not constructed at all: `shape` and `hierarchy` sections are pure .rif lookups,
// and both converters are 14 instructions around a single call. The returned
// objects are owned by the rif cache, not by the caller.

// GetShape(file, name) - ToShape @ 0x0047c260.
Shape *MakeShape(const char *rif_file, const char *object_name);
// GetHierarchy(file, name) - ToHierarchy @ 0x0047c390.
Hierarchy *MakeHierarchy(const char *rif_file, const char *object_name);

// --- light ----------------------------------------------------------------------

// A GLS `light`. ToLight @ 0x0047e220 applies no conversion at all - it is seven
// doubles narrowed to float.
struct LightDesc {
  double red = 0;            // 0x21
  double green = 0;          // 0x22
  double blue = 0;           // 0x23
  double specular_red = 0;   // 0x25
  double specular_green = 0; // 0x26
  double specular_blue = 0;  // 0x27
  double range = 0;          // 0x28
};
Light *MakeLight(const LightDesc &desc);

// --- projectile -------------------------------------------------------------------

// A GLS `projectile`. ToProjectile @ 0x0047e4e0 caches blast_range squared and
// converts its `hit light` sub-object; nothing else is transformed.
struct ProjectileDesc {
  bool gravity = false;   // 0x33
  double damage = 0;      // 0x2a negative heals
  int32_t sound = 0;      // 0x20
  double max_range = 0;   // 0x2d
  double blast_damage = 0; // 0x2c
  double blast_range = 0;  // 0x28
  // ToProjectile builds this from a nested `light` section; here it is already a
  // Light, and the Projectile takes ownership (ProjectileDtor pool-frees it).
  Light *hit_light = nullptr; // 0x1f
};
Projectile *MakeProjectile(const ProjectileDesc &desc);

// --- particle generator -------------------------------------------------------------

// A GLS `pgenerator`. Only the fields below come from the script; the rest of the
// 0xd4-byte object is emitter-template state ToParticleGenerator default-initialises,
// and MakeParticleGenerator seeds it identically.
struct ParticleGeneratorDesc {
  ParticleType type = ParticleType::Smoke; // 0x41
  double rate = 0;                         // 0x43
  Vec3 coords{0, 0, 0};                    // 0x44/0x45/0x46
  double red = 0, green = 0, blue = 0, alpha = 0; // 0x21..0x24
  bool generate_generators = false;        // 0x68
  double start_scale = 0;                  // 0x64
  double end_scale = 0;                    // 0x65
  double spin = 0;                         // 0x66
  // GLS `particle TTL` 0x67, in seconds. Stored as ticks at the *calling thread's*
  // clock rate, which is why this is converted here and not at definition time.
  double particle_ttl_seconds = 0;         // 0x67
  // GLS `life`, which is field id 0x42 - absent from the master field table because
  // it bypasses CheckValue entirely: a pgenerator ParsedThing is 0x1b70, and the
  // parser stores `life` as two raw dwords in that extension. ToParticleGenerator
  // copies both, unconverted, to ParticleGenerator +0x08/+0x0c.
  //
  // Scripts write `life infinite` or `life 5 seconds`. The encoding of `infinite`
  // has NOT been recovered, so this is exposed as the raw pair: read it off a
  // parsed generator if you need to match one.
  int32_t life_low = 0;  // -> +0x08
  int32_t life_high = 0; // -> +0x0c
};
ParticleGenerator *MakeParticleGenerator(const ParticleGeneratorDesc &desc);

// --- destructibility ----------------------------------------------------------------
//
// Three variants sharing a dtor-only base vtable, dispatched by Frag @ 0x0052e220 on
// the `tag` at +0x04. Each Make* reproduces one of the three slot-8 converters, and
// each returns the family's base type - cast on `tag`.

// `destructibility { type explode|splatter }` - ToDestructibility @ 0x0047e680.
Destructibility *MakeDestructibility(DestructibilityKind type);

// `frag data { ... }` - ToFragData @ 0x0047e890.
struct FragDataDesc {
  Role *role = nullptr;         // 0x60 the fragment pieces; required
  Role *replace_role = nullptr; // 0x61 spawned in place at death
  const char *remove = nullptr; // 0x62 strdup'd onto the game heap
  int32_t scale = 0;            // 0x63
  bool replace = false;         // 0x69
  bool symmetric = false;       // 0x6a
  double blast_range = 0;       // 0x28
  double blast_damage = 0;      // 0x2c
};
FragData *MakeFragData(const FragDataDesc &desc);

// The `name` + `replace` destructibility variant - ToReplaceDestructibility
// @ 0x0047eaa0. `name` is strdup'd onto the game heap.
ReplaceDestructibility *MakeReplaceDestructibility(const char *name, bool replace);

// --- role -----------------------------------------------------------------------
//
// The orchestrator. Everything above feeds into this; MakeRole reproduces the
// 774 instructions of ToRole @ 0x0047cc20 over already-built sub-objects instead
// of parsed ones. Defaults are ParseRole @ 0x00482bb0's own.
struct RoleDesc {
  // GLS `identifier` 0x47 - this, NOT `name` 0x00, is what becomes Role::name and
  // what GetRoleByName looks up.
  const char *identifier = nullptr;

  // GLS `shape` 0x05 is one field with three meanings, dispatched on the section
  // type of whatever it referred to. Set exactly one; a hierarchy is what enables
  // hotspot resolution and the node counts.
  Shape *shape = nullptr;
  Hierarchy *hierarchy = nullptr;
  // Hotspot node names. In GLS these live on the *hierarchy section* (fields 0x03
  // and 0x04), not on the role, and ToRole reaches through to them - so they are
  // role-level here, where there is no section to reach through to.
  const char *hotspot = nullptr;
  const char *alternate_hotspot = nullptr;

  ParticleGenerator *pgen = nullptr;  // 0x07 (or 0x05, if that named a pgenerator)
  ParticleGenerator *pgen2 = nullptr; // 0x08

  // GLS `inventory shape` 0x06 - again either kind.
  Shape *inventory_shape = nullptr;
  Hierarchy *inventory_hierarchy = nullptr;
  int32_t description = 0;      // 0x1a GL_RESOURCE_ID, 0 = none
  int32_t pickup_name = 0;      // 0x1b GL_RESOURCE_ID, 0 = none
  int32_t action_on_death = 0;  // 0x5d
  double pickup_radius = 6.0;   // 0x5e; any non-zero value forces an InventoryInfo

  int32_t recon_name = 0;       // 0x7e GL_RESOURCE_IDs
  int32_t recon_ai_short = 0;   // 0x7f
  int32_t recon_ai_number = -1; // 0x80
  int32_t recon_ai_long = 0;    // 0x81
  int32_t recon_ai_long2 = 0;   // 0x82

  Light *light = nullptr;                     // 0x1e
  Projectile *projectile = nullptr;           // 0x0b
  Character *character = nullptr;             // 0x0a
  Destructibility *destructibility = nullptr; // 0x59 any of the three variants

  // GLS `sever point` 0x48: one comma-separated string, split into a list of
  // node names. Passed here in the same form.
  const char *sever_points = nullptr;
  const char *meta_sound = nullptr; // 0x88

  AIType ai = AIType::Bot;             // 0x49 - ParseRole's default is 4
  int32_t interface_beam_delay = -1;   // 0x4a
  VulnerabilityType interface_beam_effect = VulnerabilityType::Confusion; // 0x4b
  // 0x4c - required when interface_beam_effect is Script, and ignored otherwise.
  const char *interface_beam_script = nullptr;
  int32_t interface_beam_duration = -1; // 0x4d
  int32_t resistance = 0;               // 0x4e
  double resistance_factor = 0;         // 0x4f
  int32_t limit = 0;                    // 0x78
  double alpha = 1.0;                   // 0x24
  double armor = 0;                     // 0x5b
  double shields = 0;                   // 0x5c
  double recharge_rate = 0;             // 0x5f

  // The ten packed booleans of Role::flags @ 0x78, in bit order.
  bool alpha_fogging = false;            // 0x56 bit 0
  bool per_vertex_fogging = false;       // 0x58 bit 1 - forced off by alpha_fogging
  bool no_lighting = false;              // 0x73 bit 2
  bool reflective = false;               // 0x57 bit 3
  bool destination_selectable = false;   // 0x6e bit 4
  bool destroy_after_collection = true;  // 0x6f bit 5
  bool hit_test_ignore = false;          // 0x72 bit 6
  bool frag_control = false;             // 0x75 bit 7
  bool moves_on_lifts = false;           // 0x70 bit 8
  bool status_display = true;            // 0x71 bit 9
};

// Builds a Role (0xc0) through the game's own CreateRole - so it lands in the
// entity hash with a fresh id and GetRoleByName can find it - and then fills it
// exactly as ToRole does. Null only if CreateRole fails.
//
// Ownership follows ToRole: the Role takes over pgen, pgen2, light, projectile,
// character and destructibility (RoleDtor pool-frees all six), while shape and
// hierarchy stay refcounted elsewhere.
//
// Vulnerabilities are deliberately absent - ToRole does not populate that list
// either; it is filled afterwards, from `resistance` and by
// AddInterfaceBeamVulnerability.
Role *MakeRole(const RoleDesc &desc);

// --- ammo and ammo info ------------------------------------------------------------
//
// Unlike everything above, these two do not return an object: they write into the
// global tables the weapon code reads. `ToAmmo` and `ToAmmoInfo` are the only
// writers, and both are idempotent-ish - see the notes on each.

// A GLS `ammo` section. Fills the Ammo* slot at [ammo_type + weapon_type * 19],
// mirroring ToAmmo @ 0x0047d740.
struct AmmoDesc {
  int32_t ammo_type = 0;    // 0x17 0..19
  int32_t weapon_type = 0;  // 0x18 0..33
  const char *name = nullptr; // 0x00 strdup'd
  const char *file = nullptr; // 0x01 strdup'd
  double round_time = 0;    // 0x14
  double reload_time = 0;   // 0x15
  int32_t life_timer = 0;   // 0x16
  int32_t magazine_size = 0; // 0x12
  int32_t salvo_size = 0;   // 0x13
  int32_t sound = 0;        // 0x20
  double firing_speed = 1.0; // 0x2e
  Role *role = nullptr;     // 0x0b - ToAmmo converts a role here, not a projectile
};
// False when the slot is already occupied: ToAmmo only writes an empty one, so
// the first definition of an (ammo, weapon) pair wins and later ones are dropped
// silently. Also false for out-of-range indices.
bool MakeAmmo(const AmmoDesc &desc);

// A GLS `ammo info` section - fills AmmoInfos[ammo_type]. Set exactly one of
// shape / hierarchy, as ToAmmoInfo @ 0x0047d8f0 does by dispatching on the
// section type it found.
struct AmmoInfoDesc {
  int32_t ammo_type = 0;      // 0x17 0..19
  Shape *shape = nullptr;     // 0x05 when it named a shape
  Hierarchy *hierarchy = nullptr; // 0x05 when it named a hierarchy
  int32_t ammo_name = 0;      // 0x1c GL_RESOURCE_ID
  int32_t description = 0;    // 0x1a GL_RESOURCE_ID
  int32_t max_per_slot = 0;   // 0x1d
};
bool MakeAmmoInfo(const AmmoInfoDesc &desc);

// --- camera track --------------------------------------------------------------------

// A GLS `camera track`. Unlike every other converter this one needs a *loaded
// level*: it binds itself by name against the level .rif and offsets by the map
// origin, so it is only meaningful from inside a level load (a level module's
// `define`/`populate`, or the ToRole-equivalent window).
//
// Returns false when there is no level rif, or when the named track is not in it -
// in which case the object is destroyed again, exactly as the converter does.
struct CameraTrackDesc {
  const char *name = nullptr;         // 0x00 the track's object name in the rif
  ParticleGenerator *pgen = nullptr;  // 0x07
  ParticleGenerator *pgen2 = nullptr; // 0x08
};
bool MakeCameraTrack(const CameraTrackDesc &desc);

// --- the conversions, exposed because they are the whole point -----------------
//
// Angles are stored in BAM - 4096 units to a turn, which is what indexes the
// engine's SinTable/CosTable. ToCharacter converts with two different
// association orders and this reproduces both, because they are not always
// bit-identical: `scan acceptance angle` and `angular scan rate` divide by 360
// first, while `aim`, `sight angle`, `gun yaw angle` and `elevation angle`
// multiply by 4096 first.
float DegreesToBamDivFirst(double degrees); // (d / 360) * 4096
float DegreesToBamMulFirst(double degrees); // (d * 4096) / 360
// Revolutions per second straight to BAM per second - no /360, the input is
// already whole turns.
float RevolutionsToBam(double revolutions);
// Animation cycles per second as the 16.16 fixed point the animator reads,
// rounded to nearest-even (the game uses FISTP with the default control word).
float ToFixed16(double cycles_per_second);
} // namespace gk
