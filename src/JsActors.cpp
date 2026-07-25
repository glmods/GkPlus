#include "Actors.h"

#include "JsBindings.h"
#include "Roles.h"
#include "Tokens.h"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

// --- the class hierarchy -----------------------------------------------------
//
// One JSClassID per Actor subclass, prototypes chained to mirror Actors.h.
// Everything below is generated from ActorClasses.inc.h, which is also what
// pins the ordering the ladder and the chain builder both depend on.
//
// What the chain buys over one flat prototype: a subclass-only member simply is
// not there on the wrong actor, so `if (a.goto)` is a valid feature test and
// walking a whole prototype chain (a debug dump, an inspector pane) never trips
// an accessor that throws by construction - ten of the members here are
// subclass-only. `a instanceof actors.classes.MobileActor` works too.
//
// What it does NOT buy is safety. The prototype is chosen once, at wrap time,
// from what the pointer pointed at then; the wrapper holds a raw pointer into
// pool memory the game can free and recycle underneath us, and a recycled
// pointer can land on a different subclass. So the checked downcasts further
// down still re-run the RTTI predicate on every call. The chain is ergonomics
// and documentation; the downcast is the guard.

enum ActorClassIndex {
  ActorClassIndex_Actor,
#define GK_ACTOR_CLASS(Name, Parent, Predicate, Kind) ActorClassIndex_##Name,
#include "ActorClasses.inc.h"
#undef GK_ACTOR_CLASS
  ActorClassIndex_Count,
};

constexpr int ActorParentIndex[ActorClassIndex_Count] = {
    ActorClassIndex_Actor, // the root is its own parent; the walk stops here
#define GK_ACTOR_CLASS(Name, Parent, Predicate, Kind) ActorClassIndex_##Parent,
#include "ActorClasses.inc.h"
#undef GK_ACTOR_CLASS
};

// The ordering rule from ActorClasses.inc.h, enforced. Because every class is
// listed before its own base, a base's index is always greater than its
// derived classes' - except the root, which is index 0 by construction.
constexpr bool BasesFollowDerived() {
  for (int i = 1; i < ActorClassIndex_Count; ++i) {
    if (ActorParentIndex[i] != ActorClassIndex_Actor &&
        ActorParentIndex[i] <= i) {
      return false;
    }
  }
  return true;
}
static_assert(BasesFollowDerived(),
              "ActorClasses.inc.h must list every class before its base");

const char *const ActorKindNames[ActorClassIndex_Count] = {
    "actor",
#define GK_ACTOR_CLASS(Name, Parent, Predicate, Kind) Kind,
#include "ActorClasses.inc.h"
#undef GK_ACTOR_CLASS
};

JSClassID ActorClassIds[ActorClassIndex_Count];
JSClassID ActorsClassId;

// The wrapper holds the raw Actor*, per the design decision. `id` is a snapshot
// taken at wrap time and is never read back through `ptr`, which is what makes
// `valid` safe to ask on a wrapper whose actor is already gone: it consults our
// own copy of the id and the actor hash, never the actor itself.
//
// The trade-off this accepts: every other accessor dereferences `ptr` directly.
// The game frees actors (slots 64/65, and wholesale on level unload) with no
// notification, and pool_free recycles the page (Memory.h), so a wrapper kept
// across frames can end up aliasing an unrelated object. frag()/remove() null
// `ptr` because those are the two destructions we can actually see.
struct ActorWrapper {
  Actor *ptr;
  int id;
};

void ActorFinalizer(JSRuntime *rt, JSValueConst val) {
  // Frees the wrapper and nothing else - the Actor belongs to the game. Every
  // class in the hierarchy shares this, so the id has to come back out of the
  // object rather than being one we name.
  JSClassID id = 0;
  js_free_rt(rt, JS_GetAnyOpaque(val, &id));
}

