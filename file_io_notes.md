# File I/O notes — every way Gunlok opens a file

Written to answer one question: can a mod-loading system serve the game's assets out of
archives, without touching the base install? Everything here is measured against the
Ghidra DB, not inferred from the notes.

The short answer is in §6. The reason it is yes is §1: **the game reads whole files
through the import table, synchronously, on one thread.**

## 1. The shape of the I/O, in four measurements

- **Everything goes through the IAT.** Every call to `CreateFileA`, `ReadFile`,
  `SetFilePointer`, … is `CALL dword ptr [<slot>]`, or `MOV reg,[<slot>]` + `CALL reg`.
  Both forms read the slot at run time, so **one 4-byte write per slot intercepts every
  call site**, including the register-cached ones. Slot table in §5.

  That property also covers a gap in the analysis, which is worth keeping: **enumerating
  the sites by symbol reference under-counts.** Ghidra resolves most `CALL dword ptr
  [0x0064d068]` references to the external `CreateFileA` symbol but leaves some as bare
  data references to the slot - `File_Chunk`'s own call at 0x005aff71 is one, so a sweep
  over `getReferencesTo(CreateFileA)` reports 24 functions and misses the single most
  important one. Scan the *disassembly* for the slot address instead: 31 mentions, 30
  calls plus one `MOV ESI,[slot]` in `StartAttractModeDemo`. Patching the slot needs neither
  count to be right.
- **No asynchronous I/O anywhere.** All 31 `CreateFileA` sites were swept for
  `dwFlagsAndAttributes`: the only values are `0`, `0x80` (NORMAL), `0x8000080`
  (SEQUENTIAL_SCAN) and `0x10000000` (RANDOM_ACCESS). `FILE_FLAG_OVERLAPPED` appears at
  **zero** sites. (`0x40000000` shows up a lot, but as `GENERIC_WRITE` in
  `dwDesiredAccess`.) No `LockFile`, no `DuplicateHandle`, no
  `GetFileInformationByHandle` — none of the three is even imported.
- **Every open is either `OPEN_EXISTING` or `CREATE_ALWAYS`** — 21 and 9 of the 30 call
  sites, with no `OPEN_ALWAYS`, `CREATE_NEW` or `TRUNCATE_EXISTING` anywhere. So the
  disposition alone separates every read from every write, which is what lets an
  interception be sure it is never standing in front of one.
- **All file I/O is on the main thread.** A transitive callee closure from
  `ExecutorThreadProc` @ 0x00509050 visits 452 functions and reaches **neither**
  `CreateFileA` nor `fopen`/`freopen`; `File_Chunk`, `LoadOrGetRifFile`, `LoadRimFile`,
  `LoadDatFile`, `LoadGLS`, `ExecuteCommandFile` and even
  `SetCurrentDirectoryToGLDir` are all outside it. Caveat: the sweep follows direct
  calls, not virtual dispatch, so treat it as strong evidence rather than proof.
  It is corroborated by the fact that every loader chdirs, and the CWD is per-process.
- **The big asset formats are whole-file reads.** `File_Chunk` (.rif) and
  `SoundSample_LoadFile` do `GetFileSize` → `malloc` → **one** `ReadFile` → `CloseHandle`,
  and parse the buffer in memory. Nothing about them needs a real OS handle.
  **This is true and it is not the shape of the cost** — see §1.1, which was written
  after reading exactly this line and concluding the read path was fine.

### 1.1 A level load is bound by the number of reads, not by their size

Measured with `mods.read_stats()` (`GKPLUS_FILE_STATS=1`), one warm level02 load:

| bucket | calls |
|---|---|
| exactly 4 bytes | **39,386** |
| 1024 bytes | 4,751 |
| 8 KB and over | 350 |
| everything else | ~390 |

44,879 calls for 23 MB. The three call sites behind the 4-byte reads are gl.exe RVAs
`0x50112`, `0x50133` and `0x50163` — all inside `LoadOrBuildSectionAdjacency`
@ 0x0044fef0, whose known interior offsets `+0x273`/`+0x307` appear in
`game_defects_notes.md` §5. It reads the whole `<level>.map` adjacency cache **one
32-bit integer per `ReadFile` call**, and the arithmetic is exact: 39,364 of those
reads × 4 = 157,456 bytes = `level02.map` to the byte. The other 22 belong to a
different site.

It scales with the cache, so it is worst on the big levels: level10 (598 KB `.map`)
issues 153,419 reads, level12 (731 KB) issues 187,313.

At the ~4 µs a read syscall costs against the OS cache that is 600 ms of a 1.1 s
load — half the wall clock, for 157 KB of data. The 1024-byte family is the image
layer's buffered block read (§4), and it is second by the same logic.

**So the engine is syscall-bound on a warm load, and the bytes are irrelevant.** A
sampled profile says the same thing from the other end: ~50% of a load's samples are
in ntdll, under `HookedReadFile`.

### 1.2 The read-ahead layer

`src/FileHooks.cpp` therefore buffers. A handle **this layer opened** for reading
gets a 64 KB read-ahead buffer, and the file position becomes *ours* — the real
handle is seeked only on a buffer miss. Keeping the OS position in step instead
would cost a `SetFilePointer` per read and buy nothing, which is the whole reason
the position has to move into this layer.

Owning the position is only safe because this layer owns every API that can move
it: gl.exe imports no `SetFilePointerEx` and no `ReadFileEx`, uses no overlapped
I/O at any of its 31 `CreateFileA` sites (§1, §5), and a handle opened anywhere
else — including the statically linked CRT's, which go through `CreateFileW` — is
never buffered and keeps the stock path.

Measured, d3d8 renderer, warm, `GKPLUS_FILE_BUFFER=raw` against the default:

| level | reads before | reads after | blocked ms before | after |
|---|---|---|---|---|
| level02 | 46,090 | 636 | 324 | 237 |
| level10 | 153,419 | 515 | 614 | 314 |
| level12 | 187,313 | 478 | 625 | 267 |

Bytes read move by under 7% either way, which is the check that the access pattern
really is sequential — a random one would have paid 16× for every 4-byte read.

Three things pin its correctness, and the screenshot is the weakest of them:

- The **cold** path rebuilds `level04.cut` and `level04.map` **byte-identical** to
  the references (SHA256), so the adjacency build and the `.cut` serialiser see the
  same bytes through the buffer that they saw without it.
