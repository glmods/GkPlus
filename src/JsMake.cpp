#include "MakeRole.h"

#include "GLS.h"
#include "Js.h"
#include "JsBindings.h"
#include "Misc.h"
#include "Roles.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

// The `make` namespace: builds live game objects through the native constructors
// in src/MakeRole.cpp, with no ParsedThing anywhere.
//
// This replaces the object-construction half of `gls`. The difference is not just
// cost - though a definition drops from a 0x1b60-byte parsed object to a few dozen
// bytes - it is that nothing here depends on the parser. `gls` still exists for
// what only the parser can answer: the field schema, keyword probing and
// try_parse.
//
// ONE CALL BUILDS A WHOLE ROLE. Sub-objects are described inline and built inside
// make.role, which is deliberate: a Character, Light, Projectile, pgen or
// Destructibility becomes owned by the Role (RoleDtor pool-frees all six), so
// handing the same one to two roles would double-free it on level teardown. By
// never letting a script hold one, that is unrepresentable rather than merely
// discouraged. Shapes and hierarchies are the exception - the rif cache owns
// those, so they are handed out as reusable handles.

namespace gk::js {
namespace {

JSClassID AssetClassId;

// A borrowed Shape or Hierarchy out of the rif cache. Not owned by anything the
// script can destroy, so this needs no finalizer and cannot dangle within a level.
struct Asset {
  void *pointer;
  bool is_hierarchy;
};

const JSClassDef AssetClass = {"GameAsset", nullptr, nullptr, nullptr, nullptr};

JSValue NewAsset(JSContext *ctx, void *pointer, bool is_hierarchy) {
  if (!pointer) {
    return JS_NULL;
  }
  JSValue self = JS_NewObjectClass(ctx, static_cast<int>(AssetClassId));
  if (JS_IsException(self)) {
    return self;
  }
  JS_SetOpaque(self, new Asset{pointer, is_hierarchy});
  return self;
}

// JS_GetOpaque, not JS_GetOpaque2: this is a "is it one of ours?" test on a value
// that is legitimately allowed to be a plain description object, so it must not
// throw on a miss.
Asset *AssetOf(JSValueConst v) {
  return static_cast<Asset *>(JS_GetOpaque(v, AssetClassId));
}

// '_' and ' ' compare equal, so a script may spell a keyword either way - the same
// rule gk::gls::FindField and the Roles.cpp enum tables use.
bool SameGlsName(const char *a, const char *b) {
  if (!a || !b) {
    return false;
  }
  auto fold = [](char c) {
    if (c == '_') {
      return ' ';
    }
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
  };
  for (; *a && *b; ++a, ++b) {
    if (fold(*a) != fold(*b)) {
      return false;
    }
  }
  return *a == *b;
}

// --- property readers -------------------------------------------------------------
//
// Each leaves *out untouched when the property is absent, so the Desc struct's own
// default - which is the GLS section constructor's default - stands.

bool Missing(JSContext *ctx, JSValueConst v) {
  return JS_IsUndefined(v) || JS_IsNull(v);
}

// Range-checks against the section's own declared bounds, which is the one thing
// worth keeping from the CheckValue path: `gls::FindField` reads them straight off
// the game, so the error text matches what a .gls would have been told.
bool InRange(JSContext *ctx, gls::SectionType section, const char *name,
             double value) {
  const gls::FieldInfo *field = gls::FindField(section, name);
  if (!field || field->type != gls::FieldType::Float) {
    return true; // not a GLS field, or not numeric - nothing declared to check
  }
  if (value < field->min_float || value > field->max_float) {
    JS_ThrowRangeError(ctx, "%g is out of range for '%s' (%g..%g)", value, name,
                       field->min_float, field->max_float);
    return false;
  }
  return true;
}

bool GetNumber(JSContext *ctx, JSValueConst obj, const char *name,
               gls::SectionType section, double *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (Missing(ctx, v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  double d = 0;
  int rc = JS_ToFloat64(ctx, &d, v);
  JS_FreeValue(ctx, v);
  if (rc < 0 || !InRange(ctx, section, name, d)) {
    return false;
  }
  *out = d;
  return true;
}

bool GetInt(JSContext *ctx, JSValueConst obj, const char *name, int32_t *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (Missing(ctx, v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  int32_t n = 0;
  int rc = JS_ToInt32(ctx, &n, v);
  JS_FreeValue(ctx, v);
  if (rc < 0) {
    return false;
  }
  *out = n;
  return true;
}

bool GetBool(JSContext *ctx, JSValueConst obj, const char *name, bool *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (!Missing(ctx, v)) {
    *out = JS_ToBool(ctx, v) != 0;
  }
  JS_FreeValue(ctx, v);
  return true;
}

bool GetString(JSContext *ctx, JSValueConst obj, const char *name,
               std::string *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (Missing(ctx, v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  const char *text = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  if (!text) {
    return false;
  }
  *out = text;
  JS_FreeCString(ctx, text);
  return true;
}

// An enum field: a keyword, or the raw number for the spaces whose tables are not
// fully recovered. `resolve` returns -1 for a name it does not know.
bool GetEnum(JSContext *ctx, JSValueConst obj, const char *name,
             int (*resolve)(const char *), const char *what, int32_t *out) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    return false;
  }
  if (Missing(ctx, v)) {
    JS_FreeValue(ctx, v);
    return true;
  }
  if (JS_IsString(v)) {
    const char *text = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!text) {
      return false;
    }
    // A null resolver means no keyword table has been recovered for this field,
    // so it takes the number a .gls would have parsed to - say so rather than
    // silently accepting a name and storing nonsense.
    int value = resolve ? resolve(text) : -1;
    if (value < 0) {
      JS_ThrowRangeError(ctx, resolve ? "'%s' is not a known %s"
                                      : "'%s': %s takes a number, not a name",
                         text, what);
      JS_FreeCString(ctx, text);
      return false;
    }
    JS_FreeCString(ctx, text);
    *out = value;
    return true;
  }
  int32_t n = 0;
  int rc = JS_ToInt32(ctx, &n, v);
  JS_FreeValue(ctx, v);
  if (rc < 0) {
    return false;
  }
  *out = n;
  return true;
}

int ResolveAi(const char *name) {
  AIType t = AITypeFromName(name);
  return t == AIType::Count ? -1 : static_cast<int>(t);
}
int ResolveActionOnDeath(const char *name) {
  ActionOnDeath v = ActionOnDeathFromName(name);
  return v == ActionOnDeath::Unspecified ? -1 : static_cast<int>(v);
}
int ResolveResistance(const char *name) {
  Resistance v = ResistanceFromName(name);
  return v == Resistance::None ? -1 : static_cast<int>(v);
}
int ResolveParticleType(const char *name) {
  // ParticleTypeFromName falls back to Explosion, so an unknown name would pass
  // silently; round-trip it to tell a real `explosion` from a typo.
  auto t = ParticleTypeFromName(name);
  const char *back = ParticleTypeName(t);
  return (back && SameGlsName(back, name)) ? static_cast<int>(t) : -1;
}

// --- nested sub-object descriptions -------------------------------------------------

// `{ rif, object }`, or an existing handle from make.shape / make.hierarchy.
bool ReadAsset(JSContext *ctx, JSValueConst v, bool want_hierarchy,
               void **out, std::string *hotspot, std::string *alternate) {
  if (Missing(ctx, v)) {
    return true;
  }
  if (Asset *asset = AssetOf(v)) {
    *out = asset->pointer;
    return true;
  }
  if (!JS_IsObject(v)) {
    JS_ThrowTypeError(ctx, "expected {rif, object} or a make.%s handle",
                      want_hierarchy ? "hierarchy" : "shape");
    return false;
  }
  std::string rif;
  std::string object;
  if (!GetString(ctx, v, "rif", &rif) ||
      !GetString(ctx, v, "object", &object)) {
    return false;
  }
  if (rif.empty() || object.empty()) {
    JS_ThrowTypeError(ctx, "a %s needs both 'rif' and 'object'",
                      want_hierarchy ? "hierarchy" : "shape");
    return false;
  }
  if (hotspot && !GetString(ctx, v, "hotspot", hotspot)) {
    return false;
  }
  if (alternate && !GetString(ctx, v, "alternate_hotspot", alternate)) {
    return false;
  }
  *out = want_hierarchy
             ? static_cast<void *>(MakeHierarchy(rif.c_str(), object.c_str()))
             : static_cast<void *>(MakeShape(rif.c_str(), object.c_str()));
  if (!*out) {
    JS_ThrowReferenceError(ctx, "no %s '%s' in '%s'",
                           want_hierarchy ? "hierarchy" : "shape",
                           object.c_str(), rif.c_str());
    return false;
  }
  return true;
}

bool ReadCharacter(JSContext *ctx, JSValueConst v, CharacterDesc *d) {
  auto S = gls::SectionType::Character;
  void *customisation = nullptr;
  void *shadow = nullptr;
  JSValue ch = JS_GetPropertyStr(ctx, v, "customisation_hierarchy");
  bool ok = !JS_IsException(ch) &&
            ReadAsset(ctx, ch, true, &customisation, nullptr, nullptr);
  JS_FreeValue(ctx, ch);
  if (!ok) {
    return false;
  }
  JSValue sh = JS_GetPropertyStr(ctx, v, "shadow_hierarchy");
  ok = !JS_IsException(sh) && ReadAsset(ctx, sh, true, &shadow, nullptr, nullptr);
  JS_FreeValue(ctx, sh);
  if (!ok) {
    return false;
  }
  d->customisation_hierarchy = static_cast<Hierarchy *>(customisation);
  d->shadow_hierarchy = static_cast<Hierarchy *>(shadow);

  return GetNumber(ctx, v, "walking_speed", S, &d->walking_speed) &&
         GetNumber(ctx, v, "turning_speed", S, &d->turning_speed) &&
         GetNumber(ctx, v, "strength", S, &d->strength) &&
         GetNumber(ctx, v, "aim", S, &d->aim) &&
         GetNumber(ctx, v, "sight_angle", S, &d->sight_angle) &&
         GetNumber(ctx, v, "sight_range", S, &d->sight_range) &&
         GetNumber(ctx, v, "hearing_range", S, &d->hearing_range) &&
         GetNumber(ctx, v, "aggression", S, &d->aggression) &&
         GetNumber(ctx, v, "scan_delay", S, &d->scan_delay) &&
         GetNumber(ctx, v, "scan_acceptance_angle", S,
                   &d->scan_acceptance_angle) &&
         GetNumber(ctx, v, "angular_scan_rate", S, &d->angular_scan_rate) &&
         GetNumber(ctx, v, "mine_laying_time", S, &d->mine_laying_time) &&
         GetNumber(ctx, v, "damage_multiplier", S, &d->damage_multiplier) &&
         GetNumber(ctx, v, "shot_speed_multiplier", S,
                   &d->shot_speed_multiplier) &&
         GetNumber(ctx, v, "target_cycle_time", S, &d->target_cycle_time) &&
         GetNumber(ctx, v, "weapon_cycle_time", S, &d->weapon_cycle_time) &&
         GetNumber(ctx, v, "weapon_cycle_time2", S, &d->weapon_cycle_time2) &&
         GetNumber(ctx, v, "alarm_delay", S, &d->alarm_delay) &&
         GetNumber(ctx, v, "gun_yaw_angle", S, &d->gun_yaw_angle) &&
         GetNumber(ctx, v, "elevation_angle", S, &d->elevation_angle) &&
         GetNumber(ctx, v, "alert_radius", S, &d->alert_radius) &&
         GetNumber(ctx, v, "radius", S, &d->radius) &&
         GetNumber(ctx, v, "height", S, &d->height) &&
         GetNumber(ctx, v, "size", S, &d->size) &&
         GetNumber(ctx, v, "initial_first_person_range", S,
                   &d->initial_first_person_range) &&
         GetNumber(ctx, v, "maximum_first_person_range", S,
                   &d->maximum_first_person_range) &&
         GetEnum(ctx, v, "weapon", WeaponTypeFromName, "weapon", &d->weapon) &&
         GetEnum(ctx, v, "secondary_weapon", WeaponTypeFromName, "weapon",
                 &d->secondary_weapon) &&
         GetInt(ctx, v, "description", &d->description) &&
         GetInt(ctx, v, "status_window_u", &d->status_window_u) &&
         GetInt(ctx, v, "status_window_v", &d->status_window_v) &&
         GetInt(ctx, v, "blob_shadow", &d->blob_shadow) &&
         GetInt(ctx, v, "generation_limit", &d->generation_limit) &&
         GetInt(ctx, v, "max_weapon", &d->max_weapon) &&
         GetInt(ctx, v, "max_ammo", &d->max_ammo) &&
         GetInt(ctx, v, "max_module", &d->max_module) &&
         GetBool(ctx, v, "can_turn", &d->can_turn) &&
         GetBool(ctx, v, "draw_vision_cone", &d->draw_vision_cone) &&
         GetBool(ctx, v, "draw_hearing_range", &d->draw_hearing_range) &&
         GetBool(ctx, v, "always_cpu_controlled", &d->always_cpu_controlled) &&
         GetBool(ctx, v, "alertable", &d->alertable) &&
         GetBool(ctx, v, "latch_trigger", &d->latch_trigger);
}

bool ReadLight(JSContext *ctx, JSValueConst v, LightDesc *d) {
  auto S = gls::SectionType::Light;
  return GetNumber(ctx, v, "red", S, &d->red) &&
         GetNumber(ctx, v, "green", S, &d->green) &&
         GetNumber(ctx, v, "blue", S, &d->blue) &&
         GetNumber(ctx, v, "specular_red", S, &d->specular_red) &&
         GetNumber(ctx, v, "specular_green", S, &d->specular_green) &&
         GetNumber(ctx, v, "specular_blue", S, &d->specular_blue) &&
         GetNumber(ctx, v, "range", S, &d->range);
}

bool ReadProjectile(JSContext *ctx, JSValueConst v, ProjectileDesc *d) {
  auto S = gls::SectionType::Projectile;
  JSValue hit = JS_GetPropertyStr(ctx, v, "hit_light");
  if (JS_IsException(hit)) {
    return false;
  }
  if (!Missing(ctx, hit)) {
    LightDesc light;
    bool ok = JS_IsObject(hit) && ReadLight(ctx, hit, &light);
    JS_FreeValue(ctx, hit);
    if (!ok) {
      return false;
    }
    d->hit_light = MakeLight(light);
  } else {
    JS_FreeValue(ctx, hit);
  }
  return GetBool(ctx, v, "gravity", &d->gravity) &&
         GetNumber(ctx, v, "damage", S, &d->damage) &&
         GetInt(ctx, v, "sound", &d->sound) &&
         GetNumber(ctx, v, "max_range", S, &d->max_range) &&
         GetNumber(ctx, v, "blast_damage", S, &d->blast_damage) &&
         GetNumber(ctx, v, "blast_range", S, &d->blast_range);
}

bool ReadParticleGenerator(JSContext *ctx, JSValueConst v,
                           ParticleGeneratorDesc *d) {
  auto S = gls::SectionType::ParticleGenerator;
  int32_t type = static_cast<int32_t>(d->type);
  Vec3 coords = d->coords;
  if (!GetEnum(ctx, v, "type", ResolveParticleType, "particle type", &type)) {
    return false;
  }
  d->type = static_cast<ParticleType>(type);
  JSValue c = JS_GetPropertyStr(ctx, v, "coords");
  if (JS_IsException(c)) {
    return false;
  }
  if (!Missing(ctx, c) && !ToVec3(ctx, c, &coords)) {
    JS_FreeValue(ctx, c);
    return false;
  }
  JS_FreeValue(ctx, c);
  d->coords = coords;
  return GetNumber(ctx, v, "rate", S, &d->rate) &&
         GetNumber(ctx, v, "red", S, &d->red) &&
         GetNumber(ctx, v, "green", S, &d->green) &&
         GetNumber(ctx, v, "blue", S, &d->blue) &&
         GetNumber(ctx, v, "alpha", S, &d->alpha) &&
         GetNumber(ctx, v, "start_scale", S, &d->start_scale) &&
         GetNumber(ctx, v, "end_scale", S, &d->end_scale) &&
         GetNumber(ctx, v, "spin", S, &d->spin) &&
         GetNumber(ctx, v, "particle_ttl_seconds", S,
                   &d->particle_ttl_seconds) &&
         GetBool(ctx, v, "generate_generators", &d->generate_generators) &&
         GetInt(ctx, v, "life_low", &d->life_low) &&
         GetInt(ctx, v, "life_high", &d->life_high);
}

// `{ kind: "explode" | "splatter" | "frag" | "replace", ... }`
Destructibility *ReadDestructibility(JSContext *ctx, JSValueConst v, bool *ok) {
  *ok = true;
  if (Missing(ctx, v)) {
    return nullptr;
  }
  std::string kind;
  if (!JS_IsObject(v) || !GetString(ctx, v, "kind", &kind)) {
    *ok = false;
    if (JS_IsObject(v)) {
      return nullptr;
    }
    JS_ThrowTypeError(ctx, "destructibility must be {kind: ...}");
    return nullptr;
  }
  if (kind == "explode") {
    return MakeDestructibility(DestructibilityKind::Explode);
  }
  if (kind == "splatter") {
    return MakeDestructibility(DestructibilityKind::Splatter);
  }
  if (kind == "replace") {
    // `script`, not `name`: GLS spells the field `name`, but its only consumer is
    // Frag queueing it, so it is a .gcs path - and therefore takes a message
    // object as well, like every other script-name field.
    std::string script;
    bool replace = false;
    if (!GetScriptPayloadProp(ctx, v, "script", &script) ||
        !GetBool(ctx, v, "replace", &replace)) {
      *ok = false;
      return nullptr;
    }
    return reinterpret_cast<Destructibility *>(
        MakeReplaceDestructibility(script.c_str(), replace));
  }
  if (kind == "frag") {
    FragDataDesc desc;
    std::string remove;
    JSValue role = JS_GetPropertyStr(ctx, v, "role");
    JSValue replace_role = JS_GetPropertyStr(ctx, v, "replace_role");
    desc.role = RoleFromWrapper(role);
    desc.replace_role = RoleFromWrapper(replace_role);
    JS_FreeValue(ctx, role);
    JS_FreeValue(ctx, replace_role);
    auto S = gls::SectionType::FragData;
    if (!GetString(ctx, v, "remove", &remove) ||
        !GetInt(ctx, v, "scale", &desc.scale) ||
        !GetBool(ctx, v, "replace", &desc.replace) ||
        !GetBool(ctx, v, "symmetric", &desc.symmetric) ||
        !GetNumber(ctx, v, "blast_range", S, &desc.blast_range) ||
        !GetNumber(ctx, v, "blast_damage", S, &desc.blast_damage)) {
      *ok = false;
      return nullptr;
    }
    desc.remove = remove.empty() ? nullptr : remove.c_str();
    return reinterpret_cast<Destructibility *>(MakeFragData(desc));
  }
  *ok = false;
  JS_ThrowRangeError(ctx,
                     "destructibility kind must be explode, splatter, frag or "
                     "replace - got '%s'",
                     kind.c_str());
  return nullptr;
}

// --- the factories -------------------------------------------------------------------

JSValue MakeShapeJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "shape(rif, object)");
  }
  const char *rif = JS_ToCString(ctx, argv[0]);
  const char *object = rif ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (!rif || !object) {
    JS_FreeCString(ctx, rif);
    return JS_EXCEPTION;
  }
  Shape *shape = MakeShape(rif, object);
  JSValue out = shape ? NewAsset(ctx, shape, false)
                      : JS_ThrowReferenceError(ctx, "no shape '%s' in '%s'",
                                               object, rif);
  JS_FreeCString(ctx, rif);
  JS_FreeCString(ctx, object);
  return out;
}

JSValue MakeHierarchyJs(JSContext *ctx, JSValueConst, int argc,
                        JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "hierarchy(rif, object)");
  }
  const char *rif = JS_ToCString(ctx, argv[0]);
  const char *object = rif ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (!rif || !object) {
    JS_FreeCString(ctx, rif);
    return JS_EXCEPTION;
  }
  Hierarchy *h = MakeHierarchy(rif, object);
  JSValue out = h ? NewAsset(ctx, h, true)
                  : JS_ThrowReferenceError(ctx, "no hierarchy '%s' in '%s'",
                                           object, rif);
  JS_FreeCString(ctx, rif);
  JS_FreeCString(ctx, object);
  return out;
}

