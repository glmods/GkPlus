#include <cassert>
#include <string>

#include "Core.h"
#include "DetourUtils.h"
#include "LuaEngine.h"
#include "Math.h"
#include "Varint.h"

#include "Triggers.h"

namespace gk {
namespace {
enum class TriggerKind {
  Death,
  Location,
  LocationSpecified,
  LocationAll,
  LocationTimed,
  InstantDeath,
  InstantDisplace,
  Time,
  Escort,
  Proximity,
  Door,
  DoorOnce,
  DoorsEither,
  FourDoors,
  LightUp,
  Defog,
  Shot,
  BeingAttacked,
  FragScore,
  TimeLimit,
  TimeIfAlive,
  BeenAlerted,
};

struct ITrigger {};

struct TriggerData {
  TriggerKind kind;
  Vec3 coords[4];
  long long time_or_radius;
  float radius_squared;
  ITrigger *target_list_sentinel;
  int target_list_count;
  void *target_list_cache;
  unsigned target_list_cache_flags;
  unsigned char *script_name;
  int team_or_warning;
  unsigned armed;
  int last_trigger_actor;
  int unused;

  TriggerData *HookedRemoveTrigger(char c);
};

static_assert(sizeof(TriggerData) == 0x68);

TriggerData *(TriggerData::*RemoveTrigger)(char) = nullptr;

TriggerData *TriggerData::HookedRemoveTrigger(char c) {
  // FIXME: RemoveTrigger's only caller is EvaluateTriggers, which runs on the
  // executor thread (see threading_model_notes.md) — luaL_unref here races with
  // main-thread Lua. Defer the unref to the main thread instead.
  if (script_name && (*script_name & 0x80)) {
    lua_Integer ref = DecodeVarint(script_name);
    auto L = Lua::GetEngine();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
  }
  return (this->*RemoveTrigger)(c);
}

struct TriggerList {
  ITrigger *sentinel;
  int count;
  void *cached_array;
  bool cache_flag;
};
static_assert(sizeof(TriggerList) == 0x10);

using TCopyList = ThisCall<TriggerList *, TriggerList *, TriggerList *>;
TCopyList CopyList;

using TInitList = ThisCall<TriggerList *, TriggerList *>;
TInitList InitList;

using TDeleteList = ThisCall<void, TriggerList *>;
TDeleteList DeleteList;

using TInitListWithActorName =
    ThisCall<TriggerList *, TriggerList *, const char **>;
TInitListWithActorName InitListWithActorName;

using TCreateTrigger = ThisCall<ITrigger *, TriggerList *, const char **>;
TCreateTrigger CreateTrigger;

using TRegisterTriggers = FastCall<void, TriggerKind, Vec3 *, long long,
                                   TriggerList, const unsigned char *, int>;
TRegisterTriggers RegisterTriggers;

static int LuaAddTimeTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 2) {
    return luaL_error(L, "Two arguments expected");
  }

  lua_Integer time = luaL_checkinteger(L, 1);
  lua_pushvalue(L, 2);

  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  const char *actor_name = "";
  InitListWithActorName(&list, &actor_name);
  Vec3 coords[4];
  RegisterTriggers(TriggerKind::Time, coords, time, list, script, 0);
  return 0;
}

