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

// The engine's `List<ITrigger *>`. Passed BY VALUE to AddTriggerToGlobalList,
// which is why List<T> must stay a trivially-copyable aggregate. Confirmed by
// the callee's `RET 0x20` = 8 (long long) + 0x10 (TriggerList) + 4 + 4, with
// kind/coords in ECX/EDX; every call site shows the by-value slot as
// `SUB ESP,0x10 ; MOV ECX,ESP` immediately before the list ctor.
using TriggerList = List<ITrigger *>;
static_assert(sizeof(TriggerList) == 0x10);
static_assert(std::is_trivially_copyable_v<TriggerList>);

// pool_alloc'd (0x68) by AddTriggerToGlobalList and pool-freed by RemoveTrigger, which
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
// AddTriggerToGlobalList @ 0x0043e240 - registers exactly ONE trigger (it
// pool_allocs one 0x68 TriggerData and one 0x10 node, links it into the single
// global list at FirstTrigger and increments NumTriggers). `targets` is the new
// trigger's own list of actor names, not a list of triggers; the plural in the
// former name `RegisterTriggers` described the wrong thing.
//
// `targets` is passed BY VALUE, and CONSUMED: the last thing it does on every
// path, including the early-out when the executor is not running, is
// DeleteTriggers on its copy, which frees the sentinel InitList allocated.
// Calling DeleteList afterwards double-frees it - do not read the
// InitList/DeleteList pair above as bracketing this call.
//
// It copies every string it is given (strdup for `script`, malloc+strcpy per
// actor name), so caller-owned buffers only need to outlive the call. It also
// reads coords[0..3] unconditionally, with no null and no length check.
void AddTriggerToGlobalList(TriggerKind kind, Vec3 *coords,
                            long long time_or_radius, TriggerList targets,
                            const unsigned char *script, int team);
// RemoveTrigger @ 0x0050c400.
TriggerData *RemoveTrigger(TriggerData *trigger, char c);
} // namespace gk