const JSClassDef ActorClassDefs[ActorClassIndex_Count] = {
    {"Actor", ActorFinalizer, nullptr, nullptr, nullptr},
#define GK_ACTOR_CLASS(Name, Parent, Predicate, Kind)                          \
  {#Name, ActorFinalizer, nullptr, nullptr, nullptr},
#include "ActorClasses.inc.h"
#undef GK_ACTOR_CLASS
};

bool IsActorClassId(JSClassID id) {
  for (JSClassID candidate : ActorClassIds) {
    if (candidate != 0 && candidate == id) {
      return true;
    }
  }
  return false;
}

// The hierarchy costs us JS_GetOpaque2, which only ever matches a single class
// id. Membership in our own id table replaces it, and has to raise the
// TypeError that JS_GetOpaque2 used to raise for us.
ActorWrapper *WrapperOf(JSContext *ctx, JSValueConst self) {
  JSClassID id = 0;
  void *opaque = JS_GetAnyOpaque(self, &id);
  if (!IsActorClassId(id)) {
    JS_ThrowTypeError(ctx, "not an actor");
    return nullptr;
  }
  return static_cast<ActorWrapper *>(opaque);
}

Actor *Resolve(JSContext *ctx, JSValueConst self) {
  ActorWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return nullptr; // JS_GetOpaque2 already threw
  }
  if (!w->ptr) {
    JS_ThrowTypeError(ctx, "actor %d has been destroyed", w->id);
    return nullptr;
  }
  return w->ptr;
}

// The manual-RTTI slots 36-50 are inherited, so IsMobile() is true for a turret
// too - the tests have to run most-derived first, which is exactly the ordering
// rule ActorClasses.inc.h imposes for the prototype chain.
ActorClassIndex ClassIndexOf(Actor *a) {
#define GK_ACTOR_CLASS(Name, Parent, Predicate, Kind)                          \
  if (a->Predicate()) {                                                        \
    return ActorClassIndex_##Name;                                             \
  }
#include "ActorClasses.inc.h"
#undef GK_ACTOR_CLASS
  return ActorClassIndex_Actor;
}

const char *KindName(Actor *a) { return ActorKindNames[ClassIndexOf(a)]; }

// --- checked downcasts -------------------------------------------------------
//
// One prototype serves every subclass; the RTTI slot is the guard. Slot indices
// are branch-local, so each helper names the class whose vtable owns the slots
// it unlocks.

MobileActor *ResolveMobile(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return nullptr;
  }
  if (!a->IsMobile()) {
    JS_ThrowTypeError(ctx, "actor %d is a %s, not a mobile actor", a->id,
                      KindName(a));
    return nullptr;
  }
  return static_cast<MobileActor *>(a);
}

CharacterActor *ResolveCharacter(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return nullptr;
  }
  if (!a->IsCharacter()) {
    JS_ThrowTypeError(ctx, "actor %d is a %s, not a character actor", a->id,
                      KindName(a));
    return nullptr;
  }
  return static_cast<CharacterActor *>(a);
}

PickupActor *ResolvePickup(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return nullptr;
  }
  if (!a->IsPickup()) {
    JS_ThrowTypeError(ctx, "actor %d is a %s, not a pickup actor", a->id,
                      KindName(a));
    return nullptr;
  }
  return static_cast<PickupActor *>(a);
}

TurretActor *ResolveTurret(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return nullptr;
  }
  if (!a->IsTurret()) {
    JS_ThrowTypeError(ctx, "actor %d is a %s, not a turret actor", a->id,
                      KindName(a));
    return nullptr;
  }
  return static_cast<TurretActor *>(a);
}

// --- accessors ---------------------------------------------------------------

// The one raw-field read on the whole prototype, and it reads our snapshot
// rather than the actor: the collection is keyed by id and slot 78 SetTarget
// takes ids, so nothing else here works without it.
JSValue GetId(JSContext *ctx, JSValueConst self) {
  ActorWrapper *w = WrapperOf(ctx, self);
  return w ? JS_NewInt32(ctx, w->id) : JS_EXCEPTION;
}

// Never throws, so liveness can be tested without try/catch.
JSValue GetValid(JSContext *ctx, JSValueConst self) {
  ActorWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, w->ptr != nullptr && GetActorById(w->id) == w->ptr);
}

// Actors have no name field; the engine names them through the token table,
// where a token's value IS an actor id (see Tokens.h). This is the same lookup
// the console's own GETACTORNAME does.
JSValue GetName(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  Tokens *tokens = GetTokensTable();
  char *name = nullptr;
  if (!tokens || !FindTokenWithValue(tokens, static_cast<float>(a->id), &name) ||
      !name) {
    return JS_NULL;
  }
  return JS_NewString(ctx, name);
}

