#include "Roles.h"

#include "Core.h"

#include <array>
#include <cstring>

namespace gk {
Roles *GetRolesTable() {
  Roles *table;
  GetObjectAtOffset(table, 0x007b48f0);
  return table;
}

Role *GetRoleByName(const char *name) {
  FastCall<Role *, const char *> fn;
  GetObjectAtOffset(fn, 0x004ae030);
  return fn(name);
}

Role *GetRoleById(int id) {
  FastCall<Role *, int> fn;
  GetObjectAtOffset(fn, 0x004ae0d0);
  return fn(id);
}

int SpawnRole(int team_id, Role *role, Vec3 *position, Vec4 *orientation,
              int owner_id) {
  FastCall<int, int, Role *, Vec3 *, Vec4 *, int> fn;
  GetObjectAtOffset(fn, 0x00503710);
  return fn(team_id, role, position, orientation, owner_id);
}

namespace {
// Recovered with gls::ProbeKeywords. Sparse on purpose: 10 and 15 are not named
// by any shipped script, and 33 ("none") has no keyword at all.
constexpr EnumEntry WeaponTypes[] = {
    {"plasma pistol", 0},
    {"plasma pistol training", 1},
    {"plasmagnum", 2},
    {"plasmatrix", 3},
    {"laser", 4},
    {"binary laser", 5},
    {"maxim laser", 6},
    {"grenade launcher", 7},
    {"missile launcher", 8},
    {"flamethrower", 9},
    {"nanofrag", 11},
    {"epulsar", 12},
    {"repair arm", 13},
    {"interface arm", 14},
    {"enemy plasma weak", 16},
    {"enemy plasma medium", 17},
    {"enemy plasma strong", 18},
    {"enemy laser weak", 19},
    {"enemy laser medium", 20},
    {"enemy laser strong", 21},
    {"enemy grenade launcher basic", 22},
    {"enemy grenade launcher plus", 23},
    {"enemy missile launcher basic", 24},
    {"enemy missile launcher plus", 25},
    {"enemy missile launcher basic slow reload", 26},
    {"enemy laser adversor", 27},
    {"enemy plasma pulsax", 28},
    {"enemy plasma pulsox", 29},
    {"enemy plasma mini pulsox", 30},
    {"enemy epulsar obliteron", 31},
    {"enemy epulsar obliteron deadly", 32},
};

// Complete, 0..18.
constexpr EnumEntry AmmoTypes[] = {
    {"needles", 0},        {"flares", 1},         {"plasma bolts", 2},
    {"plasmaxi bolts", 3}, {"plasma shells", 4},  {"autolock bolts", 5},
    {"battery basic", 6},  {"battery plus", 7},   {"energy cells", 8},
    {"grenade basic", 9},  {"grenade plus", 10},  {"grenade EMP", 11},
    {"missile EMP", 12},   {"missile basic", 13}, {"missile plus", 14},
    {"flames", 15},        {"napalm", 16},        {"nanotech dismantler", 17},
    {"none needed", 18},
};

constexpr EnumEntry ActionsOnDeath[] = {
    {"must drop", 1},
    {"must not drop", 2},
};

constexpr EnumEntry Resistances[] = {
    {"resists laser", 2},
    {"resists explosives", 4},
    {"resists epulsar", 6},
    {"resists small arms", 8},
};

// Ids 2, 9 and 11 come from the console table (GetParticleIDFromName); the rest
// were probed. 8 is the one id neither source names.
constexpr EnumEntry ParticleTypes[] = {
    {"smoke", 0},      {"steam", 1},         {"snow", 2},
    {"fire", 3},       {"shot", 4},          {"explosion", 5},
    {"big explosion", 6}, {"corona", 7},     {"trail", 9},
    {"laser trail", 10}, {"rain", 11},       {"sparks", 12},
};

// '_' and ' ' are the same character, so a script may write either spelling -
// the same rule gk::gls::FindField uses for field names.
bool EnumNameMatches(const char *a, const char *b) {
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

template <size_t N>
const char *LookupName(const EnumEntry (&table)[N], int value) {
  for (const EnumEntry &e : table) {
    if (e.value == value) {
      return e.name;
    }
  }
  return nullptr;
}

template <size_t N>
int LookupValue(const EnumEntry (&table)[N], const char *name, int fallback) {
  for (const EnumEntry &e : table) {
    if (EnumNameMatches(e.name, name)) {
      return e.value;
    }
  }
  return fallback;
}
} // namespace

const EnumEntry *WeaponTypeNames(size_t *count) {
  if (count) {
    *count = std::size(WeaponTypes);
  }
  return WeaponTypes;
}
const char *WeaponTypeName(int value) { return LookupName(WeaponTypes, value); }
int WeaponTypeFromName(const char *name) {
  return LookupValue(WeaponTypes, name, -1);
}

const EnumEntry *AmmoTypeNames(size_t *count) {
  if (count) {
    *count = std::size(AmmoTypes);
  }
  return AmmoTypes;
}
const char *AmmoTypeName(int value) { return LookupName(AmmoTypes, value); }
int AmmoTypeFromName(const char *name) {
  return LookupValue(AmmoTypes, name, -1);
}

const char *ActionOnDeathName(ActionOnDeath value) {
  return LookupName(ActionsOnDeath, static_cast<int>(value));
}
ActionOnDeath ActionOnDeathFromName(const char *name) {
  return static_cast<ActionOnDeath>(LookupValue(
      ActionsOnDeath, name, static_cast<int>(ActionOnDeath::Unspecified)));
}

const char *ResistanceName(Resistance value) {
  return LookupName(Resistances, static_cast<int>(value));
}
Resistance ResistanceFromName(const char *name) {
  return static_cast<Resistance>(
      LookupValue(Resistances, name, static_cast<int>(Resistance::None)));
}

const char *ParticleTypeName(ParticleType value) {
  return LookupName(ParticleTypes, static_cast<int>(value));
}
ParticleType ParticleTypeFromName(const char *name) {
  return static_cast<ParticleType>(
      LookupValue(ParticleTypes, name, static_cast<int>(ParticleType::Explosion)));
}

AmmoInfo *GetAmmoInfos() {
  AmmoInfo *p;
  GetObjectAtOffset(p, 0x007b5d40);
  return p;
}

Ammo **GetAmmoTable() {
  Ammo **p;
  GetObjectAtOffset(p, 0x007b5ec0);
  return p;
}

Ammo *GetAmmo(int ammo_type, int weapon_type) {
  if (ammo_type < 0 || ammo_type > MaxAmmoType || weapon_type < 0 ||
      weapon_type > MaxWeaponType) {
    return nullptr;
  }
  return GetAmmoTable()[ammo_type + weapon_type * AmmoTypeCount];
}

namespace {
// index (== AIType) -> lowercase name; kept in lockstep with enum class AIType
// (Roles.h) by the array size against AIType::Count.
constexpr std::array<const char *, static_cast<size_t>(AIType::Count)>
    ai_type_names = {
        "bot",
        "scavenger",
        "mine",
        "minebot",
        "reserved",
        "blocker",
        "waiting",
        "pathfinder",
        "track_object",
        "tumbleweed",
        "pickup",
        "background_creature",
        "flying_background_creature",
        "centipede",
        "centibody",
        "node",
        "node_waiting",
        "swarm",
        "popup",
        "president",
        "turret",
};
} // namespace

const char *AITypeName(AIType type) {
  auto i = static_cast<size_t>(type);
  if (i >= ai_type_names.size()) {
    return nullptr;
  }
  return ai_type_names[i];
}

AIType AITypeFromName(const char *name) {
  if (name) {
    for (size_t i = 0; i < ai_type_names.size(); ++i) {
      if (std::strcmp(name, ai_type_names[i]) == 0) {
        return static_cast<AIType>(i);
      }
    }
  }
  return AIType::Count;
}
} // namespace gk
