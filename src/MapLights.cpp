#include "MapLights.h"

// windows.h first: detours.h needs the architecture macros it defines, and without it the errors
// come out of detours.h as "Unknown architecture" and a wall of unknown types. NOMINMAX because
// windows.h otherwise #defines min and max, which WIN32_LEAN_AND_MEAN does not suppress - the
// report below calls std::min and the errors land there rather than here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <detours.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member pointers, and
// merely declaring them inside namespace gk hides the global templates that take a plain
// function pointer. The same reason src/Script.cpp includes <detours.h> directly.
#include "Core.h"
#include "Map.h"
#include "Rif.h"
#include "Vfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gk {
namespace {

// `LoadOrGetRifFile` @ 0x004ae960 — RifFile * __fastcall(const char *name /*ECX*/,
// bool reuse_cached_and_prefer_opt /*DL*/), bare RET.
//
// __fastcall with arguments in ECX **and** EDX, which is why this is a free function pointer and
// not the member-pointer trick InputFix uses: a __thiscall member puts only `this` in ECX and
// everything else on the stack, so modelling this one that way would read EDX as garbage and
// clean the wrong number of bytes.
FastCall<void *, const char *, int> LoadOrGetRifFile = nullptr;

// What the hook saw last, and the generation that says whether it is new. The hook runs on the
// main thread during a level load and every reader is on the main thread too, so none of this is
// synchronised.
std::string PendingPath;
float PendingScale = 0.0f;
uint32_t PendingGeneration = 0;

// The parsed result, and the generation and map it was parsed for. Two keys, not one: a level
// reload gives a new Map, and a level whose rif is re-read gives a new generation.
std::vector<MapLight> Lights;
float Ambience = 0.0f;
uint32_t LoadedGeneration = 0;
const Map *LoadedMap = nullptr;
bool Loaded = false;
std::string LoadedPath;
std::string LoadedSource;
std::string LoadedError;
std::string LoadedSetName;
uint32_t RifLoadsSeen = 0;
uint32_t LevelRifLoadsSeen = 0;

std::string LowerAscii(const char *text) {
  std::string out(text != nullptr ? text : "");
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c == '/') {
      c = '\\';
    }
  }
  return out;
}

// `<stem>.loc` and `<stem>.opt` are the engine's own generated sidecars and both carry the whole
// LIGHTSET - but the `.rif` is what a mod can replace, so it is the one to read. Normalising here
// rather than at the read site keeps "which file" one decision.
std::string NormaliseToRif(const std::string &lowered) {
  if (lowered.size() < 4) {
    return lowered;
  }
  const std::string extension = lowered.substr(lowered.size() - 4);
  if (extension == ".loc" || extension == ".opt") {
    return lowered.substr(0, lowered.size() - 4) + ".rif";
  }
  return lowered;
}

// Every rif the game loads passes through here - units, objects, hierarchies and the level - so
// the level's is picked out by its directory. `ToMap` passes the map section's `file` field
// verbatim, and all 32 shipped scripts put it under `levels\` except `railway.gls`, which names
// a bare `railway.rif`; that one is caught by the `.loc`/`.opt` normalisation above only if it is
// also under levels\, so it is deliberately NOT special-cased - see the report line for a level
// whose lights did not load.
bool LooksLikeLevelRif(const std::string &lowered) {
  return lowered.rfind("levels\\", 0) == 0;
}

void *__fastcall HookedLoadOrGetRifFile(const char *name, int prefer) {
  void *rif = LoadOrGetRifFile(name, prefer);
  ++RifLoadsSeen;
  if (name == nullptr || rif == nullptr) {
    return rif;
  }
  const std::string lowered = LowerAscii(name);
  if (!LooksLikeLevelRif(lowered)) {
    return rif;
  }
  ++LevelRifLoadsSeen;
  // **Read the scale here and copy it out.** The rif object does not survive its own load:
  // LoadOrGetRifFile clears the cache on every miss and LoadLevel clears it again right after
  // ConvertParsedObjects, so retaining this pointer would leave a dangling read at play time.
  PendingScale = RifUnitScale(rif);
  PendingPath = NormaliseToRif(lowered);
  ++PendingGeneration;
  return rif;
}

