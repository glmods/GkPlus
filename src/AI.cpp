#include "AI.h"
#include "Roles.h"

#include <array>

namespace gk {

AIModule::AIModule(lua_State *L) : Module{L} {}

// gk.ai.types: index (== AIType) -> lowercase name, and name -> index. Kept in lockstep
// with enum class AIType (Roles.h) by the static_assert below.
static std::array<const char *, static_cast<size_t>(AIType::Count)> ai_types = {
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

int AIModule::Register(lua_State *L) {
  lua_newtable(L);

  lua_createtable(L, ai_types.size(), ai_types.size());
  for (int i = 0; i < ai_types.size(); ++i) {
    lua_pushstring(L, ai_types[i]);
    lua_seti(L, -2, i);

    lua_pushinteger(L, i);
    lua_setfield(L, -2, ai_types[i]);
  }
  lua_setfield(L, -2, "types");

  return 1;
}
} // namespace gk