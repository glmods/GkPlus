#include <cassert>

#include "Core.h"
#include "LuaEngine.h"
#include "Math.h"
#include "Roles.h"
#include "Vulnerability.h"

#include <cassert>
#include <cstdint>
#include <span>

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
  Hierarchy *customisation_hierarchy;
  Hierarchy *shadow_hierarchy;
  int blob_shadow;
  char *description;
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
  Light *hit_light;     // 0x1c GLS 0x1f (owned, ToLight)
};

// See role_subobjects_notes.md. Only ~15 fields come from GLS; the 0x30..0xa8 block is
// five {vec4 of 1.0f, int count=2} interpolation channels (the first is RGBA colour),
// default-initialised by ToParticleGenerator. No owned heap pointers.
struct ParticleGenerator {
  int type;             // 0x00 GLS 'type' 0x41 (0..12)
  int kind;             // 0x04 constant 5 (object-kind tag)
  int field0x8;         // 0x08 copied from parsed+0x1b60
  int field0xc;         // 0x0c copied from parsed+0x1b64 (not the lifespan; see 0xd0)
  float rate;           // 0x10 GLS 'rate' 0x43
  Vec3 coords;          // 0x14 GLS x/y/z 0x44-46
  Vec3 field0x20;
  field field0x2c;
  float red;            // 0x30 GLS 'red' 0x21  (colour channel base)
  float green;          // 0x34 GLS 'green' 0x22
  float blue;           // 0x38 GLS 'blue' 0x23
  float alpha;          // 0x3c GLS 'alpha' 0x24
  int field0x40;        // 0x40 channel-0 keyframe count (=2)
  field field0x44;
  float field0x48;
  float field0x4c;
  float field0x50;
  float field0x54;
  int field0x58;
  short field0x5c;
  short field0x5e;
  void *field0x60;
  float field0x64;
  float field0x68;
  float field0x6c;
  float field0x70;
  int field0x74;
  void *field0x78;
  float field0x7c;
  float field0x80;
  float field0x84;
  float field0x88;
  int field0x8c;
  float field0x90;
  float field0x94;
  float field0x98;
  float field0x9c;
  float field0xa0;
  int field0xa4;
  float field0xa8;
  field field0xac;
  float field0xb0;
  bool generate_generators;
  uint8_t field0xb5;
  short field0xb6;
  void *field0xb8;
  field field0xbc;
  void *field0xc0;
  float start_scale;    // 0xc4 GLS 'start scale' 0x64
  float end_scale;      // 0xc8 GLS 'end scale' 0x65
  float spin;           // 0xcc GLS 'spin' 0x66
  field lifespan_ticks; // 0xd0 GLS 'particle TTL' 0x67 * current clock rate
};

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
  Role *role;         // GLS 'role' 0x60 - fragment pieces
  Role *replace_role; // GLS 'replace role' 0x61 - actor spawned in place at death
  char *remove;       // GLS 'remove' 0x62 (owned)
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
  char *name;   // GLS 'name' 0x00 (owned)
  bool replace; // GLS 'replace' 0x69
  uint8_t pad[3];
};
static_assert(sizeof(ReplaceDestructibility) == 0x10);

struct InventoryInfo {
  Hierarchy *hierarchy;
  Shape *shape;
  char *description;
  char *pickup_name;
  float pickup_radius;
  int action_on_death;
};

struct Role {
  char *name;
  char *recon_name;
  char *recon_ai_short;
  int recon_ai_number;
  char *recon_ai_long;
  char *recon_ai_long2;
  Shape *shape;
  Hierarchy *hierarchy;
  ParticleGenerator *pgen;
  ParticleGenerator *pgen2;
  char *meta_sound;
  // Resolved positions of the hotspot / alternate-hotspot nodes within the role's
  // hierarchy (see HierarchyResolveNamedPointPos @ 0x00594890). Zero for shape/pgen roles.
  Vec3 hotspot_point;           // 0x2c
  Vec3 alternate_hotspot_point; // 0x38
  char *hotspot;                // 0x44 hotspot node name
  char *alternate_hotspot;      // 0x48 alternate hotspot node name
  // Counts of present hierarchy nodes in slot ranges 26..30 / 21..25 (ToRole).
  int num_hier_nodes_26_30;     // 0x4c
  int num_hier_nodes_21_25;     // 0x50
  int limit;
  Light *light;
  Projectile *projectile;
  Character *character;
  InventoryInfo *inventory_info;
  // Vulnerabilities: a 16-byte list header {sentinel, count, cached_array, cache_valid}.
  // Entries are added outside ToRole (by the vulnerability-processing path).
  VulnList *vulnerabilities;    // 0x68
  int num_vulnerabilities;      // 0x6c
  void *vuln_cached_array;      // 0x70 flattened-array cache, freed+nulled on edit
  int vuln_cache_valid;         // 0x74
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
  int interface_beam_delay;
  int interface_beam_effect;
  char *interface_beam_script;
  int interface_beam_duration;
  int resistance;
  float resistance_factor;
  float armor;
  float shields;
  float recharge_rate;
  float alpha;
  Destructibility *destructibility;
  // Sever points: a 16-byte list header {sentinel, count, cached_array, cache_valid} of
  // body-part/attachment name strings, split from the "sever point" GLS field on ','.
  // Uses trigger-style nodes, so RoleDtor cleans it with TriggerList::DeleteTriggers.
  void *sever_points;              // 0xac list head
  int num_sever_points;            // 0xb0
  void *sever_point_cached_array;  // 0xb4 freed+nulled on each insertion
  int sever_point_cache_valid;     // 0xb8
  int id;
};
static_assert(offsetof(Role, hotspot) == 0x44);
static_assert(offsetof(Role, ai) == 0x7c);
static_assert(offsetof(Role, id) == 0xbc);
static_assert(offsetof(Role, vulnerabilities) == 0x68);
static_assert(sizeof(Role) == 0xc0);

struct RoleNode {
  Role *role;
  RoleNode *next;
};

struct Roles {
  unsigned num_roles;
  unsigned num_buckets;
  unsigned bucket_mask;
  RoleNode **buckets;
};

bool RoleWrapper::operator==(const RoleWrapper &) const = default;

int RoleWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "<Role %p>", role);
  return 1;
}

int RoleWrapper::get_id() { return role->id; }
int RoleWrapper::get_type() { return static_cast<int>(role->ai); }
std::string_view RoleWrapper::get_name() {
  if (role->name) {
    return role->name;
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

  std::span<RoleNode *> remaining_buckets;
  RoleNode *node{};

  int next(lua_State *L) {
    while (!node) {
      if (remaining_buckets.empty()) {
        return 0;
      }

      node = remaining_buckets.front();
      remaining_buckets = remaining_buckets.subspan(1);
    }

    lua_pushinteger(L, node->role->id);
    Lua::Create<RoleWrapper>(L, node->role);
    node = node->next;

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
        Lua::Create<RoleIterator>(
            L, std::span(roles->buckets, roles->num_buckets));
        return 2;
      }));
  lua_setfield(L, -2, "__pairs");
  lua_setmetatable(L, -2);

  return 1;
}

} // namespace gk