JSValue GetKind(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  return a ? JS_NewString(ctx, KindName(a)) : JS_EXCEPTION;
}

JSValue GetRole(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  return a->role ? NewRoleWrapper(ctx, a->role) : JS_NULL;
}

JSValue GetTarget(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  Actor *target = a->GetAttackTarget(); // slot 15
  return target ? NewActorWrapper(ctx, target) : JS_NULL;
}

JSValue GetCenter(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  Vec3 out{};
  a->GetCenterCoords(&out); // slot 4
  return NewVec3(ctx, out);
}

JSValue GetHotspotName(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  char *hotspot = a->GetHotspot(); // slot 31
  return hotspot ? JS_NewString(ctx, hotspot) : JS_NULL;
}

JSValue GetAmmo(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  return a ? JS_NewInt32(ctx, a->GetAmmoCount()) : JS_EXCEPTION; // slot 22
}

JSValue GetSize(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  return a ? JS_NewInt32(ctx, a->GetSize()) : JS_EXCEPTION; // slot 35
}

JSValue GetMine(JSContext *ctx, JSValueConst self) {
  Actor *a = Resolve(ctx, self);
  return a ? JS_NewBool(ctx, a->IsMine()) : JS_EXCEPTION; // slot 8
}

JSValue SetMine(JSContext *ctx, JSValueConst self, JSValueConst v) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  a->SetIsMine(JS_ToBool(ctx, v) != 0); // slot 9
  return JS_UNDEFINED;
}

JSValue GetTurretEnabled(JSContext *ctx, JSValueConst self) {
  TurretActor *t = ResolveTurret(ctx, self);
  return t ? JS_NewBool(ctx, t->IsTurretEnabled()) : JS_EXCEPTION; // slot 103
}

JSValue SetTurretEnabled(JSContext *ctx, JSValueConst self, JSValueConst v) {
  TurretActor *t = ResolveTurret(ctx, self);
  if (!t) {
    return JS_EXCEPTION;
  }
  t->SetTurretEnabled(JS_ToBool(ctx, v) != 0); // slot 100
  return JS_UNDEFINED;
}

// Read-only predicate slots. All of them are `return this->SlotN()`, so they
// share one body keyed by magic rather than fifteen near-identical functions.
enum ActorFlag {
  FlagAlive,          // slot 6
  FlagAttacking,      // slot 7
  FlagEnabled,        // slot 13
  FlagMoving,         // slot 14
  FlagVisible,        // slot 61
  FlagTargetable,     // slot 60
  FlagInteractable,   // slot 62
  FlagCanBePickedUp,  // slot 63
  FlagPendingOrders,  // slot 32
};

JSValue GetActorFlag(JSContext *ctx, JSValueConst self, int magic) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  bool value = false;
  switch (magic) {
  case FlagAlive:
    value = a->IsAlive();
    break;
  case FlagAttacking:
    value = a->IsAttacking();
    break;
  case FlagEnabled:
    value = a->IsEnabled();
    break;
  case FlagMoving:
    value = a->IsMoving();
    break;
  case FlagVisible:
    value = a->IsVisible();
    break;
  case FlagTargetable:
    value = a->IsTargetable();
    break;
  case FlagInteractable:
    value = a->IsInteractable();
    break;
  case FlagCanBePickedUp:
    value = a->CanBePickedUp();
    break;
  case FlagPendingOrders:
    value = a->HasPendingOrders();
    break;
  }
  return JS_NewBool(ctx, value);
}

// The out-parameter stat slots. Note the getter/setter slots are NOT adjacent:
// armor is 18/28 and shield is 19/29 (Actors.h:141-164).
enum ActorStat {
  StatHealth,        // slots 3 / 2
  StatArmor,         // slots 18 / 28
  StatShield,        // slots 19 / 29
  StatStrengthRatio, // slot 5, read-only: strength / character->strength
};

JSValue GetActorStat(JSContext *ctx, JSValueConst self, int magic) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  float value = 0.0f;
  switch (magic) {
  case StatHealth:
    a->GetHealth(&value);
    break;
  case StatArmor:
    a->GetArmorValue(&value);
    break;
  case StatShield:
    a->GetShieldValue(&value);
    break;
  case StatStrengthRatio:
    a->GetStrengthRatio(&value);
    break;
  }
  return JS_NewFloat64(ctx, value);
}

