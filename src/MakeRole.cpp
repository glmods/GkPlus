#include "MakeRole.h"

#include "Console.h"
#include "Core.h"
#include "Map.h"
#include "Memory.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // GetCurrentThreadId, for the particle TTL clock choice

#include <cmath>
#include <cstring>

namespace gk {
namespace {
// ToCharacter's three .rdata constants, read out of the binary rather than
// guessed: 0x00663ca0, 0x00663cb0 and 0x00663cc0.
constexpr double DegreesPerTurn = 360.0; // DAT_00663ca0
constexpr double BamPerTurn = 4096.0;    // DAT_00663cb0
constexpr float FixedScale = 65536.0f;   // FLOAT_00663cc0
constexpr float StatusWindowScale = 1.0f / 1024.0f; // DAT_00663c40

// The game rounds with FISTP under the default x87 control word, i.e.
// round-to-nearest-even. std::lrintf honours the current rounding mode, which is
// that same mode unless something has changed it - so this matches bit for bit,
// where a naive (int)(x + 0.5f) would not for halfway cases.
int RoundToNearest(float value) { return static_cast<int>(std::lrintf(value)); }

// The game's own strdup @ 0x0044e1a0, which allocates through the pool - not the
// CRT's. Every string these objects own has to come from here, because their
// destructors free through pool_free (see Memory.h).
char *GameStrdup(const char *text) {
  if (!text) {
    return nullptr;
  }
  FastCall<char *, const char *> fn;
  GetObjectAtOffset(fn, 0x0044e1a0);
  return fn(text);
}

// Allocates `size` bytes of pool and zeroes them. The converters do not zero -
// they assign every field they care about and leave the rest as pool garbage -
// but several of these structs are mirrored with pool_unique_ptr members, and
// calling reset() on an unconstructed one would run the deleter over garbage.
// Zeroing makes those members null, which is the only state reset() may start
// from. The observable difference is confined to fields the game never reads
// before writing (Character's three AI scratch dwords).
void *ZeroedPool(size_t size) {
  void *raw = pool_alloc(size);
  if (raw) {
    std::memset(raw, 0, size);
  }
  return raw;
}
} // namespace

float DegreesToBamDivFirst(double degrees) {
  return static_cast<float>((degrees / DegreesPerTurn) * BamPerTurn);
}

float DegreesToBamMulFirst(double degrees) {
  return static_cast<float>((degrees * BamPerTurn) / DegreesPerTurn);
}

float RevolutionsToBam(double revolutions) {
  return static_cast<float>(revolutions * BamPerTurn);
}

int ToFixed16(double cycles_per_second) {
  return RoundToNearest(static_cast<float>(cycles_per_second) * FixedScale);
}

Character *MakeCharacter(const CharacterDesc &d) {
  auto *c = static_cast<Character *>(pool_alloc(sizeof(Character)));
  if (!c) {
    return nullptr;
  }
  // ToCharacter nulls exactly these four before filling anything, and leaves
  // field0x94/0x98/0x9c untouched - they are runtime scratch the engine owns.
  c->customisation_hierarchy = nullptr;
  c->shadow_hierarchy = nullptr;
  c->blob_shadow = 0;
  c->description = nullptr;

  c->scan_delay = static_cast<float>(d.scan_delay);
  c->scan_acceptance_angle = DegreesToBamDivFirst(d.scan_acceptance_angle);
  c->angular_scan_rate = DegreesToBamDivFirst(d.angular_scan_rate);
  c->mine_laying_time = static_cast<float>(d.mine_laying_time);
  c->walking_speed = ToFixed16(d.walking_speed);
  c->turning_speed = RevolutionsToBam(d.turning_speed);
  c->strength = static_cast<float>(d.strength);
  c->aim = DegreesToBamMulFirst(d.aim);
  c->alertable = d.alertable;
  c->latch_trigger = d.latch_trigger;
  c->generation_limit = d.generation_limit;
  c->sight_angle = DegreesToBamMulFirst(d.sight_angle);
  c->gun_yaw_angle = DegreesToBamMulFirst(d.gun_yaw_angle);
  c->elevation_angle = DegreesToBamMulFirst(d.elevation_angle);
  c->sight_range = static_cast<float>(d.sight_range);
  c->sight_range_squared = c->sight_range * c->sight_range;
  c->hearing_range = static_cast<float>(d.hearing_range);
  c->hearing_range_squared = c->hearing_range * c->hearing_range;
  c->alert_radius = static_cast<float>(d.alert_radius);
  c->aggression = static_cast<float>(d.aggression);
  c->size = static_cast<float>(d.size);
  c->damage_multiplier = static_cast<float>(d.damage_multiplier);
  c->shot_speed_multiplier = static_cast<float>(d.shot_speed_multiplier);
  c->target_cycle_delay = static_cast<float>(d.target_cycle_time);
  c->weapon_cycle_time = static_cast<float>(d.weapon_cycle_time);
  c->weapon_cycle_time2 = static_cast<float>(d.weapon_cycle_time2);
  c->alarm_delay = static_cast<float>(d.alarm_delay);
  // Both are stored pre-multiplied, which is why the Character has no plain
  // radius or height to read back.
  c->radius_times_size = static_cast<float>(d.radius) * c->size;
  c->height_times_size = static_cast<float>(d.height) * c->size;
  c->can_turn = d.can_turn;
  c->draw_vision_cone = d.draw_vision_cone;
  c->draw_hearing_range = d.draw_hearing_range;
  c->always_cpu_controlled = d.always_cpu_controlled;
  c->weapon = d.weapon;
  c->secondary_weapon = d.secondary_weapon;
  c->status_window_u = static_cast<float>(d.status_window_u) * StatusWindowScale;
  c->status_window_v = static_cast<float>(d.status_window_v) * StatusWindowScale;
  c->max_weapon = d.max_weapon;
  c->max_ammo = d.max_ammo;
  c->max_module = d.max_module;
  c->initial_first_person_range =
      static_cast<float>(d.initial_first_person_range);
  c->maximum_first_person_range =
      static_cast<float>(d.maximum_first_person_range);

  // Anything that turns re-derives its walking speed as distance per turn rather
  // than animation cycles per second. Note the double rounding: this takes the
  // *already rounded* fixed-point value back to float, divides, and rounds again
  // - reproducing that is the difference between matching the game and being
  // half an ulp out on every turning character.
  if (c->turning_speed > 0.0f && c->size > 0.0f) {
    float cycles = static_cast<float>(c->walking_speed) / FixedScale;
    c->walking_speed = RoundToNearest((cycles / c->size) * FixedScale);
  }
  // A max below the initial is clamped up, not down.
  if (c->maximum_first_person_range <= c->initial_first_person_range &&
      c->initial_first_person_range != c->maximum_first_person_range) {
    c->maximum_first_person_range = c->initial_first_person_range;
  }

  if (d.description != 0) {
    // Deliberately the raw GetResourceString rather than gk::ResourceString:
    // that one normalizes a missing id to "", and ToCharacter stores whatever
    // comes back - including null, which the engine does test for.
    static void *LocalizedStrings;
    static FastCall<const char *, void *, unsigned> GetResourceString;
    if (!GetResourceString) {
      GetObjectAtOffset(LocalizedStrings, 0x00725664);
      GetObjectAtOffset(GetResourceString, 0x00579000);
    }
    c->description = const_cast<char *>(
        GetResourceString(&LocalizedStrings,
                          static_cast<unsigned>(d.description)));
  }
  c->customisation_hierarchy = d.customisation_hierarchy;
  c->shadow_hierarchy = d.shadow_hierarchy;
  c->blob_shadow = d.blob_shadow;
  return c;
}

// --- shape and hierarchy --------------------------------------------------------

Shape *MakeShape(const char *rif_file, const char *object_name) {
  FastCall<Shape *, const char *, const char *> get_shape;
  GetObjectAtOffset(get_shape, 0x004ae570);
  return get_shape(rif_file, object_name);
}

Hierarchy *MakeHierarchy(const char *rif_file, const char *object_name) {
  FastCall<Hierarchy *, const char *, const char *> get_hierarchy;
  GetObjectAtOffset(get_hierarchy, 0x004ae390);
  return get_hierarchy(rif_file, object_name);
}

// --- light ------------------------------------------------------------------------

Light *MakeLight(const LightDesc &d) {
  auto *l = static_cast<Light *>(pool_alloc(sizeof(Light)));
  if (!l) {
    return nullptr;
  }
  l->red = static_cast<float>(d.red);
  l->green = static_cast<float>(d.green);
  l->blue = static_cast<float>(d.blue);
  l->specular_red = static_cast<float>(d.specular_red);
  l->specular_green = static_cast<float>(d.specular_green);
  l->specular_blue = static_cast<float>(d.specular_blue);
  l->range = static_cast<float>(d.range);
  return l;
}

// --- projectile ---------------------------------------------------------------------

Projectile *MakeProjectile(const ProjectileDesc &d) {
  auto *p = static_cast<Projectile *>(ZeroedPool(sizeof(Projectile)));
  if (!p) {
    return nullptr;
  }
  p->gravity = d.gravity;
  p->field0x1 = p->field0x2 = p->field0x3 = 0;
  p->max_range = static_cast<float>(d.max_range);
  p->damage = static_cast<float>(d.damage);
  p->blast_damage = static_cast<float>(d.blast_damage);
  p->blast_range = static_cast<float>(d.blast_range);
  p->blast_range_squared = p->blast_range * p->blast_range;
  p->sound = d.sound;
  p->hit_light.reset(d.hit_light);
  return p;
}

// --- particle generator ---------------------------------------------------------------

ParticleGenerator *MakeParticleGenerator(const ParticleGeneratorDesc &d) {
  auto *g =
      static_cast<ParticleGenerator *>(ZeroedPool(sizeof(ParticleGenerator)));
  if (!g) {
    return nullptr;
  }
  // ToParticleGenerator's default-initialisation, reproduced store for store. The
  // 1.0f-filled channels and the 2s are emitter interpolation state, not GLS data;
  // `kind` = 5 is read by ParticleEmitter_Ctor as "take the default from
  // ParticleTypeInfos[type]", so it is not an inert tag and must be seeded.
  g->kind = 5;
  const PGenChannel ones{0, {1.0f, 1.0f, 1.0f, 1.0f}, 2};
  g->colour = ones;
  g->channel_b = ones;
  g->channel_c = ones;
  g->channel_d = ones;
  g->channel_e = ones;
  g->field0xa8 = 1.0f;
  g->field0xac.flt = 0.0f;
  g->field0xb0 = 4.0f;
  g->start_scale = 1.0f;
  g->end_scale = 1.0f;
  g->spin = 0.0f;
  g->lifespan_ticks.integer = 0;

  g->type = d.type;
  g->field0x8 = d.life_low;
  g->field0xc = d.life_high;
  g->rate = static_cast<float>(d.rate);
  g->coords = d.coords;
  // The colour channel's vec4 is the GLS red/green/blue/alpha; its trailing count
  // stays 2, and channel_b is zeroed rather than left at ones.
  g->colour.v[0] = static_cast<float>(d.red);
  g->colour.v[1] = static_cast<float>(d.green);
  g->colour.v[2] = static_cast<float>(d.blue);
  g->colour.v[3] = static_cast<float>(d.alpha);
  g->colour.trail = 2;
  g->channel_b.v[0] = g->channel_b.v[1] = g->channel_b.v[2] =
      g->channel_b.v[3] = 0.0f;
  g->channel_b.trail = 2;
  g->use_channel_cd = false;
  g->field0x5d = d.generate_generators; // written as one word with use_channel_cd
  g->generate_generators = d.generate_generators;
  g->start_scale = static_cast<float>(d.start_scale);
  g->end_scale = static_cast<float>(d.end_scale);
  g->spin = static_cast<float>(d.spin);

  // Seconds -> ticks at the *calling thread's* clock rate: the client and the
  // executor keep separate ones, and ToParticleGenerator picks with the same
  // GetCurrentThreadId() == ExecutingThread test (see threading_model_notes.md).
  int *client_rate;
  int *executor_rate;
  unsigned long *executing_thread;
  GetObjectAtOffset(client_rate, 0x007c07dc);
  GetObjectAtOffset(executor_rate, 0x007c07ac);
  GetObjectAtOffset(executing_thread, 0x007b9d7c);
  int rate = (GetCurrentThreadId() == *executing_thread) ? *executor_rate
                                                         : *client_rate;
  g->lifespan_ticks.integer = static_cast<int>(
      static_cast<double>(static_cast<unsigned>(rate)) * d.particle_ttl_seconds);
  return g;
}

// --- destructibility -------------------------------------------------------------------

namespace {
// The three variants share a dtor-only vtable each; the converters install them by
// address, so a native builder has to as well.
void *VtableAt(uintptr_t offset) {
  void *p;
  GetObjectAtOffset(p, offset);
  return p;
}
// Every one of these objects is `{vtbl, tag, ...}` with a *pure* virtual dtor in
// the mirror, so the vptr is written through the raw pointer rather than by
// constructing - there is nothing here C++ can construct.
void InstallVtable(void *object, uintptr_t vtable_offset) {
  *static_cast<void **>(object) = VtableAt(vtable_offset);
}
} // namespace

Destructibility *MakeDestructibility(DestructibilityKind type) {
  void *raw = ZeroedPool(sizeof(Destructibility));
  if (!raw) {
    return nullptr;
  }
  InstallVtable(raw, 0x00663080); // DestructibilityVtbl_
  auto *d = static_cast<Destructibility *>(raw);
  d->tag = type;
  return d;
}

FragData *MakeFragData(const FragDataDesc &desc) {
  void *raw = ZeroedPool(sizeof(FragData));
  if (!raw) {
    return nullptr;
  }
  InstallVtable(raw, 0x00663084); // PTR_FragDataDtor_00663084
  auto *f = static_cast<FragData *>(raw);
  f->tag = DestructibilityKind::FragData;
  f->role = desc.role;
  f->replace_role = desc.replace_role;
  f->remove.reset(desc.remove ? GameStrdup(desc.remove) : nullptr);
  f->scale = desc.scale;
  f->replace = desc.replace;
  f->symmetric = desc.symmetric;
  f->pad[0] = f->pad[1] = 0;
  f->blast_range = static_cast<float>(desc.blast_range);
  f->blast_damage = static_cast<float>(desc.blast_damage);
  return f;
}

// --- role ------------------------------------------------------------------------

namespace {
// Lazily allocated the way ToRole does it: six separate fields can each be the
// first to need one, and each re-checks.
InventoryInfo *EnsureInventoryInfo(Role *role) {
  if (!role->inventory_info) {
    auto *info = static_cast<InventoryInfo *>(ZeroedPool(sizeof(InventoryInfo)));
    role->inventory_info.reset(info);
  }
  return role->inventory_info.get();
}

// GetResourceString, borrowed - the strings live in the active glres*.dll and
// ToRole stores the pointer without copying.
char *ResourceStringPtr(int32_t id) {
  static void *LocalizedStrings;
  static FastCall<const char *, void *, unsigned> GetResourceString;
  if (!GetResourceString) {
    GetObjectAtOffset(LocalizedStrings, 0x00725664);
    GetObjectAtOffset(GetResourceString, 0x00579000);
  }
  return const_cast<char *>(
      GetResourceString(&LocalizedStrings, static_cast<unsigned>(id)));
}

// The `sever point` string is one comma-separated list; ToRole splits it in place
// and appends a strdup of each token to the role's list.
void AppendSeverPoints(Role *role, const char *csv) {
  if (!csv) {
    return;
  }
  void *node_vtbl;
  GetObjectAtOffset(node_vtbl, 0x0065207c); // TriggerVtbl - the node's dtor slot
  for (const char *cursor = csv; *cursor;) {
    const char *end = cursor;
    while (*end && *end != ',') {
      ++end;
    }
    std::string token(cursor, static_cast<size_t>(end - cursor));
    auto *node = static_cast<List_Member<pool_string> *>(
        ZeroedPool(sizeof(List_Member<pool_string>)));
    if (!node) {
      return;
    }
    *reinterpret_cast<void **>(node) = node_vtbl;
    node->data.reset(GameStrdup(token.c_str()));
    // Tail append against the sentinel CreateRole already linked.
    auto *sentinel = role->sever_points.sentinel;
    node->next = sentinel;
    node->prev = sentinel->prev;
    sentinel->prev->next = node;
    sentinel->prev = node;
    role->sever_points.n_entries++;
    role->sever_points.calculated_indices = false;
    if (role->sever_points.entry_pointers) {
      pool_free(role->sever_points.entry_pointers);
      role->sever_points.entry_pointers = nullptr;
    }
    cursor = *end ? end + 1 : end;
  }
}

// The bounding-box size ToRole derives the character's collision extents from.
// The three sources are the hierarchy (when its +0x58 flag is set), the shape, or
// a unit cube when the role has neither.
Vec3 GeometrySize(const Role *role) {
  auto diff3 = [](const void *base, size_t lo, size_t hi) {
    auto *f = static_cast<const float *>(base);
    return Vec3{f[hi / 4 + 0] - f[lo / 4 + 0], f[hi / 4 + 1] - f[lo / 4 + 1],
                f[hi / 4 + 2] - f[lo / 4 + 2]};
  };
  if (role->hierarchy) {
    if (*(reinterpret_cast<const char *>(role->hierarchy) + 0x58) == 0) {
      return {1.0f, 1.0f, 1.0f};
    }
    return diff3(role->hierarchy, 0x68, 0x74);
  }
  if (role->shape) {
    return diff3(role->shape, 0x48, 0x54);
  }
  return {1.0f, 1.0f, 1.0f};
}
} // namespace

Role *MakeRole(const RoleDesc &d) {
  FastCall<Role *> create_role;
  GetObjectAtOffset(create_role, 0x004add90);
  Role *r = create_role();
  if (!r) {
    return nullptr;
  }
  // CreateRole has already nulled these, but ToRole re-nulls them and the cost is
  // nothing.
  r->shape = nullptr;
  r->hierarchy = nullptr;
  r->pgen.reset();
  r->pgen2.reset();

  r->pgen.reset(d.pgen);
  r->pgen2.reset(d.pgen2);

  if (d.shape) {
    r->shape = d.shape;
  } else if (d.hierarchy) {
    r->hierarchy = d.hierarchy;
    r->hotspot_point = {0, 0, 0};
    if (d.hotspot) {
      r->hotspot.reset(GameStrdup(d.hotspot));
      // ThisCall, not FastCall, and that is measured rather than chosen: the
      // hierarchy arrives in ECX (`MOV EDI,ECX` at 0x005948bb) but BOTH remaining
      // arguments are on the stack - the out Vec3 at [EBP+8] (written by the
      // `MOVQ [EBX]` / `MOV [EBX+8]` pair) and the node name at [EBP+0xc] (the
      // second operand of the stricmp at 0x005948d2) - and it ends in `RET 0x8`.
      // Declared __fastcall the Vec3 went to EDX where nothing reads it, the name
      // landed in the out-pointer slot, and the name argument was read off
      // whatever the caller's stack happened to hold: an access violation inside
      // ___ascii_stricmp, every time, which is what make.role used to do.
      ThisCall<void, Hierarchy *, Vec3 *, const char *> resolve;
      GetObjectAtOffset(resolve, 0x00594890); // HierarchyResolveNamedPointPos
      resolve(r->hierarchy, &r->hotspot_point, d.hotspot);
    }
    // The alternate hotspot starts as a copy of the main one, so a hierarchy with
    // only a `hotspot` gives both points the same value.
    r->alternate_hotspot_point = r->hotspot_point;
    if (d.alternate_hotspot) {
      r->alternate_hotspot.reset(GameStrdup(d.alternate_hotspot));
      ThisCall<void, Hierarchy *, Vec3 *, const char *> resolve;
      GetObjectAtOffset(resolve, 0x00594890);
      resolve(r->hierarchy, &r->alternate_hotspot_point, d.alternate_hotspot);
    }
    // Node-presence counts over slots 21..25 and 26..30, gated on the hierarchy
    // actually having a node table at +0x74.
    if (*reinterpret_cast<void *const *>(
            reinterpret_cast<const char *>(r->hierarchy) + 0x74)) {
      FastCall<char, Hierarchy *, int> has_node;
      GetObjectAtOffset(has_node, 0x005bdca0); // HierarchyHasNode
      for (int slot = 26; slot < 31; ++slot) {
        if (has_node(r->hierarchy, slot - 5)) {
          r->num_hier_nodes_21_25++;
        }
        if (has_node(r->hierarchy, slot)) {
          r->num_hier_nodes_26_30++;
        }
      }
    }
  }

  if (d.inventory_shape || d.inventory_hierarchy) {
    InventoryInfo *info = EnsureInventoryInfo(r);
    if (info) {
      info->shape = d.inventory_shape;
      info->hierarchy = d.inventory_hierarchy;
    }
  }

  if (d.identifier) {
    r->name.reset(GameStrdup(d.identifier));
  }
  if (d.description != 0) {
    if (InventoryInfo *info = EnsureInventoryInfo(r)) {
      info->description = ResourceStringPtr(d.description);
    }
  }
  if (d.recon_name != 0) {
    r->recon_name = ResourceStringPtr(d.recon_name);
  }
  if (d.recon_ai_short != 0) {
    r->recon_ai_short = ResourceStringPtr(d.recon_ai_short);
  }
  if (d.recon_ai_number != 0) {
    r->recon_ai_number = d.recon_ai_number;
  }
  if (d.recon_ai_long != 0) {
    r->recon_ai_long = ResourceStringPtr(d.recon_ai_long);
  }
  if (d.recon_ai_long2 != 0) {
    r->recon_ai_long2 = ResourceStringPtr(d.recon_ai_long2);
  }
  if (d.pickup_name != 0) {
    if (InventoryInfo *info = EnsureInventoryInfo(r)) {
      info->pickup_name = ResourceStringPtr(d.pickup_name);
    }
  }
  if (d.action_on_death != 0) {
    if (InventoryInfo *info = EnsureInventoryInfo(r)) {
      info->action_on_death = d.action_on_death;
    }
  }
  // Note the default is 6.0, not 0 - so in practice every role built through the
  // GLS path ends up with an InventoryInfo whether it is a pickup or not.
  if (d.pickup_radius != 0.0) {
    if (InventoryInfo *info = EnsureInventoryInfo(r)) {
      info->pickup_radius = static_cast<float>(d.pickup_radius);
    }
  }

  r->light.reset(d.light);
  r->projectile.reset(d.projectile);
  if (d.character) {
    r->character.reset(d.character);
    Character *c = d.character;
    c->derived_hier_extent = 0;
    Vec3 size = GeometrySize(r);
    if (r->hierarchy) {
      // Only a multi-node hierarchy contributes this one.
      auto *nodes = *reinterpret_cast<void *const *>(
          reinterpret_cast<const char *>(r->hierarchy) + 0x74);
      if (nodes && *(reinterpret_cast<const int *>(nodes) + 1) > 1) {
        auto *table = *reinterpret_cast<void *const *>(
            reinterpret_cast<const char *>(r->hierarchy) + 0x90);
        c->derived_hier_extent = *(reinterpret_cast<const int *>(table) + 5);
        c->derived_hier_extent =
            static_cast<int>(static_cast<float>(c->derived_hier_extent) * c->size);
      }
    }
    c->derived_radius = (size.x <= size.z ? size.z : size.x) * 0.5f;
    c->derived_height = size.y;
    // A GLS radius/height of 0 means "use the model's own extents", which is the
    // default and what almost every shipped character relies on.
    if (c->radius_times_size == 0.0f) {
      c->radius_times_size = c->derived_radius;
    }
    if (c->height_times_size == 0.0f) {
      c->height_times_size = c->derived_height;
    }
  }

  AppendSeverPoints(r, d.sever_points);
  if (d.meta_sound) {
    r->meta_sound.reset(GameStrdup(d.meta_sound));
  }

  r->ai = d.ai;
  r->interface_beam_delay = d.interface_beam_delay;
  r->interface_beam_effect = d.interface_beam_effect;
  if (d.interface_beam_effect == VulnerabilityType::Script) {
    // ToRole abandons the whole role here when the script is missing - and leaks
    // it, because it bails before caching. Refusing up front is the same
    // behaviour without the leak.
    if (!d.interface_beam_script) {
      Print("MakeRole: interface beam effect is script but no script given");
      return nullptr;
    }
    r->interface_beam_script = GameStrdup(d.interface_beam_script);
  }
  r->interface_beam_duration = d.interface_beam_duration;
  r->resistance = d.resistance;
  r->resistance_factor = static_cast<float>(d.resistance_factor);

  // ToRole drops a chunk hanging off the shape at +0x8c for everything that is
  // not a TrackObject - shared geometry the role does not need a private copy of.
  if (r->shape && r->ai != AIType::TrackObject) {
    void **extra = reinterpret_cast<void **>(
        reinterpret_cast<char *>(r->shape) + 0x8c);
    if (*extra) {
      FastCall<void, void *> release;
      GetObjectAtOffset(release, 0x00595c60);
      release(*extra);
      pool_free(*extra);
      *extra = nullptr;
    }
  }

  r->alpha_fogging = d.alpha_fogging;
  // Alpha fogging wins: the two are mutually exclusive and ToRole forces the
  // per-vertex bit off rather than reporting a conflict.
  r->per_vertex_fogging = !d.alpha_fogging && d.per_vertex_fogging;
  r->reflective = d.reflective;
  r->destination_selectable = d.destination_selectable;
  r->destroy_after_collection = d.destroy_after_collection;
  r->moves_on_lifts = d.moves_on_lifts;
  r->status_display = d.status_display;
  r->hit_test_ignore = d.hit_test_ignore;
  r->no_lighting = d.no_lighting;
  r->frag_control = d.frag_control;

  r->limit = d.limit;
  r->alpha = static_cast<float>(d.alpha);
  r->destructibility.reset(d.destructibility);
  r->armor = static_cast<float>(d.armor);
  r->shields = static_cast<float>(d.shields);
  r->recharge_rate = static_cast<float>(d.recharge_rate);
  return r;
}

// --- ammo and ammo info ------------------------------------------------------------

bool MakeAmmo(const AmmoDesc &d) {
  if (d.ammo_type < 0 || d.ammo_type > MaxAmmoType || d.weapon_type < 0 ||
      d.weapon_type > MaxWeaponType) {
    return false;
  }
  Ammo **slot =
      &GetAmmoTable()[d.ammo_type + d.weapon_type * AmmoTypeCount];
  // ToAmmo's gate: it only fills an empty slot, so redefining a pair is a silent
  // no-op rather than an overwrite.
  if (*slot) {
    return false;
  }
  auto *a = static_cast<Ammo *>(ZeroedPool(sizeof(Ammo)));
  if (!a) {
    return false;
  }
  *slot = a;
  a->round_time = static_cast<float>(d.round_time);
  a->reload_time = static_cast<float>(d.reload_time);
  a->life_timer = d.life_timer;
  a->magazine_size = d.magazine_size;
  a->sound = d.sound;
  a->salvo_size = d.salvo_size;
  a->file.reset(GameStrdup(d.file));
  a->name.reset(GameStrdup(d.name));
  a->firing_speed = static_cast<float>(d.firing_speed);
  a->role = d.role;
  return true;
}

bool MakeAmmoInfo(const AmmoInfoDesc &d) {
  if (d.ammo_type < 0 || d.ammo_type > MaxAmmoType) {
    return false;
  }
  AmmoInfo &info = GetAmmoInfos()[d.ammo_type];
  // Never both: ToAmmoInfo nulls whichever the section did not name.
  info.shape = d.shape;
  info.hierarchy = d.shape ? nullptr : d.hierarchy;
  info.ammo_name = d.ammo_name;
  info.description = d.description;
  info.max_per_slot = d.max_per_slot;
  return true;
}

// --- camera track ----------------------------------------------------------------------

bool MakeCameraTrack(const CameraTrackDesc &d) {
  // Both are required by the converter, and `file` is not merely optional here:
  // AcquireLevelRifForLocators strlen's ECX with no null check.
  if (!d.name || !d.file) {
    return false;
  }
  FastCall<void *, const char *> acquire_rif;
  GetObjectAtOffset(acquire_rif, 0x00483da0);
  void *rif = acquire_rif(d.file);
  if (!rif) {
    return false;
  }
  Map *map = GetCurrentMap();
  if (!map) {
    return false;
  }

  void *raw = pool_alloc(0xa0);
  if (!raw) {
    return false;
  }
  FastCall<void *, void *> track_ctor;
  GetObjectAtOffset(track_ctor, 0x004dc660);
  auto *track = static_cast<void **>(track_ctor(raw));
  if (!track) {
    return false;
  }
  track[0x1a] = d.pgen;  // +0x68
  track[0x1b] = d.pgen2; // +0x6c

  // LoadCameraTrackFromRif: walks the rif's REBENVDT/SPECLOBJ for the CUTSHEAD
  // whose CUTSCDAT name matches, and loads the path into the track offset by the
  // map origin. Failure destroys the object through its own slot 0, which is what
  // the converter does rather than leaking it.
  //
  // The track is in ECX and the rif in EDX; the name and a *by-value* Vec3 are the
  // only stack arguments, which is the RET 0x10. Passing the origin as three
  // floats instead would push the same 12 bytes but move the name into ECX.
  FastCall<char, void *, void *, const char *, Vec3> bind;
  GetObjectAtOffset(bind, 0x005aa920);
  if (!bind(track, rif, d.name, map->neg_origin)) {
    auto **vtbl = static_cast<void ***>(static_cast<void *>(track));
    auto dtor = reinterpret_cast<ThisCall<void *, void *, unsigned char>>((*vtbl)[0]);
    dtor(track, 1);
    return false;
  }
  return true;
}

ReplaceDestructibility *MakeReplaceDestructibility(const char *script,
                                                   bool replace) {
  void *raw = ZeroedPool(sizeof(ReplaceDestructibility));
  if (!raw) {
    return nullptr;
  }
  InstallVtable(raw, 0x00663088); // PTR_ReplaceDestructibilityDtor_00663088
  auto *r = static_cast<ReplaceDestructibility *>(raw);
  r->tag = DestructibilityKind::ReplaceDestructibility;
  r->script.reset(script ? GameStrdup(script) : nullptr);
  r->replace = replace;
  r->pad[0] = r->pad[1] = r->pad[2] = 0;
  return r;
}
} // namespace gk
