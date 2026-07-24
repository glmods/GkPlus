#include "Map.h"

#include "Core.h"
#include "HashTable.h"
#include "List.h"
#include "LuaEngine.h"
#include "Math.h"
#include "Memory.h"
#include "Roles.h"

#include <cassert>
#include <cstdint>

namespace gk {
// Level geometry, as produced by ToMap's cold path and by the <ScriptFileName>.cut
// warm path. Triangle/quad vertex slots are ABSOLUTE pointers into the vertex
// array (verts + idx * 0xc), not indices - the .cut reader fixes them up on load.
struct LevelMeshTri {
  Vec3 *v0;
  Vec3 *v1;
  Vec3 *v2;
  float plane[3];
  unsigned flags;
};
static_assert(sizeof(LevelMeshTri) == 0x1c);

struct LevelMeshQuad {
  Vec3 *v0;
  Vec3 *v1;
  Vec3 *v2;
  Vec3 *v3;
  float plane[3];
  unsigned flags;
};
static_assert(sizeof(LevelMeshQuad) == 0x20);

struct LevelMeshHeader {
  int tri_count;
  LevelMeshTri *tris;
  int quad_count;
  LevelMeshQuad *quads;
  int vert_count;
  Vec3 *verts;
};
static_assert(sizeof(LevelMeshHeader) == 0x18);

// The `use <role> in team <n> for "<rif object>" [as "<token>"]` bindings of a
// map section. Stored in a PlacedObjectBindingMap embedded at ParsedMap+0x1b60
// (which is why ParsedMap is 0x1b78 and every other section type is 0x1b60),
// and consumed - one shot, each entry removed as it is spawned - by ToMap.
// The payload of a binding entry. PlacedObjectBinding_Dtor @ 0x0047ec00 frees the
// two strings and drops a reference on the parsed role.
struct PlacedObjectBinding {
  pool_string object_name; // RIF object name (owned copy)
  pool_string token_name;  // the `as "..."` clause, null if absent
  void *role;        // the parsed `role` section (ParsedThing*), ref_count++
  int team;          // index into TeamSlots[]
  bool overridable;  // 0 => a duplicate object name is an error
  uint8_t pad[3];
};
static_assert(offsetof(PlacedObjectBinding, team) == 0x0c);
static_assert(sizeof(PlacedObjectBinding) == 0x14);

// Hash = sum of toupper(c) over object_name; lookup compares with __stricmp.
// Embedded at ParsedMap+0x1b60. This is the polymorphic form of the template, and
// the vtable confirms it: 0x0066328c is exactly three slots - the v1.1 node
// allocation policy - with ParsedMap's own ParsedThingVtbl starting right after at
// 0x00663298. The payload is held by value, so a node is 0x14 + the link = 0x18
// with `next` at +0x14, which is what the old flat struct was describing.
using PlacedObjectBindingMap = HashTable<PlacedObjectBinding>;
using PlacedObjectBindingNode = PlacedObjectBindingMap::Node;
static_assert(sizeof(PlacedObjectBindingMap) == 0x14);
static_assert(sizeof(PlacedObjectBindingNode) == 0x18);
static_assert(offsetof(PlacedObjectBindingMap, n_entries) == 0x04);
static_assert(offsetof(PlacedObjectBindingMap, chains) == 0x10);
static_assert(offsetof(PlacedObjectBindingNode, next) == 0x14);

// Embedded reader/writer lock at Map+0x04. RWLock_Lock @ 0x00579700 and
// RWLock_Unlock @ 0x005797c0 touch only +0x00 and +0x1c; the interior is opaque.
struct RWLock {
  uint8_t unk[0x20];
};
static_assert(sizeof(RWLock) == 0x20);

// THE level object: TheMap @ 0x00739090, malloc(0x18c), built by Map_Ctor
// @ 0x00470f20 and decorated by ToMap @ 0x0047f160. A non-null TheMap makes
// ToMap skip its entire geometry phase, which is the seam a native level
// builder hooks. See level_loading_notes.md.
//
// The Map dtor @ 0x00471e60 owns only the three strings below; scene_object and
// sky_object are refcounted, and the Map itself is pool-freed as 0x18c.
//
// Offsets 0x24..0x88 and 0x8c..0xa4 are still largely unmapped: they are only
// reached through __thiscall methods invoked directly on TheMap (FUN_0048cf50
// and the 0x00472xxx / 0x0048xxxx cluster), not through field access at the
// call sites, so a global-reference sweep does not see them.
// Primary base subobject at Map+0x00 (0xa4 bytes). The whole primary chain
// declares exactly ONE virtual: the vtables 0x00663e5c <- 0x00652818 <-
// 0x00652824 are each a single slot, so nothing down the chain adds one. Its
// destructor is FUN_00489ae0, tail-called by the Map dtor with this+0x00; it
// owns the 0x24 list and the arrays at 0x34/0x38/0x3c and 0x7c/0x80/0x84/0x88.
struct MapBase {
  // Nulls dangling references into two arrays of objects being released.
  virtual void ClearReferencesTo(void *removed_ranges) = 0;

