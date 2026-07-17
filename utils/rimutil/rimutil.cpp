#include <bit>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
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
#pragma pack(pop)

using spng_context =
    std::unique_ptr<spng_ctx,
                    decltype([](spng_ctx *ctx) { spng_ctx_free(ctx); })>;

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

template <typename T> static T read_be(T val) {
  if constexpr (std::endian::native == std::endian::big) {
    return val;
  } else {
    return std::byteswap(val);
  }
}

static int decompressS3tc(std::ostream &os, S3tcData &data) {
  std::vector<squish::u8> out_image;
  out_image.resize(data.width * data.height * 4);
  int flags;
  switch (data.fourCC) {
  case 'DXT1':
    flags = squish::kDxt1;
    break;
  case 'DXT3':
    flags = squish::kDxt3;
    break;
  case 'DXT5':
    flags = squish::kDxt5;
    break;
  default: {
    std::cerr << "Invalid image: uknown S3TC format" << std::endl;
    return 1;
  }
  }
  squish::DecompressImage(out_image.data(), data.width, data.height, data.data,
                          flags);
  auto spng = spng_context(spng_ctx_new(SPNG_CTX_ENCODER));
  spng_set_option(spng.get(), SPNG_ENCODE_TO_BUFFER, 1);
  spng_ihdr ihdr{
      .width = data.width,
      .height = data.height,
      .bit_depth = 8,
      .color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA,
  };
  spng_set_ihdr(spng.get(), &ihdr);
  int ret = spng_encode_image(spng.get(), out_image.data(), out_image.size(),
                              SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
  if (ret) {
    std::cerr << "Encoding error: " << spng_strerror(ret) << std::endl;
    return 1;
  }
  size_t png_size;
  void *png_buf{};

  png_buf = spng_get_png_buffer(spng.get(), &png_size, &ret);

  if (!png_buf) {
    std::cerr << "Encoding error: " << spng_strerror(ret) << std::endl;
    return 1;
  }

  os.write(static_cast<char *>(png_buf), png_size);
  free(png_buf);
  return 0;
}

Form *FindILBM(Chunk &data) {
  auto form = std::get_if<Form>(&data.body);
  if (!form) {
    return nullptr;
  }
  if (data.id == 'FORM' && form->type == 'ILBM') {
    return form;
  }
  for (auto &child : form->chunks) {
    auto child_form = FindILBM(child);
    if (child_form) {
      return child_form;
    }
  }
  return nullptr;
}

int main(int argc, char *argv[]) {
  std::vector<std::string_view> args{argv, argv + argc};

  if (args.size() < 4) {
    std::cerr << "Usage: rimutil <compress|decompress> <input> <output>"
              << std::endl;
    return 1;
  }

  if (args[1] != "decompress") {
    std::cerr << "Compression not implemented yet" << std::endl;
    return 1;
  }

  std::ifstream f{args[2].data(), std::ios::binary | std::ios::ate};
  auto end = f.tellg();
  f.seekg(0);
  auto data = read_chunk<read_be<uint32_t>>(f, f.tellg(), end);
  auto form = FindILBM(data);
  if (!form) {
    std::cerr << "Invalid image" << std::endl;
    return 1;
  }

  std::optional<std::vector<char>> bmhd, s3tc, cmap, body;

  for (auto &subchunk : form->chunks) {
    switch (subchunk.id) {
    case 'BMHD':
      if (bmhd.has_value()) {
        std::cerr << "Invalid image: duplicate BMHD" << std::endl;
        return 1;
      }
      bmhd = std::get<std::vector<char>>(subchunk.body);
      break;
    case 'S3TC':
      if (s3tc.has_value()) {
        std::cerr << "Invalid image: duplicate S3TC" << std::endl;
        return 1;
      }
      s3tc = std::get<std::vector<char>>(subchunk.body);
      break;
    case 'CMAP':
      if (cmap.has_value()) {
        std::cerr << "Invalid image: duplicate CMAP" << std::endl;
        return 1;
      }
      cmap = std::get<std::vector<char>>(subchunk.body);
      break;
    case 'BODY':
      if (body.has_value()) {
        std::cerr << "Invalid image: duplicate BODY" << std::endl;
        return 1;
      }
      body = std::get<std::vector<char>>(subchunk.body);
      break;
    }
  }

  if (!bmhd.has_value()) {
    std::cerr << "Invalid image: missing BMHD" << std::endl;
    return 1;
  }

  if (bmhd->size() != sizeof(BitmapHeader)) {
    std::cerr << "Invalid image: wrong BMHD size" << std::endl;
    return 1;
  }

  BitmapHeader &header = *reinterpret_cast<BitmapHeader *>(bmhd->data());

  switch (header.eCompression) {
  case Compression::S3tc: {
    if (!s3tc.has_value()) {
      std::cerr << "Invalid image: missing S3TC" << std::endl;
      return 1;
    }
    if (s3tc->size() < sizeof(S3tcData)) {
      std::cerr << "Invalid image: wrong S3TC size" << std::endl;
      return 1;
    }
    S3tcData &data = *reinterpret_cast<S3tcData *>(s3tc->data());
    data.width = read_be(data.width);
    data.height = read_be(data.height);
    data.dataSize = read_be(data.dataSize);
    if (s3tc->size() < sizeof(S3tcData) + data.dataSize) {
      std::cerr << "Invalid image: wrong S3TC size" << std::endl;
      return 1;
    }
    std::ofstream os(args[3].data(), std::ios::binary);
    return decompressS3tc(os, data);
  }
  case Compression::RunLength:
    break;
  case Compression::None:
    break;
  }
}