JSValue SetActorStat(JSContext *ctx, JSValueConst self, JSValueConst v,
                     int magic) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  float value = static_cast<float>(d);
  switch (magic) {
  case StatHealth:
    a->SetHealth(value);
    break;
  case StatArmor:
    a->SetArmorValue(value);
    break;
  case StatShield:
    a->SetShieldValue(value);
    break;
  }
  return JS_UNDEFINED;
}

// --- methods -----------------------------------------------------------------

JSValue ActorDamage(JSContext *ctx, JSValueConst self, int argc,
                    JSValueConst *argv) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "damage(amount, flag) expects an amount");
  }
  double amount = 0.0;
  if (JS_ToFloat64(ctx, &amount, argv[0])) {
    return JS_EXCEPTION;
  }
  bool flag = argc > 1 && JS_ToBool(ctx, argv[1]);
  // slot 68; a negative amount heals.
  return JS_NewBool(ctx, a->ApplyDamage(static_cast<float>(amount), flag));
}

// Both of the destructions we can see. Nulling `ptr` turns every later access
// into a TypeError instead of a write into recycled pool memory.
JSValue ActorFrag(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  ActorWrapper *w = WrapperOf(ctx, self);
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  a->Frag(); // slot 64
  w->ptr = nullptr;
  return JS_UNDEFINED;
}

JSValue ActorRemove(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  ActorWrapper *w = WrapperOf(ctx, self);
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  a->Delete(); // slot 65
  w->ptr = nullptr;
  return JS_UNDEFINED;
}

JSValue ActorAssociate(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "associate(script, one_shot) expects a script");
  }
  const char *script = JS_ToCString(ctx, argv[0]);
  if (!script) {
    return JS_EXCEPTION;
  }
  bool one_shot = argc > 1 && JS_ToBool(ctx, argv[1]);
  // slot 66. PickupActor strdups the name, so our buffer is not retained.
  a->Associate(const_cast<char *>(script), one_shot ? 1 : 0);
  JS_FreeCString(ctx, script);
  return JS_UNDEFINED;
}

JSValue ActorDissociate(JSContext *ctx, JSValueConst self, int,
                        JSValueConst *) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  a->Dissociate(); // slot 67
  return JS_UNDEFINED;
}

JSValue ActorSetTarget(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "set_target(id, mode) expects an actor id");
  }
  int32_t target = 0;
  int32_t mode = 0;
  if (JS_ToInt32(ctx, &target, argv[0])) {
    return JS_EXCEPTION;
  }
  if (argc > 1 && JS_ToInt32(ctx, &mode, argv[1])) {
    return JS_EXCEPTION;
  }
  a->SetTarget(target, mode); // slot 78
  return JS_UNDEFINED;
}

JSValue ActorClearTarget(JSContext *ctx, JSValueConst self, int,
                         JSValueConst *) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  a->ClearTarget(); // slot 79
  return JS_UNDEFINED;
}

JSValue ActorSetTeam(JSContext *ctx, JSValueConst self, int argc,
                     JSValueConst *argv) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "set_team(id) expects a team id");
  }
  int32_t team = 0;
  if (JS_ToInt32(ctx, &team, argv[0])) {
    return JS_EXCEPTION;
  }
  a->SetTeamID(team); // slot 33
  return JS_UNDEFINED;
}

JSValue ActorSetPosition(JSContext *ctx, JSValueConst self, int argc,
                         JSValueConst *argv) {
  Actor *a = Resolve(ctx, self);
  if (!a) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "set_position(position, orientation, stamp) expects a position");
  }
  Vec3 position = a->position;
  Vec4 orientation = a->orientation;
  if (!ToVec3(ctx, argv[0], &position)) {
    return JS_EXCEPTION;
  }
  if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
      !ToVec4(ctx, argv[1], &orientation)) {
    return JS_EXCEPTION;
  }
  // The third argument is stored to both +0xd8 (as float) and +0xdc (as int);
  // it reads as a timestamp and every known caller supplies one. 0 is inert.
  int32_t stamp = 0;
  if (argc > 2 && JS_ToInt32(ctx, &stamp, argv[2])) {
    return JS_EXCEPTION;
  }
  // slot 53 rather than a raw write to +0xa0: the raw store would skip the
  // orientation quaternion and the update timestamps.
  a->SetPositionAndOrientation(&position, &orientation, stamp);
  return JS_UNDEFINED;
}

