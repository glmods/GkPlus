#pragma once

// A DDS codec registered into Gunlok's own image layer.
//
// This is the game-facing half; `src/Dds.h` is the pure format half. The split is the
// same one `src/FileHooks` and `src/Vfs` have: everything here speaks the engine's ABI
// and can only run inside Gunlok, everything there is testable in a harness.
//
// **This is a registration, not a detour.** The image layer picks its decoder by magic
// bytes through an open registration function - `RegisterImageCodec` @ 0x005c8360 builds
// a 256-way trie, `SniffAndCreateImage` @ 0x005c8a80 walks it longest-prefix and calls
// the matching factory - and nothing anywhere on the texture path reads a file
// extension. So `Ground\ground.dds` in a `BMPNAMES` entry reaches `CreateFileA` verbatim
// and comes back here. `file_io_notes.md` §4 is the measurement.
//
// DDS rather than PNG because it carries pre-compressed DXT blocks and a mip chain,
// which puts it on the engine's S3TC path - the one path that bypasses format selection
// entirely, and the only way to get a mip chain at all, since the engine has no filter
// anywhere in it. See the header comment in `src/Dds.h`.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Dds.h"

namespace gk::image {

// Gunlok's image interface, in vtable order.
//
// Modelled per the repo convention: declaration-ordered pure virtuals, no explicit vtbl
// member, so the implicit vptr occupies 0x00 and the first data member starts at 0x04.
// Unlike every other struct mirror in `src/`, this one is *implemented* rather than
// described - the engine calls these slots on an object we construct - so the signatures
// have to be right in both arity and convention. On the MSVC x86 ABI a virtual member
// function is `__thiscall` with `this` in ECX, which is exactly what the engine expects,
// and slot 0 is the scalar deleting destructor MSVC emits for a virtual `~T()`.
//
// Every arity below is evidenced by the callee's `RET` form; the slots ending in a bare
// `RET` take no stack arguments. A wrong arity here drifts ESP and faults somewhere
// unrelated much later, per the standing warning in CLAUDE.md.
//
// The data layout is the engine's, not ours. `+0x08`/`+0x0c` in particular are read
// *directly* by `FillSurfaceFromImage` @ 0x005c6950 rather than through a slot, so they
// must hold the current mip level's dimensions at all times.
struct EngineImage {
  virtual ~EngineImage() = default;                                       // slot 0
  virtual int GetMaxPaletteColours() = 0;                                 // slot 1
  virtual int GetMinPaletteColours() = 0;                                 // slot 2
  virtual int WantPalettized(int destination_is_palettized) = 0;          // slot 3
  virtual int GetAlphaBits() = 0;                                         // slot 4
  virtual void BindImageChunks(unsigned max_colours) = 0;                 // slot 5
  virtual int GetExtraMipCount() = 0;                                     // slot 6
  virtual int GetPassCount() = 0;                                         // slot 7
  // Slot 8 returns the image's **palette**, not a pass token: the BMP codec's
  // reads the table into the base's +0x24 buffer and returns it, and the base's
  // slot 14 dispatches on that pointer being null to choose a truecolour rather
  // than an indexed converter. (First recorded here as `BeginPass` from its call
  // position; that was wrong.) Null is the correct answer for any source with no
  // palette.
  virtual void *ReadPalette() = 0;                                        // slot 8
  virtual int IsBottomUp() = 0;                                           // slot 9
  virtual void GetSrcRow(void **out, int row) = 0;                        // slot 10
  virtual void GetSrcRow2(void **out, int row) = 0;                       // slot 11
  virtual void OnSrcRow(void *row) = 0;                                   // slot 12
  virtual void OnSrcRow2(void *row) = 0;                                  // slot 13
  virtual void ConvertRows(void *dst, void *unused2, const void *src,     // slot 14
                           const void *src2, int x_offset, int pixels,
                           int selector) = 0;
  virtual void SelectMipLevel(int level, unsigned max_colours) = 0;       // slot 15
  virtual int Slot16() = 0;                                               // slot 16
  virtual void Finalize(int ok) = 0;                                      // slot 17
  virtual int IsS3tc() = 0;                                               // slot 18
  virtual int GetS3tcFourCc() = 0;                                        // slot 19
  // Slot 20 is **`void`, not `int`** - both call sites clobber EAX with the source's
  // status flags on the very next instruction (0x005c7d34 after the slot-20 call,
  // 0x005c7eac after slot 21's). `RimOpenAndScan` confirms it from the other side: its
  // success path falls out with EAX == 0 and its failure path returns whatever
  // `GetLastError` left there, so the shipped implementation follows no return convention
  // at all. Failure is reported by writing `RimLoadErrorCode` instead - see ScanHeader's
  // body. The `int` this was first written as is Ghidra's default for an unread return.
  virtual void ScanHeader(void *source) = 0;                              // slot 20
  virtual void PrepareDecode(int keep_whole_image, int max_colours) = 0;  // slot 21
  virtual void ReleaseScratch(int ok) = 0;                                // slot 22
  virtual EngineImage *DetachDecodedImage() = 0;                          // slot 23

  // --- the engine's own data layout, 0x04..0x30 -----------------------------------
  //
  // There is **no base constructor in the binary** - each shipped factory inlines the
  // initialisation - so nothing here is set for us. `pool_alloc` does not zero, and the
  // shipped factories leave 0x08..0x20 uninitialised until slot 20 and the engine fill
  // them. We zero everything anyway.

