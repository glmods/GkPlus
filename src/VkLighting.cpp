#include "VkLighting.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Core.h"
#include "Dds.h"
#include "VkResources.h"
#include "Vfs.h"

namespace gk {
namespace vulkan {
namespace {

// D3DFORMAT values, as VkResources' MapFormat spells them. Only what a `.dds` can arrive as.
enum : uint32_t {
  kD3DFmtA8R8G8B8 = 21,
  kD3DFmtDXT1 = 0x31545844, // 'DXT1'
  kD3DFmtDXT3 = 0x33545844, // 'DXT3'
};

bool Enabled = true;
LightingMapParams Params;
LightingMapStats TheStats;

// One `.rim` name's answer, cached forever - including the negative one, which is the common case
// and the one that must not cost a file probe per frame. Keyed by the lowered `.rim` path.
struct Entry {
  TextureImage image;   // valid only when `found`
  std::string source;   // where it came from, for the readback
  std::string error;    // why not, when it was found and refused
  bool found = false;
};

std::map<std::string, Entry> Entries;

// Base image bindless slot -> lighting image bindless slot. Rebuilt when the texture registry
// changes, exactly like the material override's table and for the same reason.
std::vector<uint32_t> ByBase;
// Our own images' slots, so IsLightingImage can answer without a name comparison.
std::vector<bool> Mine;
uint64_t ResolvedGeneration = 0;

std::string Lowered(const std::string &text) {
  std::string out = text;
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// `bitmaps\lava.rim` -> `bitmaps/lava`. Separators are normalised to forward slashes because that
// is what the VFS speaks; the real-file candidates are built back the other way.
std::string Stem(const std::string &rim_path) {
  std::string out = rim_path;
  for (char &c : out) {
    if (c == '\\') {
      c = '/';
    }
  }
  const size_t dot = out.find_last_of('.');
  const size_t slash = out.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    out.resize(dot);
  }
  return out;
}

// The four names a companion may have, in the order they are tried. Two suffix spellings times
// two roots: under the `graphics` GLDir, which is where every `.rim` name measured so far is
// relative to, and bare, for one acquired under another category.
std::vector<std::string> Candidates(const std::string &rim_path) {
  const std::string stem = Stem(rim_path);
  std::vector<std::string> out;
  for (const char *suffix : {" lighting.dds", "_lighting.dds"}) {
    out.push_back("graphics/" + stem + suffix);
    out.push_back(stem + suffix);
  }
  return out;
}

bool ReadRealFile(const std::string &path, std::vector<char> &out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0) {
    return false;
  }
  out.resize(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(out.data(), size);
  return static_cast<size_t>(file.gcount()) == out.size();
}

// The mod VFS first, then the real file. `vfs::Read` is case-insensitive by construction (see
// Vfs.h) and Windows' own filesystem is too, so neither half depends on the casing a `.rim` name
// happens to carry - which is undiscoverable, since it comes out of a `.gls` or an exe literal.
bool LoadCandidate(const std::string &vpath, std::vector<char> &bytes, std::string &source) {
  if (vfs::Read(vpath.c_str(), &bytes) && !bytes.empty()) {
    source = "mod:" + vpath;
    return true;
  }
  std::string real = vfs::GameDir() + vpath;
  for (char &c : real) {
    if (c == '/') {
      c = '\\';
    }
  }
  if (ReadRealFile(real, bytes)) {
    source = real;
    return true;
  }
  return false;
}

uint32_t FormatFor(dds::Format format) {
  switch (format) {
  case dds::Format::Dxt1:
    return kD3DFmtDXT1;
  case dds::Format::Dxt3:
    return kD3DFmtDXT3;
  default:
    // Both uncompressed forms land on the one 32-bit layout D3D and Vulkan agree about; a 24-bit
    // source is widened on the way in (see UploadLevels), because there is no three-byte VkFormat
    // worth having and expanding costs one pass over rows that are being copied anyway.
    return kD3DFmtA8R8G8B8;
  }
}

// Uploads every level `Parse` kept. `data` is the whole file; a Level's offset is from its start.
//
// The pitch is what D3D would report for a locked surface of that level: bytes per row of BLOCKS
// for the compressed formats, bytes per row of texels otherwise - which is exactly what
// `UploadIntoTextureImage` documents it wants, so nothing here knows how the image is laid out.
bool UploadLevels(const TextureImage &image, const dds::Image &parsed, const char *data) {
  std::vector<uint8_t> widened;
  for (size_t level = 0; level < parsed.levels.size(); ++level) {
    const dds::Level &info = parsed.levels[level];
    const char *pixels = data + info.offset;
    uint32_t pitch = 0;
    if (dds::IsCompressed(parsed.format)) {
      const uint32_t blocks = (info.width + 3) / 4;
      pitch = blocks * dds::BlockBytes(parsed.format);
    } else if (parsed.format == dds::Format::Argb32) {
      pitch = info.width * 4;
    } else {
      // B,G,R -> B,G,R,255. The alpha channel of a lighting map is unread, so the value is
      // arbitrary; 255 is what a reader of a hex dump would expect to see.
      widened.resize(size_t(info.width) * info.height * 4);
      for (size_t i = 0; i < size_t(info.width) * info.height; ++i) {
        widened[i * 4 + 0] = static_cast<uint8_t>(pixels[i * 3 + 0]);
        widened[i * 4 + 1] = static_cast<uint8_t>(pixels[i * 3 + 1]);
        widened[i * 4 + 2] = static_cast<uint8_t>(pixels[i * 3 + 2]);
        widened[i * 4 + 3] = 255;
      }
      pixels = reinterpret_cast<const char *>(widened.data());
      pitch = info.width * 4;
    }
    if (!UploadIntoTextureImage(image, static_cast<uint32_t>(level), 0, 0, info.width,
                                info.height, pixels, pitch)) {
      return false;
    }
    TheStats.bytes_uploaded += info.size;
  }
  return true;
}

// Finds, decodes and uploads one companion. Returns the cache entry, found or not; a miss is
// recorded so the file system is asked once per name per session.
const Entry &LoadFor(const std::string &lowered_rim) {
  const auto existing = Entries.find(lowered_rim);
  if (existing != Entries.end()) {
    return existing->second;
  }
  Entry entry;
  ++TheStats.names_probed;

  std::vector<char> bytes;
  std::string source;
  bool have = false;
  for (const std::string &candidate : Candidates(lowered_rim)) {
    if (LoadCandidate(candidate, bytes, source)) {
      have = true;
      break;
    }
  }
  if (!have) {
    return Entries.emplace(lowered_rim, entry).first->second;
  }

  ++TheStats.maps_found;
  entry.source = source;
  std::string error;
  const std::optional<dds::Image> parsed = dds::Parse(bytes.data(), bytes.size(), &error);
  if (!parsed.has_value()) {
    entry.error = error.empty() ? "not a DDS this build can read" : error;
    ++TheStats.load_failures;
    DebugWrite("gkplus: lighting map " + source + ": " + entry.error + "\n");
    return Entries.emplace(lowered_rim, entry).first->second;
  }
  if (!CreateTextureImage(entry.image, parsed->width, parsed->height,
                          static_cast<uint32_t>(parsed->levels.size()),
                          FormatFor(parsed->format))) {
    entry.error = "no image could be created for it";
    ++TheStats.load_failures;
    DebugWrite("gkplus: lighting map " + source + ": " + entry.error + "\n");
    return Entries.emplace(lowered_rim, entry).first->second;
  }
  if (!UploadLevels(entry.image, *parsed, bytes.data())) {
    entry.error = "a mip level would not upload";
    ++TheStats.load_failures;
    DestroyTextureImage(entry.image);
    DebugWrite("gkplus: lighting map " + source + ": " + entry.error + "\n");
    return Entries.emplace(lowered_rim, entry).first->second;
  }
  // Named by the file it came from, so `render.textures` says what it is rather than showing an
  // unnamed image beside every texture. The name is what IsLightingImage exists to keep out of the
  // material override's name search.
  NameTextureImage(entry.image, source);
  entry.found = true;
  ++TheStats.images_created;
  DebugWrite("gkplus: lighting map " + source + " -> image " +
             std::to_string(entry.image.index) + "\n");
  return Entries.emplace(lowered_rim, entry).first->second;
}

// Rebuilds `ByBase` from the live image set, loading any companion not seen before.
//
// The generation is re-read at the END rather than at the start, because creating and naming an
// image bumps it - reading it first would leave the table permanently stale and rescan every
// draw. The cost of that ordering is that a base image created *during* this pass is picked up on
// the next bump instead, which is the same frame in practice.
void ResolveLightingMaps() {
  ByBase.clear();
  ++TheStats.resolves;
  if (!Enabled) {
    ResolvedGeneration = TextureRegistryGeneration();
    return;
  }
  const std::vector<TextureImageInfo> images = TextureImages();
  for (const TextureImageInfo &image : images) {
    if (image.name.empty() || IsLightingImage(image.index)) {
      continue;
    }
    const Entry &entry = LoadFor(Lowered(image.name));
    if (!entry.found) {
      continue;
    }
    if (ByBase.size() <= image.index) {
      ByBase.resize(size_t(image.index) + 1, kNoLightingMap);
    }
    ByBase[image.index] = entry.image.index;
    if (Mine.size() <= entry.image.index) {
      Mine.resize(size_t(entry.image.index) + 1, false);
    }
    Mine[entry.image.index] = true;
  }
  ResolvedGeneration = TextureRegistryGeneration();
}

} // namespace

void SetLightingMaps(bool enabled) {
  if (Enabled == enabled) {
    return;
  }
  Enabled = enabled;
  if (enabled) {
    // **Switching it back on re-reads every file**, and that is the authoring gesture as much as
    // the A/B one: `render.lighting_maps = false` then `true` picks up a map dropped in - or
    // edited - while the game is running, where nothing else would ever ask again. The cache is
    // what keeps a texture with no companion from costing a file probe per frame, so it has to be
    // droppable for the same reason it exists.
    //
    // The images go with it rather than being kept and reused, or an edit would be invisible
    // while the stale image stayed live; `DestroyTextureImage` waits for the device, and this
    // runs between frames on the main thread, not inside a recording.
    ShutdownLightingMaps();
  }
  // Forced rather than left to the generation check: switching it on has to load what was never
  // loaded while it was off, and nothing about the registry has changed to say so.
  ResolveLightingMaps();
}

bool LightingMaps() { return Enabled; }

const LightingMapParams &LightingParams() { return Params; }

LightingMapParams &MutableLightingParams() { return Params; }

uint32_t LightingMapFor(uint32_t texture_index) {
  if (!Enabled || texture_index >= ByBase.size()) {
    return kNoLightingMap;
  }
  return ByBase[texture_index];
}

bool IsLightingImage(uint32_t texture_index) {
  return texture_index < Mine.size() && Mine[texture_index];
}

void EnsureLightingMapsResolved() {
  if (ResolvedGeneration != TextureRegistryGeneration()) {
    ResolveLightingMaps();
  }
}

void ShutdownLightingMaps() {
  for (auto &pair : Entries) {
    if (pair.second.found) {
      DestroyTextureImage(pair.second.image);
    }
  }
  Entries.clear();
  ByBase.clear();
  Mine.clear();
  ResolvedGeneration = 0;
}

const LightingMapStats &LightingMapCounters() { return TheStats; }

LightingMapStats &MutableLightingCounters() { return TheStats; }

std::string DescribeLightingMaps() {
  EnsureLightingMapsResolved();
  std::string out;
  char line[512];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
  };
  add("lighting maps: %s\n", Enabled ? "on" : "OFF (render.lighting_maps = true)");
  add("  bump_scale %.3f  bump_diffuse %.3f  specular_scale %.3f  gloss %.1f..%.1f  "
      "specular_from_diffuse %.3f\n",
      Params.bump_scale, Params.bump_diffuse, Params.specular_scale, Params.gloss_min,
      Params.gloss_max, Params.specular_from_diffuse);
  add("  %llu names probed, %llu with a file, %llu refused, %llu images, %llu materials lit\n",
      (unsigned long long)TheStats.names_probed, (unsigned long long)TheStats.maps_found,
      (unsigned long long)TheStats.load_failures, (unsigned long long)TheStats.images_created,
      (unsigned long long)TheStats.materials_lit);
  // Only the names something was found for, plus the refusals. Listing every miss would print one
  // line per texture in the level, and "no companion file" is the normal case rather than news.
  size_t listed = 0;
  for (const auto &pair : Entries) {
    if (pair.second.source.empty()) {
      continue;
    }
    ++listed;
    add("  %-44s %s\n", pair.first.c_str(),
        pair.second.found ? pair.second.source.c_str()
                          : (pair.second.source + "  REFUSED: " + pair.second.error).c_str());
  }
  if (listed == 0) {
    out += "  no companion file found for any texture\n"
           "  a texture bitmaps\\lava.rim takes graphics/bitmaps/lava lighting.dds,\n"
           "  from a mod under gkplus/mods or from the file itself\n";
  }
  return out;
}

} // namespace vulkan
} // namespace gk