JSValue ActorGoto(JSContext *ctx, JSValueConst self, int argc,
                  JSValueConst *argv) {
  MobileActor *m = ResolveMobile(ctx, self);
  if (!m) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "goto(destination, priority) expects a position");
  }
  Vec3 destination{};
  if (!ToVec3(ctx, argv[0], &destination)) {
    return JS_EXCEPTION;
  }
  double priority = 1.0;
  if (argc > 1 && JS_ToFloat64(ctx, &priority, argv[1])) {
    return JS_EXCEPTION;
  }
  // slot 88; gated on can_turn, strength and the current order's priority.
  return JS_NewInt32(ctx,
                     m->Goto(&destination, static_cast<float>(priority)));
}

JSValue ActorDie(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  ActorWrapper *w = WrapperOf(ctx, self);
  MobileActor *m = ResolveMobile(ctx, self);
  if (!m) {
    return JS_EXCEPTION;
  }
  m->Die(); // slot 89 - ends in slots 82 and 64, so the actor is gone
  w->ptr = nullptr;
  return JS_UNDEFINED;
}

JSValue ActorSetWeapon(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
  MobileActor *m = ResolveMobile(ctx, self);
  if (!m) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "set_weapon(type) expects a weapon type");
  }
  int32_t weapon = 0;
  if (JS_ToInt32(ctx, &weapon, argv[0])) {
    return JS_EXCEPTION;
  }
  m->SetWeapon(weapon); // slot 94; 0x21 = none
  return JS_UNDEFINED;
}

// The three trailing arguments of slots 96/97 are unidentified; they are passed
// through as-is and default to 0, which is what the engine's own callers use for
// an ordinary ranged attack.
JSValue ActorAttackTarget(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv) {
  CharacterActor *c = ResolveCharacter(ctx, self);
  if (!c) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "attack_target(actor) expects an actor");
  }
  // Any class in the hierarchy is an acceptable target, so this is the same
  // membership test the receiver goes through.
  ActorWrapper *target = WrapperOf(ctx, argv[0]);
  if (!target) {
    return JS_EXCEPTION;
  }
  if (!target->ptr) {
    return JS_ThrowTypeError(ctx, "actor %d has been destroyed", target->id);
  }
  int32_t args[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (argc > i + 1 && JS_ToInt32(ctx, &args[i], argv[i + 1])) {
      return JS_EXCEPTION;
    }
  }
  c->AttackTarget(target->ptr, args[0], static_cast<char>(args[1]),
                  static_cast<char>(args[2])); // slot 96
  return JS_UNDEFINED;
}

JSValue ActorAttackPosition(JSContext *ctx, JSValueConst self, int argc,
                            JSValueConst *argv) {
  CharacterActor *c = ResolveCharacter(ctx, self);
  if (!c) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "attack_position(position) expects a position");
  }
  Vec3 position{};
  if (!ToVec3(ctx, argv[0], &position)) {
    return JS_EXCEPTION;
  }
  int32_t args[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (argc > i + 1 && JS_ToInt32(ctx, &args[i], argv[i + 1])) {
      return JS_EXCEPTION;
    }
  }
  c->AttackPosition(&position, args[0], static_cast<char>(args[1]),
                    static_cast<char>(args[2])); // slot 97
  return JS_UNDEFINED;
}

JSValue ActorStopAttacking(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv) {
  CharacterActor *c = ResolveCharacter(ctx, self);
  if (!c) {
    return JS_EXCEPTION;
  }
  int32_t reason = 0;
  if (argc > 0 && JS_ToInt32(ctx, &reason, argv[0])) {
    return JS_EXCEPTION;
  }
  c->StopAttacking(reason); // slot 98
  return JS_UNDEFINED;
}

JSValue ActorSetAmmoType(JSContext *ctx, JSValueConst self, int argc,
                         JSValueConst *argv) {
  CharacterActor *c = ResolveCharacter(ctx, self);
  if (!c) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "set_ammo_type(type) expects an ammo type");
  }
  int32_t type = 0;
  if (JS_ToInt32(ctx, &type, argv[0])) {
    return JS_EXCEPTION;
  }
  c->SetAmmoType(type); // slot 99
  return JS_UNDEFINED;
}

