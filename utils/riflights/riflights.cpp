// Print the `LIGHTSET` of a `.rif`, as `src/Rif.cpp` reads it.
//
// This exists to make `src/Rif` testable at all. Nothing else in `src/` can run outside Gunlok -
// `GetBaseAddress()` derives from the host exe's entry point - but `src/Rif` touches no game
// memory, so it can be driven over all 563 shipped files here and cross-checked against the
// independent Python decoder in `blender/io_scene_rif`, which was written from the same
// measurements by a different route. `utils/riflights/tests/test_lights.py` is that check.
//
//     riflights <file.rif> [more.rif ...]
//
// One `light` line per `STDLIGHT`, tab-separated and in file order, so a diff against the Python
// side is a plain text diff. Fields are printed in the file's own units - rif units for position
// and range, the packed 0x00RRGGBB colour - because converting them needs a loaded map and this
// side has none.

// Relative, not via an include path - see this target's CMakeLists.txt for why `-I src` breaks
// the CRT headers.
#include "../../src/Rif.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool ReadFile(const char *path, std::vector<unsigned char> &out) {
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(file);
    return false;
  }
  out.resize(static_cast<size_t>(size));
  const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  return read == out.size();
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <file.rif> [more.rif ...]\n", argv[0]);
    return 2;
  }
  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    std::vector<unsigned char> bytes;
    if (!ReadFile(argv[i], bytes)) {
      std::fprintf(stderr, "%s: cannot read\n", argv[i]);
      ++failures;
      continue;
    }
    gk::rif::LightSet set;
    std::string error;
    if (!gk::rif::ReadLightSet(bytes.data(), bytes.size(), &set, &error)) {
      std::fprintf(stderr, "%s: %s\n", argv[i], error.c_str());
      ++failures;
      continue;
    }
    // A file with no light set prints the header line and nothing else. That is the normal case -
    // 501 of the 563 shipped files - so it must be a result rather than a diagnostic.
    std::printf("file\t%s\tlights=%zu\tname=%s\tambience=%d%s\n", argv[i], set.lights.size(),
                set.name.c_str(), set.ambience, set.have_ambience ? "" : "\t(absent)");
    for (const gk::rif::Light &light : set.lights) {
      std::printf("light\t%d\t%d\t%d\t%d\t%.6f\t%d\t%d\t0x%06x\t%d\t%d", light.number,
                  light.location[0], light.location[1], light.location[2], light.brightness,
                  light.spread, light.range, light.colour, light.engine_flags,
                  light.local_flags);
      for (float value : light.orientation) {
        std::printf("\t%.6f", value);
      }
      std::printf("\n");
    }
  }
  return failures == 0 ? 0 : 1;
}
