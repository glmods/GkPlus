#include "Roles.h"

#include "Actors.h"
#include "JsBindings.h"

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

JSClassID RoleClassId;
JSClassID RolesClassId;

// Same shape and the same trade-off as ActorWrapper (see JsActors.cpp). Roles
// live longer than actors - they are destroyed wholesale by DestroyRoles on
// level unload rather than one at a time - so the dangling window is coarser,
// but it is the same window.
struct RoleWrapper {
  Role *ptr;
  int id;
};

void RoleFinalizer(JSRuntime *rt, JSValueConst val) {
  js_free_rt(rt, JS_GetOpaque(val, RoleClassId));
}

const JSClassDef RoleClass = {
    "Role", RoleFinalizer, nullptr, nullptr, nullptr,
};

RoleWrapper *WrapperOf(JSContext *ctx, JSValueConst self) {
  return static_cast<RoleWrapper *>(JS_GetOpaque2(ctx, self, RoleClassId));
}

Role *Resolve(JSContext *ctx, JSValueConst self) {
  RoleWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return nullptr;
  }
  if (!w->ptr) {
    JS_ThrowTypeError(ctx, "role %d has been destroyed", w->id);
    return nullptr;
  }
  return w->ptr;
}

JSValue NewStringOrNull(JSContext *ctx, const char *s) {
  return s ? JS_NewString(ctx, s) : JS_NULL;
}

// --- accessors ---------------------------------------------------------------

JSValue GetId(JSContext *ctx, JSValueConst self) {
  RoleWrapper *w = WrapperOf(ctx, self);
  return w ? JS_NewInt32(ctx, w->id) : JS_EXCEPTION;
}

JSValue GetValid(JSContext *ctx, JSValueConst self) {
  RoleWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, w->ptr != nullptr && GetRoleById(w->id) == w->ptr);
}

JSValue GetName(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewStringOrNull(ctx, r->name.get()) : JS_EXCEPTION;
}

JSValue GetHotspot(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewStringOrNull(ctx, r->hotspot.get()) : JS_EXCEPTION;
}

JSValue GetAlternateHotspot(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewStringOrNull(ctx, r->alternate_hotspot.get()) : JS_EXCEPTION;
}

JSValue GetMetaSound(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewStringOrNull(ctx, r->meta_sound.get()) : JS_EXCEPTION;
}

JSValue GetAi(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewStringOrNull(ctx, AITypeName(r->ai)) : JS_EXCEPTION;
}

JSValue SetAi(JSContext *ctx, JSValueConst self, JSValueConst v) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  const char *name = JS_ToCString(ctx, v);
  if (!name) {
    return JS_EXCEPTION;
  }
  AIType type = AITypeFromName(name);
  if (type == AIType::Count) {
    JSValue err = JS_ThrowRangeError(ctx, "unknown AI type '%s'", name);
    JS_FreeCString(ctx, name);
    return err;
  }
  JS_FreeCString(ctx, name);
  r->ai = type;
  return JS_UNDEFINED;
}

JSValue GetHotspotPoint(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewVec3(ctx, r->hotspot_point) : JS_EXCEPTION;
}

JSValue GetAlternateHotspotPoint(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  return r ? NewVec3(ctx, r->alternate_hotspot_point) : JS_EXCEPTION;
}

// The sever-point names, split from the GLS `sever point` field on ','.
JSValue GetSeverPoints(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) {
    return arr;
  }
  uint32_t i = 0;
  for (const pool_string &point : r->sever_points) {
    if (!point) {
      continue;
    }
    if (JS_SetPropertyUint32(ctx, arr, i++,
                             JS_NewString(ctx, point.get())) < 0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }
  return arr;
}

enum RoleStat {
  StatArmor,
  StatShields,
  StatRechargeRate,
  StatResistanceFactor,
  StatAlpha,
};

JSValue GetRoleStat(JSContext *ctx, JSValueConst self, int magic) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  float value = 0.0f;
  switch (magic) {
  case StatArmor:
    value = r->armor;
    break;
  case StatShields:
    value = r->shields;
    break;
  case StatRechargeRate:
    value = r->recharge_rate;
    break;
  case StatResistanceFactor:
    value = r->resistance_factor;
    break;
  case StatAlpha:
    value = r->alpha;
    break;
  }
  return JS_NewFloat64(ctx, value);
}