JSValue ActorSetPickupEnabled(JSContext *ctx, JSValueConst self, int argc,
                              JSValueConst *argv) {
  PickupActor *p = ResolvePickup(ctx, self);
  if (!p) {
    return JS_EXCEPTION;
  }
  p->SetPickupEnabled(argc > 0 && JS_ToBool(ctx, argv[0])); // slot 83
  return JS_UNDEFINED;
}

JSValue ActorSetRequiredItem(JSContext *ctx, JSValueConst self, int argc,
                             JSValueConst *argv) {
  PickupActor *p = ResolvePickup(ctx, self);
  if (!p) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "set_required_item(name) expects a role name");
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }
  p->SetRequiredItem(name); // slot 85; frees the old name and strdups this one
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

JSValue ActorToString(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  ActorWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  char buf[64];
  if (!w->ptr) {
    std::snprintf(buf, sizeof(buf), "[Actor %d (destroyed)]", w->id);
  } else {
    std::snprintf(buf, sizeof(buf), "[Actor %d %s]", w->id, KindName(w->ptr));
  }
  return JS_NewString(ctx, buf);
}

const JSCFunctionListEntry ActorProto[] = {
    JS_CGETSET_DEF("id", GetId, nullptr),
    JS_CGETSET_DEF("name", GetName, nullptr),
    JS_CGETSET_DEF("kind", GetKind, nullptr),
    JS_CGETSET_DEF("valid", GetValid, nullptr),
    JS_CGETSET_DEF("role", GetRole, nullptr),
    JS_CGETSET_DEF("target", GetTarget, nullptr),
    JS_CGETSET_DEF("center", GetCenter, nullptr),
    JS_CGETSET_DEF("hotspot", GetHotspotName, nullptr),
    JS_CGETSET_DEF("ammo", GetAmmo, nullptr),
    JS_CGETSET_DEF("size", GetSize, nullptr),
    JS_CGETSET_DEF("mine", GetMine, SetMine),
    JS_CGETSET_MAGIC_DEF("alive", GetActorFlag, nullptr, FlagAlive),
    JS_CGETSET_MAGIC_DEF("attacking", GetActorFlag, nullptr, FlagAttacking),
    JS_CGETSET_MAGIC_DEF("enabled", GetActorFlag, nullptr, FlagEnabled),
    JS_CGETSET_MAGIC_DEF("moving", GetActorFlag, nullptr, FlagMoving),
    JS_CGETSET_MAGIC_DEF("visible", GetActorFlag, nullptr, FlagVisible),
    JS_CGETSET_MAGIC_DEF("targetable", GetActorFlag, nullptr, FlagTargetable),
    JS_CGETSET_MAGIC_DEF("interactable", GetActorFlag, nullptr,
                         FlagInteractable),
    JS_CGETSET_MAGIC_DEF("can_be_picked_up", GetActorFlag, nullptr,
                         FlagCanBePickedUp),
    JS_CGETSET_MAGIC_DEF("has_pending_orders", GetActorFlag, nullptr,
                         FlagPendingOrders),
    JS_CGETSET_MAGIC_DEF("health", GetActorStat, SetActorStat, StatHealth),
    JS_CGETSET_MAGIC_DEF("armor", GetActorStat, SetActorStat, StatArmor),
    JS_CGETSET_MAGIC_DEF("shield", GetActorStat, SetActorStat, StatShield),
    JS_CGETSET_MAGIC_DEF("strength_ratio", GetActorStat, nullptr,
                         StatStrengthRatio),
    JS_CFUNC_DEF("damage", 2, ActorDamage),
    JS_CFUNC_DEF("frag", 0, ActorFrag),
    JS_CFUNC_DEF("remove", 0, ActorRemove),
    JS_CFUNC_DEF("associate", 2, ActorAssociate),
    JS_CFUNC_DEF("dissociate", 0, ActorDissociate),
    JS_CFUNC_DEF("set_target", 2, ActorSetTarget),
    JS_CFUNC_DEF("clear_target", 0, ActorClearTarget),
    JS_CFUNC_DEF("set_team", 1, ActorSetTeam),
    JS_CFUNC_DEF("set_position", 3, ActorSetPosition),
    JS_CFUNC_DEF("toString", 0, ActorToString),
};