  RWLock lock;            // 0x04 held while the section adjacency is rebuilt
  List<void *> field0x24;   // 0x24
  uint8_t unk0x34[0x54];  // 0x34
  void **sections;        // 0x88 section table
  int num_sections;       // 0x8c
  uint8_t unk0x90[0x4];   // 0x90
  List<void *> field0x94;   // 0x94
};
static_assert(offsetof(MapBase, sections) == 0x88);
static_assert(sizeof(MapBase) == 0xa4);

// Second base subobject, landing at Map+0xa4 purely because MapBase is 0xa4
// bytes. A refcounted-object base shared across the engine: its root vtable
// 0x006522e8 is referenced by ~28 classes, and the same {vptr, refcount} pair
// appears at +0x9c/+0xa0 on the scene objects Map holds at 0xc8 and 0x188
// (both released with the identical "decrement, then call slot 0 with 1"
// sequence). Chain 0x006522e8 (1 slot) <- 0x0065281c (2, slot 1 __purecall)
// <- 0x00652828 (2). It is exactly 2 slots: 0x0065282c has no references at
// all, while 0x00652830 is the start of an unrelated class's table.
struct RefCountedBase {
  virtual ~RefCountedBase() = 0; // slot 0: scalar deleting destructor
  virtual void Stub1() = 0;      // slot 1: __purecall in the middle base