template <TriggerKind Kind> static int LuaAddLocationTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 3) {
    return luaL_error(L, "Three arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  lua_Integer radius = luaL_checkinteger(L, 2);
  lua_pushvalue(L, 3);

  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  const char *actor_name = "";
  InitListWithActorName(&list, &actor_name);
  RegisterTriggers(Kind, coords, radius, list, script, 0);
  return 0;
}

static int LuaAddLocationSpecifiedTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 4) {
    return luaL_error(L, "At least four arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  lua_Integer radius = luaL_checkinteger(L, 2);
  lua_pushvalue(L, 3);

  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  InitList(&list);
  for (int i = 4; i <= nargs; ++i) {
    const char *actor = luaL_checkstring(L, i);
    CreateTrigger(&list, &actor);
  }
  RegisterTriggers(TriggerKind::LocationSpecified, coords, radius, list, script,
                   0);
  DeleteList(&list);
  return 0;
}

static int LuaAddInstantDeathTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 2) {
    return luaL_error(L, "Two arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  lua_Integer radius = luaL_checkinteger(L, 2);
  lua_pushvalue(L, 3);

  TriggerList list;
  const char *actor_name = "";
  InitListWithActorName(&list, &actor_name);
  RegisterTriggers(TriggerKind::InstantDeath, coords, radius, list, nullptr, 0);
  return 0;
}

static int LuaAddInstantDisplaceTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 3) {
    return luaL_error(L, "Three arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  lua_Integer radius = luaL_checkinteger(L, 2);
  lua_pushvalue(L, 3);
  coords[1] = Lua::check<Vec3>(L, 3);

  TriggerList list;
  const char *actor_name = "";
  InitListWithActorName(&list, &actor_name);
  RegisterTriggers(TriggerKind::InstantDisplace, coords, radius, list, nullptr,
                   0);
  return 0;
}

static int LuaAddDeathTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 2) {
    return luaL_error(L, "At least two arguments expected");
  }

  Vec3 coords[4];
  lua_pushvalue(L, 1);

  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  InitList(&list);
  for (int i = 2; i <= nargs; ++i) {
    const char *actor = luaL_checkstring(L, i);
    CreateTrigger(&list, &actor);
  }
  RegisterTriggers(TriggerKind::Death, coords, 0, list, script, 0);
  DeleteList(&list);
  return 0;
}

template <TriggerKind Kind> static int LuaAddDoorTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 3) {
    return luaL_error(L, "At least three arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  coords[1] = coords[0];

  TriggerList list;
  InitList(&list);
  for (int i = 2; i <= nargs; ++i) {
    const char *actor = luaL_checkstring(L, i);
    CreateTrigger(&list, &actor);
  }
  RegisterTriggers(Kind, coords, 0, list, nullptr, 0);
  DeleteList(&list);
  return 0;
}

template <TriggerKind Kind> static int LuaAddDoorsTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 4) {
    return luaL_error(L, "At least four arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  coords[1] = Lua::check<Vec3>(L, 2);

  TriggerList list;
  InitList(&list);
  for (int i = 3; i <= nargs; ++i) {
    const char *actor = luaL_checkstring(L, i);
    CreateTrigger(&list, &actor);
  }
  RegisterTriggers(Kind, coords, 0, list, nullptr, 0);
  DeleteList(&list);
  return 0;
}

static int LuaAddFourDoorsTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 6) {
    return luaL_error(L, "Six arguments expected");
  }
  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  coords[1] = Lua::check<Vec3>(L, 2);
  coords[2] = Lua::check<Vec3>(L, 3);
  coords[3] = Lua::check<Vec3>(L, 4);

  lua_Integer radius = luaL_checkinteger(L, 5);

  lua_pushvalue(L, 6);
  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);

  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  const char *actor_name = "";
  InitListWithActorName(&list, &actor_name);
  RegisterTriggers(TriggerKind::Time, coords, radius, list, script, 0);
  return 0;
}

static int LuaAddShotTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 4) {
    return luaL_error(L, "Four arguments expected");
  }
  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);
  coords[1] = Lua::check<Vec3>(L, 2);

  lua_Integer count = luaL_checkinteger(L, 3);

  const char *role = luaL_checkstring(L, 4);

  TriggerList list;
  const char *actor_name = "";
  InitListWithActorName(&list, &actor_name);
  RegisterTriggers(TriggerKind::Shot, coords, count, list,
                   reinterpret_cast<const unsigned char *>(role), 0);
  return 0;
}

static int LuaAddEscortTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 4) {
    return luaL_error(L, "At least four arguments expected");
  }

  Vec3 coords[4];
  coords[0] = Lua::check<Vec3>(L, 1);

  lua_Integer radius = luaL_checkinteger(L, 2);

  lua_pushvalue(L, 3);
  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);
  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  InitList(&list);
  for (int i = 4; i <= nargs; ++i) {
    const char *actor = luaL_checkstring(L, i);
    CreateTrigger(&list, &actor);
  }
  RegisterTriggers(TriggerKind::Escort, coords, radius, list, script, 0);
  DeleteList(&list);
  return 0;
}

static int LuaAddProximityTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 3) {
    return luaL_error(L, "Three arguments expected");
  }

  Vec3 coords[4];

  lua_Integer radius = luaL_checkinteger(L, 1);

  TriggerList list;
  InitList(&list);
  const char *actor = luaL_checkstring(L, 2);
  CreateTrigger(&list, &actor);

  lua_pushvalue(L, 3);
  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);
  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  RegisterTriggers(TriggerKind::Proximity, coords, radius, list, script, 0);
  DeleteList(&list);
  return 0;
}

