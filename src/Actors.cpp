#include "Actors.h"

#include "Core.h"
#include "LuaEngine.h"
#include "Math.h"
#include "Vulnerability.h"

#include <cassert>
#include <span>

namespace gk {
union field {
  int integer;
  float flt;
  char *str;
  Role *role;
};

// 0x28-byte weapon instance hanging off CharacterActor::weapon_data.
struct Weapon {
  field field0x0;
  float range;        // 0x04 base range, scaled by CharacterActor::weapon_range
  int weapon_type;    // 0x08 cases 4..0x20; 0xe is special-cased
  float ammo_rate;    // 0x0c from AmmoInfos, scaled by the per-thread delta time
  float ammo_param;   // 0x10 second AmmoInfos float
  field field0x14;
  field field0x18;    // 0x18/0x1c thresholds compared against the ammo count
  field field0x1c;
  int ammo_type;      // 0x20 0x12 = none/melee
  field field0x24;
};
static_assert(sizeof(Weapon) == 0x28);

struct Actor {
  int field0x4;
  bool field0x8;
  char pad0x9[3];
  int id;                     // 0x0c
  VulnList *vulnerabilities;  // 0x10
  int num_vulnerabilities;    // 0x14
  void *field0x18;
  char pad0x1c[0x14];
  float alarm_delay;          // 0x30
  char pad0x34[0xc];
  void *attachment;           // 0x40 ref-counted; released by slot 52
  int field0x44;
  char pad0x48[8];
  int ai_type;                // 0x50
  int field0x54;
  char pad0x58[8];
  float aggression;           // 0x60
  Vec3 field0x64;
  char pad0x70[0xc];
  unsigned flags;             // 0x7c slot 82 tests 0x8, sets 0x200
  int field0x80;
  char pad0x84[4];
  float field0x88;            // 0x88 last-action timestamp (slot 93 gate)
  char pad0x8c[4];
  int field0x90;
  int field0x94;
  float field0x98;
  char pad0x9c[4];
  Vec3 position;              // 0xa0
  Vec4 orientation;           // 0xac
  int team_id;                // 0xbc
  Role *role;                 // 0xc0 (`entity` in the Ghidra DB)
  Role *frag_role;            // 0xc4
  int field0xc8;              // 0xc8/0xcc 64-bit last-update timestamp
  int field0xcc;
  int field0xd0;
  int field0xd4;
  float field0xd8;            // 0xd8 latched into 0xdc on death
  int field0xdc;
  void *anim_object;          // 0xe0 3D model, driven by slots 71-74
  Vec3 top_coords;            // 0xe4
  float armor_value;          // 0xf0
  float strength;             // 0xf4 current health pool
  float health;               // 0xf8
  int field0xfc;
  char pad0x100[0xc];
  bool position_set;          // 0x10c
  bool visible;               // 0x10d slot 61
  char pad0x10e[2];
  float field0x110;
  char field0x114;
  bool is_dead;               // 0x115 set by slot 65; slot 6 returns !is_dead
  char pad0x116[2];
  int field0x118;             // 0x118 slot 12
  char pad0x11c[4];