JSValue MakeRoleJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "role({identifier, ...})");
  }
  JSValueConst v = argv[0];
  RoleDesc d;
  std::string identifier, hotspot, alternate, sever_points, meta_sound,
      beam_script;

  // Geometry first, because the hierarchy carries the hotspot names.
  void *shape = nullptr;
  void *hierarchy = nullptr;
  JSValue s = JS_GetPropertyStr(ctx, v, "shape");
  bool ok = !JS_IsException(s) &&
            ReadAsset(ctx, s, false, &shape, nullptr, nullptr);
  JS_FreeValue(ctx, s);
  if (!ok) {
    return JS_EXCEPTION;
  }
  JSValue h = JS_GetPropertyStr(ctx, v, "hierarchy");
  ok = !JS_IsException(h) &&
       ReadAsset(ctx, h, true, &hierarchy, &hotspot, &alternate);
  JS_FreeValue(ctx, h);
  if (!ok) {
    return JS_EXCEPTION;
  }
  d.shape = static_cast<Shape *>(shape);
  d.hierarchy = static_cast<Hierarchy *>(hierarchy);

  // Owned sub-objects, each built here and handed straight to MakeRole.
  JSValue chr = JS_GetPropertyStr(ctx, v, "character");
  if (JS_IsException(chr)) {
    return JS_EXCEPTION;
  }
  if (!Missing(ctx, chr)) {
    CharacterDesc cd;
    if (!JS_IsObject(chr) || !ReadCharacter(ctx, chr, &cd)) {
      JS_FreeValue(ctx, chr);
      return JS_EXCEPTION;
    }
    d.character = MakeCharacter(cd);
  }
  JS_FreeValue(ctx, chr);

  JSValue lit = JS_GetPropertyStr(ctx, v, "light");
  if (JS_IsException(lit)) {
    return JS_EXCEPTION;
  }
  if (!Missing(ctx, lit)) {
    LightDesc ld;
    if (!JS_IsObject(lit) || !ReadLight(ctx, lit, &ld)) {
      JS_FreeValue(ctx, lit);
      return JS_EXCEPTION;
    }
    d.light = MakeLight(ld);
  }
  JS_FreeValue(ctx, lit);

  JSValue prj = JS_GetPropertyStr(ctx, v, "projectile");
  if (JS_IsException(prj)) {
    return JS_EXCEPTION;
  }
  if (!Missing(ctx, prj)) {
    ProjectileDesc pd;
    if (!JS_IsObject(prj) || !ReadProjectile(ctx, prj, &pd)) {
      JS_FreeValue(ctx, prj);
      return JS_EXCEPTION;
    }
    d.projectile = MakeProjectile(pd);
  }
  JS_FreeValue(ctx, prj);

  for (auto [key, slot] :
       {std::pair{"pgen", &d.pgen}, std::pair{"pgen2", &d.pgen2}}) {
    JSValue g = JS_GetPropertyStr(ctx, v, key);
    if (JS_IsException(g)) {
      return JS_EXCEPTION;
    }
    if (!Missing(ctx, g)) {
      ParticleGeneratorDesc gd;
      if (!JS_IsObject(g) || !ReadParticleGenerator(ctx, g, &gd)) {
        JS_FreeValue(ctx, g);
        return JS_EXCEPTION;
      }
      *slot = MakeParticleGenerator(gd);
    }
    JS_FreeValue(ctx, g);
  }

  JSValue des = JS_GetPropertyStr(ctx, v, "destructibility");
  if (JS_IsException(des)) {
    return JS_EXCEPTION;
  }
  bool des_ok = true;
  d.destructibility = ReadDestructibility(ctx, des, &des_ok);
  JS_FreeValue(ctx, des);
  if (!des_ok) {
    return JS_EXCEPTION;
  }

  // Inventory geometry - either kind, like the role's own.
  void *inv_shape = nullptr;
  void *inv_hier = nullptr;
  JSValue inv = JS_GetPropertyStr(ctx, v, "inventory_shape");
  ok = !JS_IsException(inv) &&
       ReadAsset(ctx, inv, false, &inv_shape, nullptr, nullptr);
  JS_FreeValue(ctx, inv);
  if (!ok) {
    return JS_EXCEPTION;
  }
  JSValue invh = JS_GetPropertyStr(ctx, v, "inventory_hierarchy");
  ok = !JS_IsException(invh) &&
       ReadAsset(ctx, invh, true, &inv_hier, nullptr, nullptr);
  JS_FreeValue(ctx, invh);
  if (!ok) {
    return JS_EXCEPTION;
  }
  d.inventory_shape = static_cast<Shape *>(inv_shape);
  d.inventory_hierarchy = static_cast<Hierarchy *>(inv_hier);

  auto S = gls::SectionType::Role;
  int32_t ai = static_cast<int32_t>(d.ai);
  int32_t beam_effect = static_cast<int32_t>(d.interface_beam_effect);
  int32_t action_on_death = 0;
  int32_t resistance = 0;
  if (!GetString(ctx, v, "identifier", &identifier) ||
      !GetString(ctx, v, "hotspot", &hotspot) ||
      !GetString(ctx, v, "alternate_hotspot", &alternate) ||
      !GetString(ctx, v, "sever_points", &sever_points) ||
      !GetString(ctx, v, "meta_sound", &meta_sound) ||
      // A .gcs name, or a message object - the beam's effect goes on the script
      // queue like any trigger's, so both shapes work here too.
      !GetScriptPayloadProp(ctx, v, "interface_beam_script", &beam_script) ||
      !GetEnum(ctx, v, "ai", ResolveAi, "AI type", &ai) ||
      !GetEnum(ctx, v, "action_on_death", ResolveActionOnDeath,
               "action on death", &action_on_death) ||
      !GetEnum(ctx, v, "resistance", ResolveResistance, "resistance",
               &resistance) ||
      !GetInt(ctx, v, "description", &d.description) ||
      !GetInt(ctx, v, "pickup_name", &d.pickup_name) ||
      !GetInt(ctx, v, "recon_name", &d.recon_name) ||
      !GetInt(ctx, v, "recon_ai_short", &d.recon_ai_short) ||
      !GetInt(ctx, v, "recon_ai_number", &d.recon_ai_number) ||
      !GetInt(ctx, v, "recon_ai_long", &d.recon_ai_long) ||
      !GetInt(ctx, v, "recon_ai_long2", &d.recon_ai_long2) ||
      !GetInt(ctx, v, "interface_beam_delay", &d.interface_beam_delay) ||
      !GetInt(ctx, v, "interface_beam_duration", &d.interface_beam_duration) ||
      // No keyword table recovered for the beam effect; it is a VulnerabilityType
      // and takes the number.
      !GetEnum(ctx, v, "interface_beam_effect", nullptr,
               "interface beam effect", &beam_effect) ||
      !GetInt(ctx, v, "limit", &d.limit)) {
    return JS_EXCEPTION;
  }
  if (!GetNumber(ctx, v, "pickup_radius", S, &d.pickup_radius) ||
      !GetNumber(ctx, v, "resistance_factor", S, &d.resistance_factor) ||
      !GetNumber(ctx, v, "alpha", S, &d.alpha) ||
      !GetNumber(ctx, v, "armor", S, &d.armor) ||
      !GetNumber(ctx, v, "shields", S, &d.shields) ||
      !GetNumber(ctx, v, "recharge_rate", S, &d.recharge_rate) ||
      !GetBool(ctx, v, "alpha_fogging", &d.alpha_fogging) ||
      !GetBool(ctx, v, "per_vertex_fogging", &d.per_vertex_fogging) ||
      !GetBool(ctx, v, "no_lighting", &d.no_lighting) ||
      !GetBool(ctx, v, "reflective", &d.reflective) ||
      !GetBool(ctx, v, "destination_selectable", &d.destination_selectable) ||
      !GetBool(ctx, v, "destroy_after_collection",
               &d.destroy_after_collection) ||
      !GetBool(ctx, v, "hit_test_ignore", &d.hit_test_ignore) ||
      !GetBool(ctx, v, "frag_control", &d.frag_control) ||
      !GetBool(ctx, v, "moves_on_lifts", &d.moves_on_lifts) ||
      !GetBool(ctx, v, "status_display", &d.status_display)) {
    return JS_EXCEPTION;
  }
  d.ai = static_cast<AIType>(ai);
  d.action_on_death = action_on_death;
  d.resistance = resistance;
  d.interface_beam_effect = static_cast<VulnerabilityType>(beam_effect);
  d.identifier = identifier.empty() ? nullptr : identifier.c_str();
  d.hotspot = hotspot.empty() ? nullptr : hotspot.c_str();
  d.alternate_hotspot = alternate.empty() ? nullptr : alternate.c_str();
  d.sever_points = sever_points.empty() ? nullptr : sever_points.c_str();
  d.meta_sound = meta_sound.empty() ? nullptr : meta_sound.c_str();
  d.interface_beam_script =
      beam_script.empty() ? nullptr : beam_script.c_str();

  // CreateRole does a bucket-head insert into the roles hash (@ 0x007b48f0) plus two
  // pool_allocs, and the executor reads that table from ExecutorThreadProc itself as well
  // as from EvaluateTriggers and every AiThink_*. See gk::ExecutorPause in Misc.h.
  Role *role;
  {
    ExecutorPause pause;
    role = MakeRole(d);
  }
  if (!role) {
    return JS_ThrowInternalError(ctx, "the role could not be created");
  }
  return NewRoleWrapper(ctx, role);
}

