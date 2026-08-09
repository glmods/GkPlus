#include "Rif.h"

#include <cstring>

// Last, and on purpose: it is a C header that #defines MAX_DEPTH, which is exactly the kind of
// name a later include would collide with. Nothing below this line includes anything else.
#include "huffman.h"

namespace gk {
namespace rif {
namespace {

// The 12-byte chunk header: an 8-character id, then a total size INCLUDING the header.
constexpr size_t kHeaderSize = 12;
// `HuffmanPackage` is 8 + 4 + 4 + 44 + 256, and the compressed payload begins immediately after
// it - `HuffmanDecompress` reads it as `(int *)(inpackage + 1)`. Asserted rather than assumed
// because a padded struct would make every offset below wrong by silence.
static_assert(sizeof(HuffmanPackage) == 316, "the compressed payload starts right after this");

// Ids whose body is nothing but child chunks. Taken from the same list the Python decoder uses,
// which is measured over all 563 files: anything not here is an opaque leaf, and that is what
// makes an unknown chunk harmless rather than a parse failure.
//
// Only three of these are on the path to a light set - REBINFF2 -> REBENVDT -> LIGHTSET - but
// descending the rest costs nothing and keeps a mod's file from being mis-parsed because it
// nests differently from the shipped ones.
const char *const kContainers[] = {
    "REBINFF2", "RBOBJECT", "REBSHAPE", "SUBSHAPE", "MODULEDT", "OBJPRJDT",
    "ANIMSEQU", "ANIMFRAM", "ASALTTEX", "SHPEXTFL", "SHPMORPH", "SHPFRAGS",
    "REBENVDT", "SPECLOBJ", "OBJCHIER", "OBANSEQS", "OBANSEQC", "SOUNDEXD",
    "LIGHTSET", "DUMMYOBJ", "CUTSHEAD", "CUTSCUSR", "CUTTRACK", "SUBRIFFL",
    // AvP's Object_Interface_Data_Chunk. Its id is seven characters NUL-padded, which is why
    // every comparison here is over exactly 8 bytes rather than a C string compare.
    "OBINTDT\0",
};

bool IdIs(const unsigned char *id, const char *want) {
  return std::memcmp(id, want, 8) == 0;
}

bool IsContainer(const unsigned char *id) {
  for (const char *candidate : kContainers) {
    if (IdIs(id, candidate)) {
      return true;
    }
  }
  return false;
}

int32_t ReadI32(const unsigned char *p) {
  int32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint32_t ReadU32(const unsigned char *p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// 16.16 fixed point. `ONE_FIXED` is 65536 (AvP's System.h), and nothing in this family is a float
// on disk - reading these as floats yields denormals and NaNs, which is what originally gave the
// layout away.
float FromFixed(int32_t v) { return static_cast<float>(v) / 65536.0f; }

constexpr size_t kStdLightSize = 84;

Light DecodeLight(const unsigned char *body) {
  Light light;
  light.number = ReadI32(body + 0x00);
  for (int i = 0; i < 3; ++i) {
    light.location[i] = ReadI32(body + 0x04 + 4 * i);
  }
  for (int i = 0; i < 9; ++i) {
    light.orientation[i] = FromFixed(ReadI32(body + 0x10 + 4 * i));
  }
  light.brightness = FromFixed(ReadI32(body + 0x34));
  light.spread = ReadI32(body + 0x38);
  light.range = ReadI32(body + 0x3c);
  light.colour = ReadU32(body + 0x40);
  light.engine_flags = ReadI32(body + 0x44);
  light.local_flags = ReadI32(body + 0x48);
  return light;
}

// Walks one container's body. `depth` exists because a malformed file can nest forever and this
// runs on bytes a mod supplied; the shipped set never exceeds a handful of levels.
void WalkChildren(const unsigned char *body, size_t size, int depth, LightSet *out) {
  if (depth > 16) {
    return;
  }
  size_t offset = 0;
  while (offset + kHeaderSize <= size) {
    const unsigned char *id = body + offset;
    const int32_t total = ReadI32(body + offset + 8);
    // A size that does not advance, or runs past the parent, ends the walk rather than throwing:
    // whatever was recovered before it is still true, and a light set that is short is easier to
    // notice than a whole level that failed to load.
    if (total < static_cast<int32_t>(kHeaderSize) ||
        offset + static_cast<size_t>(total) > size) {
      return;
    }
    const unsigned char *child = body + offset + kHeaderSize;
    const size_t child_size = static_cast<size_t>(total) - kHeaderSize;

    if (IdIs(id, "STDLIGHT")) {
      if (child_size >= kStdLightSize) {
        out->lights.push_back(DecodeLight(child));
      }
    } else if (IdIs(id, "AMBIENCE")) {
      if (child_size >= 4) {
        out->ambience = ReadI32(child);
        out->have_ambience = true;
      }
    } else if (IdIs(id, "LTSETHDR")) {
      // char[8] name + int pad. Trimmed at the first NUL, because the shipped bodies are
      // "NORMALLT" followed by four zero bytes and a caller comparing against a C string should
      // not have to know that.
      if (child_size >= 8) {
        size_t length = 0;
        while (length < 8 && child[length] != 0) {
          ++length;
        }
        out->name.assign(reinterpret_cast<const char *>(child), length);
      }
    } else if (IsContainer(id)) {
      WalkChildren(child, child_size, depth + 1, out);
    }
    offset += static_cast<size_t>(total);
  }
}

} // namespace

bool Decompress(const void *bytes, size_t size, std::vector<unsigned char> *out,
                std::string *error) {
  const auto *begin = static_cast<const unsigned char *>(bytes);
  if (size < kHeaderSize) {
    if (error != nullptr) {
      *error = "too small to be a chunk file";
    }
    return false;
  }
  if (!IdIs(begin, "REBCRIF1")) {
    // Already plain. The root is checked by the caller, not here, so this stays the one function
    // that only answers "compressed or not".
    out->assign(begin, begin + size);
    return true;
  }
  if (size < sizeof(HuffmanPackage)) {
    if (error != nullptr) {
      *error = "REBCRIF1 header is truncated";
    }
    return false;
  }
  // Copied rather than cast in place: the decompressor takes a non-const pointer and reads up to
  // a word past the end of the compressed block, so it needs a buffer it owns the tail of. The
  // Python port needed the same padding for the same reason.
  std::vector<unsigned char> owned(begin, begin + size);
  owned.resize(size + 8, 0);
  auto *package = reinterpret_cast<HuffmanPackage *>(owned.data());
  const int declared = package->UncompressedDataSize;
  if (declared <= 0) {
    if (error != nullptr) {
      *error = "REBCRIF1 declares no uncompressed data";
    }
    return false;
  }
  // HuffmanDecompress resizes its output to declared + 16 - the decoder writes past the end -
  // so the vector is always larger than the file. Shrinking to the declared size afterwards is
  // what stops the extra bytes being walked as a chunk header.
  std::vector<unsigned char> plain;
  HuffmanDecompress(package, plain);
  if (plain.size() < static_cast<size_t>(declared)) {
    if (error != nullptr) {
      *error = "REBCRIF1 decompressed short";
    }
    return false;
  }
  plain.resize(static_cast<size_t>(declared));
  out->swap(plain);
  return true;
}

bool ReadLightSet(const void *bytes, size_t size, LightSet *out, std::string *error) {
  *out = LightSet{};
  std::vector<unsigned char> plain;
  if (!Decompress(bytes, size, &plain, error)) {
    return false;
  }
  if (plain.size() < kHeaderSize || !IdIs(plain.data(), "REBINFF2")) {
    if (error != nullptr) {
      *error = "not a REBINFF2 chunk file";
    }
    return false;
  }
  const int32_t total = ReadI32(plain.data() + 8);
  // The root's declared size is trusted only as far as the buffer goes. A file truncated in
  // transit still yields whatever chunks precede the cut.
  size_t body = plain.size() - kHeaderSize;
  if (total >= static_cast<int32_t>(kHeaderSize) &&
      static_cast<size_t>(total) <= plain.size()) {
    body = static_cast<size_t>(total) - kHeaderSize;
  }
  WalkChildren(plain.data() + kHeaderSize, body, 0, out);
  return true;
}

} // namespace rif
} // namespace gk
