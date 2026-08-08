#include "ImageCodec.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "Console.h"
#include "Core.h"

namespace gk::image {
namespace {

// __fastcall void RegisterImageCodec(const char *magic /*ECX*/, Factory /*EDX*/).
// Bare RET at the end of 0x005c8360, so no stack arguments and both go in registers.
using Factory = EngineImage *(__cdecl *)();
FastCall<void, const char *, Factory> RegisterImageCodec;

// The image layer's failure channel, and the only one a codec has: slot 20's return value
// is discarded. `DecodeWithImage` zeroes this immediately *before* calling slot 20
// (0x005c7d21) and tests it immediately after the status-flag ladder (0x005c7e00), where
// a non-zero value aborts the decode and returns NULL - before `ChooseSurfaceFormat` and
// before any surface exists. Each rung of that ladder is guarded by "only if still zero",
// so a code set here is never overwritten.
int *RimLoadErrorCode = nullptr;  // @ 0x00838b0c
int *RimLoadLastError = nullptr;  // @ 0x00838b14

// The shipped vocabulary. `RimOpenAndScan` uses 8 for a structurally bad file and 5 when
// the OS reported something, which is the distinction reproduced here.
enum class LoadError : int {
  Truncated = 6,  // premature EOF / short read
  Malformed = 8,  // the file was readable but is not something we can render
};

void FailLoad(LoadError code) {
  GetObjectAtOffset(RimLoadErrorCode, 0x00838b0c);
  GetObjectAtOffset(RimLoadLastError, 0x00838b14);
  *RimLoadErrorCode = static_cast<int>(code);
  // Mirror the shipped path, so a diagnostic reader does not attribute a stale Win32
  // error from an unrelated call to this failure.
  *RimLoadLastError = 0;
}

// The byte source the image layer hands a codec. Ten slots; only the read side is
// modelled, because that is all a decoder may touch - the write slots are unsupported
// stubs on the memory-backed implementation.
//
// **Only slots 5, 7, 8 and 9 mean the same thing on both implementations.** Slot 1
// (bytes-remaining) returns a hardcoded -1 on the memory source, and slot 3's block read
// sets `*out_avail` to whatever was asked for without clamping and hands back a pointer
// into the caller's buffer - so a "read the whole file" built on either would over-read a
// mod-served file. Slot 7 is the unbuffered bulk read, identical on both, and it is the
// only one used here.
struct ImageSource {
  virtual void *ScalarDeletingDtor(unsigned flags) = 0;      // 0
  virtual int BytesRemaining() = 0;                          // 1  -1 on memory sources
  virtual void *BeginWrite(int *out_len, int len) = 0;       // 2  unsupported on memory
  virtual void *BeginRead(int *out_avail, int want) = 0;     // 3  unclamped on memory
  virtual void EndWrite(unsigned len) = 0;                   // 4  unsupported on memory
  virtual void EndRead(int consumed) = 0;                    // 5
  virtual void WriteRaw(const void *src, unsigned len) = 0;  // 6  unsupported on memory
  virtual void ReadRaw(void *dst, unsigned len) = 0;         // 7  the one we use
  virtual int Tell() = 0;                                    // 8
  virtual void Seek(int absolute_position) = 0;              // 9  absolute, no whence

  unsigned status;  // +0x04
};

// Status bits worth testing after a read. 0x01 is EOF and 0x02 a short read; both map to
// `RimLoadErrorCode = 6` in the engine's own ladder.
//
// These are only set by the **Win32** source. The memory-backed one's `ReadRaw` is an
// unchecked `memcpy` with no length to compare against, so a truncated mod-served file
// cannot be detected this way - which is why every read below is also bounded by what the
// header itself declares.
constexpr unsigned kSourceEof = 0x01;
constexpr unsigned kSourceShortRead = 0x02;

// A DDS image, presented to the engine as if it were the S3TC half of a `.RIM`.
//
// The whole design is "look exactly like `RimImage` on its S3TC path, and nothing like it
// anywhere else". That path is narrow: the engine asks for the block bytes of one block
// row at a time and memcpy's them into the locked surface, and every decision about the
// destination format is taken from the fourcc we report rather than from any candidate
// list. Everything else - palettes, passes, row scratch, bottom-up flipping - is
// inapplicable and is answered with the inert value.
class DdsImage final : public EngineImage {
public:
  // --- format identity: the three slots that put us on the S3TC path ----------------