JSValue SetRoleStat(JSContext *ctx, JSValueConst self, JSValueConst v,
                    int magic) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v)) {
    return JS_EXCEPTION;
  }
  float value = static_cast<float>(d);
  switch (magic) {
  case StatArmor:
    r->armor = value;
    break;
  case StatShields:
    r->shields = value;
    break;
  case StatRechargeRate:
    r->recharge_rate = value;
    break;
  case StatResistanceFactor:
    r->resistance_factor = value;
    break;
  case StatAlpha:
    r->alpha = value;
    break;
  }
  return JS_UNDEFINED;
}

enum RoleCount {
  CountResistance,
  CountLimit,
  CountHierNodes26To30,
  CountHierNodes21To25,
};

JSValue GetRoleCount(JSContext *ctx, JSValueConst self, int magic) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  int value = 0;
  switch (magic) {
  case CountResistance:
    value = r->resistance;
    break;
  case CountLimit:
    value = r->limit;
    break;
  case CountHierNodes26To30:
    value = r->num_hier_nodes_26_30;
    break;
  case CountHierNodes21To25:
    value = r->num_hier_nodes_21_25;
    break;
  }
  return JS_NewInt32(ctx, value);
}

// The ten packed booleans at Role+0x78. Bitfields have no address, so each one
// needs its own read and write rather than a generic field reference.
enum RoleFlag {
  FlagAlphaFogging,
  FlagPerVertexFogging,
  FlagNoLighting,
  FlagReflective,
  FlagDestinationSelectable,
  FlagDestroyAfterCollection,
  FlagHitTestIgnore,
  FlagFragControl,
  FlagMovesOnLifts,
  FlagStatusDisplay,
};

JSValue GetRoleFlag(JSContext *ctx, JSValueConst self, int magic) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  bool value = false;
  switch (magic) {
  case FlagAlphaFogging:
    value = r->alpha_fogging;
    break;
  case FlagPerVertexFogging:
    value = r->per_vertex_fogging;
    break;
  case FlagNoLighting:
    value = r->no_lighting;
    break;
  case FlagReflective:
    value = r->reflective;
    break;
  case FlagDestinationSelectable:
    value = r->destination_selectable;
    break;
  case FlagDestroyAfterCollection:
    value = r->destroy_after_collection;
    break;
  case FlagHitTestIgnore:
    value = r->hit_test_ignore;
    break;
  case FlagFragControl:
    value = r->frag_control;
    break;
  case FlagMovesOnLifts:
    value = r->moves_on_lifts;
    break;
  case FlagStatusDisplay:
    value = r->status_display;
    break;
  }
  return JS_NewBool(ctx, value);
}

JSValue SetRoleFlag(JSContext *ctx, JSValueConst self, JSValueConst v,
                    int magic) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  bool value = JS_ToBool(ctx, v) != 0;
  switch (magic) {
  case FlagAlphaFogging:
    r->alpha_fogging = value;
    break;
  case FlagPerVertexFogging:
    r->per_vertex_fogging = value;
    break;
  case FlagNoLighting:
    r->no_lighting = value;
    break;
  case FlagReflective:
    r->reflective = value;
    break;
  case FlagDestinationSelectable:
    r->destination_selectable = value;
    break;
  case FlagDestroyAfterCollection:
    r->destroy_after_collection = value;
    break;
  case FlagHitTestIgnore:
    r->hit_test_ignore = value;
    break;
  case FlagFragControl:
    r->frag_control = value;
    break;
  case FlagMovesOnLifts:
    r->moves_on_lifts = value;
    break;
  case FlagStatusDisplay:
    r->status_display = value;
    break;
  }
  return JS_UNDEFINED;
}

// --- sub-object snapshots ----------------------------------------------------
//
// These are pool_unique_ptr members with no independent identity and no id to
// re-resolve by, so a wrapper class would be the wrong shape: a plain snapshot
// built on demand is exactly what they are.

void SetFloat(JSContext *ctx, JSValue obj, const char *name, float value) {
  JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, value));
}

