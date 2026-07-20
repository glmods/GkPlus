#pragma once

#include "LuaEngine.h"
#include "Math.h"
#include "Module.h"

#include <optional>
#include <string_view>

namespace gk {
struct Map;
struct Role;
struct RoleWrapper;

// The level object built by ToMap and stored in TheMap @ 0x00739090.
// See level_loading_notes.md; the full 0x18c layout lives in Map.cpp.
struct MapWrapper final {
  static constexpr const char *metatable_name = "Map";
  static void setup_metatable(lua_State *L);

  Map *map;
  MapWrapper(Map *map);

  bool operator==(const MapWrapper &) const;

  int to_string(lua_State *L) const;

  std::string_view get_bitmap();
  std::string_view get_shadow_object_rif();
  std::string_view get_shadow_object_name();
  int get_num_sections();
  bool get_adjacency_built();
  // Map_Ctor stores the origin negated (each component XORed with 0x80000000);
  // this getter flips it back to the origin the map was actually built with.
  Vec3 get_origin();
  Vec3 get_bounds_min();
  Vec3 get_bounds_max();
  Vec3 get_camera_focus_min();
  Vec3 get_camera_focus_max();
  float get_min_camera_focus_height();
  float get_max_camera_focus_height();
  std::optional<Vec3> get_default_position();

  // Converts a level .rif locator position (integer world units, as stored in
  // the rif object at +0x44/0x48/0x4c) into the world-space position ToMap
  // would spawn a placed object at: rif_pos * GetWorldUnitScale() - origin.
  int to_world(lua_State *L);

  using type = MapWrapper;
  using fields = Lua::Fields<
      Lua::Getter<"bitmap", &type::get_bitmap>,
      Lua::Getter<"shadow_object_rif", &type::get_shadow_object_rif>,
      Lua::Getter<"shadow_object_name", &type::get_shadow_object_name>,
      Lua::Getter<"num_sections", &type::get_num_sections>,
      Lua::Getter<"adjacency_built", &type::get_adjacency_built>,
      Lua::Getter<"origin", &type::get_origin>,
      Lua::Getter<"bounds_min", &type::get_bounds_min>,
      Lua::Getter<"bounds_max", &type::get_bounds_max>,
      Lua::Getter<"camera_focus_min", &type::get_camera_focus_min>,
      Lua::Getter<"camera_focus_max", &type::get_camera_focus_max>,
      Lua::Getter<"min_camera_focus_height", &type::get_min_camera_focus_height>,
      Lua::Getter<"max_camera_focus_height", &type::get_max_camera_focus_height>,
      Lua::Getter<"default_position", &type::get_default_position>,
      Lua::Function<"to_world", type, &type::to_world>>;
};

// One entry of TeamSlots @ 0x007b3ec4 (stride 0xc4). Only `active` is mapped;
// ToMap skips a binding's whole team when it is 0.
struct TeamSlotWrapper final {
  static constexpr const char *metatable_name = "TeamSlot";
  static void setup_metatable(lua_State *L);

  int index;

  TeamSlotWrapper(int index);

  bool operator==(const TeamSlotWrapper &) const;

  int to_string(lua_State *L) const;

  int get_index();
  bool get_active();

  using type = TeamSlotWrapper;
  using fields = Lua::Fields<Lua::Getter<"index", &type::get_index>,
                             Lua::Getter<"active", &type::get_active>>;
};

class MapModule final : public Module<MapModule> {
public:
  static constexpr const char *module_name = "gk.map";

  MapModule(lua_State *L);

  int Register(lua_State *L);
};
} // namespace gk