  // Non-zero here is what makes `ChooseSurfaceFormatForImage` take its S3TC branch,
  // where both arms end at our own fourcc and the candidate list is never consulted.
  int IsS3tc() override { return dds::IsCompressed(dds_.format) ? 1 : 0; }

  int GetS3tcFourCc() override { return static_cast<int>(dds::FourCc(dds_.format)); }

  // For a compressed source this is the engine's own rule for a `.RIM`
  // (`(fourcc == 'DXT1') ? 0 : 8`), and it selects DXT1-vs-DXT3 as the surface.
  //
  // For an **uncompressed** source it is 8 even when the image is fully opaque, and
  // that is load-bearing rather than descriptive: it is what makes every candidate
  // fail `alpha <= maxAlphaBits` so the walk falls through to the 32-bit fallback
  // descriptor instead of settling on a 4-bit one. See the note in src/Dds.h.
  int GetAlphaBits() override { return dds::AlphaBits(dds_.format); }

  // --- palette: we have none --------------------------------------------------------
  //
  // `RimOpenAndScan` leaves max = 0 and min = -1 on its S3TC path, and reports them
  // through these two slots. Matching that exactly is cheaper than reasoning about what
  // the negotiation would do with anything else.
  int GetMaxPaletteColours() override { return 0; }
  int GetMinPaletteColours() override { return -1; }

  // The engine stores this into `+0x20` and the base's row converters branch on it. A
  // block-compressed source is never palettized whatever the destination would prefer,
  // so refuse rather than echoing the argument back the way the base default does.
  int WantPalettized(int /*destination_is_palettized*/) override { return 0; }

  // --- the level chain --------------------------------------------------------------

  // "Extra" levels, not total: the engine creates `n + 1` and walks `level <= n`.
  //
  // `src/Dds.h` has already dropped any level below 4x4, because the engine's S3TC row
  // loop cannot terminate on one. So this is the count of levels that are actually safe,
  // never the count the file declared.
  int GetExtraMipCount() override {
    return static_cast<int>(dds_.levels.size()) - 1;
  }

  // Absolute level index, and by the time this returns the base `width`/`height` fields
  // must describe it - `FillSurfaceFromImage` reads them directly, and errors with code 8
  // if either is zero. Called before each level after the first, never before level 0.
  void SelectMipLevel(int level, unsigned /*max_colours*/) override {
    if (level < 0 || static_cast<size_t>(level) >= dds_.levels.size()) {
      return;  // out of range: leave the previous level in place rather than fault
    }
    current_level_ = static_cast<size_t>(level);
    const dds::Level &lvl = dds_.levels[current_level_];
    width = lvl.width;
    height = lvl.height;
    ExpandLevel();
  }

  // --- the row pull -----------------------------------------------------------------

  // One "row" is a block row: the engine steps its counter by 4 on the S3TC path, so
  // `row` is 0, 4, 8 ... and each call must produce the blocks for that band.
  void GetSrcRow(void **out, int row) override {
    *out = nullptr;
    if (current_level_ >= dds_.levels.size() || row < 0) {
      return;
    }
    const dds::Level &lvl = dds_.levels[current_level_];
    if (!dds::IsCompressed(dds_.format)) {
      // One row of pixels, from the expanded 32-bit copy. The engine steps `row` by
      // 1 here, not 4 - the stride-of-4 walk is the S3TC branch only.
      const size_t stride = static_cast<size_t>(lvl.width) * 4;
      const size_t offset = static_cast<size_t>(row) * stride;
      if (offset + stride > expanded_.size()) {
        return;
      }
      *out = expanded_.data() + offset;
      return;
    }
    const size_t block_row = static_cast<size_t>(row) / 4;
    const size_t stride = BlockRowBytes(lvl);
    const size_t offset = block_row * stride;
    if (offset >= lvl.size) {
      return;  // past the end of the level; the engine would be over-reading
    }
    *out = file_.data() + lvl.offset + offset;
  }

