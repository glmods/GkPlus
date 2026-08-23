#pragma once

// DDS parsing, and nothing else.
//
// Deliberately pure: this file knows the DDS container and Gunlok's *constraints* on
// it, but touches no game memory and calls no game function. That is the same split
// `src/Vfs` (lookup) and `src/FileHooks` (the ABI that consults it) already have, and
// for the same reason - it is the half a throwaway harness can exercise, per
// `harness_testing_notes.md`. The game-facing image object that feeds these blocks to
// the engine lives in `src/ImageCodec`.
//
// Why DDS at all: Gunlok's image layer picks its decoder by *magic bytes*, through an
// open registration function (`RegisterImageCodec` @ 0x005c8360 - see `file_io_notes.md`
// §4), and nothing anywhere on the texture path reads a file extension. So a new
// container is a registration rather than a detour, and a `BMPNAMES` entry may name
// `Ground\ground.dds` as freely as it names a `.RIM`.
//
// DDS specifically, rather than PNG or KTX, because it carries **pre-compressed DXT
// blocks plus a mip chain**, which is exactly what the engine's S3TC path already
// expects. That buys two things no uncompressed format can:
//
//   * An S3TC source overrides format selection entirely - both arms of
//     `ChooseSurfaceFormatForImage`'s slot-18 branch end at the source's own fourcc, so
//     the candidate list, `Use32BitTextures` and the untraced compress-on-load staging
//     path are all bypassed (`rif_chunk_format.md`, "Use32BitTextures ... is not free").
//   * The engine **never generates mip levels** - there is no filter anywhere in the
//     binary - so a source without a chain shimmers. DDS carries one authored by a real
//     tool. See `rif_chunk_format.md`, "The engine never generates mip levels".

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gk::dds {

// The only two block formats that exist as far as Gunlok is concerned.
//
// This is a whitelist by name, not a block-size test, and that is the whole point.
// DXT2/DXT4/DXT5 share DXT3's 16-byte block, so the engine would derive an identical
// pitch, memcpy the blocks verbatim into a DXT3 surface, and render garbage alpha with
// no error raised anywhere - `SurfaceDesc_SetCompressedFormat` @ 0x005c6820 drops an
// unrecognised fourcc *silently*. `rimutil` refuses DXT5 by name for exactly this
// reason; so does this. See `rif_chunk_format.md`, "Only DXT1 and DXT3 exist as far as
// Gunlok is concerned".
enum class Format {
  Dxt1,   // 8 bytes/block, opaque or 1-bit alpha -> the engine reads alpha_bits 0
  Dxt3,   // 16 bytes/block, explicit 4-bit alpha -> alpha_bits 8
  Rgb24,  // uncompressed B,G,R           - truecolour, see below
  Argb32, // uncompressed B,G,R,A         - truecolour, see below
};

/// Whether \p format is one of the S3TC block formats, and therefore whether
/// the 4x4 block rules below apply to it.
constexpr bool IsCompressed(Format format) {
  return format == Format::Dxt1 || format == Format::Dxt3;
}

// Bytes per pixel on disk. Meaningless for the compressed formats.
constexpr unsigned SourceBytesPerPixel(Format format) {
  return format == Format::Rgb24 ? 3u : 4u;
}

// The uncompressed formats exist for one reason: **they are the only way to get
// true 24-bit colour into this engine.**
//
// A compressed source is stuck with DXT's endpoint quantisation, and an opaque
// uncompressed one is worse - `ChooseSurfaceFormatForImage` keeps the *last
// accepted* candidate, every uncompressed candidate accepts an `alpha_bits = 0`
// source, and the lowest-precision one is last. Measured: a 24bpp BMP lands on a
// 4-bit-per-channel surface, banding a grey ramp to 16 steps.
//
// The escape is to report `alpha_bits = 8`. That fails every candidate's
// `alpha <= maxAlphaBits` test (the caps are 4, 1, 0, 0) and skips the compressed
// ones (the source is not S3TC), so *nothing* is accepted and the **fallback
// descriptor** wins - which `Use32BitTextures = 1` makes A8R8G8B8, with the decode
// format equal to the surface format and no staging in between. `src/ImageCodec`
// does both halves. Full walk-through in `rif_chunk_format.md` under
// "Use32BitTextures ... is not free".
//
// These also escape the 4x4 mip floor: that row loop only decrements by 4 on the
// S3TC path, so an uncompressed chain may run to 1x1.

// The fourcc to hand back through the image object's slot 19 (`GetS3tcFourCC`). The
// engine tests for `DXT1` only and maps everything else to "8 alpha bits", i.e. it
// treats an unknown fourcc as DXT3 - so this must be exact.
constexpr uint32_t FourCc(Format format) {
  return format == Format::Dxt1 ? 0x31545844u /* 'DXT1' */ : 0x33545844u /* 'DXT3' */;
}

// Alpha bits to report through slot 4. DXT1 is 0 (the engine's own rule for a `.RIM`),
// DXT3 is 8 - and the uncompressed formats are **deliberately 8 even when the image is
// opaque**, because that is what makes the candidate walk reject everything and fall
// through to the 32-bit descriptor. See the note above.
constexpr int AlphaBits(Format format) { return format == Format::Dxt1 ? 0 : 8; }

/// Bytes one 4x4 block occupies: 8 for DXT1, 16 for DXT3. Meaningless for the
/// uncompressed formats, where LevelBytes() does not consult it.
constexpr unsigned BlockBytes(Format format) { return format == Format::Dxt1 ? 8u : 16u; }

// Bytes one mip level occupies, by the standard S3TC rule: whole 4x4 blocks, with
// dimensions rounded up. The tail of a chain (2x2, 1x1) still costs one full block,
// which is why this rounds rather than scaling.
constexpr size_t LevelBytes(Format format, unsigned width, unsigned height) {
  if (!IsCompressed(format)) {
    // Tightly packed, which is what every writer produces for a DDS. There is no
    // row padding in this container the way there is in a BMP.
    return static_cast<size_t>(width) * height * SourceBytesPerPixel(format);
  }
  const size_t blocks_x = width < 4 ? 1 : (width + 3) / 4;
  const size_t blocks_y = height < 4 ? 1 : (height + 3) / 4;
  return blocks_x * blocks_y * BlockBytes(format);
}

// Every dimension a **compressed** source presents must be a multiple of 4. This is not a
// stylistic rule about block alignment - it is a **termination condition**.
//
// `FillSurfaceFromImage`'s row loop seeds its counter with the level's height and, on the
// S3TC path, decrements by 4 per iteration (`SUB EAX,0x3` at 0x005c7410 then `SUB EAX,0x1`
// at 0x005c742f), exiting on the counter reaching *exactly* zero. A height of 4 lands on
// zero and exits. A height of 2 goes to -2 and a height of 1 to -3, and the test never
// sees zero again: the loop runs away, writing past the locked surface. There is no clamp
// anywhere on that path.
//
// So the tail of an ordinary mip chain - the 2x2 and 1x1 levels every texture tool
// writes - is not merely useless here, it is fatal. `Parse` drops it.
//
// **Uncompressed sources are exempt**: the `-= 3` is inside the `IsS3tc()` branch, so
// the counter drops by 1 per row and terminates at any height. Their chains are kept
// whole.
constexpr unsigned kDimensionAlignment = 4;

// One mip level, located inside the caller's buffer. `offset` is from the start of the
// whole file, so a level's bytes are `data + offset` for `size` bytes.
struct Level {
  unsigned width;
  unsigned height;
  size_t offset;
  size_t size;
};

// A parsed, validated DDS.
//
// `levels` holds only what the engine can safely consume: `levels[0]` is the base image,
// and the chain stops at the last level whose width and height are both multiples of 4.
// `dropped_tail_levels` counts what the file carried past that point - normally 2 (the
// 2x2 and 1x1 of a full chain), and worth reporting rather than hiding, because a modder
// who authored a chain and sees a shorter one used deserves to know why.
struct Image {
  Format format;
  unsigned width;
  unsigned height;
  std::vector<Level> levels;
  unsigned dropped_tail_levels;
};

// The magic plus `DDS_HEADER`: everything needed to size the rest of the file.
inline constexpr size_t kHeaderBytes = 4 + 124;

// How many bytes the whole file occupies, judged from its first `kHeaderBytes`.
//
// This exists because of how the file has to be read inside the engine. The byte source
// the image layer hands a codec has **no usable length query** - the Win32 implementation
// reports bytes-remaining, but the memory-backed one (which is what a mod-served file may
// arrive through) returns a hardcoded -1, and the seek slot is absolute-only with no
// SEEK_END. So a codec cannot ask how big the file is; it has to read a self-describing
// header and compute the rest. Two reads, no length query, identical behaviour on both
// source implementations.
//
// Returns nullopt on the same grounds `Parse` would refuse, so a malformed header is
// rejected before a second read is attempted.
std::optional<size_t> MeasureFile(const void *header, size_t size, std::string *error);

// Parse and validate a whole `.dds` file held in memory.
//
// Returns nullopt on anything this engine cannot render, with a human-readable reason in
// `*error` (which may be null). Refusal is always by name and never by guess: an
// unsupported fourcc, a DX10 extension header, a cubemap or a volume texture are all
// rejected outright rather than reinterpreted, because every one of those failure modes
// is silent further down.
std::optional<Image> Parse(const void *data, size_t size, std::string *error);

// The magic `RegisterImageCodec` is given. Four bytes, no NUL inside, so it is an
// ordinary C string - and the trie is longest-prefix, so it cannot collide with the
// seven codecs the engine registers ("BM", "FORM", "LIST", "CAT ", "P4", "P5", "P6").
inline constexpr const char *kMagic = "DDS ";

}  // namespace gk::dds
