#pragma once

#include <lua.hpp>

#include <imgui.h>

namespace gk {
struct GuiFont {
  static constexpr const char *metatable_name = "GuiFont";
  static void setup_metatable(lua_State *L);

  ImFont *font;
  bool operator==(const GuiFont &) const;
};

void PushImgui(lua_State *L);
} // namespace gk