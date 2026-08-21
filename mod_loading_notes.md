# Mod loading: archives and directories over the game's data tree

The PhysicsFS-backed virtual filesystem and the IAT patching that makes the engine consult
it. `file_io_notes.md` is the measurement this rests on - read its sections 1 and 5 before
touching either file.

### Mod loading (`src/Vfs.h/cpp`, `src/FileHooks.h/cpp`)

Archives and directories layered over Gunlok's data tree, so a mod can add or replace any
file the engine loads with nothing in the base install changing. A mod is a `.zip` (or any
archive PhysicsFS reads) or a plain directory **anywhere on disk**, and its contents
**mirror the game's own directory tree**:

```
D:/mods/bigger-bugs.zip
  metadata/info.json         <- who this mod is
  metadata/README.md
  metadata/icon_small.png    (optional)
  metadata/icon_big.png      (optional)
  rif/units/bug.rif          <- replaces <Gunlok>\rif\units\bug.rif
  scripts/defaults.gsh
  sound/robots.dat
```

`metadata` is the one directory in a mod that is **not** game content: the engine has no such
category, so nothing an engine open asks for can ever land in it.

**There is no mods directory.** Nothing here knows where a mod lives, only what path it was
handed. A path is absolute, or **relative to the profile** — which is what keeps a mod list
portable, since a `settings.json` naming `mods/bigger-bugs.zip` follows `GKPLUS_PROFILE`
rather than hard-coding a location that exists on one machine. The `mods/` in that is a
layout whoever wrote the config chose; `src/Vfs` has no notion of it, and `src/Profile.h`
says the same from the other side. Resolving a relative path against the *profile* rather
than the process's current directory is not a convenience either: at the point this layer is
first reached the CWD is whichever GLDir the engine chdir'd into to open the file that
triggered it, so a CWD-relative path would land somewhere unpredictable — with no profile
directory to resolve against, a relative path is refused rather than guessed at.

`file_io_notes.md` is the measurement this rests on — read §1 and §5 before touching either
file.

### Two steps, because they answer different questions

**`load` reads a mod's metadata and nothing else.** It is how a script — or a manager UI —
learns what a *named* mod is before deciding anything, and it puts no file in front of the
engine. **`enable` declares the active set, in order**, and is the only thing that does:

```js
import { mods } from "gk";

const tweaks = mods.load("mods/tweaks");             // relative to the profile
const hiRes  = mods.load("D:/gunlok-mods/hi-res.zip"); // or anywhere at all
mods.enable(tweaks, hiRes);          // hi-res wins a shared file
```

**Nothing enables on its own, and nothing is discovered on its own either.** `vfs::Initialize`
starts PhysicsFS with an empty search path; every enabled mod comes from a `vfs::Enable` call
*naming a path*, and in a running game those come from the profile's **boot script**
(`core.boot`), which runs at `FileHookSystem`'s first intercepted open — the last instant
before the engine reads an asset, and therefore the only point from which the decision still
applies to everything the game will load.

There is deliberately **no directory enumeration** anywhere in this layer. A mod is named by a
script or by something a script read out of config; a mod sitting next to one that is named
does not load, so a profile with no boot module runs the unmodified game. That rules out the whole family of "it was enabled because it was in the folder"
surprises, each of which has actually happened in this repo: a directory renamed
`cutscene-test.disabled` that was still mounted and still served, so a "baseline" render
comparison ran for several rounds against itself; and a leftover `gkpbr-preview` mod quietly
replacing an asset in every later session (the bug commit 6655629 spent a session chasing).
A listing cannot tell any of those from an intention, and a blessed directory would put the
problem one indirection away rather than remove it — so there is not one. A config holds
paths, and `mods.enable(settings.boot.mods)` is the whole of it.

**`enable` replaces rather than adds.** Enabling a shorter list is how a mod is switched off,
enabling the same list in a different order is how one is reordered, and `mods.enable()` with
no arguments is the unmodified game — which is the A/B to reach for when a mod is suspected of
something, and which no longer needs files moved out of the tree. So the load order is stated
in one place instead of being accumulated by a sequence of calls whose order is then its only
record. It re-mounts from scratch every time, which re-reads each archive's central directory;
a boot script calls it once and the cost is invisible, and in exchange reordering and disabling
are the same operation as enabling.