  virtual ~Actor() = 0;
  virtual void OnCreate() = 0;
  virtual void SetHealth(float) = 0;
  virtual void GetHealth(float *) = 0;
  virtual Vec3 *GetCenterCoords(Vec3 *) = 0;
  virtual void GetStrengthRatio(float *) = 0;
  virtual bool IsAlive() = 0;
  virtual bool IsAttacking() = 0;
  virtual bool IsMine() = 0;
  virtual void SetIsMine(bool) = 0;
  virtual Character *GetCharacter() = 0;
  virtual Weapon *GetWeapon() = 0;
  virtual int GetField0x118() = 0;
  virtual bool IsEnabled() = 0;
  virtual bool IsMoving() = 0;
  virtual Actor *GetAttackTarget() = 0;
  virtual void *GetInventory() = 0;
  virtual bool HasCustomisationHierarchy() = 0;
  virtual void GetArmorValue(float *) = 0;
  virtual void GetShieldValue(float *) = 0;
  virtual void ApplyArmorDamage() = 0;
  virtual void ApplyShieldDamage() = 0;
  virtual int GetAmmoCount() = 0;
  virtual int GetField0x304() = 0;
  virtual void *GetAIController() = 0;
  virtual int GetField0x18c() = 0;
  virtual void SetField0x18c(int) = 0;
  virtual void Stub27() = 0;
  virtual void SetArmorValue(float) = 0;
  virtual void SetShieldValue(float) = 0;
  virtual bool GetField0x188() = 0;
  virtual char *GetHotspot() = 0;
  virtual bool HasPendingOrders() = 0;
  virtual void SetTeamID(int) = 0;
  virtual void *GetInventoryListPtr() = 0;
  virtual int GetSize() = 0;
  virtual bool IsMobile() = 0;
  virtual bool IsCharacter() = 0;
  virtual bool IsProjectile() = 0;
  virtual bool IsTrackObject() = 0;
  virtual bool IsNode() = 0;
  virtual bool IsCentipede() = 0;
  virtual bool IsCentibody() = 0;
  virtual bool IsBackgroundCreature() = 0;
  virtual bool IsFlyingBackgroundCreature() = 0;
  virtual bool IsPickup() = 0;
  virtual bool IsTumbleweed() = 0;
  virtual bool IsPopup() = 0;
  virtual bool IsBlocker() = 0;
  virtual bool IsPresident() = 0;
  virtual bool IsTurret() = 0;
  virtual void InitPositionAndTiming(int, int, float) = 0;
  virtual void ReleaseAttachment() = 0;
  virtual void SetPositionAndOrientation(Vec3 *, Vec4 *, int) = 0;
  virtual void SetField0x188(bool) = 0;
  virtual void OnPrePhysics() = 0;
  virtual void OnCollisionResponse() = 0;
  virtual void Raycast(int *, int, int, int *) = 0;
  virtual void SweepTest(int *, int, int, int, int *) = 0;
  virtual void OnDamageReceived() = 0;
  virtual bool IsTargetable() = 0;
  virtual bool IsVisible() = 0;
  virtual bool IsInteractable() = 0;
  virtual bool CanBePickedUp() = 0;
  virtual void Frag() = 0;
  virtual void Delete() = 0;
  virtual void Associate(char *script, char one_shot) = 0;
  virtual void Dissociate() = 0;
  virtual bool ApplyDamage(float, bool) = 0;
  virtual void OnHealthChanged() = 0;
  virtual void SyncPositionAndBroadcast(int, int, float) = 0;
  virtual void PlayAnimation(int, int, int, int, int, int) = 0;
  virtual void BlendAnimation(int, int, int, int, int, int, int) = 0;
  virtual void PlayAnimationEx(int, int, int, int, int, int, int, int) = 0;
  virtual void SetAnimationState(unsigned int, int) = 0;
  virtual bool HasCustomAnimation() = 0;
  virtual void OnAnimationComplete() = 0;
  virtual void OnAnimationEvent() = 0;
  virtual void SetTarget(int, int) = 0;
  virtual void ClearTarget() = 0;
  virtual void ChangeOwnerAndTeam(int, int, int) = 0;
  virtual void ReleaseFromOwner() = 0;
  virtual void ActivateInWorld() = 0;
};
static_assert(sizeof(Actor) == 0x120);
static_assert(offsetof(Actor, ai_type) == 0x50);
static_assert(offsetof(Actor, position) == 0xa0);
static_assert(offsetof(Actor, orientation) == 0xac);

struct MobileActor : Actor {
  char pad0x120[0x40];
  Character *character;        // 0x160 cached role->character
  char pad0x164[4];
  float sight_range;           // 0x168
  float initial_first_person_range;
  float alert_radius;          // 0x170
  float sight_angle;           // 0x174
  int walking_speed;           // 0x178
  float field0x17c;            // 0x17c slot 60 compares this against a global
  char pad0x180[4];
  bool is_moving;              // 0x184 slot 14
  char field0x185;
  bool is_mine;                // 0x186 slots 8/9
  bool can_be_picked_up;       // 0x187 slot 63
  bool field0x188;             // 0x188 slots 30/54
  char pad0x189[3];
  int field0x18c;              // 0x18c slots 25/26; used as a team-slot index
  short field0x190;            // 0x190 anim channel (lo) + busy flag (hi)
  char pad0x192[2];
  void *inventory;             // 0x194 slot 16; 0x44-byte container
  void *field0x198;            // 0x198 slot 92; Hierarchy* override
  void *inventory_list;        // 0x19c slot 34 returns &inventory_list
  char pad0x1a0[0x14];
  float aim;                   // 0x1b4
  char pad0x1b8[4];
  int move_state;              // 0x1bc slot 62; MobileActor::SetMoveState
  void *dest_node;             // 0x1c0 nav node (was `path_handle`)
  short waypoint_active;       // 0x1c4
  char pad0x1c6[6];
  void *path_object;           // 0x1cc ref-counted; slot 52
  Vec3 collision_bounds;       // 0x1d0
  Vec3 goto_target;            // 0x1dc
  float goto_priority;         // 0x1e8
  float goto_priority_current; // 0x1ec
  void *order_queue;           // 0x1f0 list header {sentinel,count,cache,valid}
  int order_queue_count;       // 0x1f4 slot 32
  void *field0x1f8;
  char pad0x1fc[4];
  void *ai_controller;         // 0x200 slot 24
  void *waypoints_sentinel;    // 0x204 embedded waypoint list header
  int waypoints_count;         // 0x208
  void *waypoints_cache;       // 0x20c
  bool waypoints_cache_valid;  // 0x210
  char pad0x211[3];
  void *field0x214;
  char pad0x218[0xc];
  void *waypoint_ptr;          // 0x224 cursor into the waypoint list
  void *waypoint_list;         // 0x228 -> &waypoints_sentinel
  char pad0x22c[4];

