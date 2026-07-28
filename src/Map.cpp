#include "Map.h"

#include "Core.h"
#include "Roles.h"

#include <cstdint>

namespace gk {
Map *GetCurrentMap() {
  Map **the_map;
  GetObjectAtOffset(the_map, 0x00739090);
  return *the_map;
}

// There is no global world-unit-scale getter, and 0x005a9b40 was never one: that
// function is `CopyDword`, a `__fastcall(dest, src)` four-byte copy that returns
// dest. A previous version of this file called it with no arguments through a
// `FastCall<float *>`, so it dereferenced whatever the caller had left in EDX and
// wrote through EDX's counterpart in ECX.
//
// ToMap reads the scale straight out of the loaded rif object - it is the first
// float of the record `AcquireLevelRifForLocators` / `LoadOrGetRifFile` hands back,
// the same pointer it passes to `RifFilterObjectsByName` in EDX. All three
// `CopyDword` call sites in ToMap (0x00480f65, 0x00481151, 0x00481898) copy that
// float into a local and `MULSS` the FILD'd integer locator coordinates by it.
//
// So the scale is per-rif data. A caller has to have the rif in hand, which every
// caller that needs it does.
float RifUnitScale(const void *rif) {
  return rif ? *reinterpret_cast<const float *>(rif) : 0.0f;
}

TeamSlot *GetTeamSlots() {
  TeamSlot **slots;
  GetObjectAtOffset(slots, 0x007b3ec4);
  return *slots;
}

int GetNumTeamSlots() {
  int *n;
  GetObjectAtOffset(n, 0x007b3ec0);
  return *n;
}

Vec3 MapOrigin(const Map *map) {
  return {-map->neg_origin.x, -map->neg_origin.y, -map->neg_origin.z};
}

Vec3 MapToWorld(const Map *map, const void *rif, Vec3 rif_pos) {
  float scale = RifUnitScale(rif);
  return {rif_pos.x * scale + map->neg_origin.x,
          rif_pos.y * scale + map->neg_origin.y,
          rif_pos.z * scale + map->neg_origin.z};
}

// The placed-object tail of ToMap minus the rif lookup: runs both spawn factories
// exactly as ToMap does (server via ServerSpawnActorForTeam @ 0x005035b0, client
// via ClientSpawnActorForTeam @ 0x004fce90), gated on IsExecutorRunning
// @ 0x00502da0 / IsClientRoutingActive @ 0x004fccc0.
int MapSpawn(Role *role, int team, Vec3 *position, Vec4 *orientation) {
  if (team < 0 || team >= GetNumTeamSlots()) {
    return -1;
  }

  FastCall<bool> is_executor_running;
  GetObjectAtOffset(is_executor_running, 0x00502da0);
  FastCall<bool> is_client_routing_active;
  GetObjectAtOffset(is_client_routing_active, 0x004fccc0);
  FastCall<int, int, Role *, Vec3 *, Vec4 *> server_spawn;
  GetObjectAtOffset(server_spawn, 0x005035b0);
  FastCall<void *, int, Role *, Vec3 *, Vec4 *> client_spawn;
  GetObjectAtOffset(client_spawn, 0x004fce90);

  int id = -1;
  if (is_executor_running()) {
    id = server_spawn(team, role, position, orientation);
  }
  if (is_client_routing_active()) {
    id = static_cast<int>(reinterpret_cast<intptr_t>(
        client_spawn(team, role, position, orientation)));
  }
  return id;
}
} // namespace gk