- A handle entry is **overwritten, never `try_emplace`d**. Windows recycles handle
  values, and an entry left by a close this layer did not see would otherwise be
  kept and the new file read from the old file's position.
- A settled level02 screenshot differs from the unbuffered one by 0.88% of channel
  samples — and **two runs at the same setting differ by 2.17%**, so the A/B is
  inside the run-to-run animation-phase noise rather than clean. Camera and actor
  counts are identical. Take the byte-exact sidecar rebuild as the real evidence.

## 2. Paths are relative, and the CWD names the category

`SetCurrentDirectoryToGLDir(GLDir)` @ 0x00466b80 has **53 references**. The pattern at
every loader is chdir → open with a bare relative name → `SetCurrentDirectory()`
@ 0x00466b90 to restore (which restores `CurrentDirectory`, captured once by `LoadGLDirs`
@ 0x00466b30 before it parses `gldirs.gls`).

`GLDir` has seven values: `GL_Scripts` 0, `GL_FMVs` 1, `GL_RIFs` 2, `GL_Graphics` 3,
`GL_Sounds` 4, `GL_Fonts` 5, `GL_Dumps` 6. The directories themselves come from
`gldirs.gls` via `ParseGLDirs` @ 0x00466c20, so they are data, not constants.

Consequences for anything intercepting an open:

- The string reaching `CreateFileA` is usually **relative** (`Units\bug.rif`), so a hook
  must resolve it against the current directory to know what was asked for.
- The current directory is itself the **category**, which is a free mapping onto archive
  mount points.
- **Separators and case are inconsistent in the shipped data.** The binary's own literals
  include `objects/tf_flag.rif` and `User Interface/Main Menu.RIF` (forward slash,
  mixed case) beside `bitmaps\water.rim` and `units\plates 2 1024.rim`. Any lookup layer
  has to fold both.

## 3. The read sites, classified

Win32 handle family. 24 functions call `CreateFileA`; two of them (`D3DX_MapFileForRead`,
`D3DX_CreateFileForWrite`) are **statically-linked D3DX** (that region also contains libpng and
zlib strings) and two (`File_OpenForRead`, `File_OpenForWrite`) have no callers at all.

| What | Function | Notes |
|---|---|---|
| **.rif** | `File_Chunk::File_Chunk` @ 0x005afeb0 | the one entry point; whole-file read. Wrapper `BuildRifFileObject` @ 0x005a9b50 has 7 callers, plus `RifCacheEntry_Load` @ 0x004af2b0 |
| **.rif (nested)** | `SUBRIFFL_Chunk` @ 0x005b0ec0 | same shape |
| **.rim / images** | `ImageFile_Read` @ 0x005c6850 | **already has a memory-source path** — see §4 |
| **sound samples** | `SoundSample_LoadFile` @ 0x005d3740, `SoundSample_ReadWholeFile` @ 0x005d3940 | whole-file read; reached from `SoundSystem_GetOrLoadSample` @ 0x0058bdb0 / `SoundSystem_LoadSampleIntoSlot` @ 0x00589b30 |
| **.dat sound banks** | `DatFile_Ctor` @ 0x0058d0d0 ← `LoadDatFile` @ 0x0058c2a0 | `FILECHNK` container |
| **level rif locators** | `AcquireLevelRifForLocators` @ 0x00483da0 | |
| level sidecar caches | `ToMap` @ 0x0047f160, `LoadOrBuildSectionAdjacency` @ 0x0044fef0, `RewriteCacheTailWithChunk` @ 0x00579070, `BakeStaticShadows` @ 0x005542c0 | read **and** write (`.cut` / `.map` / `.opt`) |
| rif `.opt` recompress | `File_Chunk_WriteFile` @ 0x005b03b0, `RewriteCacheTailWithChunk` @ 0x00579070 | write, then `DeleteFileA` + `MoveFileA` |
| rif vs `.opt` freshness | `IsFirstFileNewer` @ 0x004af430 | the only `GetFileTime` consumer besides `ToMap` |
| savegames | `SaveGame`, `LoadGame`, `PeekSaveGameScriptName`, `MenuLoadGame`, `ReadSaveGameHeader` @ 0x0056cb00, `Unit_RestoreFromSave` @ 0x004b8d90 | keep on the real filesystem |
| demos | `LoadDemoFile`, `CommandSaveDemo`, `StartAttractModeDemo` @ 0x0046c170 | |

CRT `fopen` family — the game statically links a **UCRT** (`__acrt_*`,
`common_sopen_dispatch<char>`, `___stdio_common_vfprintf` are all present), so `_fopen`
@ 0x005f067e is the game's own private copy and hooking it cannot affect GkPlus's `/MD`
runtime. Seven callers, and only six of them read:

| What | Function | Already reachable from GkPlus? |
|---|---|---|
| **.gls** | `LoadGLS` | yes — `gls::ParseSource` feeds the parser from memory |
| **.gsh** | `ParseGSH` | yes — same parser, same seam |
| **.gcs / batch** | `ExecuteCommandFile` @ 0x0043f250 | yes — already detoured by `ScriptQueueSystem` |
| cutscene camera tracks | `CameraTrack_LoadFromCutscene` | no |
| `credits.mca` | `LoadCreditsScript` @ 0x004dbdc0 | no |
| `GLkeys.cfg` | `ReadGLKeys` / `WriteGLKeys` | config — leave alone |
| screenshots, logs | `OpenNextScreenshotFile` @ 0x005a5f40, `LogString`, `AppendLogLine` @ 0x0058ced0 | writes — leave alone |

No `_access`, `_stat`, `_findfirst`, `_wfopen` or `tmpfile` in game code: the game never
probes for a file except by opening it (the one exception is
`GetFileAttributesA` in `File_Chunk_WriteFile`).

Directory enumeration is **one function**: `EnumerateFilesIntoFileList` @ 0x004e6ef0
(`FindFirstFileA`/`FindNextFileA`, node 0x14c bytes), reached only through
`RefreshFileList` @ 0x004e6840 — save/demo/multiplayer-level lists.

## 4. The two seams the game already has, and the two it does not

The engine independently reinvented "read this from memory instead" twice, and both are
usable:

