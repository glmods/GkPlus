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

struct Weapon {
  field field0x0;
  field field0x4;
  field field0x8;
  field field0xc;
  field field0x10;
  field field0x14;
  field field0x18;
  field field0x1c;
  field field0x20;
  field field0x24;
};

struct Actor {
  int unk1;
  int unk2;
  int id;
  VulnList *vulnerabilities;
  int num_vulnerabilities;
  int unkx[14];
  int ai_type;
  int unk3[19];
  Vec3 position;
  Vec4 orientation;
  int team_id;
  Role *role;
  char unk4[92];

  virtual ~Actor() = 0;
  virtual void OnCreate() = 0;
  virtual void SetHealth(float) = 0;
  virtual void GetHealth(float *) = 0;
  virtual Vec3 *GetCenterCoords(Vec3 *) = 0;
  virtual void GetStrengthRatio(float *) = 0;
  virtual bool IsAlive() = 0;
  virtual bool IsSpecialType_0() = 0;
  virtual bool IsSpecialType_1() = 0;
  virtual void OnUpdate() = 0;
  virtual Character *GetCharacter() = 0;
  virtual Weapon *GetWeapon() = 0;
  virtual int GetField0x118() = 0;
  virtual bool IsEnabled() = 0;
  virtual bool IsMoving() = 0;
  virtual Weapon *GetSecondaryWeapon() = 0;
  virtual int GetMovementState() = 0;
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
  virtual bool HasInventory() = 0;
  virtual void SetTeamID(int) = 0;
  virtual void *GetInventoryListPtr() = 0;
  virtual int GetSize() = 0;
  virtual bool IsMobile() = 0;
  virtual bool IsCharacter() = 0;
  virtual bool IsUnknownActor() = 0;
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
  virtual void OnPostUpdate() = 0;
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
  char unk5[272];
  virtual void UpdateMineDetectionAndBounds() = 0;
  virtual void EquipToFirstOpenSlot(int, int) = 0;
  virtual void OnPreThink() = 0;
  virtual void OnPostThink(Vec3 *) = 0;
  virtual void OnPreUpdate() = 0;
  virtual int Goto(Vec3 *, float) = 0;
  virtual void Die() = 0;
  virtual void AddWaypoint(long long *, int, char, int) = 0;
  virtual void GetNavigationTarget(int *) = 0;
  virtual int GetField0x198() = 0;
  virtual void PlayActionAnimation(int, float) = 0;
  virtual void OnMobileDamageReceived() = 0;
};
static_assert(sizeof(MobileActor) == 0x230);

struct BlockerActor : Actor {
  char unk5[16];
};
static_assert(sizeof(BlockerActor) == 0x130);

struct PickupActor : Actor {
  char unk5[20];
  char *associated_script;
  char *unk_string0x138;
  char unk6[20];

  virtual void SetPickupEnabled(bool) = 0;
  virtual void OnPickedUp(MobileActor *) = 0;
  virtual void SetField0x138(const char *) = 0;
};
static_assert(sizeof(PickupActor) == 0x150);
static_assert(offsetof(PickupActor, associated_script) == 0x134);
static_assert(offsetof(PickupActor, unk_string0x138) == 0x138);

struct TrackObjectActor : Actor {
  char unk5[152];
};
static_assert(sizeof(TrackObjectActor) == 0x1b8);

struct TumbleweedActor : Actor {};
static_assert(sizeof(TumbleweedActor) == 0x120);

struct BackgroundCreatureActor : Actor {};
static_assert(sizeof(BackgroundCreatureActor) == 0x120);

struct CharacterActor : MobileActor {
  char unk6[136];
  Weapon *weapon;
  char unk7[76];
  virtual void SetField0x304(int) = 0;
  virtual void AttackTarget(int, int, char, char) = 0;
  virtual void AttackPosition(long long *, int, char, char) = 0;
  virtual void StopAttacking(int) = 0;
  virtual void ExecuteSpecialAbility(int) = 0;
};
static_assert(sizeof(CharacterActor) == 0x308);
static_assert(offsetof(CharacterActor, weapon) == 0x2b8);

struct NodeActor : MobileActor {
  char unk6[72];
};
static_assert(sizeof(NodeActor) == 0x278);

struct PresidentActor : MobileActor {
  char unk6[16];
  virtual void PresidentMethod(long long, int) = 0;
};
static_assert(sizeof(PresidentActor) == 0x240);

struct FlyingBackgroundCreatureActor : BackgroundCreatureActor {};
static_assert(sizeof(FlyingBackgroundCreatureActor) == 0x120);

struct CentibodyActor : CharacterActor {
  char unk7[8];
};
static_assert(sizeof(CentibodyActor) == 0x310);

struct PopupActor : CharacterActor {
  char unk7[8];
};
static_assert(sizeof(PopupActor) == 0x310);

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