// The mod VFS first, then the install - the same two roots and the same order src/VkLighting
// uses for a lighting map, so a mod that replaces a level's geometry replaces its lights with it.
bool LoadBytes(const std::string &vpath, std::vector<unsigned char> &bytes, std::string &source) {
  std::vector<char> served;
  if (vfs::Read(vpath.c_str(), &served) && !served.empty()) {
    bytes.assign(served.begin(), served.end());
    source = "mod:" + vpath;
    return true;
  }
  std::string real = vfs::GameDir() + "rif\\" + vpath;
  for (char &c : real) {
    if (c == '/') {
      c = '\\';
    }
  }
  std::FILE *file = std::fopen(real.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    return false;
  }
  bytes.resize(static_cast<size_t>(size));
  const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    return false;
  }
  source = real;
  return true;
}

void Reload() {
  Lights.clear();
  Ambience = 0.0f;
  LoadedSource.clear();
  LoadedError.clear();
  LoadedSetName.clear();
  LoadedPath = PendingPath;
  LoadedGeneration = PendingGeneration;
  LoadedMap = GetCurrentMap();
  Loaded = true;

  if (LoadedPath.empty()) {
    LoadedError = "no level rif seen yet";
    return;
  }
  if (PendingScale <= 0.0f) {
    LoadedError = "the rif reported no unit scale";
    return;
  }
  const Map *map = LoadedMap;
  if (map == nullptr) {
    LoadedError = "no map loaded";
    return;
  }

  std::vector<unsigned char> bytes;
  if (!LoadBytes(LoadedPath, bytes, LoadedSource)) {
    LoadedError = "cannot read " + LoadedPath;
    return;
  }
  rif::LightSet set;
  std::string error;
  if (!rif::ReadLightSet(bytes.data(), bytes.size(), &set, &error)) {
    LoadedError = error.empty() ? "not a rif this build can read" : error;
    return;
  }
  LoadedSetName = set.name;
  Ambience = set.have_ambience ? static_cast<float>(set.ambience) / 65536.0f : 0.0f;

  Lights.reserve(set.lights.size());
  for (const rif::Light &light : set.lights) {
    MapLight out{};
    // The same transform every placed object goes through - a per-component scale and offset with
    // NO swizzle, so the game's world shares the rif's Y-down convention and a direction taken
    // out of the orientation below needs no change of basis either.
    const Vec3 rif_pos{static_cast<float>(light.location[0]),
                       static_cast<float>(light.location[1]),
                       static_cast<float>(light.location[2])};
    // The scale captured at load, not one read back now: the rif is gone by the time this runs.
    out.position = {rif_pos.x * PendingScale + map->neg_origin.x,
                    rif_pos.y * PendingScale + map->neg_origin.y,
                    rif_pos.z * PendingScale + map->neg_origin.z};
    // Range is a distance, so it takes the scale and NOT the origin.
    out.range = static_cast<float>(light.range) * PendingScale;
    out.brightness = light.brightness;
    out.colour[0] = static_cast<float>((light.colour >> 16) & 0xffu) / 255.0f;
    out.colour[1] = static_cast<float>((light.colour >> 8) & 0xffu) / 255.0f;
    out.colour[2] = static_cast<float>(light.colour & 0xffu) / 255.0f;
    std::memcpy(out.orientation, light.orientation, sizeof(out.orientation));
    out.spread = light.spread;
    out.flags = light.engine_flags;
    out.number = light.number;
    Lights.push_back(out);
  }
}

// A level change is either a new Map or a new rif. Checking both is what makes reloading the same
// level pick the lights up again, where a Map-only test would keep the previous set if the
// allocator handed back the same address.
void EnsureLoaded() {
  if (!Loaded || LoadedGeneration != PendingGeneration || LoadedMap != GetCurrentMap()) {
    Reload();
  }
}

} // namespace