**A later entry wins.** `enable(a, b)` means `b` overrides `a`, which is the direction every
mod manager reads a load order in. It falls out of `PHYSFS_mount` *prepending* — each mount
outranks the one before it — so walking the argument list forward puts the last one first in
the search path. The collection reports the same sequence, weakest first: `mods[0]` is the
lowest-priority mod and the last index wins.

**The base install is not a mod and is not in the load order.** It is *underneath* every mod
without being part of the sequence: only mod content is mounted, so a lookup miss is what makes
the engine read the real file, and that is what "the base game underneath everything" means
here. `vfs::Load` therefore **refuses the game directory by name** — mounting it would cost an
index walk over every shipped asset and change nothing about what the engine reads. That refusal
is a guard against a real mistake rather than a hypothetical one, `mods.game_dir` being right
there in the same namespace: an earlier revision of this design presented the install *as* a
Mod, and because that record was not interned, `enable` given it mounted the entire install and
put a `Gunlok` entry in the middle of the load order (measured, and caught by the harness
below).

### The metadata contract

`metadata/info.json` gives `name`, `author`, `website`, `license` and `version`; every field
is a **string** and every field is optional. `metadata/README.md` is expected too; the two
icons are optional and must be real PNGs.

**A mod that fails any of that still loads and still enables.** `mod.name` falls back to the
entry name on disk and `mod.problems` lists what was missing or malformed — a manager UI shows
that, and nothing else acts on it. Being strict would have meant every mod predating the
contract stopped loading rather than reading as incomplete, including the ones this repo's own
tooling writes (`pbr`'s and `lightmap`'s preview mods, both of which now write metadata).

Four things there are decisions rather than defaults:

- **Case is folded on the way in**, for the same reason `g_index` exists: PhysicsFS is
  case-sensitive inside an archive, so a mod shipping `Metadata/Info.json` would otherwise
  have no metadata at all. `FindChild` enumerates and compares case-insensitively; a
  `metadata` directory holds four entries, so the walk is free.
- **A number is reported, not tolerated.** An unquoted `"version": 1.3` read back through
  `json::Document::Get` as `"1.2999999999999998"` — the text is whatever the JSON codec's
  number formatter produces, and quickjs-ng 0.15.1's is not shortest-round-trip here (V8
  prints the same double as `1.3`). Reporting a version nobody wrote is worse than an empty
  one beside a problem saying why.
- **`readme` has CRLF normalised to LF.** The field is Markdown text whose only consumer is
  something that displays it; a mod authored on Windows carries CRLF and a stray `\r` renders
  as a box in ImGui, so normalising once here is the alternative to every consumer doing it.
- **An icon is checked against the PNG signature** and dropped if it fails, rather than handed
  to whatever ends up decoding it. `icon_small()` / `icon_big()` are **methods**, because each
  returns a copy of the file's bytes and a getter reached from a per-frame panel would
  allocate the whole PNG every frame with nothing to say so.

The metadata is read through a **private mount point** (`gkplus-inspect`), never the ordinary
search path, and that is load-bearing twice over: the mod is being inspected before anything
has decided to enable it, so it must not be visible to `Resolve` while that happens; and every
mod has a `metadata/info.json`, so reading one through the merged view would find whichever
mod is on top rather than the one being asked about. Nothing rebuilds `g_index` for that mount,
so it is invisible to every other entry point.

**Records are interned by canonical path and never freed.** `Load` normalises through
`GetFullPathNameA`, strips the trailing separator and keys on the lowercased result, so two
spellings of one archive are one record — where two mounts of differing spellings would have
mounted it twice, `PHYSFS_mount` matching by `strcmp` on the string it was given. That is also
what makes a `Mod *` an identity the JS layer can wrap with no finalizer, and what keeps the
inspection mount from ever colliding with a real one: a path in the search path is by
construction already interned.

Six things decide the rest of the shape, in decreasing order of how much else depends on them:

- **What is enabled is a script's decision, not this file's, and it is stated rather than
  found.** It used to be a directory listing, which meant the most consequential thing about
  a launch could not be varied without moving files about — no A/B of one mod against none,
  no per-profile mod set, no ordering other than alphabetical — and, worse, meant a mod could
  be in play because of where it happened to sit. `Vfs` now only knows how to load a named
  path and enable an ordered set; `core.boot` knows which paths, usually from a list in
  `settings.json`. The cost is that the capability has to exist *before* the first asset read,
  which is the whole reason the script host has an early phase at all — and that a tool
  writing a mod tree has to tell its operator to enable it, which is why `pbr`'s and
  `lightmap`'s install hints now lead with `mods.enable(...mods, mods.load(...))`, naming
  the absolute path they wrote to rather than a profile-relative one (those two always write
  under `<Gunlok>\gkplus`, which is the profile only by default).
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
  everything enabled and resolves through it. This is not a nicety: the casing the engine
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
  `Shutdown` also deliberately leaves `g_loaded` alone: a `Mod *` is documented as good for
  the life of the process and the JS layer wraps one with no finalizer, so freeing the records
  there would trade a leak that ends with the process for a dangling pointer.
- **`mods.served` / `mods.recent` exist because a working mod is invisible** — the replaced
  asset loads and the game looks identical. They are the only way to tell "enabled" from
  "being read", and `recent` reports the VFS path, which answers "under what name did the
  engine ask for my file". Read `served` **after** something has been loaded: before the
  first VFS lookup it is 0 whether or not anything is enabled.

**Verified in the running game**, 66 checks over a throwaway profile with three mods — a
directory mod with complete metadata and two PNG icons, a `.zip` with no metadata at all, and
a directory whose `info.json` is present and wrong — plus a stray `readme.txt` beside them
that the boot module does not name:

- the boot module named its three and enabled them, `mods.count` is 3 with no base install
  among them, the load order reads `Alpha Overhaul, 20-beta.zip, Gamma` starting at `order` 0,
  and the stray `readme.txt` was never touched because nothing looks in the directory;
- `mods.discover`, `mods.dir` and `mods.base` are all `undefined`, `mods[0].base` is too, and
  `mods.load(mods.game_dir)` throws with "not a mod" — so the install cannot enter the load
  order even by being asked for;
- **path resolution**: a relative path landed under the *profile* rather than under whichever
  GLDir the engine was in, an absolute path reached the same record, `mods/./10-alpha` interned
  to it as well, and a mod named by absolute path outside the profile loaded;
- every `info.json` field, the README (with its CRLF normalised), both icons read back as PNG
  bytes, and the collection keyed by display name *and* by entry name;
- the mod with no metadata loaded, named itself from disk, enabled, and reported both missing
  files; the one with a bad `author` and a numeric `version` reported all four problems and
  handed out neither the field nor the fake PNG;
- **load order end to end**: two mods shipping one `scripts/conflict.txt`, where the later
  wins, reordering reverses it, `enable` with a shorter list switches one off (and it then
  serves nothing and reports `order` -1), a repeated path keeps its last position,
  `mods.enable()` leaves nothing serving at all, an array of paths (what a config list looks
  like) enables all three in order, and spreading the collection back in re-enables it;
- `load` interning (a second spelling of one path is the same record), throwing on the stray
  file and on nothing at all, `enable` refusing a number with a `TypeError`, and a failed load
  leaving the enabled set untouched;
- and a real `level02` load with `mods.served` rising and `mods.recent` naming the mod's own
  `scripts/defaults.gsh`.

It is a check that can fail: an earlier revision of this design, in which the install was
itself a `Mod` at the bottom of the order, is what the harness caught mounting the whole game
directory. `examples/boot.mjs` has a second harness of its own — Node plus a stub `gk` and a
recording console — which asserts that it names each mod out of config, survives one bad entry,
and discovers nothing.

Not covered, deliberately: **Bink** (`BinkOpen` takes a file name and opens it inside
BINKW32.DLL, off gl.exe's IAT, so music and FMV still come off disk), `glres<lang>.dll`
(LoadLibrary needs a real file), and **directory enumeration** —
`EnumerateFilesIntoFileList` is unhooked, so a mod cannot add a savegame or multiplayer
level to those menus; `levels.add` is the route for that.