- **The GLS parser** takes its input through a `{dtor, Read, GetFileName}` source object
  (vtbl @ 0x00652904). Documented in `src/GLS.h`; `gls::ParseSource` is the wrapper.
- **The image loader** dispatches in two stages. `ImageFile_Read` @ 0x005c6850 tests
  `req->name` (+0x04) first: non-null means `CreateFileA` into `req->handle`, null means a
  tail-jump to `ImageFile_DecodeFromSource` @ 0x005c84a0, which is where the
  `handle == INVALID_HANDLE_VALUE` test lives. With that and a non-null `req->memory`
  (+0x0c) it builds a memory-backed byte source (`ImageSource_Memory_vtbl` @ 0x0066efe0)
  instead of the Win32 one (`ImageSource_Win32Handle_vtbl` @ 0x0066efb8) and decodes from
  the buffer. `req->+0x10` is a third path: a pre-built image object, skipping all I/O.
  Both sources implement the same **10-slot** vtable — the tables at 0x0066ef90 (the abstract
  base, `ImageSource_vtbl_abstract`), 0x0066efb8, 0x0066efe0 and 0x0066f008 are spaced 0x28
  apart — with read at slot 3, tell at 8 and seek at 9;
  bodies in 0x005dc300..0x005dc9f0. (An earlier revision of this section said the dispatch
  was on `req->handle` and the vtable was 8 slots. Both were wrong.)

  The request itself is `ImageLoadRequest`, 0x5c bytes, filled by a printf-style option
  parser @ 0x005c9d00 from the varargs of `ImageFile_Load` @ 0x005c6720 (`__cdecl`, option
  string first, four call sites). The options: `s` name +0x04, `h` handle +0x08 (default
  -1), `p` memory +0x0c, `r` pre-built image +0x10, `N` bytes-consumed out +0x18, `f` flags
  +0x1c, `W`/`H` created-surface dims out +0x20/+0x24, `OX`/`OY` source dims out
  +0x28/+0x2c, `B` image-object out +0x30, `t` existing target texture +0x34, `c`
  callback+context +0x3c, `a` count+array of 0x1c-byte sub-image records +0x44, `m`/`n`
  dimension caps +0x50/+0x54, `A` destination alpha bits out +0x58.

### The image layer picks its decoder by magic bytes, and the registry is open

Not a seam so much as a front door. `RegisterImageCodec` @ 0x005c8360
(`__fastcall void(const char *magic /*ECX*/, Image *(*factory)() /*EDX*/)`, bare `RET`)
inserts a NUL-terminated magic string into a 256-way trie rooted at 0x00838c58 — one
`pool_alloc(0x408)` node per byte, 256 child dwords plus the factory at +0x400.
`SniffAndCreateImage` @ 0x005c8a80 (`__fastcall Image *(ImageSource * /*ECX*/)`) pulls
bytes one at a time through slot 3, walks the trie keeping the **last** factory it saw
(longest prefix wins), rewinds with tell/seek, and calls the factory with **no arguments**.
No match sets `RimLoadErrorCode = 7`.

Seven codecs are registered, from tail-jump thunks whose addresses sit in the C++ static-init
table at 0x0064dbe8..0x0064dc04 — which is the **tail** of the MSVC static-initializer array at
0x0064d38c-0x0064dc47, **559 entries** bounded by `__xc_a` @ 0x0064d388 and `__xc_z` @ 0x0064dc48;
the seven codec thunks are entries 535-542 of it. Each is three instructions —
`MOV EDX,factory ; MOV ECX,magic ; JMP 0x005c8360`:

```
0x0043c520  "BM"     -> 0x005dd3f0      0x0043c590  "P4"   -> 0x005e02c0
0x0043c560  "FORM"   -> 0x005de290      0x0043c5a0  "P5"   -> 0x005e0270
0x0043c570  "LIST"   -> 0x005de300      0x0043c5b0  "P6"   -> 0x005e0220
0x0043c580  "CAT "   -> 0x005de370
```

Those thunks are the reason a reference query on `RegisterImageCodec` is worth nothing until they
are defined: while they sat as raw bytes `getReferencesTo(0x005c8360)` returned **zero** callers
for a function with seven. All seven are defined now and the query returns seven call edges, which
is the confirmation this section was written to predict. A zero-xref count on a registration
function is evidence about the *database*, not about the binary — the same trap as an undefined
vtable (`ghidra_analysis_notes.md`, Analysis Traps).

