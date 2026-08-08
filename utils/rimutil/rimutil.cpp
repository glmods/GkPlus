// rimutil - convert between Gunlok `.RIM` textures and PNG.
//
//   rimutil decompress <in.RIM> <out.png>
//   rimutil compress   <in.png> <out.RIM> [--format dxt1|dxt3|body] [--raw]
//
// A `.RIM` is IFF (big-endian sizes, 4-character ids, odd bodies padded) carrying
// an ILBM whose image is either an `S3TC` DXT payload or a planar `CMAP`+`BODY`
// pair. `rif_chunk_format.md` has the full format; the parts that decide this
// file's shape:
//
// - **Gunlok reads `BODY` first and only falls back to `S3TC`.** Both are written
//   here. On decode the palettized variant wins for the same reason.
// - **Only DXT1 and DXT3 exist as far as the engine is concerned.** Its candidate
//   texture-format list holds exactly those two, and the setter that receives the
//   fourcc drops anything else *silently* - a DXT5 file renders with garbage alpha
//   rather than failing. So `compress` refuses to emit DXT5 even though squish
//   would happily produce it and `decompressS3tc` below can still read one.
// - **A `BODY` plane row is padded to a byte, not to the word boundary the ILBM
//   spec requires.** Only observable when `ceil(width/8)` is odd, i.e. 8-pixel-wide
//   mip levels, and getting it wrong corrupts exactly those.
// - **`BODY` is exactly lossless**: the palette is built from the distinct colours
//   actually present and `nPlanes` from its size, with no cap worth caring about
//   (the engine passes "no limit" on any true-colour destination and the decoder
//   accumulates into a uint32, so nPlanes up to 31 works). **Lossless on disk is
//   not lossless in the engine**, though: Gunlok ignores the `ALPH` chunk a
//   palettized image has to carry alpha in, so `compress --format body` refuses
//   graded alpha. See `AlphaShape` for the measurement.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <spng.h>
#include <squish.h>

using pos_type = std::istream::pos_type;

struct Chunk;

struct Form {
  uint32_t type;
  std::vector<Chunk> chunks;
};

struct Chunk {
  uint32_t id;
  std::variant<Form, std::vector<char>> body;
};

enum class Masking : uint8_t { None, HasMask, HasTransparentColor, Lasso };

enum class Compression : uint8_t { None, RunLength, S3tc };

#pragma pack(push, 1)
struct BitmapHeader {
  uint16_t width;
  uint16_t height;
  uint16_t xTopLeft;
  uint16_t yTopLeft;
  uint8_t nBitPlanes;
  Masking eMasking;
  Compression eCompression;
  uint8_t flags;
  uint16_t iTranspCol;
  uint8_t xAspectRatio;
  uint8_t yAspectRatio;
  uint16_t xMax;
  uint16_t yMax;
};

struct S3tcData {
  uint32_t flags;
  uint32_t fourCC;
  uint16_t redWeight;
  uint16_t blueWeight;
  uint16_t greenWeight;
  uint16_t width;
  uint16_t height;
  uint32_t dataSize;
  char data[];
};

// `ALPH` is a 6-byte header then a raw blob, and the engine feeds it to the very
// same planar decoder as `BODY` with `bits` standing in for `nPlanes`.
struct AlphaHeader {
  uint16_t width;
  uint16_t height;
  uint8_t bits;
  Compression eCompression;
};
#pragma pack(pop)

// The perceptual channel weights every shipped S3TC chunk carries, per-mille and
// summing to 999. Nothing in the loader reads them; they are copied verbatim so a
// written file is indistinguishable from a shipped one.
static constexpr uint16_t kRedWeight = 309;
static constexpr uint16_t kBlueWeight = 82;
static constexpr uint16_t kGreenWeight = 608;

using spng_context =
    std::unique_ptr<spng_ctx,
                    decltype([](spng_ctx *ctx) { spng_ctx_free(ctx); })>;

template <typename T> static T read_be(T val) {
  if constexpr (std::endian::native == std::endian::big) {
    return val;
  } else {
    return std::byteswap(val);
  }
}

template <auto ReadInt>
static Chunk read_chunk(std::istream &s, pos_type start, pos_type end) {
  uint32_t id;
  s.read(reinterpret_cast<char *>(&id), sizeof(id));
  id = ReadInt(id);

  uint32_t size;
  s.read(reinterpret_cast<char *>(&size), sizeof(size));
  size = ReadInt(size);

  if (size + 8 + start > end) {
    throw std::runtime_error("Invalid IFF file");
  }

  end = size + 8 + start;

  switch (id) {
  case 'FORM':
  case 'LIST':
  case 'CAT ':
  case 'PROP': {
    uint32_t type;
    s.read(reinterpret_cast<char *>(&type), sizeof(type));
    type = ReadInt(type);
    std::vector<Chunk> body;
    while (s.tellg() < end) {
      auto chunk_start = s.tellg();
      body.emplace_back(read_chunk<ReadInt>(s, chunk_start, end));
    }
    return {id, Form{type, body}};
  } break;
  default: {
    std::vector<char> body;
    body.resize(size + size % 2);
    s.read(body.data(), body.size());
    body.resize(size);
    return {id, body};
  }
  }
}

// ---------------------------------------------------------------------------
// IFF writing
// ---------------------------------------------------------------------------

struct ByteWriter {
  std::vector<char> v;