  // The second plane exists for the uncompressed converters (a separate alpha source).
  // There is none here, and the S3TC copy ignores it.
  void GetSrcRow2(void **out, int /*row*/) override { *out = nullptr; }

  // Handed back the two pointers above. `RimImage` makes both of these bare returns.
  void OnSrcRow(void * /*row*/) override {}
  void OnSrcRow2(void * /*row*/) override {}

  // The copy itself, and the one place the block arithmetic has to be exactly the
  // engine's. `RimConvertRows`' S3TC body is:
  //
  //     src += (bits_per_pixel_column * x_offset) / 8
  //     memcpy(dst, src, (bits_per_pixel_column * pixels) / 8)
  //
  // where the engine derives `bits_per_pixel_column` from the S3TC payload size as
  // `((payload * 4 / height) * 8) / width`. For any DXT that reduces to a constant -
  // 16 for DXT1's 8-byte block, 32 for DXT3's 16-byte block - because a block row is
  // `(width / 4) * block_bytes`. Deriving it rather than hardcoding 16/32 keeps the two
  // halves of the arithmetic visibly the same quantity.
  //
  // `unused2` and `selector` are pushed at every call site (the arity is fixed by the
  // callee's `RET 0x1c`) but neither is read on the S3TC path.
  void ConvertRows(void *dst, void * /*unused2*/, const void *src,
                   const void * /*src2*/, int x_offset, int pixels,
                   int /*selector*/) override {
    if (dst == nullptr || src == nullptr || pixels <= 0 || x_offset < 0) {
      return;
    }
    // Uncompressed is the simple case: source and destination are both B,G,R,A, so a
    // row is 4 bytes per pixel either way and the copy is direct. That equality is
    // exactly what reporting alpha_bits = 8 buys - without it the destination would be
    // a packed 16-bit format and this would have to quantise.
    if (!dds::IsCompressed(dds_.format)) {
      const size_t skip = static_cast<size_t>(x_offset) * 4;
      const size_t length = static_cast<size_t>(pixels) * 4;
      std::memcpy(dst, static_cast<const unsigned char *>(src) + skip, length);
      return;
    }

    const unsigned bits = BitsPerPixelColumn();
    const size_t skip = (static_cast<size_t>(bits) * static_cast<size_t>(x_offset)) / 8;
    const size_t length = (static_cast<size_t>(bits) * static_cast<size_t>(pixels)) / 8;
    std::memcpy(dst, static_cast<const unsigned char *>(src) + skip, length);
  }

  // --- the inert remainder ----------------------------------------------------------

  // One pass, one sweep of the rows.
  int GetPassCount() override { return 1; }

  // No palette: DXT blocks carry their own colour. Returning null is what tells the
  // base's converter dispatch this is a truecolour source - and our own ConvertRows
  // ignores the argument entirely, so it is doubly inert.
  void *ReadPalette() override { return nullptr; }

  // Top-down. The S3TC branch advances the row index by 4 unconditionally and ignores
  // this, but a truthful answer costs nothing.
  int IsBottomUp() override { return 0; }

  // `RimImage` binds its IFF chunks here and derives its pitch. Our state is already
  // complete after ScanHeader and SelectMipLevel, so there is nothing to bind.
  void BindImageChunks(unsigned /*max_colours*/) override {}