  int refcount; // +0x04 (Map+0xa8) LoadGame addrefs; FUN_004e2090 releases
};
static_assert(sizeof(RefCountedBase) == 0x8);

struct Map : MapBase, RefCountedBase {
  bool adjacency_built;   // 0x0ac run-once gate in LoadOrBuildSectionAdjacency
  bool field0xad;         // 0x0ad
  uint8_t pad0xae[2];     // 0x0ae
  uint8_t unk0xb0[0x4];   // 0x0b0
  Vec4 field0xb4;         // 0x0b4 {1,1,1,1} in Map_Ctor
  int field0xc4;          // 0x0c4 = 2 in Map_Ctor
  void *scene_object;     // 0x0c8 0x1f0 bytes, FUN_0059c3a0(sceneObject, 1)
  pool_string bitmap;     // 0x0cc map section field 0x02
  bool field0xd0;         // 0x0d0
  bool field0xd1;         // 0x0d1 set to 1 at the end of Map_Ctor
  bool field0xd2;         // 0x0d2
  uint8_t pad0xd3;        // 0x0d3
  Vec3 field0xd4;         // 0x0d4
  bool field0xe0;         // 0x0e0
  uint8_t pad0xe1[3];     // 0x0e1
  Vec3 field0xe4;         // 0x0e4 z (0x0ec) = FLT_MAX in Map_Ctor
  float field0xf0;        // 0x0f0 = 0.5
  float field0xf4;        // 0x0f4 = 0.5
  int field0xf8;          // 0x0f8
  List<void *> field0xfc;   // 0x0fc
  List<void *> field0x10c;  // 0x10c
  // NEGATED map origin - Map_Ctor XORs each component of its `origin` argument
  // with 0x80000000 before storing it, and ToMap ADDs this to scaled rif
  // locator coordinates. Net effect: pos = rif_pos * world_unit_scale - origin.
  Vec3 neg_origin;        // 0x11c
  Vec3 bounds_min;        // 0x128 world bounds, read as a pair by LoadLevel
  Vec3 bounds_max;        // 0x134
  // Camera focus bounds. Only the y components reach the game's globals:
  // 0x144 -> MinCameraFocusHeight @ 0x006a574c, 0x150 -> MaxCameraFocusHeight
  // @ 0x007b3ea8. Both come from the `.loc` locator named by the map section's
  // `min/max camera focus height` fields (0x53 / 0x52).
  Vec3 camera_focus_min;  // 0x140
  Vec3 camera_focus_max;  // 0x14c
  unsigned rif_time_low;  // 0x158 FILETIME of the level .rif; the .cut/.map
  unsigned rif_time_high; // 0x15c cache stamp is compared against this pair
  pool_string shadow_object_rif;  // 0x160 map section field 0x54
  pool_string shadow_object_name; // 0x164 map section field 0x55
  Vec3 default_position;    // 0x168 valid only if has_default_position
  bool has_default_position; // 0x174 gates ConsoleParsePosition's use of it
  uint8_t pad0x175[3];      // 0x175
  Vec3 field0x178;          // 0x178
  bool field0x184;          // 0x184
  uint8_t pad0x185[3];      // 0x185
  void *sky_object;         // 0x188 lazily built (0x1f0, FUN_0059c0f0)
};
// The two base subobjects must land where the game puts them - these are what
// prove the multiple-inheritance layout reproduces the original.
static_assert(offsetof(Map, sections) == 0x88);
static_assert(offsetof(Map, num_sections) == 0x8c);
static_assert(offsetof(Map, refcount) == 0xa8);
static_assert(offsetof(Map, adjacency_built) == 0xac);
static_assert(offsetof(Map, scene_object) == 0xc8);
static_assert(offsetof(Map, bitmap) == 0xcc);
static_assert(offsetof(Map, neg_origin) == 0x11c);
static_assert(offsetof(Map, bounds_min) == 0x128);
static_assert(offsetof(Map, bounds_max) == 0x134);
static_assert(offsetof(Map, camera_focus_min) == 0x140);
static_assert(offsetof(Map, camera_focus_max) == 0x14c);
static_assert(offsetof(Map, rif_time_low) == 0x158);
static_assert(offsetof(Map, shadow_object_rif) == 0x160);
static_assert(offsetof(Map, shadow_object_name) == 0x164);
static_assert(offsetof(Map, default_position) == 0x168);
static_assert(offsetof(Map, has_default_position) == 0x174);
static_assert(offsetof(Map, sky_object) == 0x188);
static_assert(sizeof(Map) == 0x18c);

// One entry of TeamSlots @ 0x007b3ec4. Only `active` is mapped so far: ToMap
// skips a binding's entire team when it is 0, which is how `extreme`-only and
// multiplayer-only teams are excluded without touching the bindings.
struct TeamSlot {
  uint8_t unk0x00[0x69]; // 0x00
  bool active;           // 0x69
  uint8_t unk0x6a[0x5a]; // 0x6a
};
static_assert(offsetof(TeamSlot, active) == 0x69);
static_assert(sizeof(TeamSlot) == 0xc4);

static Map **TheMap;
static TeamSlot **TeamSlots;
static int *NumTeamSlots;
static FastCall<float *> GetWorldUnitScale;
static FastCall<int, int, Role *, Vec3 *, Vec4 *> ServerSpawnActorForTeam;
static FastCall<void *, int, Role *, Vec3 *, Vec4 *> ClientSpawnActorForTeam;
static FastCall<bool> IsExecutorRunning;
static FastCall<bool> IsClientRoutingActive;

MapModule::MapModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(TheMap, 0x00739090);
  GetObjectAtOffset(TeamSlots, 0x007b3ec4);
  GetObjectAtOffset(NumTeamSlots, 0x007b3ec0);
  GetObjectAtOffset(GetWorldUnitScale, 0x005a9b40);
  GetObjectAtOffset(ServerSpawnActorForTeam, 0x005035b0);
  GetObjectAtOffset(ClientSpawnActorForTeam, 0x004fce90);
  GetObjectAtOffset(IsExecutorRunning, 0x00502da0);
  GetObjectAtOffset(IsClientRoutingActive, 0x004fccc0);
}

namespace {
std::string_view str(const char *s) {
  if (s) {
    return s;
  } else {
    return {};
  }
}
} // namespace

// --- MapWrapper --------------------------------------------------------------

bool MapWrapper::operator==(const MapWrapper &) const = default;

MapWrapper::MapWrapper(Map *map) : map(map) { assert(map); }

void MapWrapper::setup_metatable(lua_State *L) {}

int MapWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "<Map %p>", map);
  return 1;
}

std::string_view MapWrapper::get_bitmap() { return str(map->bitmap.get()); }
std::string_view MapWrapper::get_shadow_object_rif() {
  return str(map->shadow_object_rif.get());
}
std::string_view MapWrapper::get_shadow_object_name() {
  return str(map->shadow_object_name.get());
}
int MapWrapper::get_num_sections() { return map->num_sections; }
bool MapWrapper::get_adjacency_built() { return map->adjacency_built; }

Vec3 MapWrapper::get_origin() {
  return {-map->neg_origin.x, -map->neg_origin.y, -map->neg_origin.z};
}
Vec3 MapWrapper::get_bounds_min() { return map->bounds_min; }
Vec3 MapWrapper::get_bounds_max() { return map->bounds_max; }
Vec3 MapWrapper::get_camera_focus_min() { return map->camera_focus_min; }
Vec3 MapWrapper::get_camera_focus_max() { return map->camera_focus_max; }
float MapWrapper::get_min_camera_focus_height() {
  return map->camera_focus_min.y;
}
float MapWrapper::get_max_camera_focus_height() {
  return map->camera_focus_max.y;
}