  virtual void UpdateMineDetectionAndBounds() = 0;
  virtual void EquipToFirstOpenSlot(int, int) = 0;
  virtual void QueueOrderKind10(int) = 0;
  virtual void QueueOrderPosition(Vec3 *, int, char) = 0;
  virtual void QueueOrderTarget(int, char) = 0;
  virtual int Goto(Vec3 *, float) = 0;
  virtual void Die() = 0;
  virtual void AddWaypoint(Vec3 *, int, char, int) = 0;
  virtual void GetNavigationTarget(int *) = 0;
  virtual void *GetField0x198() = 0;
  virtual void PlayActionAnimation(int, float) = 0;
  virtual void SetWeapon(int) = 0;
};
static_assert(sizeof(MobileActor) == 0x230);
static_assert(offsetof(MobileActor, character) == 0x160);
static_assert(offsetof(MobileActor, is_mine) == 0x186);
static_assert(offsetof(MobileActor, inventory) == 0x194);
static_assert(offsetof(MobileActor, ai_controller) == 0x200);

// 0x120..0x12f is the standard {sentinel, count, cache, cache_valid} list header.
// It records every nav-mesh polygon whose "blocked" bit (0x100) this actor set, so
// slot 82 can undo them.
struct BlockerActor : Actor {
  void *blocked_polys;       // 0x120
  int num_blocked_polys;     // 0x124
  void *blocked_polys_cache; // 0x128
  char blocked_polys_cache_valid; // 0x12c
  char pad0x12d[3];
};
static_assert(sizeof(BlockerActor) == 0x130);
static_assert(offsetof(BlockerActor, blocked_polys) == 0x120);

struct PickupActor : Actor {
  int enabled;              // 0x120 slot 13
  int action_on_death;      // 0x124
  int pickup_type;          // 0x128 == role->character->aggression * 10
  char is_destructible;     // 0x12c gates GetArmorValue and SetPickupEnabled
  char pad0x12d[3];
  char pad0x130[4];
  char *associated_script;  // 0x134 set by slot 66 Associate
  char *unk_string0x138;    // 0x138 set by slot 85; NOT associated_script
  bool is_script_oneshot;   // 0x13c
  char pad0x13d[3];
  int respawn_delay_mode;   // 0x140 1 -> x2, 2 -> x1, 3 -> x0.5
  float respawn_at_time;    // 0x144 game-time deadline (was `max_distance`)
  int has_associated_script; // 0x148 slot 75; set by slot 66 Associate
  float pickup_radius;      // 0x14c