  // The base implementation allocates a palette buffer, a row-pointer array and a row
  // scratch buffer into +0x24/+0x28/+0x2c. We serve blocks straight out of `file_` and
  // override every consumer of those three, so we allocate nothing and they stay null.
  void PrepareDecode(int /*keep_whole_image*/, int /*max_colours*/) override {}
  void ReleaseScratch(int /*ok*/) override {}

  void Finalize(int /*ok*/) override {}
  int Slot16() override { return 0; }

  // Paired with the request's `B` option, not with a Release: the engine writes the
  // result into `*(void**)(req + 0x30)` and the caller then owns it, releasing it through
  // slot 0. Returning `this` with a bumped refcount is what `RimImage` does, and it is
  // what keeps the object alive past `RunImageCodec`'s decrement. Only `LoadRimFile2`
  // passes `B` today, but it is reachable.
  EngineImage *DetachDecodedImage() override {
    ++refcount;
    return this;
  }

  void ScanHeader(void *source) override;

private:
  size_t BlockRowBytes(const dds::Level &level) const {
    const size_t blocks_x = level.width < 4 ? 1 : level.width / 4;
    return blocks_x * dds::BlockBytes(dds_.format);
  }

  unsigned BitsPerPixelColumn() const {
    // (block_bytes * 8) / 4 pixels per block column.
    return dds::BlockBytes(dds_.format) * 2;
  }

  // Expands the current level to B,G,R,A, which is the destination surface's own
  // layout. A 32-bit source is already in it and only needs copying; a 24-bit one
  // gains an opaque alpha byte. Done per level rather than for the whole chain so the
  // peak cost is one level, and it is a no-op for a compressed source.
  void ExpandLevel() {
    expanded_.clear();
    if (dds::IsCompressed(dds_.format) || current_level_ >= dds_.levels.size()) {
      return;
    }
    const dds::Level &lvl = dds_.levels[current_level_];
    const size_t pixels = static_cast<size_t>(lvl.width) * lvl.height;
    const unsigned src_bpp = dds::SourceBytesPerPixel(dds_.format);
    if (lvl.offset + pixels * src_bpp > file_.size()) {
      return;
    }
    expanded_.resize(pixels * 4);
    const unsigned char *src = file_.data() + lvl.offset;
    unsigned char *dst = expanded_.data();
    for (size_t i = 0; i < pixels; ++i, src += src_bpp, dst += 4) {
      dst[0] = src[0];
      dst[1] = src[1];
      dst[2] = src[2];
      dst[3] = src_bpp == 4 ? src[3] : 0xff;
    }
  }