JSValue MakeAmmoJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "ammo({ammo_type, weapon_type, ...})");
  }
  JSValueConst v = argv[0];
  AmmoDesc d;
  std::string name, file;
  auto S = gls::SectionType::Ammo;
  JSValue role = JS_GetPropertyStr(ctx, v, "role");
  d.role = RoleFromWrapper(role);
  JS_FreeValue(ctx, role);
  if (!GetEnum(ctx, v, "ammo_type", AmmoTypeFromName, "ammo type",
               &d.ammo_type) ||
      !GetEnum(ctx, v, "weapon_type", WeaponTypeFromName, "weapon type",
               &d.weapon_type) ||
      !GetString(ctx, v, "name", &name) || !GetString(ctx, v, "file", &file) ||
      !GetNumber(ctx, v, "round_time", S, &d.round_time) ||
      !GetNumber(ctx, v, "reload_time", S, &d.reload_time) ||
      !GetInt(ctx, v, "life_timer", &d.life_timer) ||
      !GetInt(ctx, v, "magazine_size", &d.magazine_size) ||
      !GetInt(ctx, v, "salvo_size", &d.salvo_size) ||
      !GetInt(ctx, v, "sound", &d.sound) ||
      !GetNumber(ctx, v, "firing_speed", S, &d.firing_speed)) {
    return JS_EXCEPTION;
  }
  d.name = name.empty() ? nullptr : name.c_str();
  d.file = file.empty() ? nullptr : file.c_str();
  return JS_NewBool(ctx, MakeAmmo(d));
}

