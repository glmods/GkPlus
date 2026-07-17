#include "ImGuiBindings.h"
#include "LuaEngine.h"

#include <imgui.h>

namespace gk {
void GuiFont::setup_metatable(lua_State *L) {}

bool GuiFont::operator==(const GuiFont &) const = default;

static int LuaNextWindowSize(lua_State *L) {
  auto width = (float)luaL_checknumber(L, 1);
  auto height = (float)luaL_checknumber(L, 2);
  ImGui::SetNextWindowSize({width, height}, ImGuiCond_FirstUseEver);
  return 0;
}

static int LuaWindow(lua_State *L) {
  auto name = Lua::to<const char *>(L, 1);
  bool open;
  if (ImGui::Begin(name, &open)) {
    lua_pushvalue(L, 2);
    int res = lua_pcall(L, 0, 0, 0);
    ImGui::End();

    if (res != LUA_OK) {
      return lua_error(L);
    }

    lua_pushboolean(L, 1);
    lua_pushboolean(L, open);
    return 2;
  }

  ImGui::End();
  lua_pushboolean(L, 0);
  return 1;
}

static int LuaChild(lua_State *L) {
  auto name = Lua::to<const char *>(L, 1);
  ImVec2 sz{
      Lua::opt<float>(L, 3, 0.f),
      Lua::opt<float>(L, 4, 0.f),
  };

  if (ImGui::BeginChild(name, sz)) {
    lua_pushvalue(L, 2);
    int res = lua_pcall(L, 0, 0, 0);
    if (res != LUA_OK) {
      ImGui::EndChild();
      return lua_error(L);
    }
  }

  ImGui::EndChild();
  return 0;
}

static int LuaSeparator(lua_State *L) {
  ImGui::Separator();
  return 0;
}

static int LuaSameLine(lua_State *L) {
  ImGui::SameLine();
  return 0;
}

static int LuaNewLine(lua_State *L) {
  ImGui::NewLine();
  return 0;
}

static int LuaSpacing(lua_State *L) {
  ImGui::Spacing();
  return 0;
}

static int LuaText(lua_State *L) {
  auto text = Lua::to<std::string_view>(L, 1);
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
  return 0;
}

static int LuaTextDisabled(lua_State *L) {
  auto text = Lua::to<const char *>(L, 1);
  ImGui::TextDisabled("%s", text);
  return 0;
}

static int LuaTextColored(lua_State *L) {
  auto text = Lua::to<const char *>(L, 1);
  auto r = Lua::check<float>(L, 2);
  auto g = Lua::check<float>(L, 3);
  auto b = Lua::check<float>(L, 4);
  ImGui::TextColored({r, g, b, 1.0f}, "%s", text);
  return 0;
}

static int LuaButton(lua_State *L) {
  auto label = Lua::to<const char *>(L, 1);
  if (ImGui::Button(label)) {
    lua_pushvalue(L, 2);
    lua_call(L, 0, 0);
  }
  return 0;
}

static int LuaSmallButton(lua_State *L) {
  auto label = Lua::to<const char *>(L, 1);
  if (ImGui::SmallButton(label)) {
    lua_pushvalue(L, 2);
    lua_call(L, 0, 0);
  }
  return 0;
}

static int LuaInputText(lua_State *L) {
  auto label = Lua::to<const char *>(L, 1);
  char buf[256] = {};

  if (ImGui::InputText(label, buf, 256, ImGuiInputTextFlags_EnterReturnsTrue)) {
    lua_pushvalue(L, 2);
    lua_pushstring(L, buf);
    lua_call(L, 1, 0);
  }

  return 0;
}

static int LuaSetScrollHereY(lua_State *L) {
  auto pos = Lua::check<float>(L, 1);
  ImGui::SetScrollHereY(pos);
  return 0;
}

static int LuaSliderFloat(lua_State *L) {
  auto label = Lua::to<const char *>(L, 1);
  auto value = Lua::check<float>(L, 2);
  auto min = Lua::check<float>(L, 3);
  auto max = Lua::check<float>(L, 4);
  auto res = ImGui::SliderFloat(label, &value, min, max);
  if (res) {
    lua_pushvalue(L, 5);
    lua_pushnumber(L, value);
    lua_call(L, 1, 0);
  }
  return 0;
}

static int LuaDragFloat(lua_State *L) {
  auto label = luaL_tolstring(L, 1, nullptr);
  auto value = (float)luaL_checknumber(L, 2);
  auto speed = (float)luaL_checknumber(L, 3);
  auto min = (float)luaL_checknumber(L, 4);
  auto max = (float)luaL_checknumber(L, 5);
  auto res = ImGui::DragFloat(label, &value, speed, min, max);
  if (res) {
    lua_pushvalue(L, 6);
    lua_pushnumber(L, value);
    lua_call(L, 1, 0);
  }
  return 0;
}

static int LuaTable(lua_State *L) {
  auto name = Lua::to<std::string_view>(L, 1);
  auto num_columns = Lua::check<int>(L, 2);

  if (ImGui::BeginTable(name.data(), num_columns,
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable)) {
    lua_pushvalue(L, 3);
    int res = lua_pcall(L, 0, 0, 0);
    ImGui::EndTable();

    if (res != LUA_OK) {
      return lua_error(L);
    }

    return 0;
  }

  ImGui::EndTable();
  return 0;
}

static int LuaTableNextRow(lua_State *L) {
  ImGui::TableNextRow();
  return 0;
}

static int LuaTableNextColumn(lua_State *L) {
  lua_pushboolean(L, ImGui::TableNextColumn());
  return 1;
}

static int LuaTableSetupColumn(lua_State *L) {
  auto name = Lua::to<std::string_view>(L, 1);

  ImGui::TableSetupColumn(name.data());
  return 0;
}

static int LuaTableScrollFreeze(lua_State *L) {
  auto cols = Lua::check<int>(L, 1);
  auto rows = Lua::check<int>(L, 2);

  ImGui::TableSetupScrollFreeze(cols, rows);
  return 0;
}

static int LuaTableHeadersRow(lua_State *L) {
  ImGui::TableHeadersRow();
  return 0;
}

static int LuaTableGetSortSpecs(lua_State *L) {
  auto specs = ImGui::TableGetSortSpecs();
  if (!specs) {
    return 0;
  }
  lua_createtable(L, specs->SpecsCount, 0);
  for (int i = 0; i < specs->SpecsCount; ++i) {
    auto &spec = specs->Specs[i];

    lua_createtable(L, 0, 2);
    lua_pushinteger(L, spec.ColumnIndex);
    lua_setfield(L, -2, "column");

    switch (spec.SortDirection) {
    case ImGuiSortDirection_None:
      lua_pushstring(L, "none");
      break;
    case ImGuiSortDirection_Ascending:
      lua_pushstring(L, "ascending");
      break;
    case ImGuiSortDirection_Descending:
      lua_pushstring(L, "descending");
      break;
    }
    lua_setfield(L, -2, "direction");

    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int LuaUseFont(lua_State *L) {
  auto font = Lua::check<GuiFont *>(L, 1);

  ImGui::PushFont(font->font);
  lua_pushvalue(L, 2);
  int res = lua_pcall(L, 0, 0, 0);
  ImGui::PopFont();

  if (res != LUA_OK) {
    return lua_error(L);
  }
  return 0;
}

static int LuaTreeNode(lua_State *L) {
  auto name = Lua::to<std::string_view>(L, 1);

  if (ImGui::TreeNode(name.data())) {
    lua_pushvalue(L, 2);
    int res = lua_pcall(L, 0, 0, 0);
    ImGui::TreePop();

    if (res != LUA_OK) {
      return lua_error(L);
    }

    return 0;
  }

  return 0;
}

static int LuaTreeNodeID(lua_State *L) {
  auto id = Lua::to<std::string_view>(L, 1);
  auto label = Lua::to<std::string_view>(L, 2);

  if (ImGui::TreeNode(id.data(), "%s", label.data())) {
    lua_pushvalue(L, 3);
    int res = lua_pcall(L, 0, 0, 0);
    ImGui::TreePop();

    if (res != LUA_OK) {
      return lua_error(L);
    }

    return 0;
  }

  return 0;
}

void PushImgui(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, LuaNextWindowSize);
  lua_setfield(L, -2, "set_next_window_size");

  lua_pushcfunction(L, LuaWindow);
  lua_setfield(L, -2, "window");

  lua_pushcfunction(L, LuaChild);
  lua_setfield(L, -2, "child");

  lua_pushcfunction(L, LuaSeparator);
  lua_setfield(L, -2, "separator");

  lua_pushcfunction(L, LuaSameLine);
  lua_setfield(L, -2, "same_line");

  lua_pushcfunction(L, LuaNewLine);
  lua_setfield(L, -2, "new_line");

  lua_pushcfunction(L, LuaSpacing);
  lua_setfield(L, -2, "spacing");

  lua_pushcfunction(L, LuaText);
  lua_setfield(L, -2, "text");

  lua_pushcfunction(L, LuaTextDisabled);
  lua_setfield(L, -2, "text_disabled");

  lua_pushcfunction(L, LuaTextColored);
  lua_setfield(L, -2, "text_colored");

  lua_pushcfunction(L, LuaButton);
  lua_setfield(L, -2, "button");

  lua_pushcfunction(L, LuaSmallButton);
  lua_setfield(L, -2, "small_button");

  lua_pushcfunction(L, LuaInputText);
  lua_setfield(L, -2, "input_text");

  lua_pushcfunction(L, LuaSetScrollHereY);
  lua_setfield(L, -2, "set_scroll_here_y");

  lua_pushcfunction(L, LuaSliderFloat);
  lua_setfield(L, -2, "slider_float");

  lua_pushcfunction(L, LuaDragFloat);
  lua_setfield(L, -2, "drag_float");

  lua_pushcfunction(L, LuaTable);
  lua_setfield(L, -2, "table");

  lua_pushcfunction(L, LuaTableNextRow);
  lua_setfield(L, -2, "table_next_row");

  lua_pushcfunction(L, LuaTableNextColumn);
  lua_setfield(L, -2, "table_next_column");

  lua_pushcfunction(L, LuaTableSetupColumn);
  lua_setfield(L, -2, "table_setup_column");

  lua_pushcfunction(L, LuaTableScrollFreeze);
  lua_setfield(L, -2, "table_scroll_freeze");

  lua_pushcfunction(L, LuaTableHeadersRow);
  lua_setfield(L, -2, "table_headers_row");

  lua_pushcfunction(L, LuaTableGetSortSpecs);
  lua_setfield(L, -2, "table_get_sort_specs");

  lua_pushcfunction(L, LuaUseFont);
  lua_setfield(L, -2, "use_font");

  lua_pushcfunction(L, LuaTreeNode);
  lua_setfield(L, -2, "tree_node");

  lua_pushcfunction(L, LuaTreeNodeID);
  lua_setfield(L, -2, "tree_node_id");
}

} // namespace gk