template <TriggerKind Kind>
static int LuaAddBeingAttackedTrigger(lua_State *L) {
  int nargs = lua_gettop(L);
  if (nargs < 4) {
    return luaL_error(L, "At least two arguments expected");
  }

  Vec3 coords[4];

  lua_pushvalue(L, 1);
  lua_Integer fref = luaL_ref(L, LUA_REGISTRYINDEX);
  unsigned char script[10] = {};
  EncodeVarint(fref, script);

  TriggerList list;
  InitList(&list);
  for (int i = 2; i <= nargs; ++i) {
    const char *actor = luaL_checkstring(L, i);
    CreateTrigger(&list, &actor);
  }
  RegisterTriggers(Kind, coords, 0, list, script, 0);
  DeleteList(&list);
  return 0;
}
} // namespace

TriggersModule::TriggersModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(CopyList, 0x0044c950);
  GetObjectAtOffset(InitList, 0x0044ca10);
  GetObjectAtOffset(InitListWithActorName, 0x0044c900);
  GetObjectAtOffset(CreateTrigger, 0x0044e8c0);
  GetObjectAtOffset(DeleteList, 0x0044ce40);
  GetObjectAtOffset(RegisterTriggers, 0x0043e240);
  GetObjectAtOffset(RemoveTrigger, 0x0050c400);

  gk::DetourAttach(&RemoveTrigger, &TriggerData::HookedRemoveTrigger);
}

TriggersModule::~TriggersModule() {
  gk::DetourDetach(&RemoveTrigger, &TriggerData::HookedRemoveTrigger);
}

int TriggersModule::Register(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, LuaAddTimeTrigger);
  lua_setfield(L, -2, "add_time");

  lua_pushcfunction(L, LuaAddLocationTrigger<TriggerKind::Location>);
  lua_setfield(L, -2, "add_location");

  lua_pushcfunction(L, LuaAddLocationTrigger<TriggerKind::LocationTimed>);
  lua_setfield(L, -2, "add_location_timed");

  lua_pushcfunction(L, LuaAddLocationSpecifiedTrigger);
  lua_setfield(L, -2, "add_location_specified");

  lua_pushcfunction(L, LuaAddLocationTrigger<TriggerKind::LocationAll>);
  lua_setfield(L, -2, "add_location_all");

  lua_pushcfunction(L, LuaAddInstantDeathTrigger);
  lua_setfield(L, -2, "add_instant_death");

  lua_pushcfunction(L, LuaAddInstantDisplaceTrigger);
  lua_setfield(L, -2, "add_instant_displace");

  lua_pushcfunction(L, LuaAddDeathTrigger);
  lua_setfield(L, -2, "add_death");

  lua_pushcfunction(L, LuaAddDoorTrigger<TriggerKind::Door>);
  lua_setfield(L, -2, "add_door");

  lua_pushcfunction(L, LuaAddDoorTrigger<TriggerKind::DoorOnce>);
  lua_setfield(L, -2, "add_door_once");

  lua_pushcfunction(L, LuaAddDoorsTrigger<TriggerKind::Door>);
  lua_setfield(L, -2, "add_doors");

  lua_pushcfunction(L, LuaAddDoorsTrigger<TriggerKind::DoorsEither>);
  lua_setfield(L, -2, "add_doors_either");

  lua_pushcfunction(L, LuaAddFourDoorsTrigger);
  lua_setfield(L, -2, "add_four_doors");

  lua_pushcfunction(L, LuaAddShotTrigger);
  lua_setfield(L, -2, "add_shot");

  lua_pushcfunction(L, LuaAddEscortTrigger);
  lua_setfield(L, -2, "add_escort");

  lua_pushcfunction(L, LuaAddProximityTrigger);
  lua_setfield(L, -2, "add_proximity");

  lua_pushcfunction(L, LuaAddBeingAttackedTrigger<TriggerKind::BeingAttacked>);
  lua_setfield(L, -2, "add_being_attacked");

  lua_pushcfunction(L, LuaAddBeingAttackedTrigger<TriggerKind::BeenAlerted>);
  lua_setfield(L, -2, "add_been_alerted");
  return 1;
}
} // namespace gk