std::optional<Vec3> MapWrapper::get_default_position() {
  if (map->has_default_position) {
    return map->default_position;
  } else {
    return std::nullopt;
  }
}

int MapWrapper::to_world(lua_State *L) {
  auto x = Lua::check<float>(L, 2);
  auto y = Lua::check<float>(L, 3);
  auto z = Lua::check<float>(L, 4);

  float scale = *GetWorldUnitScale();
  Lua::Create<Vec3>(L, Vec3{x * scale + map->neg_origin.x,
                            y * scale + map->neg_origin.y,
                            z * scale + map->neg_origin.z});
  return 1;
}

// --- TeamSlotWrapper ---------------------------------------------------------

bool TeamSlotWrapper::operator==(const TeamSlotWrapper &) const = default;

TeamSlotWrapper::TeamSlotWrapper(int index) : index(index) {}

void TeamSlotWrapper::setup_metatable(lua_State *L) {}

int TeamSlotWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "<TeamSlot %d>", index);
  return 1;
}

int TeamSlotWrapper::get_index() { return index; }
bool TeamSlotWrapper::get_active() { return (*TeamSlots)[index].active; }

// --- gk.map ------------------------------------------------------------------

namespace {
struct TeamSlotIterator {
  static constexpr const char *metatable_name = "TeamSlotIterator";
  static void setup_metatable(lua_State *L) {}

  int next_index{};

  int next(lua_State *L) {
    if (next_index >= *NumTeamSlots) {
      return 0;
    }

    int index = next_index++;
    lua_pushinteger(L, index);
    Lua::Create<TeamSlotWrapper>(L, index);
    return 2;
  }
};

// Pushes the `teams` table: teams[i] -> TeamSlot, #teams, pairs(teams).
void PushTeams(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);

  lua_pushcfunction(L, [](lua_State *L) {
    int index = static_cast<int>(luaL_checkinteger(L, 2));
    if (index < 0 || index >= *NumTeamSlots) {
      return 0;
    }
    Lua::Create<TeamSlotWrapper>(L, index);
    return 1;
  });
  lua_setfield(L, -2, "__index");

  lua_pushcfunction(L, [](lua_State *L) {
    lua_pushinteger(L, *NumTeamSlots);
    return 1;
  });
  lua_setfield(L, -2, "__len");

  lua_pushcfunction(
      L, ([](lua_State *L) {
        Lua::PushMemberFunction<TeamSlotIterator, &TeamSlotIterator::next>(L);
        Lua::Create<TeamSlotIterator>(L);
        return 2;
      }));
  lua_setfield(L, -2, "__pairs");

  lua_setmetatable(L, -2);
}

// current() -> Map, or nil outside a loaded level.
int MapCurrent(lua_State *L) {
  if (*TheMap) {
    Lua::Create<MapWrapper>(L, *TheMap);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int MapWorldUnitScale(lua_State *L) {
  lua_pushnumber(L, *GetWorldUnitScale());
  return 1;
}

// spawn(role, team, position, orientation) -> actor id, or nil.
// The placed-object tail of ToMap minus the rif lookup: runs both spawn
// factories exactly as ToMap does, so on a listen host the executor and the
// client each get their own actor and the client's id is the one returned
// (which is the id ToMap would have put in the `as "..."` token).
int MapSpawn(lua_State *L) {
  auto role = Lua::check<RoleWrapper>(L, 1);
  int team = Lua::check<int>(L, 2);
  auto position = Lua::check<Vec3 *>(L, 3);
  auto orientation = Lua::check<Vec4 *>(L, 4);

  if (team < 0 || team >= *NumTeamSlots) {
    return luaL_error(L, "team %d out of range (0..%d)", team,
                      *NumTeamSlots - 1);
  }

  int id = -1;
  if (IsExecutorRunning()) {
    id = ServerSpawnActorForTeam(team, role.role, position, orientation);
  }
  if (IsClientRoutingActive()) {
    id = static_cast<int>(reinterpret_cast<intptr_t>(
        ClientSpawnActorForTeam(team, role.role, position, orientation)));
  }

  if (id == -1) {
    lua_pushnil(L);
  } else {
    lua_pushinteger(L, id);
  }
  return 1;
}
} // namespace

int MapModule::Register(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, MapCurrent);
  lua_setfield(L, -2, "current");
  lua_pushcfunction(L, MapWorldUnitScale);
  lua_setfield(L, -2, "world_unit_scale");
  lua_pushcfunction(L, MapSpawn);
  lua_setfield(L, -2, "spawn");

  PushTeams(L);
  lua_setfield(L, -2, "teams");

  return 1;
}
} // namespace gk