// Each of these carries exactly the members its class introduces; the rest is
// inherited through the chain. `goto` keeps the engine's own name for
// MobileActor slot 88 - reserved words are barred as identifiers, not as member
// names, so `actor.goto(dst, 1.0)` parses.
const JSCFunctionListEntry MobileActorProto[] = {
    JS_CFUNC_DEF("goto", 2, ActorGoto),
    JS_CFUNC_DEF("die", 0, ActorDie),
    JS_CFUNC_DEF("set_weapon", 1, ActorSetWeapon),
};

const JSCFunctionListEntry CharacterActorProto[] = {
    JS_CFUNC_DEF("attack_target", 4, ActorAttackTarget),
    JS_CFUNC_DEF("attack_position", 4, ActorAttackPosition),
    JS_CFUNC_DEF("stop_attacking", 1, ActorStopAttacking),
    JS_CFUNC_DEF("set_ammo_type", 1, ActorSetAmmoType),
};

const JSCFunctionListEntry TurretActorProto[] = {
    JS_CGETSET_DEF("turret_enabled", GetTurretEnabled, SetTurretEnabled),
};

const JSCFunctionListEntry PickupActorProto[] = {
    JS_CFUNC_DEF("set_pickup_enabled", 1, ActorSetPickupEnabled),
    JS_CFUNC_DEF("set_required_item", 1, ActorSetRequiredItem),
};

struct ProtoProps {
  const JSCFunctionListEntry *entries;
  int len;
};

// Only five of the sixteen classes add anything. The other eleven still get a
// prototype of their own, so the chain - and therefore instanceof - tells the
// truth about a class that happens to add no JS surface yet.
ProtoProps PropsFor(int index) {
  switch (index) {
  case ActorClassIndex_Actor:
    return {ActorProto, static_cast<int>(std::size(ActorProto))};
  case ActorClassIndex_MobileActor:
    return {MobileActorProto, static_cast<int>(std::size(MobileActorProto))};
  case ActorClassIndex_CharacterActor:
    return {CharacterActorProto,
            static_cast<int>(std::size(CharacterActorProto))};
  case ActorClassIndex_TurretActor:
    return {TurretActorProto, static_cast<int>(std::size(TurretActorProto))};
  case ActorClassIndex_PickupActor:
    return {PickupActorProto, static_cast<int>(std::size(PickupActorProto))};
  default:
    return {nullptr, 0};
  }
}

// Registers all sixteen classes and chains their prototypes. The root first,
// then the rest in reverse listing order - which is base-before-derived, by the
// ordering rule BasesFollowDerived() asserts.
bool RegisterActorClasses(JSContext *ctx) {
  ProtoProps root = PropsFor(ActorClassIndex_Actor);
  if (!EnsureClass(ctx, &ActorClassIds[ActorClassIndex_Actor],
                   &ActorClassDefs[ActorClassIndex_Actor], root.entries,
                   root.len, JS_UNDEFINED)) {
    return false;
  }
  for (int i = ActorClassIndex_Count - 1; i >= 1; --i) {
    // Per-context, so it has to be re-read rather than cached across contexts.
    JSValue parent = JS_GetClassProto(ctx, ActorClassIds[ActorParentIndex[i]]);
    ProtoProps props = PropsFor(i);
    bool ok = EnsureClass(ctx, &ActorClassIds[i], &ActorClassDefs[i],
                          props.entries, props.len, parent);
    JS_FreeValue(ctx, parent);
    if (!ok) {
      return false;
    }
  }
  return true;
}

JSValue ThrowNotConstructible(JSContext *ctx, JSValueConst, int,
                              JSValueConst *) {
  return JS_ThrowTypeError(ctx,
                           "actors are created by the game, not by script");
}

