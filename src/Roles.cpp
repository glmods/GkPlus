#include <cassert>

#include "Core.h"
#include "HashTable.h"
#include "List.h"
#include "LuaEngine.h"
#include "Math.h"
#include "Memory.h"
#include "Roles.h"
#include "Vulnerability.h"

#include <cstdint>

namespace gk {
union field {
  int integer;
  float flt;
  void *ptr;
  char *str;
};

struct Hierarchy;
struct Shape;

struct Character {
  float walking_speed;
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
  float radius_times_size;
  float heigth_times_size;
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
  field field0x94;  // runtime scratch - not set by ToCharacter
  field field0x98;  // runtime scratch - not set by ToCharacter
  int field00x9c;   // runtime scratch - not set by ToCharacter
  int status_window_v;
  int status_window_u;
  float strength;
  int weapon;
  int secondary_weapon;
  bool always_cpu_controlled;
  uint8_t field0xb5;
  uint8_t field0xb6;
  uint8_t field0xb7;
};
static_assert(sizeof(Character) == 0xb8);

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
// (0x007c1964) that supplies every per-type default. Names come from the console keyword
// table in GetParticleIDFromName @ 0x0044c340; 7, 8 and 10 have no keyword and are not yet
// identified. Explosion is also what that parser returns for an unrecognised name.
enum class ParticleType : int {
  Smoke = 0,
  Steam = 1,
  Snow = 2,
  Fire = 3,
  Shot = 4,
  Explosion = 5,
  BigExplosion = 6,
  Trail = 9,
  Rain = 11,
  Sparks = 12,
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
  // Gate for the channel_c/channel_d feature: when set, ParticleEmitter_Ctor calls
  // FUN_0057a040(&channel_c, &channel_d, &field0xb8, 5.5f, field0xa8, field0xac, field0xb0).
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
  Vec3 field0xb8;       // 0xb8 ctor'd/dtor'd; passed by address to FUN_0057a040
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

struct Destructibility {
  virtual ~Destructibility() = 0;
  DestructibilityKind tag; // Explode/Splatter for this base variant; else FragData/ReplaceDestructibility
};

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

struct ReplaceDestructibility {
  virtual ~ReplaceDestructibility() = 0;
  DestructibilityKind tag; // = ReplaceDestructibility
  pool_string name; // GLS 'name' 0x00
  bool replace;     // GLS 'replace' 0x69
  uint8_t pad[3];
};
static_assert(sizeof(ReplaceDestructibility) == 0x10);

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

bool RoleWrapper::operator==(const RoleWrapper &) const = default;

int RoleWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "<Role %p>", role);
  return 1;
}

int RoleWrapper::get_id() { return role->id; }
int RoleWrapper::get_type() { return static_cast<int>(role->ai); }
std::string_view RoleWrapper::get_name() {
  if (role->name) {
    return role->name.get();
  } else {
    return {};
  }
}
void RoleWrapper::get_vulnerabilities(lua_State *L) {
  CreateVulnerabilityTable(L, role->vulnerabilities);
}

void RoleWrapper::setup_metatable(lua_State *L) {}
RoleWrapper::RoleWrapper(Role *role) : role(role) { assert(role); }

static Roles *roles;
static FastCall<Role *, const char *> GetRoleByName;
static FastCall<Role *, int> GetRoleById;
static FastCall<int, int, Role *, Vec3 *, Vec4 *, int> SpawnRole;

int RoleWrapper::spawn(lua_State *L) {
  int team_id = Lua::check<int>(L, 2);
  auto position = Lua::check<Vec3 *>(L, 3);
  auto orientation = Lua::check<Vec4 *>(L, 4);
  int owner_id = Lua::check<int>(L, 5);

  int actor_id = SpawnRole(team_id, role, position, orientation, owner_id);
  lua_pushinteger(L, actor_id);
  return 1;
}

struct RoleIterator {
  static constexpr const char *metatable_name = "RoleIterator";
  static void setup_metatable(lua_State *L) {}

  Roles::iterator cur{};
  Roles::iterator last{};

  int next(lua_State *L) {
    if (cur == last) {
      return 0;
    }

    Role *role = *cur;
    ++cur;

    lua_pushinteger(L, role->id);
    Lua::Create<RoleWrapper>(L, role);
    return 2;
  }
};

RolesModule::RolesModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(roles, 0x007b48f0);
  GetObjectAtOffset(GetRoleByName, 0x004ae030);
  GetObjectAtOffset(GetRoleById, 0x004ae0d0);
  GetObjectAtOffset(SpawnRole, 0x00503710);
}

int RolesModule::Register(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);

  lua_pushcfunction(L, [](lua_State *L) {
    int isnum;
    int id = lua_tonumberx(L, 2, &isnum);
    if (isnum) {
      auto role = GetRoleById(id);
      if (role) {
        Lua::Create<RoleWrapper>(L, role);
        return 1;
      } else {
        return 0;
      }
    }

    auto name = Lua::to<std::string_view>(L, 2);
    auto role = GetRoleByName(name.data());

    if (role) {
      Lua::Create<RoleWrapper>(L, role);
      return 1;
    } else {
      return 0;
    }
  });
  lua_setfield(L, -2, "__index");

  lua_pushcfunction(
      L, ([](lua_State *L) {
        Lua::PushMemberFunction<RoleIterator, &RoleIterator::next>(L);
        Lua::Create<RoleIterator>(L, roles->begin(), roles->end());
        return 2;
      }));
  lua_setfield(L, -2, "__pairs");
  lua_setmetatable(L, -2);

  return 1;
}

} // namespace gk