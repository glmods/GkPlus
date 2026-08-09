#pragma once

// The level's own lights, in world space - the rig that BAKED its per-vertex colours, which the
// shipped engine loads and then never reads.
//
// `src/Rif` is the decoder and knows nothing about the game; this is the half that knows where
// the file is, what scale it is in and when to reload. See rif_chunk_format.md for `STDLIGHT`
// and vulkan_renderer_notes.md for what the renderer does with the result.
//
// ## Why this needs a hook
//
// **The level's rif object does not survive its own load.** `LoadLevel` calls `RifCache_Clear`
// @ 0x004aead0 immediately after `ConvertParsedObjects` (@ 0x004e0e70), and `LoadOrGetRifFile`
// itself clears the cache on every miss - so by the time a level is playable the rif is freed and
// the cache is empty. No global holds it and none holds its path either: `ToMap` keeps the
// handle in a register for the duration of the call, and `Map` retains only the *shadow* object's
// rif name (field 0x54), never the map section's own `file` (0x01).
//
// So the path has to be caught in flight. `LoadOrGetRifFile` @ 0x004ae960 is the seam because it
// is the one every route passes through: `ToMap` calls it directly on the cold path and reaches
// it through `AcquireLevelRifForLocators` on both warm ones. Hooking Acquire instead would miss
// the cold path entirely - the load where a level is being built for the first time.
//
// The obvious shortcut does not work and was measured rather than assumed: **the rif path is not
// derivable from the `.gls` name.** It holds for 28 of the 32 shipped scripts and fails on four,
// including the campaign level `prison.gls` -> `levels\S3 Level.rif` and `railway.gls` ->
// `railway.rif`, which has no `levels\` prefix at all. Guessing would have produced no lights on
// those maps and looked like a feature that simply does nothing.

#include "Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gk {

// One `STDLIGHT`, converted into the units the world is in.
//
// The 3x3 orientation is carried raw rather than reduced to a direction, but **which row is the
// axis is now settled: row 2, elements 6..8**. Fitted against `SHPVTINT` over four levels
// (`utils/riflights/fit_bake.py`): rows 0 and 1 both make the fit worse, and negating row 2
// collapses it to r 0.02, which is what identifies an axis rather than a coincidence.
//
// It stays a matrix here because the *consumer* decides what to do with it, and two of the three
// facts around it are conditional: the cone applies only to lights **without** `LFlag_Omni` (the
// flag is a real switch - level04's fit goes 0.831 -> 0.640 when the cone is wrongly applied to
// its omni lights, and to 0.926 when they are exempted), and `spread` shapes nothing at all.
struct MapLight {
  Vec3 position;      // world space, via MapToWorld
  float range;        // world units - the rif value scaled the same way the position is
  float brightness;   // the file's 16.16, decoded; 0.2 .. 2.0 across the shipped set
  float colour[3];    // 0..1, from the packed 0x00RRGGBB
  float orientation[9]; // row-major 3x3, orthonormal. Uninterpreted, see above
  int32_t spread;     // 79 .. 2044; 1000 in 78% of the shipped lights
  // LFlag_*, from AvP's prototyp.h: 0x1 CosAtten, 0x2 CosSpreadAtten, 0x4 Omni. Only ever 3 or 7.
  int32_t flags;
  int32_t number;     // the editor's own id, unique within a file
};

// The hook. One detour on `LoadOrGetRifFile`, live for the process lifetime, so this is a member
// of `Subsystems` in entry.cpp like every other hook-owning subsystem.
//
// It does no work beyond recording a path and a scale - the file is parsed lazily on the first
// `MapLights()` after a level change, because a level load is not the place to add I/O.
class MapLightSystem {
public:
  MapLightSystem();
  ~MapLightSystem();

  MapLightSystem(const MapLightSystem &) = delete;
  MapLightSystem &operator=(const MapLightSystem &) = delete;
};

// The current level's lights, parsed on first use after a level change and cached until the next.
// Empty for a level whose rif carries no `LIGHTSET` - which is 501 of the 563 shipped files and
// includes `level03`, so an empty result is ordinary rather than a failure.
const std::vector<MapLight> &MapLights();

// `AMBIENCE` as a 0..1 fraction (the file's 16.16 over 65536), or 0 where the file has none.
// **A per-channel `max()` floor, not a term to add** - it is scalar and colourless, and Gunlok's
// 2048 is 3.125%. That is AvP's own renderer's reading of it, not an inference.
float MapAmbience();

// What was loaded, from where, and what was refused. Worth reading before concluding the feature
// does nothing: a level with no light set looks exactly like a loader that failed.
std::string MapLightReport();

} // namespace gk
