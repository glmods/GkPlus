#pragma once

#include "List.h"
#include "Math.h"
#include "Memory.h"

#include <type_traits>

namespace gk {
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

// Payload of a trigger's target list: an actor-name entry created by CreateTrigger.
struct ITrigger {};

// The engine's `List<ITrigger *>`. Passed BY VALUE to RegisterTriggers, which is
// why List<T> must stay a trivially-copyable aggregate.
using TriggerList = List<ITrigger *>;
static_assert(sizeof(TriggerList) == 0x10);
static_assert(std::is_trivially_copyable_v<TriggerList>);

// pool_alloc'd (0x68) by RegisterTriggers and pool-freed by RemoveTrigger, which
// also frees the script name. The target-list sentinel is pool_alloc'd and then
// leaked, so it stays a raw pointer inside TriggerList.
struct TriggerData {
  TriggerKind kind;
  Vec3 coords[4];
  long long time_or_radius;
  float radius_squared;
  TriggerList targets;
  pool_unique_ptr<unsigned char> script_name;
  int team_or_warning;
  unsigned armed;
  int last_trigger_actor;
  int unused;
};
static_assert(sizeof(TriggerData) == 0x68);

// --- Native API over the trigger system -------------------------------------

TriggerList *CopyList(TriggerList *dst, TriggerList *src);   // 0x0044c950
TriggerList *InitList(TriggerList *list);                    // 0x0044ca10
TriggerList *InitListWithActorName(TriggerList *list,
                                   const char **name);       // 0x0044c900
ITrigger *CreateTrigger(TriggerList *list,
                        const char **actor_name);            // 0x0044e8c0
void DeleteList(TriggerList *list);                          // 0x0044ce40
// RegisterTriggers @ 0x0043e240 - `targets` is passed BY VALUE.
void RegisterTriggers(TriggerKind kind, Vec3 *coords, long long time_or_radius,
                      TriggerList targets, const unsigned char *script, int team);
// RemoveTrigger @ 0x0050c400.
TriggerData *RemoveTrigger(TriggerData *trigger, char c);
} // namespace gk
