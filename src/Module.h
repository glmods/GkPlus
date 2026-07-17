#pragma once

#include <lua.hpp>

namespace gk {

template <typename T>
concept HasRegister = requires(T *t, lua_State *L) { t->Register(L); };

template <typename Derived> class Module {
public:
  Module(lua_State *L) {
    if constexpr (HasRegister<Derived>) {
      lua_getglobal(L, "package");
      lua_getfield(L, -1, "preload");

      lua_pushlightuserdata(L, this);
      lua_pushcclosure(
          L,
          [](lua_State *L) {
            auto mod =
                static_cast<Derived *>(lua_touserdata(L, lua_upvalueindex(1)));
            return mod->Register(L);
          },
          1);
      lua_setfield(L, -2, Derived::module_name);
      lua_pop(L, 2);
    }
  }
};
} // namespace gk