**Seven is the complete set**, and that is now confirmed rather than assumed: a survey went looking
for further image codecs and found none. The 17 static-init entries immediately after the seven are
**13 `IffChunk_Register` thunks plus 4 guard stubs — not codecs** (`rif_chunk_format.md`, "The IFF
chunk classes the binary registers"), so the run of registrations ends there.

| Magic | Factory | Object |
|---|---|---|
| `"BM"` | 0x005dd3f0 | 0x48 bytes, vtable 0x006711b4 |
| `"FORM"` / `"LIST"` / `"CAT "` | 0x005de290 / 0x005de300 / 0x005de370 | `RimImage`, 0x90 bytes, vtable 0x00671248 |
| `"P4"` / `"P5"` / `"P6"` | 0x005e02c0 / 0x005e0270 / 0x005e0220 | Netpbm |

**Nothing on this path dispatches on a file extension**, and no PNG/TGA/JPEG/DDS awareness
exists in the engine's own image code (0x005c6000..0x005e1000) — the `\x89PNG` signature at
0x006841d4 and the JFIF strings at 0x00682d51 belong to the libpng/libjpeg inside the
statically-linked D3DX8, reached only from 0x0042xxxx. So a new image format is a
*registration* rather than a detour. The cost is the interface: 24 vtable slots, of which
the BMP codec overrides 5 and the base supplies the rest, plus a base data layout that
`FillSurfaceFromImage` @ 0x005c6950 reads directly (+0x08 width, +0x0c height, +0x14 flags,
+0x18/+0x1c caps, +0x20 palettized). The generic consumers reach everything
*format-specific* through slots — `ChooseSurfaceFormatForImage` @ 0x005c7880 uses only
slots 1, 2, 3, 4, 18 and 19 — so a GkPlus-owned image object is viable, but slots 6..15 are
a row-streaming pull interface driven by the engine's loop and the base class's own size
was not established.

#### The `"BM"` codec works, and a mod can ship a BMP today

Not a stub, and not dead weight - it is a complete BMP reader, and because dispatch is by
magic bytes it is reachable **with no GkPlus code at all**: name a BMP whatever the engine
asks for (`Graphics\Bitmaps\Main Menu 01.RIM`, say) and it loads. Measured in the running
game at 8, 24 and 4 bpp; all three render correctly.

**The palette repack is correct, including the channel swap.** A BMP palette entry is an
`RGBQUAD` - B, G, R, reserved - while the base class keeps a 3-byte RGB table at `+0x24`,
so both a reorder and a pad-byte drop are needed. Slot 8 (`ReadPalette`, 0x005dd060) does
exactly that: three sequential byte reads written to `dst[2]`, `dst[1]`, `dst[0]`, then a
fourth byte consumed **only** when `biSize >= 0x28`, which correctly distinguishes
`RGBQUAD` from `BITMAPCOREHEADER`'s 3-byte `RGBTRIPLE`. Confirmed on screen: authored pure
red, blue and green come back exactly, with no swap. `biClrUsed` is honoured, falling back
to `1 << biBitCount` only when it is zero; a count above 0x100 sets `RimLoadErrorCode = 8`.

The envelope, from `ScanHeader` @ 0x005dcd50:

| | |
|---|---|
| `biSize` | 12, 40 or 64 only, else error 7 |
| `biBitCount` | **1, 2, 4, 8, 24** accepted; 16 and 32 rejected with error 8 |
| `biCompression` | **any non-zero value refused** with error 7 - `BI_RLE8`/`BI_RLE4`/`BI_BITFIELDS` fail cleanly rather than mis-decoding |
| `biPlanes` | must be 1 |
| row padding | honoured - `+0x3c = (-(width * bitCount) >> 3) & 3`, skipped after every row |
| dimensions | no power-of-two, square or minimum check anywhere in the codec |

**Defect: top-down BMPs are not handled.** `biHeight` is read as a plain 32-bit load with
no sign test, no negation and no `abs` anywhere, and slot 9 (`IsBottomUp`, 0x005dd200) is
literally `MOV AL,1; RET` - it returns bottom-up unconditionally and reads no state. A
negative `biHeight` therefore becomes a huge unsigned height, which passes
`FillSurfaceFromImage`'s only guard (`width == 0 || height == 0`). So this is worse than
"renders upside down": it is not handled at all. The likeliest surface is `CreateTexture`
rejecting the height and yielding `RimLoadErrorCode = 1` - inferred, not measured. **Write
bottom-up BMPs.**

**A palettized BMP never lands on a palettized surface.** BMP overrides only five slots
(0, 8, 9, 12, 20) and inherits the rest, so it presents as opaque (`GetAlphaBits` = 0),
single-level (`GetExtraMipCount` = 0), non-S3TC, with `min/maxPaletteColours = f10`. In
`ChooseSurfaceFormatForImage` every *uncompressed* candidate then passes - none of them is
palettized, so the `minPal` test short-circuits, and only R3G3B2 caps
`maxPaletteColours`, at exactly 0x100. The only palettized format `SurfaceDesc_SetFormat`
recognises is D3DFMT 0x29 (P8) and **nothing ever registers it**, so the palette is always
expanded.

Measured on this machine, all three source depths land on a **4-bit-per-channel** surface:
the rendered grey ramp quantises to 16 levels in steps of 17 (= 255/15), i.e. A4R4G4B4 or
X4R4G4B4. That is a *refinement* of what the candidate list alone predicts - reading the
list suggests R3G3B2 or X1R5G5B5 wins, and neither matches a step of 17 - so which
candidate actually wins is device enumeration, not a constant. What is invariant is that it
is never palettized.

Practical envelope to hand a modder: **uncompressed, bottom-up, 1/2/4/8/24 bpp, any
dimensions the device accepts.** Prefer 24 bpp - the palette buys nothing, since it is
expanded either way. And note a BMP carries **no alpha and no mip chain**, so it will
shimmer at distance; a `.dds` (see `src/ImageCodec`) is the better choice for real assets.

One idiom worth knowing if you write a codec: BMP **retains the `ImageSource *`** in
`+0x44` across `ScanHeader` -> slot 8 -> slot 12, rather than copying the file, and drives
the buffered layer through three helpers - `ReadU16` @ 0x005dd440, `ReadU32` @ 0x005dd4a0
and `Skip` @ 0x005dd560 (which accepts a **negative** count to seek backwards). That is a
third read idiom beside slots 3 and 7. It is safe only because all three calls happen
inside one `DecodeWithImage`; `src/ImageCodec`'s slurp-and-forget is the more robust shape.

#### Writing a codec: the image object's contract

Measured while building the DDS codec (`src/Dds`, `src/ImageCodec`). All `__thiscall`,
`this` in ECX, and every arity below is evidenced by the callee's `RET` form.

**The base class is 0x30 bytes and has no constructor** - each shipped factory inlines the
initialisation, and `pool_alloc` does not zero, so a factory that forgets a field leaves
garbage in it.

| off | meaning | written by | read by |
|---|---|---|---|
| 0x00 | vptr | factory | everything |
| 0x04 | refcount, init 1 | factory | `RunImageCodec` decrements; slot 23 |
| 0x08 | **current level's width** | slot 20 / slot 15 | `FillSurfaceFromImage` **directly** |
| 0x0c | **current level's height** | slot 20 / slot 15 | `FillSurfaceFromImage` **directly** |
| 0x10 | palette colour count, 0 = true-colour | slot 20 | base slots 1, 2, 5, 7, 21 |
| 0x14 | the request's `f` flags | `DecodeWithImage` @ 0x005c7d1e | slots' consumers |
| 0x18 | mip-skip (`m` = `VramTextureReduction`) | `DecodeWithImage` @ 0x005c7d12 | the skip loop |
| 0x1c | max dimension (`n`) | `DecodeWithImage` @ 0x005c7d18 | the downscale loop |
| 0x20 | destination-is-palettized byte | `ChooseSurfaceFormatForImage` | base slots 14, 23 |
| 0x24 / 0x28 / 0x2c | base-owned decode scratch: palette buffer, row-pointer array, row buffer | base slot 21 | base slots 10, 23 |

Those last three are what makes 0x30 rather than 0x24 the size, and `RimImage` **reuses**
them as its own chunk-list state - legal only because it overrides slots 10, 21 and 23.

The 24 slots, with the base default where one exists:

| # | signature | RET | base |
|---|---|---|---|
| 0 | `void *ScalarDeletingDtor(unsigned flags)` | 0x4 | none |
| 1 / 2 | `int GetMax/MinPaletteColours()` | bare | `f10` |
| 3 | `bool WantPalettized(bool dest)` | 0x4 | returns its argument |
| 4 | `int GetAlphaBits()` | bare | 0 |
| 5 | `void BindImageChunks(unsigned maxColours)` | 0x4 | 0x005c7b50 |
| 6 | `int GetExtraMipCount()` | bare | 0 |
| 7 / 8 | `int GetPassCount()` / `void *ReadPalette()` | bare | `f10` / none |
| 9 | `bool IsBottomUp()` | bare | false |
| 10 / 11 | `void GetSrcRow(void **out, int row)` | 0x8 | 0x005c82b0 / `*out = 0` |
| 12 / 13 | `void OnSrcRow(void *)` | 0x4 | no-op |
| 14 | `void ConvertRows(void *dst, void *, const void *src, const void *src2, int xoff, int pixels, int selector)` | 0x1c | 0x005c7bd0 |
| 15 | `void SelectMipLevel(int level, unsigned maxColours)` | 0x8 | no-op |
| 16 / 17 | `int Slot16()` / `void Finalize(bool)` | bare / 0x4 | 0 / no-op |
| 18 / 19 | `int IsS3tc()` / `int GetS3tcFourCC()` | bare | 0 / 0 |
| 20 | `void ScanHeader(ImageSource *)` | 0x4 | none |
| 21 | `void PrepareDecode(bool keepWhole, int maxColours)` | 0x8 | 0x005c8190 |
| 22 | `void ReleaseScratch(bool ok)` | 0x4 | no-op |
| 23 | `Image *DetachDecodedImage()` | bare | 0x005c82e0 |

**Slots 20 and 21 are `void`, whatever a decompiler labels them.** Both call sites clobber
EAX on the very next instruction (0x005c7d34, 0x005c7eac), and `RimOpenAndScan` returns
**0 on success** and a `GetLastError` value on failure - an inversion that proves no
convention is being followed. Failure is reported by writing `RimLoadErrorCode`
@ 0x00838b0c: `DecodeWithImage` zeroes it immediately *before* slot 20 (0x005c7d21) and
tests it at 0x005c7e00, where non-zero aborts and returns NULL before
`ChooseSurfaceFormatForImage` runs. Each rung of the intervening flag ladder is guarded by
"only if still zero", so a code set inside `ScanHeader` survives. The vocabulary:
3 no usable format, 4 open/read failure, 5 seek/IO, 6 truncated, 7 no codec matched,
8 malformed, 9 pixels with no palette, 0xd sub-rect not 4-aligned on an S3TC source,
0xe S3TC source into a palettized destination.

**Slot 23 is not AddRef in the base** - it allocates a 0x2c-byte detached holder and
steals the scratch buffers. It pairs with the request's `B` option, not with a Release:
`DecodeWithImage` calls it only on success and writes the result to `*(void**)(req+0x30)`,
after which the caller owns that object and releases it through its own slot 0.
`LoadRimFile2`'s `"sOXOYBfamA"` is the only shipped user. `RimImage` overrides it to
`INC [ECX+4]; MOV EAX,ECX`.

**The image object may come from any heap**, because it implements its own slot 0 and
nothing else ever frees it - `RunImageCodec` only does `ADD [ESI+4],-1` then `slot0(1)`.
That is *not* an exemption from the pool for `RegisterImageCodec` itself; see below.

#### The row loop, and the 4x4 floor it imposes

`FillSurfaceFromImage` @ 0x005c6950 drives the whole decode. Per level:
`LockRect` -> `slot10`/`slot11` for the row pointers -> `slot12`/`slot13` handed them back
-> `slot14` to copy -> `UnlockRect`; then `slot15(level)` **before** each level after the
first, after which `+0x08`/`+0x0c` must already describe the new level (zero in either is
error 8). The destination advances by the `D3DLOCKED_RECT` pitch, which for a DXT surface
is bytes per block row.

`RimConvertRows`' S3TC body is the entire copy contract:

```
src += (bits_per_pixel_column * x_offset) / 8
memcpy(dst, src, (bits_per_pixel_column * pixels) / 8)
```

where `RimBindImageChunks` derives `bits_per_pixel_column` from the payload size as
`((payload * 4 / height) * 8) / width`. For any DXT that reduces to a constant - **16 for
DXT1, 32 for DXT3** - because a block row is `(width / 4) * block_bytes`.

**And the loop only terminates when a level's height is a multiple of 4.** The counter is
seeded with the height and, on the S3TC path, drops by 4 per iteration (`SUB EAX,0x3`
@ 0x005c7410 then `SUB EAX,0x1` @ 0x005c742f), exiting on **exactly** zero. Height 4 lands
on zero; height 2 goes to -2 and height 1 to -3, and the test never sees zero again - the
loop runs away past the locked surface, with no clamp anywhere on that path. So the 2x2
and 1x1 tail of an ordinary mip chain is **fatal**, not merely useless, and any codec must
stop at 4x4. (Whether shipped `.RIM` chains do stop there is a claim about the data and
was not checked; the loop arithmetic says they must.) Sub-rects must also be 4-aligned -
`slot18` non-zero plus any unaligned rect edge sets error 0xd.

No power-of-two or square requirement was found on this path.

#### A codec cannot be registered from `DllMain`

`RegisterImageCodec` allocates each trie node with the game's `pool_alloc`. `pool_alloc`
itself needs no static ctor - its free lists self-link and `PoolAllocUseLock` @ 0x007c066c
has four readers and **zero writers**, so the critical section is never entered - but its
backing store is not ours: `pool_alloc_page` calls `AllocateMemory` @ 0x00601f4a, a hot-patch
thunk into **gl.exe's own statically linked CRT allocator**, initialised by that exe's
`_mainCRTStartup`. The loader calls our `DllMain(DLL_PROCESS_ATTACH)` before that, since
we are an implicit-load dependency. The game's own seven codecs register from `.CRT$XC`,
i.e. from `_initterm` - the earliest safe point. `RegisterDdsCodec()` therefore registers
from `FileHookSystem`'s **first intercepted open** (`EnsureFirstOpen` in `src/FileHooks.cpp`),
which is later still and costs nothing: the game only opens a file from `WinMain` onwards, and
a file is always opened before its bytes can be sniffed. It is not a detour of its own because
two subsystems detouring one target do not chain - see `CLAUDE.md`'s Conventions.

There is also **no unregistration** - the trie has no removal - so a detach would leave a
factory pointer into an unloaded module. Tolerable only because `d3d8.dll` goes away at
process exit.

**But the trie is torn down at exit, and the node holding our factory is freed with it.**
`RegisterImageCodec` registers its own `_atexit` handler (`_atexit(&LAB_0064c9c0)`), and that handler
walks the 0x400-byte static root array **`ImageCodecTrieRoot` @ 0x00838c58** (256 dwords,
`CMP ESI,0x400`), calling `ImageCodecTrie_FreeSubtree` @ **0x005c8690** on each non-null child and
then `free_sized(child, 0x408)`:

```
        MOV EDI,dword ptr [ESI + 0x838c58]   ; root[i]
        TEST EDI,EDI / JZ next
        MOV ECX,EDI
        CALL 0x005c8690                      ; recursively free the subtree
        PUSH 0x408 / PUSH EDI / CALL free_sized
next:   ADD ESI,0x4 / CMP ESI,0x400 / JC loop
```

`ImageCodecTrie_FreeSubtree` is `void __fastcall(void **node)`: it iterates all 0x100 child dwords,
recursing then `free_sized(child, 0x408)` for each non-null one, and **does not free the node handed
to it** — which is why the caller does that itself. Node layout is confirmed identical to the
insert's: `{void *child[256]; void *factory /*+0x400*/; void *_pad /*+0x404*/}` = 0x408 bytes.

Ordering consequence for `src/ImageCodec.cpp`: the node carrying our `DdsImage` factory is freed by
this handler at process exit, i.e. **by the CRT's atexit chain rather than by our detach**. Since
`d3d8.dll` is an implicit-load dependency it is still mapped at that point, so this is benign — but
it is the reason a "we never unregister, nothing ever frees it" reading of this section is wrong.

#### The byte source: 10 slots, and only four mean the same thing on both

`ImageSource_Win32Handle_vtbl` @ 0x0066efb8 and `ImageSource_Memory_vtbl` @ 0x0066efe0
(plus a size-tracking wrapper at 0x0066f008) implement:
`{ScalarDeletingDtor, BytesRemaining, BeginWrite, BeginRead, EndWrite, EndRead, WriteRaw,
ReadRaw, Tell, Seek}`.

There is a **fourth table in the run: the abstract base at 0x0066ef90**, now named
`ImageSource_vtbl_abstract` — same 10 slots at the same 0x28 stride, and what identifies it as the
base is that its **slots 8-9 are `__purecall`**. So the run is base, Win32, memory, size-tracking
wrapper. The 24-slot image interface documented below is likewise now named `Image_vtbl_base`
@ 0x00671154, ending exactly at the BMP codec's table 0x006711b4.

Those four at a known stride were also the **validation case for the vtable-boundary method** — a
real slot has zero references, a table start has some — before that method was applied across the
245 sub-tables of the RIF vtable band. It returned 10/10/10 here, matching what had already been
read out of the bodies. Status flags live at `+0x04` (0x01 EOF, 0x02 short read, 0x04/0x08
Win32 error, 0x10 seek/unsupported, 0x20 read/write failed) and the engine tests them
after slots 20 and 21.

**Slots 5, 7, 8 and 9 are the safe universal set.** The traps are the others:

- **Slot 1 `BytesRemaining` returns a hardcoded -1 on the memory source** (`OR EAX,-1;
  RET`), and slot 9 `Seek` is **absolute-only, no whence, no `SEEK_END`**. So there is no
  way to ask how long a file is. A self-describing format must be read header-first and
  sized from its own header - which is what `src/ImageCodec.cpp`'s two `ReadRaw` calls do.
- **Slot 3 `BeginRead` does not clamp on the memory source**: it sets `*out_avail` to
  whatever was asked for and returns a pointer into the caller's buffer, unchecked. It is
  a *buffered* block read paired with `EndRead`, and it is what `SniffAndCreateImage` uses
  to pull the magic 1024 bytes at a time. Do not build a whole-file read on it.
- **Slot 7 `ReadRaw(void *dst, unsigned len)` is the unbuffered bulk read**, identical in
  effect on both implementations. On the Win32 source a short read sets flag 0x02; on the
  memory source it is an unchecked `memcpy` with nothing to compare against, so truncation
  there is undetectable. The texture path passes a *filename* (`"sfWHOXOYcAm"`), so a
  mod-served file arrives through the Win32 source and is covered - but that is a property
  of the current call sites, not a guarantee.

**A codec must `Seek(0)` before its first `ReadRaw`, and this is measured, not defensive.**
`SniffAndCreateImage` matches the magic through slot 3, the *buffered* block read, which
pulls 1024 bytes and advances the underlying file pointer by all of them while the logical
position only moves at `EndRead`. Its Tell/Seek rewind then leaves those two consistent
with each other but **not** with the raw pointer slot 7 reads from. Observed in the running
game with a `.dds` standing in for `Graphics\Bitmaps\Main Menu 01.RIM`: on entry to slot 20
`Tell()` reports **0**, yet a `ReadRaw` returns bytes from offset **128** - and after
`Seek(0)`, `Tell()` reports **-1024**, which is the block count still sitting in `+0x30`.
Without the seek the first bytes read are payload, the magic test fails, and the texture
silently does not load. This is the slot-3/slot-7 mixing hazard reached without mixing them
yourself: the engine already used slot 3 before handing the object over.

Nominally the source arrives at **byte 0** - `SniffAndCreateImage` does restore the logical
position it consumed. Do not retain it and do not touch its refcount -
`RunImageCodec` drops it as soon as `DecodeWithImage` returns, long before `ConvertRows`
runs, so a codec must copy what it needs.

`RimOpenAndScan` is **not** a template for reading bytes: it constructs an IFF reader into
`RimImage+0x58` (0x005e26b0) and lets the chunk library do the reading.

### Nothing on the texture path looks at a file extension

A corollary of the registry above, and the thing that makes a new image format nameable
rather than merely loadable. Scanning `.text` (0x00401000..0x0064cbff) for the immediates
`'.RIM'` (0x4d49522e), `'.rim'` (0x6d69722e), `'RIM\0'` and `'rim\0'` returns **zero hits** -
every match in the image lives in a `.rdata` literal. All 26 `.rim`/`.RIM` strings are
*complete filenames* pushed straight as arguments; none is a `sprintf`/`strcat` template.

**The only string concatenation on any texture path is a prefix.** `"bitmaps\"` @ 0x00664704
is spliced at three sites (`ShowBriefingOrDebriefScreen` @ 0x004b2182, `LoadLevel`
@ 0x004e09e8, and 0x004b25f3) with `malloc(strlen(name) + 9)` - 8 prefix bytes, the name, one
terminator, and no room for an appended extension. The name itself is
`GetResourceString(0x00725664, 0x5f9)` (`GL_MISBRF_FILENAME`; `0x5f8` for the debrief), so the
briefing background's extension lives in `glres<lang>.dll`, not in code. That is the one
naming surface a mod cannot retarget without replacing the resource DLL.

**A `BMPNAMES` entry reaches `CreateFileA` byte for byte.**
`RifFile_LoadEnvScaleAndBitmapNames` @ 0x005aadb0 builds 0xc-byte records and stores the
**chunk node's own name pointer** - no `strdup`, no copy, no truncation:

```
005aae74  MOV EAX,dword ptr [EDI + 0x14]   ; node index
005aae7f  LEA EDX,[EAX + EAX*2]            ; * 3 dwords
005aae82  MOV EAX,dword ptr [EDI + 0x0c]   ; the node's name pointer
005aae85  MOV dword ptr [ECX + EDX*4],EAX  ; record[index].name = it, verbatim
```

`BuildShapeVertexBuffers` @ 0x005ab300 reads `record+0x00` and pushes it into
`AcquireRimTexture` at 0x005ab3a2.

Nothing downstream re-derives the name either: `AcquireRimTexture` hashes the **whole**
string (`sum of _mbctolower(c)`) and resolves collisions with `__stricmp` on the full stored
path, `__strdup`ing it into `AwTexture+0x2c`; the device-lost reload `TextureManager_RecreateAll` re-passes
that stored pointer (`PUSH dword ptr [EDX + 0x2c]` @ 0x005a1c3b) rather than rebuilding a
name; and `CreateSceneObjectFromCachedMesh` replays the `.opt` cache's length-prefixed string
verbatim. (The `.opt` *writer* was not inspected - if a new extension survives a cold load and
fails on the second run, that is where to look.)

**Which of `AcquireRimTexture`'s 31 call sites a mod can name.** Twenty-two take a `.rdata`
literal - the console water/lava/swamp/oil commands, `InitConsole`'s three fonts,
`DrawInventoryItemPanel`, `EnterMainMenuScreen`, `CreateMenuBracketMeshes`,
`InitParticleSystem`, `InitDirectSound` and the splash sequence - and cannot be retargeted
without patching. Nine take the path from data: `BuildShapeVertexBuffers` @ 0x005ab3a2
(**`BMPNAMES`** - the surface that matters), `CreateSceneObjectFromCachedMesh` @ 0x0059dc67
(the `.opt` cache), 0x00513110, 0x0051cace, 0x0051d56e, 0x005250c6, 0x0052525a, 0x00567795
(all caller-supplied) and 0x0055a6e8 (a global struct field). `LoadRimFile2` @ 0x004b13c0 is a
tenth data-driven surface that bypasses `AcquireRimTexture` entirely, passing its `path`
argument to `ImageFile_Load` with the spec `"sOXOYBfamA"`.

Two consumers have **no** seam and cannot be served from memory without either a handle
shim or a materialized file:

- **Bink.** `_BinkOpen@8` has exactly two call sites — `PlayMusicTrack` @ 0x00587b60 and
  `Fmv_OpenBinkMovie` (cutscene FMV) — and both pass flags `0`, i.e. a **file name**. The open
  then happens inside BINKW32.DLL, which does not use gl.exe's IAT. (Bink 1 does accept a
  `HANDLE` under its `BINKFILEHANDLE` flag, so this is solvable, but only with a real
  handle.)