JSValue GetCharacterSnapshot(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  Character *c = r->character.get();
  if (!c) {
    return JS_NULL;
  }
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  // Raw stored values throughout, like turning_speed and the angles below, which
  // are BAM rather than revolutions and degrees. walking_speed is therefore the
  // 16.16 fixed point, so divide by 65536 for the cycles/sec `make.role` takes.
  // It is an int in the game; reading it as a float reported a denormal.
  SetFloat(ctx, obj, "walking_speed", c->walking_speed);
  SetFloat(ctx, obj, "turning_speed", c->turning_speed);
  SetFloat(ctx, obj, "aim", c->aim);
  SetFloat(ctx, obj, "scan_delay", c->scan_delay);
  SetFloat(ctx, obj, "sight_angle", c->sight_angle);
  SetFloat(ctx, obj, "sight_range", c->sight_range);
  SetFloat(ctx, obj, "hearing_range", c->hearing_range);
  SetFloat(ctx, obj, "alert_radius", c->alert_radius);
  SetFloat(ctx, obj, "aggression", c->aggression);
  SetFloat(ctx, obj, "size", c->size);
  SetFloat(ctx, obj, "damage_multiplier", c->damage_multiplier);
  SetFloat(ctx, obj, "shot_speed_multiplier", c->shot_speed_multiplier);
  SetFloat(ctx, obj, "target_cycle_delay", c->target_cycle_delay);
  SetFloat(ctx, obj, "alarm_delay", c->alarm_delay);
  SetFloat(ctx, obj, "weapon_cycle_time", c->weapon_cycle_time);
  SetFloat(ctx, obj, "strength", c->strength);
  JS_SetPropertyStr(ctx, obj, "max_weapon", JS_NewInt32(ctx, c->max_weapon));
  JS_SetPropertyStr(ctx, obj, "max_ammo", JS_NewInt32(ctx, c->max_ammo));
  JS_SetPropertyStr(ctx, obj, "max_module", JS_NewInt32(ctx, c->max_module));
  JS_SetPropertyStr(ctx, obj, "weapon", JS_NewInt32(ctx, c->weapon));
  JS_SetPropertyStr(ctx, obj, "secondary_weapon",
                    JS_NewInt32(ctx, c->secondary_weapon));
  JS_SetPropertyStr(ctx, obj, "can_turn", JS_NewBool(ctx, c->can_turn));
  JS_SetPropertyStr(ctx, obj, "alertable", JS_NewBool(ctx, c->alertable));
  JS_SetPropertyStr(ctx, obj, "always_cpu_controlled",
                    JS_NewBool(ctx, c->always_cpu_controlled));
  JS_SetPropertyStr(ctx, obj, "description",
                    NewStringOrNull(ctx, c->description));
  return obj;
}

JSValue GetProjectileSnapshot(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  Projectile *p = r->projectile.get();
  if (!p) {
    return JS_NULL;
  }
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "gravity", JS_NewBool(ctx, p->gravity));
  SetFloat(ctx, obj, "damage", p->damage); // negative heals
  JS_SetPropertyStr(ctx, obj, "sound", JS_NewInt32(ctx, p->sound));
  SetFloat(ctx, obj, "max_range", p->max_range);
  SetFloat(ctx, obj, "blast_damage", p->blast_damage);
  SetFloat(ctx, obj, "blast_range", p->blast_range);
  return obj;
}

JSValue GetLightSnapshot(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  Light *l = r->light.get();
  if (!l) {
    return JS_NULL;
  }
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  SetFloat(ctx, obj, "red", l->red);
  SetFloat(ctx, obj, "green", l->green);
  SetFloat(ctx, obj, "blue", l->blue);
  SetFloat(ctx, obj, "specular_red", l->specular_red);
  SetFloat(ctx, obj, "specular_green", l->specular_green);
  SetFloat(ctx, obj, "specular_blue", l->specular_blue);
  SetFloat(ctx, obj, "range", l->range);
  return obj;
}

JSValue GetInventoryInfoSnapshot(JSContext *ctx, JSValueConst self) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  InventoryInfo *info = r->inventory_info.get();
  if (!info) {
    return JS_NULL;
  }
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  JS_SetPropertyStr(ctx, obj, "description",
                    NewStringOrNull(ctx, info->description));
  JS_SetPropertyStr(ctx, obj, "pickup_name",
                    NewStringOrNull(ctx, info->pickup_name));
  SetFloat(ctx, obj, "pickup_radius", info->pickup_radius);
  JS_SetPropertyStr(ctx, obj, "action_on_death",
                    JS_NewInt32(ctx, info->action_on_death));
  return obj;
}

// --- methods -----------------------------------------------------------------

