#pragma once
#include <lua.hpp>

#include <concepts>
#include <optional>
#include <string_view>
#include <utility>

namespace gk::Lua {
void Init();

lua_State *GetEngine();

void Close();

template <typename T>
concept LuaObject = requires(lua_State *L) {
  { T::metatable_name } -> std::convertible_to<const char *>;
  { T::setup_metatable(L) };
};

template <typename T>
concept HasToString = requires(lua_State *L, T *obj) {
  { obj->to_string(L) } -> std::convertible_to<int>;
};

template <size_t N> struct StringLiteral {
  constexpr StringLiteral(const char (&str)[N]) { std::copy_n(str, N, value); }

  char value[N]{};
};

template <typename... T> struct Fields {};

template <typename T>
concept HasFields = requires() { typename T::fields; };

namespace detail {
template <typename T> struct interop;

template <> struct interop<int> {
  int check(lua_State *L, int idx);
  int opt(lua_State *L, int idx, int def);
  void push(lua_State *L, int value);
};

template <> struct interop<float> {
  float check(lua_State *L, int idx);
  float opt(lua_State *L, int idx, float def);
  void push(lua_State *L, float value);
};

template <> struct interop<double> {
  double check(lua_State *L, int idx);
  double opt(lua_State *L, int idx, double def);
  void push(lua_State *L, double value);
};

template <> struct interop<bool> {
  bool to(lua_State *L, int idx);
  void push(lua_State *L, bool value);
};

template <> struct interop<std::string_view> {
  std::string_view check(lua_State *L, int idx);
  std::string_view to(lua_State *L, int idx);
  std::string_view opt(lua_State *L, int idx, std::string_view def);
  void push(lua_State *L, std::string_view value);
};

template <> struct interop<const char *> {
  const char *check(lua_State *L, int idx);
  const char *to(lua_State *L, int idx);
  const char *opt(lua_State *L, int idx, const char *def);
  void push(lua_State *L, const char *value);
};

template <LuaObject T> struct interop<T *> {
  T *check(lua_State *L, int idx) {
    return static_cast<T *>(luaL_checkudata(L, idx, T::metatable_name));
  }
};
} // namespace detail

template <typename T> T check(lua_State *L, int idx) {
  detail::interop<T> interop;
  return interop.check(L, idx);
}

template <typename T> T to(lua_State *L, int idx) {
  detail::interop<T> interop;
  return interop.to(L, idx);
}

template <typename T> T opt(lua_State *L, int idx, T def) {
  detail::interop<T> interop;
  return interop.opt(L, idx, def);
}

template <typename T, typename _T = std::remove_cvref_t<T>>
void push(lua_State *L, T &&value) {
  detail::interop<_T> interop;
  interop.push(L, std::forward<T>(value));
}

template <StringLiteral Name, LuaObject T, int (T::*Func)(lua_State *)>
struct Function {
  static constexpr const char *name = Name.value;
  static constexpr int (T::*value)(lua_State *L) = Func;

  static void get(lua_State *L) {
    lua_pushcfunction(L, ([](lua_State *L) {
                        auto self = ::gk::Lua::check<T *>(L, 1);
                        return (self->*value)(L);
                      }));
  }

  static void set(lua_State *L) {
    luaL_error(L, "%s", "Attempt to assign a read-only field");
  }
};

namespace detail {
template <typename T, typename C> static T getMemberType(T C::*);
template <typename T, typename C> static C getClassType(T C::*);

template <typename T, typename C> static T getMemberType(T (C::*)());
template <typename T, typename C> static C getClassType(T (C::*)());

template <typename T, typename C> static T getMemberType(void (C::*)(T));
template <typename T, typename C> static C getClassType(void (C::*)(T));

template <typename C> static C getClassType(void (C::*)(lua_State *));
template <typename C> static C getClassType(int (C::*)(lua_State *));

template <auto Member> using class_type = decltype(getClassType(Member));
template <auto Member> using member_type = decltype(getMemberType(Member));
} // namespace detail

template <StringLiteral Name, auto Member> struct Slot {
  static constexpr const char *name = Name.value;
  using member_type = ::gk::Lua::detail::member_type<Member>;
  using class_type = ::gk::Lua::detail::class_type<Member>;

  static void get(lua_State *L) {
    auto self = ::gk::Lua::check<class_type *>(L, 1);
    ::gk::Lua::push(L, (self->*Member));
  }

  static void set(lua_State *L) {
    auto self = ::gk::Lua::check<class_type *>(L, 1);
    auto v = ::gk::Lua::check<member_type>(L, 3);
    (self->*Member) = v;
  }
};

template <StringLiteral Name, auto Member> struct ROSlot {
  static constexpr const char *name = Name.value;
  using member_type = ::gk::Lua::detail::member_type<Member>;
  using class_type = ::gk::Lua::detail::class_type<Member>;

  static void get(lua_State *L) {
    auto self = ::gk::Lua::check<class_type *>(L, 1);
    ::gk::Lua::push(L, (self->*Member));
  }

  static void set(lua_State *L) {
    luaL_error(L, "%s", "Attempt to assign a read-only field");
  }
};

template <StringLiteral Name, typename TValue, TValue **Value>
struct StaticSlot {
  static constexpr const char *name = Name.value;
  static constexpr TValue **value = Value;

  static void get(lua_State *L) { ::gk::Lua::push(L, **value); }