- **`glres<lang>.dll`.** `LoadLibrary` needs a real file on disk. Not a modding target.

## 5. IAT slot map

`.rdata` is not writable at run time; `VirtualProtect` first. Slots are absolute
addresses in the default image; add `GetBaseAddress()` per the usual convention.

**Re-verified** against the binary's actual import-thunk run (the 82 thunks at
0x0057115b-0x00571346, `address_map.md` under "Imports"): all eleven slots that
`src/FileHooks.cpp` and `src/WindowPlacement.cpp` patch match this table exactly.

| API | Slot | Needed for |
|---|---|---|
| `CreateFileA` | 0x0064d068 | the gate: virtualize or pass through |
| `ReadFile` | 0x0064d06c | core |
| `SetFilePointer` | 0x0064d228 | core (the image reader seeks) |
| `GetFileSize` | 0x0064d0b8 | core |
| `CloseHandle` | 0x0064d074 | core — **shared with events/threads/mutexes**, so it must only claim its own handles |
| `GetFileTime` | 0x0064d0bc | `.opt` freshness checks |
| `GetFileAttributesA` | 0x0064d220 | existence checks |
| `WriteFile` | 0x0064d070 | refuse on a virtual handle |
| `SetEndOfFile` | 0x0064d224 | refuse on a virtual handle |
| `CreateFileW` | 0x0064d034 | only if the CRT `fopen` path is virtualized too |
| `GetFileType` | 0x0064d12c | only then — `__wsopen_nolock` calls it |
| `SetFilePointerEx` / `GetFileSizeEx` | 0x0064d1f8 / 0x0064d17c | only then — the CRT's lowio uses these, not the non-Ex forms |
| `FindFirstFileA` / `FindNextFileA` / `FindClose` | 0x0064d0c8 / 0x0064d0cc / 0x0064d1b0 | merging archive contents into the save/level lists |
| `SetCurrentDirectoryA` / `GetCurrentDirectoryA` | 0x0064d090 / 0x0064d094 | tracking the category cheaply instead of querying per open |
| `CreateFileMappingA` / `MapViewOfFile` | 0x0064d184 / 0x0064d024 | D3DX only; a virtual handle must fail these cleanly |
| `DeleteFileA` / `MoveFileA` | 0x0064d0c4 / 0x0064d22c | the `.opt` rewrite path |