  std::vector<unsigned char> file_;
  std::vector<unsigned char> expanded_;
  dds::Image dds_{};
  size_t current_level_ = 0;
};

// Read the file and take a private copy of it.
//
// Two reads, no length query, no seeking: the header first, then exactly the payload it
// describes. That shape is forced by the byte source rather than chosen - there is no way
// to ask a memory-backed source how long it is (slot 1 answers -1) and no SEEK_END to
// discover it with, so the only safe bound is the one the format states about itself.
//
// The copy is not an optimisation, it is required: `RunImageCodec` drops the source as
// soon as `DecodeWithImage` returns, and `ConvertRows` runs long after that.
//
// **The explicit `Seek(0)` is required, not defensive.** `SniffAndCreateImage` matches the
// magic through slot 3 (`BeginRead`), the *buffered* block read, which pulls 1024 bytes
// and advances the underlying file pointer by all of them while the logical position only
// moves at `EndRead`. It then rewinds with Tell/Seek, which leaves those two consistent
// with each other but **not** with the raw pointer that slot 7 (`ReadRaw`) reads from.
//
// Measured in the running game: on entry `Tell()` reports 0, yet a `ReadRaw` returns bytes
// from offset 128 - and after `Seek(0)`, `Tell()` reports **-1024**, which is the block
// count still sitting in `+0x30`. So the seek is what puts the raw pointer where the
// logical position claims to be. Without it the first four bytes read are payload, the
// magic check fails, and the texture silently does not load.
//
// This is the slot-3/slot-7 mixing hazard from `file_io_notes.md` §4, reached without
// mixing them ourselves: the engine already used slot 3 before handing the object over.
void DdsImage::ScanHeader(void *source) {
  auto *src = static_cast<ImageSource *>(source);

  file_.assign(dds::kHeaderBytes, 0);
  src->Seek(0);
  src->ReadRaw(file_.data(), static_cast<unsigned>(file_.size()));
  if ((src->status & (kSourceEof | kSourceShortRead)) != 0) {
    Print("gkplus: .dds short read on the header");
    FailLoad(LoadError::Truncated);
    return;
  }

  std::string error;
  const std::optional<size_t> total =
      dds::MeasureFile(file_.data(), file_.size(), &error);
  if (!total.has_value()) {
    DebugWrite("GkPlus: rejected a .dds - {}\n", error);
    FailLoad(LoadError::Malformed);
    return;
  }

  if (*total > dds::kHeaderBytes) {
    file_.resize(*total);
    src->ReadRaw(file_.data() + dds::kHeaderBytes,
                 static_cast<unsigned>(*total - dds::kHeaderBytes));
    if ((src->status & (kSourceEof | kSourceShortRead)) != 0) {
      Print("gkplus: .dds short read on the payload");
      FailLoad(LoadError::Truncated);
      return;
    }
  }

  // Re-parse the whole thing rather than trusting the measurement: this is what validates
  // every level's extent and drops the sub-4x4 tail the engine's row loop cannot survive.
  std::optional<dds::Image> parsed = dds::Parse(file_.data(), file_.size(), &error);
  if (!parsed.has_value() || parsed->levels.empty()) {
    DebugWrite("GkPlus: rejected a .dds - {}\n", error);
    FailLoad(LoadError::Malformed);
    return;
  }

  dds_ = std::move(*parsed);
  current_level_ = 0;
  ExpandLevel();

  // The base fields the engine reads *directly* rather than through a slot. They describe
  // the current level, so SelectMipLevel keeps them in step from here on.
  width = dds_.levels[0].width;
  height = dds_.levels[0].height;
  palette_colours = 0;

  if (dds_.dropped_tail_levels != 0) {
    DebugWrite("GkPlus: .dds mip chain truncated at 4x4, dropping {} level(s)\n",
               dds_.dropped_tail_levels);
  }
}

EngineImage *__cdecl CreateDdsImage() {
  // nothrow: this returns straight into the engine, and an exception crossing that
  // boundary would unwind through frames with no handler.
  return new (std::nothrow) DdsImage();
}

}  // namespace

void ForceThirtyTwoBitTextures() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;

  // _dupenv_s is the "secure" spelling; getenv is what the rest of this codebase
  // uses for its GKPLUS_* knobs, so keep it consistent and silence the warning.
  const char *mode = std::getenv("GKPLUS_32BIT_TEXTURES");  // NOLINT
  if (mode != nullptr && std::strcmp(mode, "raw") == 0) {
    DebugWrite("gkplus: leaving Use32BitTextures alone (GKPLUS_32BIT_TEXTURES=raw)\n");
    return;
  }

  int *use32 = nullptr;
  GetObjectAtOffset(use32, 0x006abde0);
  if (*use32 == 0) {
    *use32 = 1;
    DebugWrite("gkplus: Use32BitTextures forced on, so a truecolour .dds gets A8R8G8B8\n");
  }
}

void RegisterDdsCodec() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;

  GetObjectAtOffset(RegisterImageCodec, 0x005c8360);
  RegisterImageCodec(dds::kMagic, &CreateDdsImage);
  DebugWrite("GkPlus: registered the DDS image codec\n");
}

}  // namespace gk::image