  void u8(uint8_t x) { v.push_back(static_cast<char>(x)); }
  void be16(uint16_t x) {
    u8(static_cast<uint8_t>(x >> 8));
    u8(static_cast<uint8_t>(x));
  }
  void be32(uint32_t x) {
    be16(static_cast<uint16_t>(x >> 16));
    be16(static_cast<uint16_t>(x));
  }
  // A fourcc inside a chunk *body* is stored so that reading it as a native
  // little-endian word yields the multi-character literal -- the S3TC code reads
  // "1TXD" off disk and compares it against 'DXT1'.
  void le32(uint32_t x) {
    u8(static_cast<uint8_t>(x));
    u8(static_cast<uint8_t>(x >> 8));
    u8(static_cast<uint8_t>(x >> 16));
    u8(static_cast<uint8_t>(x >> 24));
  }
  void raw(const void *p, size_t n) {
    auto b = static_cast<const char *>(p);
    v.insert(v.end(), b, b + n);
  }
};

static size_t chunk_body_size(const Chunk &c) {
  if (auto form = std::get_if<Form>(&c.body)) {
    size_t n = 4; // the group type
    for (auto &kid : form->chunks) {
      size_t k = chunk_body_size(kid);
      n += 8 + k + (k % 2);
    }
    return n;
  }
  return std::get<std::vector<char>>(c.body).size();
}

static void write_chunk(ByteWriter &w, const Chunk &c) {
  size_t size = chunk_body_size(c);
  w.be32(c.id);
  w.be32(static_cast<uint32_t>(size));
  if (auto form = std::get_if<Form>(&c.body)) {
    w.be32(form->type);
    for (auto &kid : form->chunks) {
      write_chunk(w, kid);
    }
  } else {
    auto &data = std::get<std::vector<char>>(c.body);
    w.raw(data.data(), data.size());
  }
  if (size % 2) {
    w.u8(0); // IFF pads an odd body to an even boundary
  }
}

// ---------------------------------------------------------------------------
// ByteRun1 (PackBits)
// ---------------------------------------------------------------------------

// n < 0x80 -> literal run of n+1 bytes; n > 0x80 -> repeat the next byte
// 0x101-n times; n == 0x80 -> no-op. The engine decodes one continuous stream
// rather than restarting per scanline, so this does too.
static std::vector<uint8_t> unpack_byterun1(const uint8_t *src, size_t src_len,
                                            size_t want) {
  std::vector<uint8_t> out;
  out.reserve(want);
  size_t i = 0;
  while (out.size() < want) {
    if (i >= src_len) {
      throw std::runtime_error("BODY: ByteRun1 stream ran out of input");
    }
    uint8_t n = src[i++];
    if (n < 0x80) {
      size_t run = static_cast<size_t>(n) + 1;
      if (i + run > src_len) {
        throw std::runtime_error("BODY: ByteRun1 literal run overruns input");
      }
      out.insert(out.end(), src + i, src + i + run);
      i += run;
    } else if (n > 0x80) {
      if (i >= src_len) {
        throw std::runtime_error("BODY: ByteRun1 repeat run overruns input");
      }
      out.insert(out.end(), 0x101 - n, src[i++]);
    }
  }
  return out;
}

static std::vector<uint8_t> pack_byterun1(const std::vector<uint8_t> &src) {
  std::vector<uint8_t> out;
  size_t i = 0;
  while (i < src.size()) {
    size_t run = 1;
    while (i + run < src.size() && run < 128 && src[i + run] == src[i]) {
      ++run;
    }
    if (run >= 2) {
      out.push_back(static_cast<uint8_t>(0x101 - run));
      out.push_back(src[i]);
      i += run;
      continue;
    }
    // A literal run, stopped one short of any 3-in-a-row so the repeat case can
    // take it instead.
    size_t lit = 1;
    while (i + lit < src.size() && lit < 128) {
      if (i + lit + 2 < src.size() && src[i + lit] == src[i + lit + 1] &&
          src[i + lit] == src[i + lit + 2]) {
        break;
      }
      ++lit;
    }
    out.push_back(static_cast<uint8_t>(lit - 1));
    out.insert(out.end(), src.begin() + i, src.begin() + i + lit);
    i += lit;
  }
  return out;
}

// ---------------------------------------------------------------------------
// planar bitplanes
// ---------------------------------------------------------------------------

static size_t plane_row_bytes(uint32_t width) {
  // Padded to a BYTE. The ILBM spec says a word; Gunlok does not, and the
  // 8-pixel-wide mip levels in the shipped set are what prove it.
  return (width + 7) / 8;
}

static std::vector<uint32_t> decode_planar(const std::vector<char> &blob,
                                           uint32_t width, uint32_t height,
                                           uint32_t planes,
                                           Compression compression,
                                           const char *what) {
  if (planes == 0 || planes > 31) {
    throw std::runtime_error(std::string(what) + ": " + std::to_string(planes) +
                             " planes is out of range (1..31)");
  }
  size_t row = plane_row_bytes(width);
  size_t need = row * planes * height;
  auto src = reinterpret_cast<const uint8_t *>(blob.data());

  std::vector<uint8_t> raw;
  const uint8_t *planar;
  if (compression == Compression::None) {
    if (blob.size() < need) {
      throw std::runtime_error(std::string(what) + ": holds " +
                               std::to_string(blob.size()) +
                               " bytes, needs " + std::to_string(need));
    }
    planar = src;
  } else {
    raw = unpack_byterun1(src, blob.size(), need);
    planar = raw.data();
  }

  std::vector<uint32_t> out(static_cast<size_t>(width) * height, 0);
  size_t o = 0;
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t *line = out.data() + static_cast<size_t>(y) * width;
    for (uint32_t p = 0; p < planes; ++p) {
      uint8_t cur = 0;
      for (uint32_t x = 0; x < width; ++x) {
        if ((x & 7) == 0) {
          cur = planar[o++];
        }
        line[x] |= static_cast<uint32_t>((cur >> 7) & 1) << p;
        cur = static_cast<uint8_t>(cur << 1);
      }
    }
  }
  return out;
}

