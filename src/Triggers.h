#pragma once

#include "List.h"
#include "Math.h"
#include "Memory.h"

#include <type_traits>

namespace gk {
/// Which condition a trigger fires on. The 22 kinds and the console syntax
/// that creates each are in `trigger_system_notes.md`; the enum's values are
/// the engine's own, taken from the `ADD TRIGGER` keyword table.
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

// pool_alloc'd (0x68) by AddTriggerToGlobalList and pool-freed by
// RemoveTriggerFromGlobalList, which also frees the script name. The target-list
// sentinel is pool_alloc'd and then leaked, so it stays a raw pointer inside
// TriggerList.
struct TriggerData;

// The node the global trigger list is built from - the ordinary AvP node
// template, nothing bespoke. 0x10 bytes with `data` at 0x0c, which is what the
// DB's own `Trigger` (`base` at 0x00 + `data` at 0x0c) records and what the tail
// insert in AddTriggerToGlobalList walks.
using TriggerNodeBase = List_Member_Base<TriggerData *>;
using TriggerNode = List_Member<TriggerData *>;
static_assert(sizeof(TriggerNode) == 0x10);

// The global list of registered triggers @ 0x006af858, and it is the
// **pointer-first** list header - CLAUDE.md's second, incompatible shape - so
// `List<T>` does not describe it and must not be used here. The sentinel is a
// separate heap node behind a pointer, so `count` sits at +0x04 where `List<T>`
// puts it at +0x0c; modelling one as the other shifts every field by 8.
// Confirmed by the four consecutive globals (`FirstTrigger` 0x006af858,
// `NumTriggers` 0x006af85c, the cache pointer 0x006af860 and its valid flag
// 0x006af864) and by AddTriggerToGlobalList invalidating the last two on every
// insert.
struct TriggerListHead {
  TriggerNodeBase *sentinel;
  int count;
  void *cache;
  bool cache_valid;
};

/// One registered trigger, 0x68 bytes: the engine's record of "when this
/// happens, run that script". One layout serves all 22 kinds, so which of
/// `coords`, `time_or_radius` and `team_or_warning` carries meaning depends on
/// `kind`; `trigger_system_notes.md` has the per-kind table.
///
/// `script_name` is where the `{kind, body}` envelope lives
/// (`src/ScriptQueue.h`), so it is not always a file name, and `SaveGame`
/// writes it verbatim.
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
/// Initialises \p list as an empty target list: sentinel, zero count.
/// \return \p list.
TriggerList *InitList(TriggerList *list);                    // 0x0044ca10
/// Initialises \p list with one entry. At every call site in the game the
/// element is the empty string, not an actor name, despite the exported name.
/// \return \p list.
TriggerList *InitListWithActorName(TriggerList *list,
                                   const char **name);       // 0x0044c900
/// Builds a trigger over \p list and registers it in the global trigger list.
/// \return the new trigger, owned by that global list.
ITrigger *CreateTrigger(TriggerList *list,
                        const char **actor_name);            // 0x0044e8c0
/// Destroys every node of \p list and its sentinel. The header itself is the
/// caller's.
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
// RemoveTriggerFromGlobalList @ 0x0050c400 - `__thiscall(TriggerData * /*ECX*/,
// byte free_flag /*stack*/)`. Frees every target name and the target list, frees
// `script_name`, walks the global list for the node whose `data` is this trigger
// and unlinks it, then decrements NumTriggers. `free_flag & 1` also
// `free_sized`s the 0x68 TriggerData, which is the MSVC scalar-deleting-
// destructor flag: pass 1 to destroy it outright.
//
// **Not idempotent, and it does not check that it found the node.** The
// `NumTriggers--` is reached from the not-found path too, so calling it twice on
// one trigger corrupts the count and double-frees. Every caller must prove the
// trigger is still linked first - `TriggerIsRegistered` below.
TriggerData *RemoveTriggerFromGlobalList(TriggerData *trigger, char free_flag);

// The global list header @ 0x006af858. Never null once a level is running; the
// sentinel itself is allocated by whatever first registers a trigger.
TriggerListHead *GetTriggerList();

// The trigger AddTriggerToGlobalList just registered, or null if it registered
// nothing - which is the normal outcome when the executor is not running, since
// that call early-outs silently. It inserts at the **tail** of the circular
// list, so this is `sentinel->prev`.
TriggerData *LastRegisteredTrigger();

// Whether `trigger` is still linked into the global list. This is the only way
// to test a retained TriggerData *: EvaluateTriggers destroys triggers itself
// (five call sites) and DeleteAllTriggers takes the lot at level teardown, both
// with no notification, so a pointer held across a frame can dangle.
bool TriggerIsRegistered(const TriggerData *trigger);
} // namespace gk