JSValue MakeAmmoInfoJs(JSContext *ctx, JSValueConst, int argc,
                       JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "ammo_info({ammo_type, ...})");
  }
  JSValueConst v = argv[0];
  AmmoInfoDesc d;
  void *shape = nullptr;
  void *hier = nullptr;
  JSValue s = JS_GetPropertyStr(ctx, v, "shape");
  bool ok = !JS_IsException(s) &&
            ReadAsset(ctx, s, false, &shape, nullptr, nullptr);
  JS_FreeValue(ctx, s);
  if (!ok) {
    return JS_EXCEPTION;
  }
  JSValue h = JS_GetPropertyStr(ctx, v, "hierarchy");
  ok = !JS_IsException(h) && ReadAsset(ctx, h, true, &hier, nullptr, nullptr);
  JS_FreeValue(ctx, h);
  if (!ok) {
    return JS_EXCEPTION;
  }
  d.shape = static_cast<Shape *>(shape);
  d.hierarchy = static_cast<Hierarchy *>(hier);
  if (!GetEnum(ctx, v, "ammo_type", AmmoTypeFromName, "ammo type",
               &d.ammo_type) ||
      !GetInt(ctx, v, "ammo_name", &d.ammo_name) ||
      !GetInt(ctx, v, "description", &d.description) ||
      !GetInt(ctx, v, "max_per_slot", &d.max_per_slot)) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, MakeAmmoInfo(d));
}

