#include "Dds.h"

#include <cstring>

namespace gk::dds {
namespace {

// The on-disk header, exactly as Microsoft documents it. Little-endian throughout, which
// costs nothing on x86 - note this is the opposite of a `.RIM`, whose IFF sizes are all
// big-endian.
//
// Laid out as offsets rather than a packed struct so there is nothing for a compiler to
// pad and nothing to static_assert about: the file is read field by field.
constexpr size_t kMagicSize = 4;
constexpr size_t kHeaderSize = 124;   // DDS_HEADER, excluding the magic
constexpr size_t kPixelFormatOffset = 72;  // of DDS_HEADER's own start
constexpr size_t kDx10HeaderSize = 20;

// DDS_HEADER fields, as offsets into the 124 bytes.
constexpr size_t kOffSize = 0;
constexpr size_t kOffFlags = 4;
constexpr size_t kOffHeight = 8;
constexpr size_t kOffWidth = 12;
constexpr size_t kOffMipMapCount = 24;
constexpr size_t kOffCaps2 = 112;

// DDS_PIXELFORMAT fields, as offsets into its own 32 bytes.
constexpr size_t kPfOffSize = 0;
constexpr size_t kPfOffFlags = 4;
constexpr size_t kPfOffFourCc = 8;
constexpr size_t kPfOffRgbBitCount = 12;
constexpr size_t kPfOffRMask = 16;
constexpr size_t kPfOffGMask = 20;
constexpr size_t kPfOffBMask = 24;
constexpr size_t kPfOffAMask = 28;

constexpr uint32_t kDdsdMipMapCount = 0x00020000;
constexpr uint32_t kDdpfAlphaPixels = 0x00000001;
constexpr uint32_t kDdpfFourCc = 0x00000004;
constexpr uint32_t kDdpfRgb = 0x00000040;
constexpr uint32_t kCaps2Cubemap = 0x00000200;
constexpr uint32_t kCaps2Volume = 0x00200000;

uint32_t ReadU32(const unsigned char *p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

bool Fail(std::string *error, const char *reason) {
  if (error != nullptr) {
    *error = reason;
  }
  return false;
}

// A fourcc as four printable characters, for an error message. Refusals here are
// diagnosed by name because the alternative - silently reinterpreting an unknown block
// format as DXT3 - is exactly the failure this whitelist exists to prevent, and it is
// invisible on screen until someone looks at the alpha.
std::string FourCcName(uint32_t fourcc) {
  std::string out(4, ' ');
  for (int i = 0; i < 4; ++i) {
    const unsigned char c = static_cast<unsigned char>((fourcc >> (i * 8)) & 0xff);
    out[i] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
  }
  return out;
}

// Everything both entry points need out of the fixed-size header. Kept in one place so
// `MeasureFile` and `Parse` can never disagree about what a header means - they are used
// in sequence on the same file, and a discrepancy would show up as a short read.
struct HeaderInfo {
  Format format;
  unsigned width;
  unsigned height;
  uint32_t declared_levels;
  bool has_dx10_header;
};

bool ParseHeader(const void *data, size_t size, HeaderInfo *out, std::string *error) {
  const auto *bytes = static_cast<const unsigned char *>(data);

  if (data == nullptr || size < kMagicSize + kHeaderSize) {
    return Fail(error, "too small to hold a DDS header");
  }
  if (std::memcmp(bytes, kMagic, kMagicSize) != 0) {
    return Fail(error, "not a DDS file (bad magic)");
  }

  const unsigned char *const header = bytes + kMagicSize;
  if (ReadU32(header + kOffSize) != kHeaderSize) {
    return Fail(error, "DDS_HEADER.dwSize is not 124");
  }

  const unsigned char *const pf = header + kPixelFormatOffset;
  if (ReadU32(pf + kPfOffSize) != 32) {
    return Fail(error, "DDS_PIXELFORMAT.dwSize is not 32");
  }

  const uint32_t pf_flags = ReadU32(pf + kPfOffFlags);
  out->has_dx10_header = false;

  if ((pf_flags & kDdpfFourCc) != 0) {
    const uint32_t fourcc = ReadU32(pf + kPfOffFourCc);
    if (fourcc == 0x31545844u /* 'DXT1' */) {
      out->format = Format::Dxt1;
    } else if (fourcc == 0x33545844u /* 'DXT3' */) {
      out->format = Format::Dxt3;
    } else if (fourcc == 0x30315844u /* 'DX10' */) {
      // The DX10 extension header carries a DXGI_FORMAT, which can express BC1 and BC2 -
      // but also everything the engine cannot render. Rejecting the whole extension is
      // the conservative reading and costs nothing: no tool needs it to write BC1/BC2.
      return Fail(error,
                  "DDS with a DX10 extension header is not supported; save as legacy "
                  "DXT1 or DXT3");
    } else {
      if (error != nullptr) {
        *error = "unsupported block format '" + FourCcName(fourcc) +
                 "'; Gunlok renders only DXT1 and DXT3";
      }
      return false;
    }
  } else {
    // Uncompressed - the only route to true 24-bit colour, see the note in Dds.h.
    //
    // Only the two channel orders D3D itself uses are accepted. Anything else would
    // need a swizzle in the row converter, and a silently swizzled texture is exactly
    // the class of failure this file refuses by name elsewhere.
    if ((pf_flags & kDdpfRgb) == 0) {
      return Fail(error,
                  "DDS is neither DDPF_FOURCC nor DDPF_RGB; luminance and alpha-only "
                  "formats are not supported");
    }
    const uint32_t bits = ReadU32(pf + kPfOffRgbBitCount);
    const uint32_t rm = ReadU32(pf + kPfOffRMask);
    const uint32_t gm = ReadU32(pf + kPfOffGMask);
    const uint32_t bm = ReadU32(pf + kPfOffBMask);
    const uint32_t am = ReadU32(pf + kPfOffAMask);
    const bool bgr = rm == 0x00ff0000u && gm == 0x0000ff00u && bm == 0x000000ffu;
    const bool has_alpha = (pf_flags & kDdpfAlphaPixels) != 0;
    if (bits == 32 && bgr && (am == 0xff000000u || (!has_alpha && am == 0))) {
      out->format = Format::Argb32;  // A8R8G8B8 / X8R8G8B8: B,G,R,A in memory
    } else if (bits == 24 && bgr) {
      out->format = Format::Rgb24;   // R8G8B8: B,G,R in memory
    } else {
      return Fail(error,
                  "unsupported uncompressed layout; save as 24-bit R8G8B8 or 32-bit "
                  "A8R8G8B8/X8R8G8B8");
    }
  }

  const uint32_t caps2 = ReadU32(header + kOffCaps2);
  if ((caps2 & kCaps2Cubemap) != 0) {
    return Fail(error, "cubemap DDS is not supported");
  }
  if ((caps2 & kCaps2Volume) != 0) {
    return Fail(error, "volume DDS is not supported");
  }

  const uint32_t width = ReadU32(header + kOffWidth);
  const uint32_t height = ReadU32(header + kOffHeight);
  if (width == 0 || height == 0) {
    return Fail(error, "zero width or height");
  }
  // A bound, not a format rule: it keeps the level walk below from running away on a
  // corrupt header, and no texture this engine loads comes near it.
  if (width > 8192 || height > 8192) {
    return Fail(error, "larger than 8192 in one dimension");
  }

  uint32_t declared_levels = 1;
  if ((ReadU32(header + kOffFlags) & kDdsdMipMapCount) != 0) {
    declared_levels = ReadU32(header + kOffMipMapCount);
    if (declared_levels == 0) {
      declared_levels = 1;
    }
  }

  // The chain cannot be longer than halving allows. A header claiming more levels than
  // that is malformed rather than interestingly deep.
  uint32_t possible_levels = 1;
  for (uint32_t w = width, h = height; w > 1 || h > 1; ++possible_levels) {
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  if (declared_levels > possible_levels) {
    return Fail(error, "dwMipMapCount exceeds the number of levels the dimensions allow");
  }

  // The base level's own dimensions must already be aligned - see kDimensionAlignment.
  // Refusing here is the whole reason this is checked at parse time rather than trusted:
  // the engine's failure mode is a runaway write past the locked surface, not an error
  // code.
  if (IsCompressed(out->format) &&
      (width % kDimensionAlignment != 0 || height % kDimensionAlignment != 0)) {
    return Fail(error,
                "a compressed DDS needs both dimensions to be multiples of 4 (the "
                "engine's S3TC row loop cannot terminate otherwise); an uncompressed "
                "one has no such limit");
  }

  out->width = width;
  out->height = height;
  out->declared_levels = declared_levels;
  return true;
}

// Where the pixel data starts, given a parsed header.
size_t PayloadOffset(const HeaderInfo &info) {
  return kMagicSize + kHeaderSize + (info.has_dx10_header ? kDx10HeaderSize : 0);
}

}  // namespace

std::optional<size_t> MeasureFile(const void *header, size_t size, std::string *error) {
  HeaderInfo info{};
  if (!ParseHeader(header, size, &info, error)) {
    return std::nullopt;
  }

  size_t total = PayloadOffset(info);
  unsigned w = info.width;
  unsigned h = info.height;
  for (uint32_t level = 0; level < info.declared_levels; ++level) {
    total += LevelBytes(info.format, w, h);
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  return total;
}

std::optional<Image> Parse(const void *data, size_t size, std::string *error) {
  HeaderInfo info{};
  if (!ParseHeader(data, size, &info, error)) {
    return std::nullopt;
  }

  Image image;
  image.format = info.format;
  image.width = info.width;
  image.height = info.height;
  image.dropped_tail_levels = 0;
  image.levels.reserve(info.declared_levels);

  size_t offset = PayloadOffset(info);

  // Walk every level the header declares - validating the whole file's extent, so a
  // truncated tail is still caught - but publish only the aligned prefix.
  bool still_usable = true;
  unsigned w = info.width;
  unsigned h = info.height;
  for (uint32_t level = 0; level < info.declared_levels; ++level) {
    const size_t level_size = LevelBytes(info.format, w, h);
    if (level_size > size || offset > size - level_size) {
      Fail(error, "truncated: the mip chain runs past the end of the file");
      return std::nullopt;
    }
    if (still_usable && IsCompressed(info.format) &&
        (w % kDimensionAlignment != 0 || h % kDimensionAlignment != 0)) {
      still_usable = false;
    }
    if (still_usable) {
      image.levels.push_back(Level{w, h, offset, level_size});
    } else {
      ++image.dropped_tail_levels;
    }
    offset += level_size;
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }

  return image;
}

}  // namespace gk::dds