Patching the IAT rather than detouring kernel32 matters: the slot is private to gl.exe,
so GkPlus's own CRT and D3D calls are untouched. It does **not** isolate the game's
statically-linked UCRT, which shares the same slots — which is why a hook must pass any
handle it did not mint straight through.

## 6. What was built on top of this

`src/Vfs.h` (lookup) and `src/FileHooks.h` (interception), over PhysicsFS 3.2.0 from
vcpkg. The measured properties above are close to the best case for a virtual filesystem:
one interception layer, synchronous single-threaded reads, whole-file loads for the two
formats that matter, and two pre-existing memory seams.

Both halves of the interception are **verified in a running game** (level01 loaded with
its 2.79 MB huffman-compressed `.rif` served through a virtual handle and its `.gcs`
served through a materialized temp file; 158 actors, and a token only the modded `.gcs`
sets came back 4242). An unmodded level loaded in the same session with the served counter
unchanged, so the passthrough path is untouched.

What PhysicsFS contributes: search-path/mount-order semantics (which *is* the mod-overlay
model), several archive formats, enumeration, a zlib licence, and a vcpkg port that builds
static x86 in seconds.

What it does not, and what therefore had to be written:

- **No `FILE *` / `HANDLE` interop.** physfs.h is explicit: "you can not directly use
  system filehandles with PhysicsFS and vice versa". The shim is the work; PhysicsFS is
  the easy half.
