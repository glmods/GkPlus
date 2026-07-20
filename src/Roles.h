#pragma once

#include "LuaEngine.h"
#include "Module.h"

#include <string_view>

namespace gk {
struct Role;

// The Role's AI class (Role.ai @ 0x7c). Selects the spawned Actor subclass in the game's
// CreateActor @ 0x00510760. Values verified against the game's AIType enum and the
// gk.ai `types` table. See role_system_notes.md / role_subobjects_notes.md.
enum class AIType : int {
  Bot,
  Scavenger,
  Mine,
  Minebot,
  Reserved,
  Blocker,
  Waiting,
  Pathfinder,
  TrackObject,
  Tumbleweed,
  Pickup,
  BackgroundCreature,
  FlyingBackgroundCreature,
  Centipede,
  Centibody,
  Node,
  NodeWaiting,
  Swarm,
  Popup,
  President,
  Turret,
  Count,
};

struct RoleWrapper final {
  static constexpr const char *metatable_name = "Role";
  static void setup_metatable(lua_State *L);

  Role *role;
  RoleWrapper(Role *role);

  bool operator==(const RoleWrapper &) const;

  int to_string(lua_State *L) const;

  int get_id();
  int get_type();
  std::string_view get_name();
  void get_vulnerabilities(lua_State *L);

  int spawn(lua_State *L);

  using type = RoleWrapper;
  using fields = Lua::Fields<
      Lua::Getter<"id", &type::get_id>, Lua::Getter<"type", &type::get_type>,
      Lua::Getter<"name", &type::get_name>,
      Lua::TableGetter<"vulnerabilities", &type::get_vulnerabilities>,
      Lua::Function<"spawn", type, &type::spawn>>;
};

class RolesModule final : public Module<RolesModule> {
public:
  static constexpr const char *module_name = "gk.roles";

  RolesModule(lua_State *L);

  int Register(lua_State *L);
};
} // namespace gk