JSValue RoleSpawn(JSContext *ctx, JSValueConst self, int argc,
                  JSValueConst *argv) {
  Role *r = Resolve(ctx, self);
  if (!r) {
    return JS_EXCEPTION;
  }
  int32_t team = 0;
  if (argc > 0 && JS_ToInt32(ctx, &team, argv[0])) {
    return JS_EXCEPTION;
  }
  Vec3 position{};
  if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
      !ToVec3(ctx, argv[1], &position)) {
    return JS_EXCEPTION;
  }
  Vec4 orientation{0.0f, 0.0f, 0.0f, 1.0f}; // identity quaternion
  if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]) &&
      !ToVec4(ctx, argv[2], &orientation)) {
    return JS_EXCEPTION;
  }
  int32_t owner = 0;
  if (argc > 3 && JS_ToInt32(ctx, &owner, argv[3])) {
    return JS_EXCEPTION;
  }

  int id = SpawnRole(team, r, &position, &orientation, owner);
  Actor *spawned = GetActorById(id);
  return spawned ? NewActorWrapper(ctx, spawned) : JS_NULL;
}

JSValue RoleToString(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  RoleWrapper *w = WrapperOf(ctx, self);
  if (!w) {
    return JS_EXCEPTION;
  }
  char buf[128];
  const char *name = w->ptr ? w->ptr->name.get() : nullptr;
  std::snprintf(buf, sizeof(buf), "[Role %d %s]", w->id,
                name ? name : (w->ptr ? "(unnamed)" : "(destroyed)"));
  return JS_NewString(ctx, buf);
}

const JSCFunctionListEntry RoleProto[] = {
    JS_CGETSET_DEF("id", GetId, nullptr),
    JS_CGETSET_DEF("name", GetName, nullptr),
    JS_CGETSET_DEF("valid", GetValid, nullptr),
    JS_CGETSET_DEF("ai", GetAi, SetAi),
    JS_CGETSET_DEF("hotspot", GetHotspot, nullptr),
    JS_CGETSET_DEF("alternate_hotspot", GetAlternateHotspot, nullptr),
    JS_CGETSET_DEF("hotspot_point", GetHotspotPoint, nullptr),
    JS_CGETSET_DEF("alternate_hotspot_point", GetAlternateHotspotPoint,
                   nullptr),
    JS_CGETSET_DEF("meta_sound", GetMetaSound, nullptr),
    JS_CGETSET_DEF("sever_points", GetSeverPoints, nullptr),
    JS_CGETSET_DEF("character", GetCharacterSnapshot, nullptr),
    JS_CGETSET_DEF("projectile", GetProjectileSnapshot, nullptr),
    JS_CGETSET_DEF("light", GetLightSnapshot, nullptr),
    JS_CGETSET_DEF("inventory_info", GetInventoryInfoSnapshot, nullptr),
    JS_CGETSET_MAGIC_DEF("armor", GetRoleStat, SetRoleStat, StatArmor),
    JS_CGETSET_MAGIC_DEF("shields", GetRoleStat, SetRoleStat, StatShields),
    JS_CGETSET_MAGIC_DEF("recharge_rate", GetRoleStat, SetRoleStat,
                         StatRechargeRate),
    JS_CGETSET_MAGIC_DEF("resistance_factor", GetRoleStat, SetRoleStat,
                         StatResistanceFactor),
    JS_CGETSET_MAGIC_DEF("alpha", GetRoleStat, SetRoleStat, StatAlpha),
    JS_CGETSET_MAGIC_DEF("resistance", GetRoleCount, nullptr, CountResistance),
    JS_CGETSET_MAGIC_DEF("limit", GetRoleCount, nullptr, CountLimit),
    JS_CGETSET_MAGIC_DEF("num_hier_nodes_26_30", GetRoleCount, nullptr,
                         CountHierNodes26To30),
    JS_CGETSET_MAGIC_DEF("num_hier_nodes_21_25", GetRoleCount, nullptr,
                         CountHierNodes21To25),
    JS_CGETSET_MAGIC_DEF("alpha_fogging", GetRoleFlag, SetRoleFlag,
                         FlagAlphaFogging),
    JS_CGETSET_MAGIC_DEF("per_vertex_fogging", GetRoleFlag, SetRoleFlag,
                         FlagPerVertexFogging),
    JS_CGETSET_MAGIC_DEF("no_lighting", GetRoleFlag, SetRoleFlag,
                         FlagNoLighting),
    JS_CGETSET_MAGIC_DEF("reflective", GetRoleFlag, SetRoleFlag,
                         FlagReflective),
    JS_CGETSET_MAGIC_DEF("destination_selectable", GetRoleFlag, SetRoleFlag,
                         FlagDestinationSelectable),
    JS_CGETSET_MAGIC_DEF("destroy_after_collection", GetRoleFlag, SetRoleFlag,
                         FlagDestroyAfterCollection),
    JS_CGETSET_MAGIC_DEF("hit_test_ignore", GetRoleFlag, SetRoleFlag,
                         FlagHitTestIgnore),
    JS_CGETSET_MAGIC_DEF("frag_control", GetRoleFlag, SetRoleFlag,
                         FlagFragControl),
    JS_CGETSET_MAGIC_DEF("moves_on_lifts", GetRoleFlag, SetRoleFlag,
                         FlagMovesOnLifts),
    JS_CGETSET_MAGIC_DEF("status_display", GetRoleFlag, SetRoleFlag,
                         FlagStatusDisplay),
    JS_CFUNC_DEF("spawn", 4, RoleSpawn),
    JS_CFUNC_DEF("toString", 0, RoleToString),
};

