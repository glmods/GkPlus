# Mod loading: archives and directories over the game's data tree

The PhysicsFS-backed virtual filesystem and the IAT patching that makes the engine consult
it. `file_io_notes.md` is the measurement this rests on - read its sections 1 and 5 before
touching either file.

### Mod loading (`src/Vfs.h/cpp`, `src/FileHooks.h/cpp`)

Archives and directories layered over Gunlok's data tree, so a mod can add or replace any
file the engine loads with nothing in the base install changing. A mod is a `.zip` (or any
archive PhysicsFS reads) or a plain directory under `<Gunlok>\gkplus\mods`, and its
contents **mirror the game's own directory tree**:

```
gkplus/mods/bigger-bugs.zip
  rif/units/bug.rif          <- replaces <Gunlok>\rif\units\bug.rif
  scripts/defaults.gsh
  sound/robots.dat
```

Mods mount in ascending name order and **a later name wins** (`20-tweaks.zip` beats
`10-base.zip`); `mods[0]` is the highest priority. `file_io_notes.md` is the measurement
this rests on — read §1 and §5 before touching either file.

**Every entry under `mods` is mounted, so renaming one does not disable it.** A directory
called `cutscene-test.disabled` still serves its contents — there is no extension filter and
no manifest to opt out of. To take a mod out of play, move it out of the tree; renaming it
produces a "baseline" run that is silently still modded, which is exactly how one in-game
comparison in this repo ran for several rounds against itself. `mods.served` is the check,
and it must be read **after** something has been loaded: before the first VFS lookup it is 0
whether or not anything is mounted, and `mods.recent` names the paths actually served.

Five things decide the shape, in decreasing order of how much else depends on them:

- **The interception is gl.exe's import table, not Detours on kernel32.** Every file call
  in the exe is `CALL dword ptr [slot]` or `MOV reg,[slot]` + `CALL reg`, and both read the
  slot at run time — so one pointer write per slot catches every call site, and catches
  *only* gl.exe. GkPlus's own runtime, PhysicsFS and D3D resolve through this DLL's imports
  and are untouched, which is also what makes the whole thing non-recursive. Nine slots:
  `CreateFileA`, `ReadFile`, `SetFilePointer`, `GetFileSize`, `GetFileTime`,
  `GetFileAttributesA`, `CloseHandle`, plus `WriteFile` and `SetEndOfFile` to refuse a
  write to a virtual file. Each patch verifies the slot currently holds the expected
  `kernel32` export and refuses otherwise, because a mistyped offset would otherwise
  overwrite an unrelated `.rdata` pointer and crash somewhere unrelated.
- **The layout is forced, not chosen.** Every loader does
  `SetCurrentDirectoryToGLDir(<category>)` and then opens a *relative* name, so "where in
  the game tree" is the only thing a hook can reconstruct. `Resolve` runs the name through
  `GetFullPathNameA` (CWD join + `.`/`..` collapse), requires the result under the game
  directory, and uses the remainder.
- **PhysicsFS is case-SENSITIVE inside an archive** (`case_sensitive = 1` in its zip
  archiver) while a mounted directory is not, so `Vfs.cpp` keeps a lowercased index of
  everything mounted and resolves through it. This is not a nicety: the casing the engine
  asks for is undiscoverable, being half `gldirs.gls` (`rif`) and half a `.gls` or exe
  literal (`bitmaps\water.rim`, `User Interface/Main Menu.RIF`). The index also
  deduplicates the merged view, which raw `PHYSFS_enumerate` does not — it reports a name
  once per search-path element, so a naive recursive walk multiplies every file under a
  directory two mods share.
- **Two shapes of interception, because the CRT cannot take a virtual handle.** A
  virtualized `CreateFileA` returns a **real kernel handle** (an unsignalled event) with
  the bytes held beside it — genuine rather than invented, so an API this layer does not
  hook (D3DX reaches `CreateFileMappingA`) gets a valid handle of the wrong type and fails
  in an orderly way. gl.exe's statically linked UCRT would instead need `CreateFileW`,
  `GetFileType`, `SetFilePointerEx` and the rest of lowio, so `fopen`/`freopen` are
  detoured directly and a hit is written out to `%TEMP%\gkplus-vfs-<pid>\` for the real
  `fopen` to open (`vfs::Materialize`). Both of those are gl.exe's private CRT copy, so
  GkPlus's own runtime is unaffected.
- **Only `OPEN_EXISTING` is virtualized**, which is exact rather than cautious: all 31
  `CreateFileA` sites use `OPEN_EXISTING` (21) or `CREATE_ALWAYS` (9) and there is no
  `OPEN_ALWAYS` anywhere, so this covers every read and cannot intercept a write. Access
  rights are deliberately *not* part of the test — `IsFirstFileNewer` @ 0x004af430 opens
  `GENERIC_READ|GENERIC_WRITE` and only reads timestamps, and it has to see the mod's file
  or a stale `.opt` wins.

Three smaller decisions that are easy to get wrong later:

- **`GetFileTime` reports the archive entry's own mtime**, so the engine's `.opt`/`.map`/
  `.cut` freshness checks keep working instead of being defeated. **`GetFileAttributesA`
  reports `FILE_ATTRIBUTE_READONLY`**, and that is load-bearing: the rif recompressor at
  0x005b03b0 rewrites its input in place unless that bit is set, which would write mod
  content into the base install.
- **Cleanup does not trust `DLL_PROCESS_DETACH`.** `Vfs.cpp` sweeps `%TEMP%` at startup for
  any `gkplus-vfs-<pid>` whose pid is no longer alive. `Shutdown()` still tries on the way
  out, but the game faults on exit already (`game_defects_notes.md` §4) and it never ran.
- **`mods.served` / `mods.recent` exist because a working mod is invisible** — the replaced
  asset loads and the game looks identical. They are the only way to tell "mounted" from
  "being read", and `recent` reports the VFS path, which answers "under what name did the
  engine ask for my file".

**Verified in a running game**: level01 loaded with its 2.79 MB huffman `.rif` served
through a virtual handle (three opens) and its `.gcs` through a materialized temp file, 158
actors, and a token only the modded `.gcs` sets read back 4242. An unmodded level loaded in
the same session with `mods.served` unchanged, so the passthrough path is untouched.

Not covered, deliberately: **Bink** (`BinkOpen` takes a file name and opens it inside
BINKW32.DLL, off gl.exe's IAT, so music and FMV still come off disk), `glres<lang>.dll`
(LoadLibrary needs a real file), and **directory enumeration** —
`EnumerateFilesIntoFileList` is unhooked, so a mod cannot add a savegame or multiplayer
level to those menus; `levels.add` is the route for that.
