#pragma once

#include "LuaEngine.h"
#include "Module.h"

#include <string_view>

namespace gk {
struct Role;

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