  virtual void SetPickupEnabled(bool) = 0;
  virtual void OnPickedUp(MobileActor *) = 0;
  virtual void SetField0x138(const char *) = 0;
};
static_assert(sizeof(PickupActor) == 0x150);
static_assert(offsetof(PickupActor, associated_script) == 0x134);
static_assert(offsetof(PickupActor, unk_string0x138) == 0x138);
static_assert(offsetof(PickupActor, respawn_at_time) == 0x144);

// Launch origin + velocity + launch time integrated against gravity, with the role's
// `projectile` sub-object supplying damage, splash and radius. Slot 51 precomputes the
// impact, slot 55 is the physics step, slot 70 the dead-reckoning (which despite the
// slot name broadcasts nothing itself). Called `UnknownActor` in older notes.
struct ProjectileActor : Actor {
  Vec3 initial_position;  // 0x120 launch anchor for the ballistic closed form
  int owner_actor_id;     // 0x12c skipped by every hit test
  float damage_scale;     // 0x130 scales damage, not velocity
  void *projectile;       // 0x134 Role::projectile
  Vec3 velocity;          // 0x138
  char pad0x144[0xc];     // 0x144 launch time, 0x148/0x14c launch tick (int64)
  unsigned flags;         // 0x150 slot 83; 0x10 gore, 0x40 guided, 0x200 dissociate
  bool hit_predicted;     // 0x154
  bool hit_is_world;      // 0x155
  char pad0x156[2];
  float time_to_impact;   // 0x158
  int hit_actor_id;       // 0x15c
  void *weapon_context;   // 0x160
  void *target_actor;     // 0x164
  Vec3 target_position;   // 0x168 written by slot 84
  int field0x174;

  virtual unsigned GetProjectileFlags() = 0;
  virtual void SetTargetPosition(Vec3) = 0;
};
static_assert(sizeof(ProjectileActor) == 0x178);
static_assert(offsetof(ProjectileActor, flags) == 0x150);
static_assert(offsetof(ProjectileActor, target_position) == 0x168);

struct TrackObjectActor : Actor {
  bool affects_map;       // 0x120
  bool path_valid;        // 0x121
  bool moving;            // 0x122
  bool reverse;           // 0x123 selects message 0xad / 0xae
  bool cycle_enabled;     // 0x124
  char pad0x125[3];
  unsigned cycle_ticks;   // 0x128
  float inv_duration;     // 0x12c elapsed * this = spline parameter t
  void *geometry;         // 0x130 entry from the Map+0x24 track list
  float coeffs[3][4];     // 0x134 cubic [x|y|z][t^3, t^2, t, 1]
  char pad0x164[0x14];
  Vec4 start_ori;         // 0x178
  Vec4 target_ori;        // 0x188
  char pad0x198[8];       // 0x198 int64 start time
  char pad0x1a0[8];
  void *riders;           // 0x1a8 list of refcounted Actor* being carried
  int num_riders;         // 0x1ac
  void *riders_cache;     // 0x1b0
  int riders_cache_valid; // 0x1b4
};
static_assert(sizeof(TrackObjectActor) == 0x1b8);
static_assert(offsetof(TrackObjectActor, geometry) == 0x130);
static_assert(offsetof(TrackObjectActor, riders) == 0x1a8);

struct TumbleweedActor : Actor {};
static_assert(sizeof(TumbleweedActor) == 0x120);

struct BackgroundCreatureActor : Actor {};
static_assert(sizeof(BackgroundCreatureActor) == 0x120);

struct CharacterActor : MobileActor {
  char pad0x230[0x28];
  float weapon_cycle_time;    // 0x258
  float weapon_cycle_offset;  // 0x25c
  char pad0x260[1];
  char attack_mode_param;     // 0x261
  char pad0x262[0x26];
  char close_range_attack;    // 0x288
  char attack_active_flag;    // 0x289
  char pad0x28a[6];
  int attack_range;           // 0x290
  char pad0x294[0x10];
  int weapon_type;            // 0x2a4
  char pad0x2a8[0x10];
  Weapon *weapon;             // 0x2b8 slot 11
  Vec3 aim_direction;         // 0x2bc (TurretActor uses it as muzzle position)
  void *hotspot;              // 0x2c8 slot 31
  void *alternate_hotspot;    // 0x2cc
  float shield_value;         // 0x2d0 slots 19/29
  char is_attacking;          // 0x2d4 slot 7
  char pad0x2d5[3];
  void *attack_target;        // 0x2d8 slot 15
  Vec3 attack_position;       // 0x2dc
  char pad0x2e8[4];           // 0x2e8..0x2ea animation-complete flags
  float weapon_range;         // 0x2ec (TurretActor uses it as muzzle speed)
  char pad0x2f0[8];           // 0x2f0/0x2f4 gun yaw/pitch, float despite the int typing
  int attack_stop_reason;     // 0x2f8
  float target_cycle_delay;   // 0x2fc
  void *selected_ammo;        // 0x300
  int field0x304;             // 0x304 slots 23/95; cannot-fire gate