  int refcount = 1;              // +0x04 factory sets 1; RunImageCodec decrements
  unsigned width = 0;            // +0x08 CURRENT LEVEL's width; read directly
  unsigned height = 0;           // +0x0c CURRENT LEVEL's height; read directly
  unsigned palette_colours = 0;  // +0x10 0 = true-colour, which is always our case
  unsigned flags = 0;            // +0x14 the request's `f` option; written by the engine
  unsigned mip_skip = 0;         // +0x18 the `m` option (= VramTextureReduction)
  unsigned max_dimension = 0;    // +0x1c the `n` option
  unsigned char destination_is_palettized = 0;  // +0x20 written by ChooseSurfaceFormat
  unsigned char pad21[3] = {};

  // +0x24/+0x28/+0x2c are base-owned decode scratch - a palette buffer, a row-pointer
  // array and a single-row buffer, all allocated by the *base* implementation of slot 21
  // and consumed by the base implementations of slots 10 and 23. We override all three
  // of those and allocate nothing, so these stay null for the object's whole life.
  // `RimImage` repurposes the same three dwords as its chunk-list state, which is legal
  // for exactly the same reason.
  void *base_palette = nullptr;      // +0x24
  void **base_row_pointers = nullptr;  // +0x28
  void *base_row_scratch = nullptr;    // +0x2c
};

// The layout is the whole contract with the engine, so it is asserted rather than
// eyeballed. 0x30 is measured: the two shipped non-RIM factories (BMP 0x48 @ 0x005dd3f0,
// P6 0x38 @ 0x005e0220) each zero exactly +0x24/+0x28/+0x2c and nothing below their own
// fields, and base slots 10, 21 and 23 are the only readers of those three.
static_assert(sizeof(EngineImage) == 0x30, "the engine's image base is 0x30 bytes");
static_assert(offsetof(EngineImage, refcount) == 0x04);
static_assert(offsetof(EngineImage, width) == 0x08);
static_assert(offsetof(EngineImage, height) == 0x0c);
static_assert(offsetof(EngineImage, palette_colours) == 0x10);
static_assert(offsetof(EngineImage, flags) == 0x14);
static_assert(offsetof(EngineImage, mip_skip) == 0x18);
static_assert(offsetof(EngineImage, max_dimension) == 0x1c);
static_assert(offsetof(EngineImage, destination_is_palettized) == 0x20);
static_assert(offsetof(EngineImage, base_palette) == 0x24);
static_assert(offsetof(EngineImage, base_row_pointers) == 0x28);
static_assert(offsetof(EngineImage, base_row_scratch) == 0x2c);

// Registers the DDS codec with the engine's image layer. Idempotent; safe to call more
// than once, though nothing should need to.
//
// **Not callable from DllMain.** `RegisterImageCodec` allocates its trie nodes with the
// game's `pool_alloc`, which bottoms out in gl.exe's *statically linked* CRT heap
// (`AllocateMemory` @ 0x00601f4a is a hot-patch thunk into it), and that heap is
// initialised by gl.exe's `_mainCRTStartup` - which runs *after* the loader has called
// our `DllMain(DLL_PROCESS_ATTACH)`, since we are an implicit-load dependency of the exe.
// The game's own seven codecs register from `.CRT$XC`, i.e. from `_initterm`, which is
// the earliest safe point.
//
// So it is called from `FileHookSystem`'s `CreateFileA` hook, on the first intercepted
// open. That anchor is not arbitrary - it is the one point that is provably both late
// enough and early enough:
//
//   * late enough, because the game only opens a file from `WinMain` onwards, which is
//     past `_initterm` and therefore past heap init;
//   * early enough, because a file must be *opened* before `SniffAndCreateImage` can read
//     a byte of it, so no image can ever be dispatched before the first open has happened.
//
// The obvious alternative - a second detour on `SetupMenus`, next to the script host's -
// was tried and is **wrong**: two `DetourAttach` calls against the same target inside one
// transaction do not chain, and the script host's hook silently stopped running (the REPL
// listener never opened). Do not re-introduce one.
//
// Idempotent, so the per-open cost after the first is a single static bool test. There is
// **no unregistration**: the trie has no removal operation, so a factory pointer into
// this module outlives any detach. Tolerable only because `d3d8.dll` goes away at process
// exit and nothing decodes an image after that.
void RegisterDdsCodec();

// Sets `Use32BitTextures` @ 0x006abde0, which is what makes an uncompressed source land
// on A8R8G8B8 instead of a 4-bit-per-channel surface. Without it a truecolour `.dds`
// gets the DXT3 fallback descriptor and the engine re-compresses it through a staging
// texture - i.e. the whole point of a truecolour import is lost.
//
// **Timing is the entire difficulty.** `PickPreferredTextureFormat` runs exactly once,
// from `InitDirect3DDevice`, and it is what seeds the fallback descriptor from this
// flag; setting it afterwards changes nothing. `WinMain` restores the flag from
// `GLkeys.cfg` once via a `MOVUPS` and never re-runs, so there is no risk of being
// clobbered later. This therefore has to be called after that restore and before the
// device exists - and `Direct3DCreate8` is exactly that window, since the interface it
// returns is what `InitDirect3DDevice` goes on to enumerate formats with.
//
// Safe for shipped content: an S3TC source overrides the candidate list outright, so
// every stock `.RIM` is unaffected. The one global cost is that the flag inflates
// `VramTextureReduction`, a floor under the user's texture-detail setting - worth at
// most one extra skipped mip level, and zero on a card with enough texture memory,
// because that computation early-outs. `GKPLUS_32BIT_TEXTURES=raw` opts out.
void ForceThirtyTwoBitTextures();

}  // namespace gk::image
