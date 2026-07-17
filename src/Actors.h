#pragma once

#include "LuaEngine.h"
#include "Math.h"
#include "Module.h"
#include "Roles.h"

namespace gk {
struct Actor;
struct Character;

struct ActorWrapper final {
  static constexpr const char *metatable_name = "Actor";
  static void setup_metatable(lua_State *L);

  Actor *actor;

  bool operator==(const ActorWrapper &other) const;

  ActorWrapper(Actor *actor);

  int to_string(lua_State *L) const;

  int new_index(lua_State *);

  int get_id();
  Vec3 get_position();
  Vec4 get_orientation();
  int get_team_id();
  RoleWrapper get_role();
  Vec3 get_center();
  int get_ai_type();
  void get_vulnerabilities(lua_State *L);
  float get_health();

  using type = ActorWrapper;
  using fields = Lua::Fields<
      Lua::Getter<"id", &type::get_id>,
      Lua::Getter<"position", &type::get_position>,
      Lua::Getter<"orientation", &type::get_orientation>,
      Lua::Getter<"team_id", &type::get_team_id>,
      Lua::Getter<"role", &type::get_role>,
      Lua::Getter<"center", &type::get_center>,
      Lua::Getter<"ai_type", &type::get_ai_type>,
      Lua::TableGetter<"vulnerabilities", &type::get_vulnerabilities>,
      Lua::Getter<"health", &type::get_health>>;
};

class ActorsModule final : public Module<ActorsModule> {
public:
  static constexpr const char *module_name = "gk.actors";

  ActorsModule(lua_State *L);

  int Register(lua_State *L);
};
} // namespace gk