  virtual void SetField0x304(int) = 0;
  virtual void AttackTarget(Actor *, int, char, char) = 0;
  virtual void AttackPosition(Vec3 *, int, char, char) = 0;
  virtual void StopAttacking(int) = 0;
  virtual void SetAmmoType(int) = 0;
};
static_assert(sizeof(CharacterActor) == 0x308);
static_assert(offsetof(CharacterActor, weapon) == 0x2b8);
static_assert(offsetof(CharacterActor, is_attacking) == 0x2d4);
static_assert(offsetof(CharacterActor, attack_target) == 0x2d8);
static_assert(offsetof(CharacterActor, field0x304) == 0x304);

struct NodeActor : MobileActor {
  char pad0x230[8];    // 0x230/0x234 int64 spawn timestamp
  char pad0x238[0x20];
  void *paths;         // 0x258 list of paths; each payload is itself a list of
  int num_paths;       // 0x25c 0x18-byte waypoint records
  void *paths_cache;   // 0x260
  int paths_cache_valid; // 0x264
  void *current_path;  // 0x268 slot 90 appends to *(current_path + 0xc)
  char pad0x26c[0xc];
};
static_assert(sizeof(NodeActor) == 0x278);
static_assert(offsetof(NodeActor, paths) == 0x258);
static_assert(offsetof(NodeActor, current_path) == 0x268);

// The escort/VIP actor: it walks to an `exita`..`exitd` map aux object and, on
// arrival, broadcasts 0x9b and deletes itself. It also defects to whichever team
// has the nearest actor.
struct PresidentActor : MobileActor {
  Vec3 exit_position;          // 0x230 slot 95
  float last_team_switch_time; // 0x23c
  virtual void SetExitPosition(Vec3) = 0;
};
static_assert(sizeof(PresidentActor) == 0x240);
static_assert(offsetof(PresidentActor, exit_position) == 0x230);

struct FlyingBackgroundCreatureActor : BackgroundCreatureActor {};
static_assert(sizeof(FlyingBackgroundCreatureActor) == 0x120);

struct CentibodyActor : CharacterActor {
  Actor *next_segment; // 0x308 refcounted; the follow-the-leader body chain
  char pad0x30c[4];
};
static_assert(sizeof(CentibodyActor) == 0x310);
static_assert(offsetof(CentibodyActor, next_segment) == 0x308);

struct PopupActor : CharacterActor {
  bool deployed;                // 0x308
  bool transition_in_progress;  // 0x309
  char pad0x30a[6];
};
static_assert(sizeof(PopupActor) == 0x310);
static_assert(offsetof(PopupActor, deployed) == 0x308);

struct CentipedeActor : CentibodyActor {};
static_assert(sizeof(CentipedeActor) == 0x310);

struct TurretActor : PopupActor {
  char unk8[16];
  virtual void SetTurretEnabled(bool) = 0;
  virtual void GetTurretAimDirection(long long *) = 0;
  virtual long long GetTurretTargetAngles() = 0;
  virtual bool IsTurretEnabled() = 0;
  virtual void SetTurretTargetAngles(int, int) = 0;
};
static_assert(sizeof(TurretActor) == 0x320);

struct ActorNode {
  Actor *actor;
  ActorNode *next;
};

struct Actors {
  int unk1;
  int num_actors;
  int num_buckets;
  int bucket_mask;
  ActorNode **buckets;
};

static Actors *actors;
static FastCall<Actor *, int> GetActorById;

bool ActorWrapper::operator==(const ActorWrapper &) const = default;

ActorWrapper::ActorWrapper(Actor *actor) : actor(actor) { assert(actor); }

int ActorWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "<Actor %p>", actor);
  return 1;
}