static std::vector<uint8_t> encode_planar(const std::vector<uint32_t> &idx,
                                          uint32_t width, uint32_t height,
                                          uint32_t planes) {
  size_t row = plane_row_bytes(width);
  std::vector<uint8_t> out(row * planes * height, 0);
  size_t o = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t *line = idx.data() + static_cast<size_t>(y) * width;
    for (uint32_t p = 0; p < planes; ++p) {
      for (uint32_t x = 0; x < width; ++x) {
        if ((line[x] >> p) & 1) {
          out[o + (x >> 3)] |= static_cast<uint8_t>(0x80 >> (x & 7));
        }
      }
      o += row;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba; // row 0 at the top, 4 bytes per pixel
};

static int write_png(std::ostream &os, const Image &img) {
  auto spng = spng_context(spng_ctx_new(SPNG_CTX_ENCODER));
  spng_set_option(spng.get(), SPNG_ENCODE_TO_BUFFER, 1);
  spng_ihdr ihdr{
      .width = img.width,
      .height = img.height,
      .bit_depth = 8,
      .color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA,
  };
  spng_set_ihdr(spng.get(), &ihdr);
  int ret = spng_encode_image(spng.get(), img.rgba.data(), img.rgba.size(),
                              SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
  if (ret) {
    std::cerr << "Encoding error: " << spng_strerror(ret) << std::endl;
    return 1;
  }
  size_t png_size;
  void *png_buf = spng_get_png_buffer(spng.get(), &png_size, &ret);
  if (!png_buf) {
    std::cerr << "Encoding error: " << spng_strerror(ret) << std::endl;
    return 1;
  }
  os.write(static_cast<char *>(png_buf), png_size);
  free(png_buf);
  return 0;
}

static Image read_png(const std::string &path) {
  std::ifstream f{path, std::ios::binary | std::ios::ate};
  if (!f) {
    throw std::runtime_error("cannot open " + path);
  }
  auto size = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<char> buf(size);
  f.read(buf.data(), size);

  auto spng = spng_context(spng_ctx_new(0));
  spng_set_crc_action(spng.get(), SPNG_CRC_USE, SPNG_CRC_USE);
  int ret = spng_set_png_buffer(spng.get(), buf.data(), buf.size());
  if (ret) {
    throw std::runtime_error(std::string("PNG: ") + spng_strerror(ret));
  }
  spng_ihdr ihdr{};
  ret = spng_get_ihdr(spng.get(), &ihdr);
  if (ret) {
    throw std::runtime_error(std::string("PNG: ") + spng_strerror(ret));
  }
  size_t out_size = 0;
  ret = spng_decoded_image_size(spng.get(), SPNG_FMT_RGBA8, &out_size);
  if (ret) {
    throw std::runtime_error(std::string("PNG: ") + spng_strerror(ret));
  }
  Image img;
  img.width = ihdr.width;
  img.height = ihdr.height;
  img.rgba.resize(out_size);
  ret = spng_decode_image(spng.get(), img.rgba.data(), out_size, SPNG_FMT_RGBA8,
                          0);
  if (ret) {
    throw std::runtime_error(std::string("PNG: ") + spng_strerror(ret));
  }
  return img;
}

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------

// One candidate image: an ILBM's own chunks, with any PROP:ILBM properties of an
// enclosing LIST folded in. IFF says a PROP supplies shared properties for every
// FORM of that type in the list, and the only shipped ALPH lives in one.
struct Variant {
  const std::vector<char> *bmhd = nullptr;
  const std::vector<char> *cmap = nullptr;
  const std::vector<char> *body = nullptr;
  const std::vector<char> *s3tc = nullptr;
  const std::vector<char> *alph = nullptr;
};

static const std::vector<char> *leaf(const Chunk &c) {
  return std::get_if<std::vector<char>>(&c.body);
}

static void collect_props(const Form &list, Variant &inherited) {
  for (auto &kid : list.chunks) {
    auto form = std::get_if<Form>(&kid.body);
    if (kid.id == 'PROP' && form && form->type == 'ILBM') {
      for (auto &prop : form->chunks) {
        switch (prop.id) {
        case 'BMHD':
          inherited.bmhd = leaf(prop);
          break;
        case 'CMAP':
          inherited.cmap = leaf(prop);
          break;
        case 'ALPH':
          inherited.alph = leaf(prop);
          break;
        default:
          break;
        }
      }
    }
  }
}

// The base image only: FORM:ILBMs reachable without descending into a MIPM,
// since a mip level is the same picture at half size.
static void collect_variants(const Chunk &c, Variant inherited,
                             std::vector<Variant> &out) {
  auto form = std::get_if<Form>(&c.body);
  if (!form) {
    return;
  }
  if (form->type == 'MIPM') {
    return;
  }
  if (c.id == 'LIST' || c.id == 'CAT ') {
    collect_props(*form, inherited);
  }
  if (c.id == 'FORM' && form->type == 'ILBM') {
    Variant v = inherited;
    for (auto &kid : form->chunks) {
      switch (kid.id) {
      case 'BMHD':
        v.bmhd = leaf(kid);
        break;
      case 'CMAP':
        v.cmap = leaf(kid);
        break;
      case 'BODY':
        v.body = leaf(kid);
        break;
      case 'S3TC':
        v.s3tc = leaf(kid);
        break;
      case 'ALPH':
        v.alph = leaf(kid);
        break;
      default:
        break;
      }
    }
    if (v.bmhd && (v.body || v.s3tc)) {
      out.push_back(v);
    }
    return;
  }
  for (auto &kid : form->chunks) {
    collect_variants(kid, inherited, out);
  }
}

static BitmapHeader parse_bmhd(const std::vector<char> &blob) {
  if (blob.size() < sizeof(BitmapHeader)) {
    throw std::runtime_error("Invalid image: BMHD is " +
                             std::to_string(blob.size()) + " bytes");
  }
  BitmapHeader h;
  std::memcpy(&h, blob.data(), sizeof(h));
  h.width = read_be(h.width);
  h.height = read_be(h.height);
  h.xTopLeft = read_be(h.xTopLeft);
  h.yTopLeft = read_be(h.yTopLeft);
  h.iTranspCol = read_be(h.iTranspCol);
  h.xMax = read_be(h.xMax);
  h.yMax = read_be(h.yMax);
  return h;
}

static Image decode_s3tc(const std::vector<char> &blob) {
  if (blob.size() < sizeof(S3tcData)) {
    throw std::runtime_error("Invalid image: wrong S3TC size");
  }
  S3tcData d;
  std::memcpy(&d, blob.data(), sizeof(d));
  uint32_t width = read_be(d.width);
  uint32_t height = read_be(d.height);
  uint32_t data_size = read_be(d.dataSize);
  if (blob.size() < sizeof(S3tcData) + data_size) {
    throw std::runtime_error("Invalid image: wrong S3TC size");
  }
  int flags;
  switch (d.fourCC) {
  case 'DXT1':
    flags = squish::kDxt1;
    break;
  case 'DXT3':
    flags = squish::kDxt3;
    break;
  case 'DXT5':
    // Gunlok cannot load this - it would land in a DXT3 surface and render with
    // garbage alpha - but decoding one costs nothing and says so plainly.
    std::cerr << "Warning: DXT5 payload; Gunlok cannot load this file"
              << std::endl;
    flags = squish::kDxt5;
    break;
  default:
    throw std::runtime_error("Invalid image: unknown S3TC format");
  }
  Image img;
  img.width = width;
  img.height = height;
  img.rgba.resize(static_cast<size_t>(width) * height * 4);
  squish::DecompressImage(img.rgba.data(), width, height,
                          blob.data() + sizeof(S3tcData), flags);
  return img;
}

static Image decode_body(const Variant &v, const BitmapHeader &h) {
  if (!v.cmap) {
    // The engine's own error 9. A BODY without a palette means nothing.
    throw std::runtime_error("Invalid image: BODY with no CMAP");
  }
  size_t colours = v.cmap->size() / 3;
  if (colours == 0) {
    throw std::runtime_error("Invalid image: empty CMAP");
  }
  auto idx = decode_planar(*v.body, h.width, h.height, h.nBitPlanes,
                           h.eCompression, "BODY");

  std::vector<uint32_t> alpha;
  if (v.alph) {
    if (v.alph->size() < sizeof(AlphaHeader)) {
      throw std::runtime_error("Invalid image: ALPH is too small");
    }
    AlphaHeader ah;
    std::memcpy(&ah, v.alph->data(), sizeof(ah));
    uint32_t aw = read_be(ah.width), ahh = read_be(ah.height);
    if (aw == h.width && ahh == h.height) {
      std::vector<char> blob(v.alph->begin() + sizeof(AlphaHeader),
                             v.alph->end());
      alpha = decode_planar(blob, aw, ahh, ah.bits, ah.eCompression, "ALPH");
    } else {
      std::cerr << "Warning: ALPH is " << aw << "x" << ahh << " but the image is "
                << h.width << "x" << h.height << "; ignoring it" << std::endl;
    }
  }

  auto pal = reinterpret_cast<const uint8_t *>(v.cmap->data());
  Image img;
  img.width = h.width;
  img.height = h.height;
  img.rgba.resize(static_cast<size_t>(h.width) * h.height * 4);
  bool clamped = false;
  for (size_t i = 0; i < idx.size(); ++i) {
    uint32_t e = idx[i];
    if (e >= colours) {
      e = 0;
      clamped = true;
    }
    img.rgba[i * 4 + 0] = pal[e * 3 + 0];
    img.rgba[i * 4 + 1] = pal[e * 3 + 1];
    img.rgba[i * 4 + 2] = pal[e * 3 + 2];
    uint8_t a = 255;
    if (!alpha.empty()) {
      a = static_cast<uint8_t>(std::min<uint32_t>(alpha[i], 255));
    } else if (h.eMasking == Masking::HasTransparentColor &&
               idx[i] == h.iTranspCol) {
      a = 0;
    }
    img.rgba[i * 4 + 3] = a;
  }
  if (clamped) {
    std::cerr << "Warning: some BODY indices fell outside the CMAP" << std::endl;
  }
  return img;
}

static int do_decompress(const std::string &in, const std::string &out) {
  std::ifstream f{in, std::ios::binary | std::ios::ate};
  if (!f) {
    std::cerr << "Cannot open " << in << std::endl;
    return 1;
  }
  auto end = f.tellg();
  f.seekg(0);
  auto root = read_chunk<read_be<uint32_t>>(f, f.tellg(), end);

  std::vector<Variant> variants;
  collect_variants(root, Variant{}, variants);
  if (variants.empty()) {
    std::cerr << "Invalid image: no ILBM with a BODY or S3TC" << std::endl;
    return 1;
  }

  // Gunlok looks for BODY first and only falls back to S3TC, and among the
  // palette-depth variants it takes the largest CMAP (its colour cap is "no
  // limit" on any true-colour destination). Match both.
  const Variant *chosen = nullptr;
  size_t best = 0;
  for (auto &v : variants) {
    if (!v.body || !v.cmap) {
      continue;
    }
    if (!chosen || v.cmap->size() > best) {
      chosen = &v;
      best = v.cmap->size();
    }
  }
  if (!chosen) {
    for (auto &v : variants) {
      if (v.s3tc) {
        chosen = &v;
        break;
      }
    }
  }
  if (!chosen) {
    std::cerr << "Invalid image: no usable image chunk" << std::endl;
    return 1;
  }

  BitmapHeader h = parse_bmhd(*chosen->bmhd);
  Image img = chosen->body ? decode_body(*chosen, h) : decode_s3tc(*chosen->s3tc);

  if (chosen->body) {
    std::cout << "BODY " << img.width << "x" << img.height << ", "
              << static_cast<int>(h.nBitPlanes) << " planes, "
              << (chosen->cmap->size() / 3) << " colours, "
              << (h.eCompression == Compression::None ? "raw" : "ByteRun1")
              << (chosen->alph ? ", ALPH" : "")
              << (h.eMasking == Masking::HasTransparentColor
                      ? ", transparent index"
                      : "")
              << std::endl;
  }

  std::ofstream os(out, std::ios::binary);
  if (!os) {
    std::cerr << "Cannot open " << out << std::endl;
    return 1;
  }
  return write_png(os, img);
}

// ---------------------------------------------------------------------------
// encoding
// ---------------------------------------------------------------------------

enum class OutFormat { Dxt1, Dxt3, Body };

static Chunk make_bmhd(uint32_t width, uint32_t height, uint8_t planes,
                       Masking masking, Compression compression,
                       uint16_t transparent) {
  ByteWriter w;
  w.be16(static_cast<uint16_t>(width));
  w.be16(static_cast<uint16_t>(height));
  w.be16(0); // xTopLeft
  w.be16(0); // yTopLeft
  w.u8(planes);
  w.u8(static_cast<uint8_t>(masking));
  w.u8(static_cast<uint8_t>(compression));
  w.u8(0); // flags
  w.be16(transparent);
  w.u8(1); // xAspectRatio
  w.u8(1); // yAspectRatio
  w.be16(static_cast<uint16_t>(width));
  w.be16(static_cast<uint16_t>(height));
  return {'BMHD', w.v};
}

static Chunk make_s3tc(uint32_t fourcc, uint32_t width, uint32_t height,
                       const std::vector<uint8_t> &payload) {
  ByteWriter w;
  w.be32(0);      // flags
  w.le32(fourcc); // stored so a native little-endian read yields 'DXTn'
  w.be16(kRedWeight);
  w.be16(kBlueWeight);
  w.be16(kGreenWeight);
  w.be16(static_cast<uint16_t>(width));
  w.be16(static_cast<uint16_t>(height));
  w.be32(static_cast<uint32_t>(payload.size()));
  w.raw(payload.data(), payload.size());
  return {'S3TC', w.v};
}

// LIST:MIPM { FORM:MIPM { CONT } } with a level count of zero, which is what
// several shipped files carry and what the engine's mip walk skips over.
static Chunk make_empty_mipm() {
  ByteWriter cont;
  cont.u8(0); // level count
  cont.u8(0);
  Chunk form{'FORM', Form{'MIPM', {Chunk{'CONT', cont.v}}}};
  return {'LIST', Form{'MIPM', {form}}};
}

static Chunk make_tran() {
  // Every shipped TRAN is eight zero bytes, and the loader gates its colour key
  // on the first byte being non-zero, so this is the inert form.
  return {'TRAN', std::vector<char>(8, 0)};
}

// How an image's alpha channel has to be carried on the BODY path - which is
// also whether the engine will honour it at all.
//
// **Gunlok ignores `ALPH`.** Measured in the running game: `Units\alpha junk.RIM`
// re-encoded as BODY loses its graded alpha - the front-end menu's selection
// lozenge comes out as its own opaque bounding rectangle instead of a soft blob -
// and re-encoding the *same pixels* with the alpha channel flattened to a uniform
// 128 produces a pixel-identical frame. The chunk never reaches the surface. The
// same PNG encoded `--format dxt3` matches stock exactly, so it is the chunk and
// not the missing mip chain.
//
// **The cause is now known, and it is this program's own bug: the `ALPH` goes in
// the wrong group.** An `ALPH` inside the `FORM:ILBM` is never found by the
// loader - measured in the running game, where it renders bit-for-bit identically
// to a file carrying no `ALPH` at all (mean 0.0000/255, max 0). The identical
// chunk placed in the `PROP:ILBM`, which is where the one shipped `ALPH`
// (`Ground\tree_alpha.RIM`) lives, renders correct graded alpha. So the engine is
// fine and the refusal below is working around a defect on this side of the line.
//
// (An earlier theory blamed `ChooseSurfaceFormatForImage` admitting only
// A8R8G8B8 for alpha bits >= 2 with `Use32BitTextures` off. Also refuted: such an
// image reaches an A8R8G8B8 staging surface and is re-compressed to DXT3.)
//
// Fixing it means emitting the `ALPH` in the `PROP` **and** capping the palette at
// 256 colours: this palettizes losslessly, so `alpha junk` yields 40,742 entries /
// 16 planes, and such a file with a PROP-placed `ALPH` crashes the game inside the
// alpha converter at 0x005df14a. Until both are done the refusal stays, because
// what it currently emits genuinely does not work. See rif_chunk_format.md, "An
// `ALPH` must be a child of `PROP:ILBM`".
//
// So a BODY encode is exactly lossless *on disk* and wrong *in the engine*
// whenever it has to emit an `ALPH`. Graded alpha is therefore refused outright.
// A cut-out needing an `ALPH` (several distinct RGBs under alpha 0) is the same
// construct and presumed equally broken, but that has not been measured, so it
// warns instead - see do_compress.
enum class AlphaShape {
  Opaque,           // no alpha to carry
  TransparentIndex, // one RGB under every transparent texel -> masking 2
  BinaryAlph,       // cut-out, several RGBs underneath -> an ALPH chunk
  GradedAlph,       // partial alpha anywhere -> an ALPH chunk
};

static AlphaShape classify_alpha(const Image &img, uint32_t *transparent_rgb) {
  size_t n = static_cast<size_t>(img.width) * img.height;

  auto rgb_of = [&](size_t i) {
    return (static_cast<uint32_t>(img.rgba[i * 4 + 0]) << 16) |
           (static_cast<uint32_t>(img.rgba[i * 4 + 1]) << 8) |
           static_cast<uint32_t>(img.rgba[i * 4 + 2]);
  };

  bool any_translucent = false, any_transparent = false;
  bool one_transparent_colour = true;
  uint32_t under = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t a = img.rgba[i * 4 + 3];
    if (a == 0) {
      if (!any_transparent) {
        under = rgb_of(i);
      } else if (rgb_of(i) != under) {
        one_transparent_colour = false;
      }
      any_transparent = true;
    } else if (a != 255) {
      any_translucent = true;
    }
  }
  if (transparent_rgb != nullptr) {
    *transparent_rgb = under;
  }

  // masking 2 spends one palette entry on "transparent", so it can only carry a
  // single colour underneath the transparent texels. That is lossless exactly
  // when they all share one, and the RGB under a transparent texel is not a
  // don't-care: bilinear filtering blends it into neighbouring opaque texels,
  // which is where dark halos come from. Anything else takes an ALPH chunk,
  // which keeps all four channels - and which the engine drops on the floor.
  if (any_translucent) {
    return AlphaShape::GradedAlph;
  }
  if (!any_transparent) {
    return AlphaShape::Opaque;
  }
  return one_transparent_colour ? AlphaShape::TransparentIndex
                                : AlphaShape::BinaryAlph;
}

struct Palettized {
  std::vector<uint32_t> idx;
  std::vector<uint8_t> cmap; // 3 bytes per entry
  uint8_t planes = 1;
  Masking masking = Masking::None;
  uint16_t transparent = 0;
  std::vector<uint8_t> alpha; // empty unless the image needs an ALPH chunk
};

static Palettized palettize(const Image &img) {
  size_t n = static_cast<size_t>(img.width) * img.height;

  auto rgb_of = [&](size_t i) {
    return (static_cast<uint32_t>(img.rgba[i * 4 + 0]) << 16) |
           (static_cast<uint32_t>(img.rgba[i * 4 + 1]) << 8) |
           static_cast<uint32_t>(img.rgba[i * 4 + 2]);
  };

  uint32_t transparent_rgb = 0;
  const AlphaShape shape = classify_alpha(img, &transparent_rgb);
  bool use_transparent_index = shape == AlphaShape::TransparentIndex;
  bool use_alph =
      shape == AlphaShape::BinaryAlph || shape == AlphaShape::GradedAlph;

  Palettized out;
  out.idx.resize(n);
  std::unordered_map<uint32_t, uint32_t> seen;
  seen.reserve(n * 2);
  uint32_t next = 0;

  for (size_t i = 0; i < n; ++i) {
    if (use_transparent_index && img.rgba[i * 4 + 3] == 0) {
      out.idx[i] = UINT32_MAX; // patched once the index is known
      continue;
    }
    auto [it, inserted] = seen.emplace(rgb_of(i), next);
    if (inserted) {
      ++next;
      out.cmap.push_back(img.rgba[i * 4 + 0]);
      out.cmap.push_back(img.rgba[i * 4 + 1]);
      out.cmap.push_back(img.rgba[i * 4 + 2]);
    }
    out.idx[i] = it->second;
  }

  if (use_transparent_index) {
    // Its own entry even when an opaque pixel shares the colour, so the two can
    // never collapse onto one index.
    out.transparent = static_cast<uint16_t>(out.cmap.size() / 3);
    out.cmap.push_back(static_cast<uint8_t>(transparent_rgb >> 16));
    out.cmap.push_back(static_cast<uint8_t>(transparent_rgb >> 8));
    out.cmap.push_back(static_cast<uint8_t>(transparent_rgb));
    out.masking = Masking::HasTransparentColor;
    for (auto &e : out.idx) {
      if (e == UINT32_MAX) {
        e = out.transparent;
      }
    }
  }

  size_t colours = out.cmap.size() / 3;
  uint32_t planes = 1;
  while ((1u << planes) < colours) {
    ++planes;
  }
  if (planes > 31) {
    throw std::runtime_error("palette needs more than 31 bitplanes");
  }
  out.planes = static_cast<uint8_t>(planes);

  if (use_alph) {
    out.alpha.resize(n);
    for (size_t i = 0; i < n; ++i) {
      out.alpha[i] = img.rgba[i * 4 + 3];
    }
  }
  return out;
}

// Round-trip guard for the claim in the header comment. A silent quantisation
// bug here is invisible in the output file and only shows up in-game, so the
// encoder checks its own work rather than trusting it.
//
// Note what it cannot see: this proves the FILE round-trips, which is a weaker
// claim than "the engine will show these pixels". A graded-alpha BODY file
// passes this and every test in utils/rimutil/tests and still renders opaque -
// see AlphaShape. That is why the refusal is up in do_compress and not here.
static void check_lossless(const Image &img, const Palettized &pal) {
  size_t n = static_cast<size_t>(img.width) * img.height;
  size_t colours = pal.cmap.size() / 3;
  for (size_t i = 0; i < n; ++i) {
    uint32_t e = pal.idx[i];
    if (e >= colours) {
      throw std::runtime_error("internal: palette index out of range");
    }
    uint8_t a = pal.alpha.empty()
                    ? (pal.masking == Masking::HasTransparentColor &&
                               e == pal.transparent
                           ? 0
                           : 255)
                    : pal.alpha[i];
    if (pal.cmap[e * 3 + 0] != img.rgba[i * 4 + 0] ||
        pal.cmap[e * 3 + 1] != img.rgba[i * 4 + 1] ||
        pal.cmap[e * 3 + 2] != img.rgba[i * 4 + 2] ||
        a != img.rgba[i * 4 + 3]) {
      throw std::runtime_error("internal: palettization is not lossless");
    }
  }
}

static int do_compress(const std::string &in, const std::string &out,
                       OutFormat format, bool rle) {
  Image img = read_png(in);
  if (img.width == 0 || img.height == 0) {
    std::cerr << "Invalid image: " << img.width << "x" << img.height
              << std::endl;
    return 1;
  }
  if (img.width > 0xffff || img.height > 0xffff) {
    std::cerr << "Image is too large for a 16-bit BMHD dimension" << std::endl;
    return 1;
  }

  std::vector<Chunk> ilbm;
  // PROP:ILBM holds the properties shared by every FORM:ILBM beside it. TRAN is
  // always there; an ALPH joins it, because that is the only scope the loader's
  // lookup can see - see the push below.
  std::vector<Chunk> prop{make_tran()};
  if (format == OutFormat::Body) {
    Palettized pal = palettize(img);
    check_lossless(img, pal);

    // An ALPH is delivered correctly now (it goes in the PROP - see below), but
    // the engine's alpha converter cannot survive a wide palette: above 256
    // colours `RimConvertIndexed_Alpha_NoMask` @ 0x005defe0 faults - measured,
    // 0xc0000005 at 0x005df14a, on a 40,742-entry re-encode of
    // `Units\alpha junk.RIM`. The same image at 8 planes renders correctly.
    //
    // This palettizes losslessly and has no quantizer, so for a photographic
    // source that also needs alpha the only honest answer is to refuse. The cap
    // applies ONLY when an ALPH is emitted: a wide palette with no alpha takes
    // the other converter and demonstrably renders.
    const size_t colours = pal.cmap.size() / 3;
    if (!pal.alpha.empty() && colours > 256) {
      std::cerr << "This image needs an ALPH chunk (its alpha cannot be carried\n"
                   "by a transparent palette index), but it palettizes to "
                << colours
                << "\ncolours and the engine's alpha converter crashes above "
                   "256.\nReduce it to 256 colours, or use --format dxt3."
                << std::endl;
      return 1;
    }
    std::vector<uint8_t> planar =
        encode_planar(pal.idx, img.width, img.height, pal.planes);
    Compression compression =
        rle ? Compression::RunLength : Compression::None;
    std::vector<uint8_t> payload =
        rle ? pack_byterun1(planar) : std::move(planar);

    ilbm.push_back(make_bmhd(img.width, img.height, pal.planes, pal.masking,
                             compression, pal.transparent));
    ilbm.push_back({'CMAP', std::vector<char>(pal.cmap.begin(), pal.cmap.end())});
    ilbm.push_back(
        {'BODY', std::vector<char>(payload.begin(), payload.end())});

    if (!pal.alpha.empty()) {
      std::vector<uint8_t> aplanar;
      {
        std::vector<uint32_t> a32(pal.alpha.begin(), pal.alpha.end());
        aplanar = encode_planar(a32, img.width, img.height, 8);
      }
      std::vector<uint8_t> abody =
          rle ? pack_byterun1(aplanar) : std::move(aplanar);
      ByteWriter w;
      w.be16(static_cast<uint16_t>(img.width));
      w.be16(static_cast<uint16_t>(img.height));
      w.u8(8); // bits, which the planar decoder uses as the plane count
      w.u8(static_cast<uint8_t>(compression));
      w.raw(abody.data(), abody.size());
      // **In the PROP, not the FORM.** An ALPH inside the FORM:ILBM is never
      // found by the loader: measured in the running game, such a file renders
      // bit-for-bit identically to one carrying no ALPH at all (mean 0.0000/255,
      // max 0), while the same chunk in the PROP renders correct graded alpha,
      // matching a 50% blend to within 1/255. The one shipped ALPH,
      // `Ground\tree_alpha.RIM`, is in the PROP for the same reason.
      prop.push_back({'ALPH', w.v});
    }

    std::cout << "BODY " << img.width << "x" << img.height << ", "
              << static_cast<int>(pal.planes) << " planes, "
              << (pal.cmap.size() / 3) << " colours, "
              << (rle ? "ByteRun1" : "raw")
              << (pal.alpha.empty() ? "" : ", ALPH")
              << (pal.masking == Masking::HasTransparentColor
                      ? ", transparent index"
                      : "")
              << std::endl;
  } else {
    if (img.width % 4 || img.height % 4) {
      std::cerr << "DXT needs both dimensions to be a multiple of 4; this is "
                << img.width << "x" << img.height << std::endl;
      return 1;
    }
    bool dxt1 = format == OutFormat::Dxt1;
    int flags = (dxt1 ? squish::kDxt1 : squish::kDxt3) |
                squish::kColourClusterFit;
    std::vector<uint8_t> payload(
        squish::GetStorageRequirements(img.width, img.height, flags));
    squish::CompressImage(img.rgba.data(), img.width, img.height,
                          payload.data(), flags);

    // nBitPlanes is never read on the S3TC path (which is why the shipped files
    // carry nonsense in it); the channel depth is the honest thing to write.
    ilbm.push_back(make_bmhd(img.width, img.height, dxt1 ? 24 : 32,
                             Masking::None, Compression::S3tc, 0));
    ilbm.push_back(
        make_s3tc(dxt1 ? 'DXT1' : 'DXT3', img.width, img.height, payload));

    std::cout << (dxt1 ? "DXT1 " : "DXT3 ") << img.width << "x" << img.height
              << ", " << payload.size() << " bytes" << std::endl;
  }

  Chunk root{'LIST',
             Form{'ILBM',
                  {Chunk{'PROP', Form{'ILBM', prop}},
                   Chunk{'FORM', Form{'ILBM', ilbm}}, make_empty_mipm()}}};

  ByteWriter w;
  write_chunk(w, root);
  std::ofstream os(out, std::ios::binary);
  if (!os) {
    std::cerr << "Cannot open " << out << std::endl;
    return 1;
  }
  os.write(w.v.data(), w.v.size());
  return 0;
}

// ---------------------------------------------------------------------------

static void usage() {
  std::cerr
      << "Usage:\n"
         "  rimutil decompress <in.RIM> <out.png>\n"
         "  rimutil compress   <in.png> <out.RIM> [options]\n"
         "\n"
         "compress options:\n"
         "  --format dxt1|dxt3|body   default dxt3\n"
         "  --raw                     BODY only: skip ByteRun1 compression\n"
         "\n"
         "dxt3 is the default because Gunlok accepts only DXT1 and DXT3, and\n"
         "DXT3's RGB is no worse while its 4-bit alpha strictly beats DXT1's\n"
         "1-bit. DXT5 is deliberately not offered: the engine drops the fourcc\n"
         "silently and renders such a file with garbage alpha. body is exactly\n"
         "lossless and needs no DXT compressor, at 2-6x the size - but it is\n"
         "lossless on DISK only: the engine ignores the ALPH chunk a palettized\n"
         "image carries alpha in, so body refuses an image with graded alpha\n"
         "and warns on a cut-out that cannot use a transparent index.\n";
}

int main(int argc, char *argv[]) {
  std::vector<std::string_view> args{argv, argv + argc};

  if (args.size() < 4) {
    usage();
    return 1;
  }

  OutFormat format = OutFormat::Dxt3;
  bool rle = true;
  for (size_t i = 4; i < args.size(); ++i) {
    if (args[i] == "--raw") {
      rle = false;
    } else if (args[i] == "--format" && i + 1 < args.size()) {
      auto v = args[++i];
      if (v == "dxt1") {
        format = OutFormat::Dxt1;
      } else if (v == "dxt3") {
        format = OutFormat::Dxt3;
      } else if (v == "body") {
        format = OutFormat::Body;
      } else if (v == "dxt5") {
        std::cerr << "dxt5 is not supported: Gunlok's texture-format list holds "
                     "only DXT1 and\nDXT3, and the setter that receives the "
                     "fourcc drops anything else silently, so\na DXT5 file "
                     "renders with garbage alpha instead of failing."
                  << std::endl;
        return 1;
      } else {
        std::cerr << "Unknown format: " << v << std::endl;
        return 1;
      }
    } else {
      std::cerr << "Unknown option: " << args[i] << std::endl;
      return 1;
    }
  }

  try {
    if (args[1] == "decompress") {
      return do_decompress(std::string(args[2]), std::string(args[3]));
    }
    if (args[1] == "compress") {
      return do_compress(std::string(args[2]), std::string(args[3]), format,
                         rle);
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  usage();
  return 1;
}
