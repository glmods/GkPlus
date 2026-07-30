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
  calls plus one `MOV ESI,[slot]` in `FUN_0046c170`. Patching the slot needs neither
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

Win32 handle family. 24 functions call `CreateFileA`; two of them (`FUN_00422cd0`,
`FUN_00422de0`) are **statically-linked D3DX** (that region also contains libpng and
zlib strings) and two (`FUN_005e25f0`, `FUN_005e2750`) have no callers at all.

| What | Function | Notes |
|---|---|---|
| **.rif** | `File_Chunk::File_Chunk` @ 0x005afeb0 | the one entry point; whole-file read. Wrapper `FUN_005a9b50` has 7 callers, plus `RifCacheEntry_Load` @ 0x004af2b0 |
| **.rif (nested)** | `SUBRIFFL_Chunk` @ 0x005b0ec0 | same shape |
| **.rim / images** | `ImageFile_Read` @ 0x005c6850 | **already has a memory-source path** — see §4 |
| **sound samples** | `SoundSample_LoadFile` @ 0x005d3740, `FUN_005d3940` | whole-file read; reached from `FUN_0058bdb0` / `FUN_00589b30` |
| **.dat sound banks** | `FUN_0058d0d0` ← `LoadDatFile` @ 0x0058c2a0 | `FILECHNK` container |
| **level rif locators** | `AcquireLevelRifForLocators` @ 0x00483da0 | |
| level sidecar caches | `ToMap` @ 0x0047f160, `LoadOrBuildSectionAdjacency` @ 0x0044fef0, `FUN_00579070`, `FUN_005542c0` | read **and** write (`.cut` / `.map` / `.opt`) |
| rif `.opt` recompress | `FUN_005b03b0`, `FUN_00579070` | write, then `DeleteFileA` + `MoveFileA` |
| rif vs `.opt` freshness | `IsFirstFileNewer` @ 0x004af430 | the only `GetFileTime` consumer besides `ToMap` |
| savegames | `SaveGame`, `LoadGame`, `PeekSaveGameScriptName`, `MenuLoadGame`, `FUN_0056cb00`, `FUN_004b8d90` | keep on the real filesystem |
| demos | `LoadDemoFile`, `CommandSaveDemo`, `FUN_0046c170` | |

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
| `credits.mca` | `FUN_004dbdc0` | no |
| `GLkeys.cfg` | `ReadGLKeys` / `WriteGLKeys` | config — leave alone |
| screenshots, logs | `FUN_005a5f40`, `LogString`, `FUN_0058ced0` | writes — leave alone |

No `_access`, `_stat`, `_findfirst`, `_wfopen` or `tmpfile` in game code: the game never
probes for a file except by opening it (the one exception is
`GetFileAttributesA` in `FUN_005b03b0`).

Directory enumeration is **one function**: `EnumerateFilesIntoFileList` @ 0x004e6ef0
(`FindFirstFileA`/`FindNextFileA`, node 0x14c bytes), reached only through
`RefreshFileList` @ 0x004e6840 — save/demo/multiplayer-level lists.

## 4. The two seams the game already has, and the two it does not

The engine independently reinvented "read this from memory instead" twice, and both are
usable:

- **The GLS parser** takes its input through a `{dtor, Read, GetFileName}` source object
  (vtbl @ 0x00652904). Documented in `src/GLS.h`; `gls::ParseSource` is the wrapper.
- **The image loader** dispatches on `req->handle`: with `INVALID_HANDLE_VALUE` and a
  non-null `req->memory` it builds a memory-backed byte source
  (`ImageSource_Memory_vtbl` @ 0x0066efe0) instead of the Win32 one
  (`ImageSource_Win32Handle_vtbl` @ 0x0066efb8) and decodes from the buffer. Both
  implement the same 8-slot vtable `{dtor, ?, ?, read, write, seek, tell, eof}`, with the
  slot bodies in 0x005dc300..0x005dc9f0.

Two consumers have **no** seam and cannot be served from memory without either a handle
shim or a materialized file:

- **Bink.** `_BinkOpen@8` has exactly two call sites — `PlayMusicTrack` @ 0x00587b60 and
  `FUN_004b0b00` (cutscene FMV) — and both pass flags `0`, i.e. a **file name**. The open
  then happens inside BINKW32.DLL, which does not use gl.exe's IAT. (Bink 1 does accept a
  `HANDLE` under its `BINKFILEHANDLE` flag, so this is solvable, but only with a real
  handle.)
- **`glres<lang>.dll`.** `LoadLibrary` needs a real file on disk. Not a modding target.

## 5. IAT slot map

`.rdata` is not writable at run time; `VirtualProtect` first. Slots are absolute
addresses in the default image; add `GetBaseAddress()` per the usual convention.

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