int ActorWrapper::get_id() { return actor->id; }
Vec3 ActorWrapper::get_position() { return actor->position; }
Vec4 ActorWrapper::get_orientation() { return actor->orientation; }
int ActorWrapper::get_team_id() { return actor->team_id; }
RoleWrapper ActorWrapper::get_role() { return {actor->role}; }
Vec3 ActorWrapper::get_center() {
  Vec3 center;
  actor->GetCenterCoords(&center);
  return center;
}
int ActorWrapper::get_ai_type() { return actor->ai_type; }
void ActorWrapper::get_vulnerabilities(lua_State *L) {
  CreateVulnerabilityTable(L, actor->vulnerabilities);
}
float ActorWrapper::get_health() {
  float res;
  actor->GetHealth(&res);
  return res;
}

int ActorWrapper::new_index(lua_State *L) {
  auto key = Lua::to<std::string_view>(L, 2);
  if (key == "position") {
    auto value = Lua::check<Vec3 *>(L, 3);
    actor->position = *value;
  } else if (key == "orientation") {
    auto value = Lua::check<Vec4 *>(L, 3);
    actor->orientation = *value;
  }
  return 0;
}

void ActorWrapper::setup_metatable(lua_State *L) {
  Lua::PushMemberFunction<ActorWrapper, &ActorWrapper::new_index>(L);
  lua_setfield(L, -2, "__newindex");
}

ActorsModule::ActorsModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(actors, 0x007ba0d8);
  GetObjectAtOffset(GetActorById, 0x0044e0b0);
}

struct ActorIterator {
  static constexpr const char *metatable_name = "ActorIterator";
  static void setup_metatable(lua_State *L) {}

  std::span<ActorNode *> remaining_buckets;
  ActorNode *node{};

  int next(lua_State *L) {
    while (!node) {
      if (remaining_buckets.empty()) {
        return 0;
      }

      node = remaining_buckets.front();
      remaining_buckets = remaining_buckets.subspan(1);
    }

    lua_pushinteger(L, node->actor->id);
    Lua::Create<ActorWrapper>(L, node->actor);
    node = node->next;

    return 2;
  }
};

int ActorsModule::Register(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);

  lua_pushcfunction(L, [](lua_State *L) {
    auto id = Lua::check<int>(L, 2);
    auto actor = GetActorById(id);

    if (actor) {
      Lua::Create<ActorWrapper>(L, actor);
      return 1;
    } else {
      return 0;
    }
  });
  lua_setfield(L, -2, "__index");

  lua_pushcfunction(
      L, ([](lua_State *L) {
        Lua::PushMemberFunction<ActorIterator, &ActorIterator::next>(L);
        Lua::Create<ActorIterator>(
            L, std::span(actors->buckets, actors->num_buckets));
        return 2;
      }));
  lua_setfield(L, -2, "__pairs");
  lua_setmetatable(L, -2);

  return 1;
}
} // namespace gk