MapLightSystem::MapLightSystem() {
  GetObjectAtOffset(LoadOrGetRifFile, 0x004ae960);
  DetourAttach(&LoadOrGetRifFile, HookedLoadOrGetRifFile);
}

MapLightSystem::~MapLightSystem() {
  DetourDetach(&LoadOrGetRifFile, HookedLoadOrGetRifFile);
}

const std::vector<MapLight> &MapLights() {
  EnsureLoaded();
  return Lights;
}

float MapAmbience() {
  EnsureLoaded();
  return Ambience;
}

uint32_t MapLightsGeneration() {
  EnsureLoaded();
  return LoadedGeneration;
}

std::string MapLightReport() {
  EnsureLoaded();
  std::string out;
  char line[512];

  std::snprintf(line, sizeof(line), "rif loads seen: %u (%u under levels\\)\n", RifLoadsSeen,
                LevelRifLoadsSeen);
  out += line;
  std::snprintf(line, sizeof(line), "level rif: %s\n",
                LoadedPath.empty() ? "(none seen)" : LoadedPath.c_str());
  out += line;
  std::snprintf(line, sizeof(line), "served from: %s\n",
                LoadedSource.empty() ? "(not read)" : LoadedSource.c_str());
  out += line;
  std::snprintf(line, sizeof(line), "unit scale: %g\n", PendingScale);
  out += line;
  if (!LoadedError.empty()) {
    std::snprintf(line, sizeof(line), "ERROR: %s\n", LoadedError.c_str());
    out += line;
  }
  std::snprintf(line, sizeof(line), "light set: %s   ambience: %.4f (a max() floor, not a term)\n",
                LoadedSetName.empty() ? "(none)" : LoadedSetName.c_str(), Ambience);
  out += line;
  // A level with no LIGHTSET is the normal case for 501 of 563 files, so it is stated rather
  // than left to look like a failure.
  std::snprintf(line, sizeof(line), "lights: %zu%s\n", Lights.size(),
                Lights.empty() && LoadedError.empty() ? "  (this rif carries no LIGHTSET)" : "");
  out += line;

  if (!Lights.empty()) {
    Vec3 lo = Lights[0].position;
    Vec3 hi = Lights[0].position;
    float min_range = Lights[0].range;
    float max_range = Lights[0].range;
    uint32_t omni = 0;
    for (const MapLight &light : Lights) {
      lo = {std::min(lo.x, light.position.x), std::min(lo.y, light.position.y),
            std::min(lo.z, light.position.z)};
      hi = {std::max(hi.x, light.position.x), std::max(hi.y, light.position.y),
            std::max(hi.z, light.position.z)};
      min_range = std::min(min_range, light.range);
      max_range = std::max(max_range, light.range);
      omni += (light.flags & 0x4) != 0 ? 1u : 0u;
    }
    // The bounds are the reading that says the transform is right: they should sit inside the
    // map's own world bounds, and a scale or origin applied wrongly puts them orders out.
    std::snprintf(line, sizeof(line),
                  "world bounds: %.1f %.1f %.1f .. %.1f %.1f %.1f\n", lo.x, lo.y, lo.z, hi.x,
                  hi.y, hi.z);
    out += line;
    const Map *map = GetCurrentMap();
    if (map != nullptr) {
      std::snprintf(line, sizeof(line), "map bounds:   %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
                    map->bounds_min.x, map->bounds_min.y, map->bounds_min.z, map->bounds_max.x,
                    map->bounds_max.y, map->bounds_max.z);
      out += line;
    }
    std::snprintf(line, sizeof(line), "range: %.2f .. %.2f world units   omni: %u of %zu\n",
                  min_range, max_range, omni, Lights.size());
    out += line;
  }
  return out;
}

} // namespace gk