// --- the collection ----------------------------------------------------------

JSValue LookupRoleById(JSContext *ctx, int id) {
  Role *r = GetRoleById(id);
  return r ? NewRoleWrapper(ctx, r) : JS_UNDEFINED;
}

JSValue LookupRoleByName(JSContext *ctx, const char *name) {
  Role *r = GetRoleByName(name);
  return r ? NewRoleWrapper(ctx, r) : JS_UNDEFINED;
}

// Keyed by id like actors, with the name as an access convenience on top.
void CollectRoleKeys(std::vector<std::string> *out) {
  Roles *table = GetRolesTable();
  if (!table) {
    return;
  }
  out->reserve(table->size());
  char buf[16];
  for (Role *r : *table) {
    if (r) {
      std::snprintf(buf, sizeof(buf), "%d", r->id);
      out->emplace_back(buf);
    }
  }
}

unsigned CountRoles() {
  Roles *table = GetRolesTable();
  return table ? table->size() : 0;
}

const CollectionOps RolesOps = {
    .class_name = "Roles",
    .lookup_id = LookupRoleById,
    .lookup_name = LookupRoleByName,
    .collect_keys = CollectRoleKeys,
    .count = CountRoles,
    .assign = nullptr, // read-only; mutate through the Role wrapper instead
    .props = nullptr,
    .props_len = 0,
};

// { bot: 0, scavenger: 1, ... }, built from AITypeName rather than a second copy
// of the table in Roles.cpp.
JSValue NewAiTypes(JSContext *ctx) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  for (int i = 0; i < static_cast<int>(AIType::Count); ++i) {
    const char *name = AITypeName(static_cast<AIType>(i));
    if (name) {
      JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, i));
    }
  }
  return obj;
}

} // namespace

JSValue NewRoleWrapper(JSContext *ctx, Role *role) {
  if (!RoleClassId) {
    return JS_ThrowInternalError(ctx, "the Role class is not registered");
  }
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(RoleClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  auto *w = static_cast<RoleWrapper *>(js_malloc(ctx, sizeof(RoleWrapper)));
  if (!w) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  w->ptr = role;
  w->id = role->id;
  JS_SetOpaque(obj, w);
  return obj;
}

// JS_GetOpaque, not JS_GetOpaque2: callers use this to accept "a Role, or
// nothing" (frag data's two role slots, an ammo's role), so a miss has to be a
// null rather than a pending exception.
Role *RoleFromWrapper(JSValueConst v) {
  auto *w = static_cast<RoleWrapper *>(JS_GetOpaque(v, RoleClassId));
  return w ? w->ptr : nullptr;
}

JSValue NewRolesNamespace(JSContext *ctx) {
  if (!EnsureClass(ctx, &RoleClassId, &RoleClass, RoleProto,
                   static_cast<int>(std::size(RoleProto)))) {
    return JS_EXCEPTION;
  }
  JSValue ns = NewCollection(ctx, &RolesClassId, &RolesOps);
  if (JS_IsException(ns)) {
    return ns;
  }

  // Built here rather than in RolesProps because it is a computed object, and
  // non-enumerable for the same reason as `count`: Object.keys(roles) is the
  // role ids.
  JSValue ai_types = NewAiTypes(ctx);
  if (JS_IsException(ai_types) ||
      JS_DefinePropertyValueStr(ctx, ns, "ai_types", ai_types,
                                JS_PROP_CONFIGURABLE) < 0) {
    JS_FreeValue(ctx, ns);
    return JS_EXCEPTION;
  }
  return ns;
}

} // namespace gk::js
