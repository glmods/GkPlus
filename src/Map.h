#pragma once

#include "HashTable.h"
#include "List.h"
#include "Math.h"
#include "Memory.h"

#include <cstddef>
#include <cstdint>

namespace gk {
struct Role;

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

/// One quad of the level's collision/navigation mesh, 0x20 bytes. Same shape
/// as LevelMeshTri with a fourth vertex; `flags` bit 0x100 means blocked, and
/// a face is walkable iff that bit is clear and `plane`'s Y component is
/// negative (`level_loading_notes.md` §5.5).
struct LevelMeshQuad {
  Vec3 *v0;
  Vec3 *v1;
  Vec3 *v2;
  Vec3 *v3;
  float plane[3];
  unsigned flags;
};
static_assert(sizeof(LevelMeshQuad) == 0x20);

/// The header of one level-geometry section, 0x18 bytes: counts and pointers
/// for the triangles, quads and shared vertices it owns. The vertices are
/// shared by pointer, which is why a weld can leave a triangle degenerate,
/// the cause of the `/GS` fast-fail in `game_defects_notes.md` §5.
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
//
// **The Ghidra database models Map as one flat 0x18c record; this header models
// it as `Map : MapBase, RefCountedBase`. The two AGREE on the layout** - every
// field lands at the same offset either way, and the split is what puts the
// second vptr at 0xa4 without a hand-written `void *sub_vtbl`. It is a
// modelling difference, not a discrepancy, and neither side needs correcting.
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

  // +0x04 (Map+0xa8) LoadGame addrefs; UnloadLevel @ 0x004e2090 (__cdecl,
  // returning bool) releases.
  int refcount;
};
static_assert(sizeof(RefCountedBase) == 0x8);

/// The loaded level. The engine's own `Map`, and one of the two places in this
/// codebase that models real multiple inheritance: MapBase is 0xa4 bytes, so
/// RefCountedBase's vptr lands at 0xa4 exactly. Each base carries its own
/// `static_assert(sizeof(...))`, and `offsetof(Map, refcount) == 0xa8` on the
/// composite is what proves the split; see the convention note in
/// `CLAUDE.md`.
///
/// The remaining field offsets are in `address_map.md`; how one is built is
/// `level_loading_notes.md`.
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
  // locator coordinates. Net effect: pos = rif_pos * RifUnitScale(rif) - origin.
  Vec3 neg_origin;        // 0x11c
  // World bounds, read as a pair by LoadLevel. **The larger corner is FIRST**, which is the
  // opposite of the order these were named in until it was measured: on level02 0x128 holds
  // (68.6, 10.0, 66.5) against 0x134's (-65.8, -14.1, -65.1), and on level01 (47.4, 30.0, 141.0)
  // against (-50.8, -28.5, -96.1) - six components across two levels, all the same way round.
  // Confirmed independently by the level's own `STDLIGHT` positions, which bracket correctly
  // inside the pair once it is read this way and not otherwise (src/MapLights.cpp).
  Vec3 bounds_max;        // 0x128
  Vec3 bounds_min;        // 0x134
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
static_assert(offsetof(Map, bounds_max) == 0x128);
static_assert(offsetof(Map, bounds_min) == 0x134);
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

// --- Native API over the level/map globals ----------------------------------

// TheMap @ 0x00739090; null outside a loaded level.
Map *GetCurrentMap();
// TeamSlots @ 0x007b3ec4 (stride 0xc4) / NumTeamSlots @ 0x007b3ec0.
TeamSlot *GetTeamSlots();
/// How many entries GetTeamSlots() has. The bound every team index is checked
/// against; it is a fixed table, not a per-level count.
int GetNumTeamSlots();

// The map origin the level was built with. Map stores it negated (see neg_origin);
// this flips it back.
Vec3 MapOrigin(const Map *map);
// The world-unit scale of a loaded rif: the first float of the object
// `AcquireLevelRifForLocators` @ 0x00483da0 / `LoadOrGetRifFile` @ 0x004ae960
// return. This is **per-rif data, not a global** - there is no world-unit-scale
// getter in the binary (see the comment in Map.cpp). Zero for a null rif.
float RifUnitScale(const void *rif);
// Convert a .rif locator position (integer rif units) into the world-space
// position ToMap would spawn a placed object at:
// rif_pos * RifUnitScale(rif) - origin. `rif` is the same handle passed to
// `RifFilterObjectsByName`.
Vec3 MapToWorld(const Map *map, const void *rif, Vec3 rif_pos);

// The placed-object tail of ToMap minus the rif lookup: runs both spawn factories
// exactly as ToMap does, so on a listen host the executor and the client each get
// their own actor and the client's id is the one returned. Returns the actor id,
// or -1 if nothing spawned. `team` must be in [0, GetNumTeamSlots()).
int MapSpawn(Role *role, int team, Vec3 *position, Vec4 *orientation);
} // namespace gk