- **PhysicsFS is case-SENSITIVE inside an archive.** `physfs_archiver_zip.c` calls
  `__PHYSFS_DirTreeInit(&info->tree, sizeof (ZIPentry), 1, 0)`, and the third parameter of
  that function is `case_sensitive`. A mounted plain *directory* goes to the Windows
  filesystem and is case-insensitive, so the two kinds of mod would not even agree with
  each other. Measured, not read: `mods.resolve` on `rif/levels/LEVEL01.rif` missed an
  entry stored as `RIF/Levels/level01.RIF` until a case-folded index was added.
  **An earlier revision of this section claimed the opposite**, on the strength of a
  summary that mislabelled which argument was which - the file is the authority, not a
  description of it.
- **Path normalization**, and the fact that the casing the engine asks for is
  undiscoverable anyway: the directory comes from `gldirs.gls` (`rif`) and the file name
  from a `.gls` or an exe literal (`bitmaps\water.rim`, `User Interface/Main Menu.RIF`).
  So `Vfs.cpp` builds a lowercased index of everything mounted and resolves through it.
  That also deduplicates the merged view, which raw `PHYSFS_enumerate` does not - it
  reports a name once per search-path element that has it, so a naive recursive walk
  multiplies every file under a directory two mods share.
- **Nothing about the game's chdir'ing.** `Resolve` runs the engine's path through
  `GetFullPathNameA` (which joins the CWD and collapses `.`/`..`), requires the result to
  sit under the game directory, and hands back the remainder.

