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
  uint8_t field0x1e;
  uint8_t field0x1f;
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
  uint8_t field0x83;
  Hierarchy *customisation_hierarchy;
  Hierarchy *shadow_hierarchy;
  int blob_shadow;
  char *description;
  field field0x94;
  field field0x98;
  int field00x9c;
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

struct Projectile {
  bool gravity;
  uint8_t field0x1;
  uint8_t field0x2;
  uint8_t field0x3;
  float damage;
  int sound;
  float max_range;
  float blast_damage;
  float blast_range;
  float blast_range_squared;
  Light *hit_light;
};

struct ParticleGenerator {
  int type;
  int field0x4;
  int field0x8;
  int life;
  float rate;
  Vec3 coords;
  Vec3 field0x20;
  field field0x2c;
  float red;
  float green;
  float blue;
  float alpha;
  int field0x40;
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
  float start_scale;
  float end_scale;
  float spin;
  field field0xd0;
};

struct Destructibility {
  void *vtbl;
  int type;
};

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
  Vec3 field0x2c;
  Vec3 field0x38;
  char *hotspot;
  char *alternate_hotspot;
  int field0x4c;
  int field0x50;
  int limit;
  Light *light;
  Projectile *projectile;
  Character *character;
  InventoryInfo *inventory_info;
  VulnList *vulnerabilities;
  int field0x6c;
  char *field0x70;
  char *field0x74;
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
  int ai;
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
  void *field0xac;
  int field0xb0;
  char *field0xb4;
  int field0xb8;
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
int RoleWrapper::get_type() { return role->ai; }
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