// `actors.classes` - one handle per class, purely so the brand checks work:
// `a instanceof actors.classes.MobileActor`, and a real `constructor.name` on
// every prototype. Calling one throws; there is no way to make an Actor from
// script.
JSValue NewActorClassesObject(JSContext *ctx) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  for (int i = 0; i < ActorClassIndex_Count; ++i) {
    const char *name = ActorClassDefs[i].class_name;
    JSValue proto = JS_GetClassProto(ctx, ActorClassIds[i]);
    JSValue ctor = JS_NewCFunction(ctx, ThrowNotConstructible, name, 0);
    // Sets ctor.prototype and proto.constructor, both non-enumerable.
    if (JS_IsException(ctor) || JS_SetConstructor(ctx, ctor, proto) < 0) {
      JS_FreeValue(ctx, proto);
      JS_FreeValue(ctx, ctor);
      JS_FreeValue(ctx, obj);
      return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, proto);
    if (JS_SetPropertyStr(ctx, obj, name, ctor) < 0) {
      JS_FreeValue(ctx, obj);
      return JS_EXCEPTION;
    }
  }
  return obj;
}

// --- the collection ----------------------------------------------------------

JSValue LookupActorById(JSContext *ctx, int id) {
  Actor *a = GetActorById(id);
  return a ? NewActorWrapper(ctx, a) : JS_UNDEFINED;
}

JSValue LookupActorByName(JSContext *ctx, const char *name) {
  Tokens *tokens = GetTokensTable();
  float value = 0.0f;
  if (!tokens || !GetTokenValue(tokens, &value, name)) {
    return JS_UNDEFINED;
  }
  // The token's value is the actor id, rounded - exactly what
  // ConsoleParseActorName does.
  return LookupActorById(ctx, static_cast<int>(std::lround(value)));
}

// Actors are keyed by id, so the enumerable keys are the ids in decimal. Names
// are an access convenience on top and are deliberately not enumerated - a token
// only names some actors, and listing both would double up.
void CollectActorKeys(std::vector<std::string> *out) {
  Actors *table = GetActorsTable();
  if (!table) {
    return;
  }
  out->reserve(table->size());
  char buf[16];
  for (Actor *a : *table) {
    if (a) {
      std::snprintf(buf, sizeof(buf), "%d", a->id);
      out->emplace_back(buf);
    }
  }
}

unsigned CountActors() {
  Actors *table = GetActorsTable();
  return table ? table->size() : 0;
}

const CollectionOps ActorsOps = {
    .class_name = "Actors",
    .lookup_id = LookupActorById,
    .lookup_name = LookupActorByName,
    .collect_keys = CollectActorKeys,
    .count = CountActors,
    .assign = nullptr, // read-only: `actors[1] = x` is a TypeError in a module
    .props = nullptr,
    .props_len = 0,
};

} // namespace

JSValue NewActorWrapper(JSContext *ctx, Actor *actor) {
  // The RTTI ladder runs once, here, and fixes the prototype for the wrapper's
  // whole life. That is sound only because an Actor never changes class - but
  // see the note on ActorWrapper for why it does not make the checked downcasts
  // redundant.
  JSClassID class_id = ActorClassIds[ClassIndexOf(actor)];
  if (!class_id) {
    return JS_ThrowInternalError(ctx, "the Actor classes are not registered");
  }
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(class_id));
  if (JS_IsException(obj)) {
    return obj;
  }
  auto *w = static_cast<ActorWrapper *>(js_malloc(ctx, sizeof(ActorWrapper)));
  if (!w) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  w->ptr = actor;
  w->id = actor->id;
  JS_SetOpaque(obj, w);
  return obj;
}

JSValue NewActorsNamespace(JSContext *ctx) {
  if (!RegisterActorClasses(ctx)) {
    return JS_EXCEPTION;
  }
  JSValue ns = NewCollection(ctx, &ActorsClassId, &ActorsOps);
  if (JS_IsException(ns)) {
    return ns;
  }
  JSValue classes = NewActorClassesObject(ctx);
  if (JS_IsException(classes)) {
    JS_FreeValue(ctx, ns);
    return JS_EXCEPTION;
  }
  // Non-enumerable, like `count`: an enumerable own property would show up in
  // Object.keys(actors) beside the actor ids. And like `count`, being an own
  // property means it shadows the indexer (quickjs.c:8734) - a token literally
  // named "classes" would be unreachable as actors["classes"].
  if (JS_DefinePropertyValueStr(ctx, ns, "classes", classes,
                                JS_PROP_CONFIGURABLE) < 0) {
    JS_FreeValue(ctx, ns);
    return JS_EXCEPTION;
  }
  return ns;
}

} // namespace gk::js