Thread safety is a non-issue (§1), and PhysicsFS's write dir is a non-feature: saves,
config, screenshots, demos and the `.cut`/`.map`/`.opt` caches stay unhooked on the real
filesystem.

The two shapes of interception, and why both exist:

- **Virtual handles** for the Win32 family. `CreateFileA` hands back a real kernel handle
  (an unsignalled event) with the bytes beside it, and eight more slots service it. A
  genuine handle rather than an invented value is what makes an unhooked API - D3DX
  reaches `CreateFileMappingA` - fail in an orderly way instead of running on a number we
  made up.
- **Materialize to a temp file** for the CRT. Serving gl.exe's static UCRT would mean
  virtualizing `CreateFileW` and then `GetFileType`, `SetFilePointerEx` and the rest of
  lowio; instead `fopen`/`freopen` are detoured and a hit is written to
  `%TEMP%\gkplus-vfs-<pid>\` for the real `fopen` to open. Costs a small write per `.gls`,
  `.gsh` or `.gcs`, and in exchange the `FILE *` is genuine.

Decisions worth knowing:

- **Only `OPEN_EXISTING` is virtualized.** All 31 `CreateFileA` sites use either
  `OPEN_EXISTING` (21) or `CREATE_ALWAYS` (9); there is no `OPEN_ALWAYS` anywhere, so
  this covers every read and cannot intercept a write. Access rights are deliberately not
  part of the test, because `IsFirstFileNewer` opens `GENERIC_READ|GENERIC_WRITE` and only
  reads timestamps.
- **`GetFileTime` reports the archive entry's own mtime**, so the engine's `.opt`/`.map`/
  `.cut` freshness checks work correctly rather than being defeated: a cache built before
  the mod was packaged is stale and gets rebuilt, one built after is not.
- **`GetFileAttributesA` reports `FILE_ATTRIBUTE_READONLY`** for a virtual file, which is
  load-bearing rather than cosmetic: the rif recompressor at 0x005b03b0 rewrites its input
  in place unless that bit is set, which would write mod content into the base install.
- **Temp-directory cleanup does not rely on `DLL_PROCESS_DETACH`.** It sweeps at startup
  for any `gkplus-vfs-<pid>` whose pid is no longer alive. `Shutdown()` still tries, but a
  crash skips it, and Gunlok in fact **faults on exit already** (`game_defects_notes.md`),
  so the sweep is the mechanism and the destructor is the optimization.

Still not covered, all documented rather than hidden:

- **Bink** (music and FMV), per §4.
- **Directory enumeration.** `EnumerateFilesIntoFileList` is not hooked, so a mod cannot
  add a savegame or a multiplayer level to those menus. Script-defined levels
  (`levels.add`) are the route for that instead.
- A GLDir pointed at an absolute path outside the game tree, which `Resolve` excludes by
  construction.
- 8.3 short paths: the game-directory prefix test is case-insensitive but not
  short-name-aware, so a process launched through one would silently disable modding.