  static void set(lua_State *L) {
    auto v = ::gk::Lua::check<TValue>(L, 3);
    **value = v;
  }
};

template <StringLiteral Name, auto Member> struct Getter {
  static constexpr const char *name = Name.value;
  using member_type = ::gk::Lua::detail::member_type<Member>;
  using class_type = ::gk::Lua::detail::class_type<Member>;

  static void get(lua_State *L) {
    auto self = ::gk::Lua::check<class_type *>(L, 1);
    ::gk::Lua::push(L, (self->*Member)());
  }

  static void set(lua_State *L) {
    luaL_error(L, "%s", "Attempt to assign a read-only field");
  }
};

template <StringLiteral Name, typename TValue, TValue (*Value)()>
struct StaticGetter {
  static constexpr const char *name = Name.value;
  static constexpr TValue (*value)() = Value;

  static void get(lua_State *L) { ::gk::Lua::push(L, (*value)()); }

  static void set(lua_State *L) {
    luaL_error(L, "%s", "Attempt to assign a read-only field");
  }
};

template <StringLiteral Name, auto Func> struct StaticFunction {
  static constexpr const char *name = Name.value;

  static void get(lua_State *L) { lua_pushcfunction(L, Func); }

  static void set(lua_State *L) {
    luaL_error(L, "%s", "Attempt to assign a read-only field");
  }
};

template <StringLiteral Name, auto Member> struct TableGetter {
  static constexpr const char *name = Name.value;
  using class_type = ::gk::Lua::detail::class_type<Member>;

  static void get(lua_State *L) {
    auto self = ::gk::Lua::check<class_type *>(L, 1);
    lua_newtable(L);
    (self->*Member)(L);
  }

  static void set(lua_State *L) {
    luaL_error(L, "%s", "Attempt to assign a read-only field");
  }
};

template <StringLiteral Name, auto Getter, auto Setter> struct GetterSetter {
  static constexpr const char *name = Name.value;
  using getter_member_type = ::gk::Lua::detail::member_type<Getter>;
  using getter_class_type = ::gk::Lua::detail::class_type<Getter>;
  using setter_member_type = ::gk::Lua::detail::member_type<Setter>;
  using setter_class_type = ::gk::Lua::detail::class_type<Setter>;

  static void get(lua_State *L) {
    auto self = ::gk::Lua::check<getter_class_type *>(L, 1);
    ::gk::Lua::push(L, (self->*Getter)());
  }

  static void set(lua_State *L) {
    auto self = ::gk::Lua::check<setter_class_type *>(L, 1);
    auto value = ::gk::Lua::check<setter_member_type>(L, 3);
    (self->*Setter)(value);
  }
};

namespace detail {
template <typename T> struct fields;
template <> struct fields<Fields<>> {
  static int get(lua_State *L) { return luaL_error(L, "%s", "Unknown field"); }
  static int set(lua_State *L) { return luaL_error(L, "%s", "Unknown field"); }
};

template <typename T, typename... Ts> struct fields<Fields<T, Ts...>> {
  static int get(lua_State *L) {
    auto name = ::gk::Lua::to<std::string_view>(L, 2);
    if (name == T::name) {
      T::get(L);
      return 1;
    }
    return fields<Fields<Ts...>>::get(L);
  }

  static int set(lua_State *L) {
    auto name = ::gk::Lua::to<std::string_view>(L, 2);
    if (name == T::name) {
      T::set(L);
      return 0;
    }
    return fields<Fields<Ts...>>::set(L);
  }
};
} // namespace detail

template <LuaObject T, typename... Args>
T *Create(lua_State *L, Args &&...args) {
  auto value = lua_newuserdata(L, sizeof(T));
  new (value) T(std::forward<Args>(args)...);

  if (luaL_newmetatable(L, T::metatable_name)) {
    lua_pushcfunction(L, [](lua_State *L) {
      auto obj = check<T *>(L, 1);
      obj->~T();
      return 0;
    });
    lua_setfield(L, -2, "__gc");

    if constexpr (HasToString<T>) {
      lua_pushcfunction(L, [](lua_State *L) {
        auto obj = check<T *>(L, 1);
        return obj->to_string(L);
      });
      lua_setfield(L, -2, "__tostring");
    }

    if constexpr (std::equality_comparable<T>) {
      lua_pushcfunction(L, [](lua_State *L) {
        auto a = check<T *>(L, 1);
        auto b = check<T *>(L, 2);
        lua_pushboolean(L, *a == *b);
        return 1;
      });
      lua_setfield(L, -2, "__eq");
    }

    if constexpr (HasFields<T>) {
      lua_pushcfunction(L, ([](lua_State *L) {
                          return detail::fields<typename T::fields>::get(L);
                        }));
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, ([](lua_State *L) {
                          return detail::fields<typename T::fields>::set(L);
                        }));
      lua_setfield(L, -2, "__newindex");
    }

    T::setup_metatable(L);
  }
  lua_setmetatable(L, -2);

  return static_cast<T *>(value);
}

namespace detail {
template <LuaObject T> struct interop<T> {
  T check(lua_State *L, int idx) {
    return *static_cast<T *>(luaL_checkudata(L, idx, T::metatable_name));
  }
  void push(lua_State *L, T &&obj) { Create<T>(L, std::forward<T>(obj)); }
};

template <typename T> struct interop<std::optional<T>> {
  void push(lua_State *L, std::optional<T> &&value) {
    if (value.has_value()) {
      ::gk::Lua::push(L, std::forward<T>(value.value()));
    } else {
      lua_pushnil(L);
    }
  }
};
} // namespace detail

template <LuaObject T, int (T::*func)(lua_State *)>
void PushMemberFunction(lua_State *L) {
  lua_pushcfunction(L, [](lua_State *L) {
    auto obj = check<T *>(L, 1);
    return (obj->*func)(L);
  });
}

} // namespace gk::Lua