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
