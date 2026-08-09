#pragma once

// Reading `.rif` chunk files - the container, the tree walk, and the one chunk family the
// shipped engine never looks at.
//
// **Pure.** No game memory, no Vulkan, no D3D, no VFS: bytes in, records out. That is what makes
// it the second thing in `src/` (after `src/Dds`) that can be exercised outside Gunlok, and
// `utils/riflights` is the harness that does - over all 563 shipped files, cross-checked against
// the independent Python decoder in `blender/io_scene_rif`.
//
// The format is Rebellion's `3dc` chunk library, shared verbatim with the published Aliens vs
// Predator source; `rif_chunk_format.md` is the reference and every offset below is measured
// there across the whole shipped set rather than inferred. Every chunk, container or leaf, is a
// 12-byte header - an 8-character id and a total size *including* the header - followed by its
// body, and a container's body is nothing but its concatenated children.
//
// Why this exists: `STDLIGHT` is the light rig that BAKED each level's per-vertex colours, it
// ships in every level file, and **no shipped code reads it** (rif_chunk_format.md, "Gunlok never
// reads it - nor any of the lighting family"). It is the only record of what lit a map, which is
// what makes re-creating that lighting at runtime possible at all.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gk {
namespace rif {

// One `STDLIGHT`: 84 bytes, all integers, in all 3,794 shipped chunks. AvP's `Light_Data` field
// for field (`win95/LTCHUNK.HPP`).
//
// The two 16.16 fields are converted to float here because that is the only lossless reading of
// them and leaving them raw invites a caller to forget; everything else is kept **in the file's
// own units** - `location` and `range` in rif units, `colour` packed - because converting to
// world space needs the map, which this file deliberately knows nothing about. `src/MapLights`
// is where that happens.
struct Light {
  int32_t number = 0;        // editor-assigned id, unique within a file but not 0..n-1
  int32_t location[3] = {};  // rif units
  // 3x3 row-major, orthonormal to within 2e-3 in 100.00% of the 3,794 shipped lights once
  // divided by 65536. Rows in the order the file stores them.
  float orientation[9] = {};
  float brightness = 0.0f;   // was 16.16; 0.2 .. 2.0 across the shipped set
  int32_t spread = 0;        // 67 distinct values, 79..2044; 1000 in 2,955 of 3,794
  int32_t range = 0;         // rif units, 3,000 .. 357,300
  uint32_t colour = 0;       // 0x00RRGGBB
  int32_t engine_flags = 0;  // only ever 3 or 7
  int32_t local_flags = 0;   // always 1
};

// A file's one `LIGHTSET`. 62 of the 563 shipped files have one, always exactly one, and always a
// direct child of the file-level `REBENVDT`; **24 of those 62 carry no lights at all**, so an
// empty set is ordinary shipped data rather than a failure.
struct LightSet {
  std::vector<Light> lights;
  // `AMBIENCE`, as the file stores it: 16.16, and 2048 (= 3.125%) in all 62. It is a per-channel
  // **max()** floor rather than an added term and it is scalar rather than a colour - see
  // rif_chunk_format.md, which takes that from AvP's own renderer.
  int32_t ambience = 0;
  bool have_ambience = false;
  // The 8-character light set name from `LTSETHDR`, which is the SELECTOR a `SHPVTINT` is chosen
  // by: an object may carry one baked-colour chunk per light set. `"NORMALLT"` in all 62, and
  // Gunlok ships no second set - so this is here to be checked rather than to be branched on.
  std::string name;
};

// Decompress a `REBCRIF1` container into plain chunk bytes, or copy through when the input is
// already plain. **150 of the 563 shipped files are uncompressed and the game reads both.**
//
// NOT thread-safe: the decompressor in `huffman/` builds its table into a file-scope global.
// Everything here runs on the main thread at level load, which is the only caller there is.
bool Decompress(const void *bytes, size_t size, std::vector<unsigned char> *out,
                std::string *error);

// The file's light set, decompressing first if needed.
//
// Returns false only when the bytes are not a chunk file this can read. **A file with no
// `LIGHTSET` is a success with an empty set** - that is 501 of the 563 shipped files, and
// treating it as an error would make the normal case look broken.
bool ReadLightSet(const void *bytes, size_t size, LightSet *out, std::string *error);

} // namespace rif
} // namespace gk