JSValue MakeCameraTrackJs(JSContext *ctx, JSValueConst, int argc,
                          JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "camera_track({name, file, pgen, pgen2})");
  }
  JSValueConst v = argv[0];
  CameraTrackDesc d;
  std::string name;
  std::string file;
  if (!GetString(ctx, v, "name", &name) || !GetString(ctx, v, "file", &file)) {
    return JS_EXCEPTION;
  }
  for (auto [key, slot] :
       {std::pair{"pgen", &d.pgen}, std::pair{"pgen2", &d.pgen2}}) {
    JSValue g = JS_GetPropertyStr(ctx, v, key);
    if (JS_IsException(g)) {
      return JS_EXCEPTION;
    }
    if (!Missing(ctx, g)) {
      ParticleGeneratorDesc gd;
      if (!JS_IsObject(g) || !ReadParticleGenerator(ctx, g, &gd)) {
        JS_FreeValue(ctx, g);
        return JS_EXCEPTION;
      }
      *slot = MakeParticleGenerator(gd);
    }
    JS_FreeValue(ctx, g);
  }
  d.name = name.empty() ? nullptr : name.c_str();
  d.file = file.empty() ? nullptr : file.c_str();
  return JS_NewBool(ctx, MakeCameraTrack(d));
}

const JSCFunctionListEntry MakeProps[] = {
    JS_CFUNC_DEF("shape", 2, MakeShapeJs),
    JS_CFUNC_DEF("hierarchy", 2, MakeHierarchyJs),
    JS_CFUNC_DEF("role", 1, MakeRoleJs),
    JS_CFUNC_DEF("ammo", 1, MakeAmmoJs),
    JS_CFUNC_DEF("ammo_info", 1, MakeAmmoInfoJs),
    JS_CFUNC_DEF("camera_track", 1, MakeCameraTrackJs),
};

} // namespace

JSValue NewMakeNamespace(JSContext *ctx) {
  if (!EnsureClass(ctx, &AssetClassId, &AssetClass, nullptr, 0)) {
    return JS_EXCEPTION;
  }
  return NewNamespace(ctx, MakeProps, static_cast<int>(std::size(MakeProps)));
}

} // namespace gk::js
