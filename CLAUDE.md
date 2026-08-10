# GkPlus - Gunlok Modding Framework

A 32-bit Windows DLL mod for the game **Gunlok** (2000). Built as a `d3d8.dll` proxy that hooks
into the game via Microsoft Detours. It is a **native C++ reverse-engineering library**: the
decompiled game structs live in headers, a typed native API wraps the game's own functions and
globals, and a handful of behavioral hooks (music volume fix, input fix, debug redirect, ImGui/D3D
overlay, and a PhysicsFS-backed mod filesystem layered over the game's data tree) run at load. The game binary is actively being reverse engineered in Ghidra, accessible
via MCP.

On top of that sits a **QuickJS scripting layer**: `src/Script.cpp` boots a runtime during the
game's `SetupMenus` and runs `<game dir>\gkplus\main.mjs`, which can import the `"gk"` C module,
add items to the game's front-end menus and draw an ImGui overlay through the object its `draw_gui`
export is handed. See "Script host" below.

## Build

- **Platform**: Windows x86 only (32-bit). Enforced in CMakeLists.txt.
- **Standard**: C++20
- **Generator**: Ninja (via CMakePresets.json)
- **Package manager**: vcpkg (`x86-windows-static-md` triplet)
- **Output**: `d3d8.dll` (MODULE library, not a regular DLL)
- **Entry point offset**: `0x005e50c8` (used to compute base address at runtime)

```
cmake --preset builtin-vcpkg
cmake --build build
cmake --build build --target copy   # copies d3d8.dll to Gunlok's Steam directory
```

`cmake --build build` is incremental and quick. The `static_assert`s in `src/Actors.h` (and the
other struct headers) are the check for any struct-layout or vtable edit — build after touching them
rather than eyeballing.
`-Winvalid-offsetof` warnings on `Actor`/its subclasses and on `Map` are pre-existing and benign
(they are non-standard-layout due to virtuals); a clean build still links `d3d8.dll`. Every struct
modelled with pure virtuals produces them — that is the expected cost of the convention below, not
a signal to switch back to an explicit vtbl field. A TU including `src/Actors.h` emits ~30 of them,
which bury your own diagnostics; filter with

```
cmake --build build 2>&1 | Select-String ': (warning|error):' | Select-String -NotMatch 'invalid-offsetof'
```

### Running the test suites

Neither suite is wired into CTest; each harness is a script taking the Gunlok directory.

Six run with **Blender absent** — this is the half of `blender/` that imports no `bpy`:

```bash
python blender/tests/test_roundtrip.py "<Gunlok dir>"   # container, 563/563 byte-exact
python blender/tests/test_schema.py "<Gunlok dir>"      # 485,663 leaf chunks, 44 ids
python blender/tests/test_shapes.py "<Gunlok dir>"      # REBSHAPE geometry
python blender/tests/test_heads.py "<Gunlok dir>"       # record chunks + keyframe timing
python blender/tests/test_cutscene.py "<Gunlok dir>"    # the cutscene codecs + ID-prop shape
python blender/tests/test_rim.py "<Gunlok dir>"         # all 513 textures, ~20 min
```

Four need Blender itself, and take the scene round trip through a real `.blend`:

```bash
blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" [N|all]
blender --background --python blender/tests/test_authoring.py -- ["<Gunlok dir>"]
blender --background --python blender/tests/test_cutscene_authoring.py -- ["<Gunlok dir>"]
blender --background --python blender/tests/test_emitter_authoring.py -- ["<Gunlok dir>"]
```

`test_scene.py` defaults to a sample rather than all 563 — pass `all` for the full run.

The `rimutil` pair takes the built exe first, and so does `riflights`:

```bash
python utils/rimutil/tests/test_decode.py <rimutil.exe> "<Gunlok dir>"
python utils/rimutil/tests/test_encode.py <rimutil.exe> ["<Gunlok dir>"]
python utils/riflights/tests/test_lights.py <riflights.exe> "<Gunlok dir>"
```

`test_lights.py` is the check on **`src/Rif`**, and it is the only test in this repo that
exercises a `src/` file: `src/Rif` touches no game memory, so `utils/riflights` can drive it over
all 563 shipped `.rif` files and compare against `blender/io_scene_rif`, which decodes the same
format by a different route. 563 files, 38 with a light set, **3,794 lights, every field exact**.
It asserts, so it is safe under any runner, and breaking one offset in `src/Rif.cpp` was confirmed
to make it fail.

`pbr/`'s five take no arguments and are run from `pbr/` (the install comes from `GUNLOK_DIR` or
the Steam registry). **They are not pytest and must not be run under it** — each file's `test_*`
functions append to a module-level `FAILURES` list instead of asserting, so pytest collection
reports the suite green whatever fails. Run each as a script and read the exit code:

```bash
uv run python tests/test_pipeline.py        # the arithmetic and the gates, synthetic
uv run python tests/test_addon_boundary.py  # the `blender/io_scene_rif` decoders gkpbr imports
uv run python tests/test_cache.py           # stage-1 fingerprints, and stale vs unknown
uv run python tests/test_renderstate.py     # the draw-log profile and its normalisation
uv run python tests/test_preview.py         # packing a map into a mod, and the de-light mask
```

`lightmap/`'s four take no arguments, need no install and no network, and are run from
`lightmap/`. They are **not** in the style above — their `test_*` functions assert, so they are
safe under any runner, and the runner has been confirmed to report and exit 1 on a deliberately
broken assertion:

```bash
uv run python tests/test_dds.py         # the DDS writer against a re-derivation of src/Dds.cpp
uv run python tests/test_openrouter.py  # the client's retries, with the POST stubbed
uv run python tests/test_prompts.py     # prompt assembly - its *bytes* are a cache key
uv run python tests/test_source.py      # what may be trimmed from an asset name, and what may not
```

Lint is `uv run --group dev ruff check .` from `blender/`, from `pbr/` and from `lightmap/`; the type check is
`npx -y -p typescript tsc -p types/tsconfig.json` (see `js_bindings_notes.md`; TypeScript is not a
dependency of this repo, which is why a bare `npx tsc` refuses to run).

### Runtime-testing outside the game

Nothing in `src/` can be exercised outside Gunlok - `GetBaseAddress()` derives from the host exe's
entry point, so every native-API call faults in a standalone process. The `src/Js*` bindings, the
script host and the REPL *can* be driven from a throwaway 32-bit harness with stubbed natives;
`harness_testing_notes.md` has the recipe and the four traps that cost time the first time round.
**Always add a deliberately-failing assertion once and confirm the harness reports it** - a harness
that cannot fail proves nothing.
### Debugging the running game

- **Automated testing loads `level02`, not `level01`.** `level01.gcs` ends in
  `PLAY CUTSCENE first contact`, so a scripted run lands in a scripted camera sequence rather
  than in the level — nothing is where a test expects it, and how far the cutscene has got
  depends on how fast the machine reached that frame. `level02.gcs` issues no `PLAY CUTSCENE`
  (of the fifteen campaign levels only `prison`, `level02`, `level03`, `level04` and `level06`
  are free of one), which makes it the cheapest in-level state to assert against:

  ```
  levels.start({script: "level02.gls", console: "level02.gcs"})
  ```

  173 actors / 294 roles, against level01's 158 / 259. Only reach for `level01` when the thing
  under test *is* level01 — an existing measurement being reproduced, or the cutscene path
  itself. Every "measured on level01" number in the notes stays as it is; it records what was
  run, not what to run next.
- `cmake --build build --target copy` fails while `gl.exe` is running (the DLL is locked). A
  clean exit rewrites `<Gunlok>\scripts\GLkeys.cfg` — a cheap "quit or crashed?" signal.
- **Prefer no debugger for crash hunting**: launch `gl.exe` directly and read WER's Application
  Error log plus the exit code. Attaching anything makes Gunlok crawl, and a fault is recorded
  either way.
- When you need a stack, **cdb** lives inside the WinDbg MSIX package and cannot be executed in
  place — copy it out. `game_defects_notes.md` has the recipe and the traps: `bp d3d8+0x...`
  silently parses `d3d8` as the hex literal 0xD3D8, cdb echoes its whole `-c` list so `.echo`
  markers appear twice (anchor greps with `^MARKER$`), and cdb will not load our clang PDB —
  symbolize with `llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address <rva>`.
- **`DebugSystem` deliberately leaves `DebugPrintWarning` unhooked** (`RedirectWarnings` in
  `src/Debug.cpp`): the GLS parser emits one warning per unset field per section, 13,000+ per
  level load, and redirecting those to `OutputDebugString` makes the game unplayable under any
  debugger. Re-enabling it is the fastest way to make in-game testing impossible.

### Dependencies (vcpkg.json)

| Package | Purpose |
|---------|---------|
| imgui (dx9-binding, win32-binding) | In-game GUI overlay |
| d3d8to9 | Direct3D 8 to 9 translation layer |
| detours | Microsoft Detours - function hooking |
| quickjs-ng | QuickJS JavaScript engine |
| dear-bindings | ImGui language bindings |
| physfs | Archive + search-path filesystem behind the mod loader (`src/Vfs.cpp`) |
| vulkan-headers, volk, vulkan-memory-allocator | The Vulkan renderer (`src/Vk*`). **volk, not the loader's import library**, so `d3d8.dll` has no load-time dependency on `vulkan-1.dll` and the game still starts on a machine with no Vulkan |

Custom vcpkg ports in `ports/` for: d3d8to9, detours, quickjs-ng, dear-bindings.
Overlay configuration in `vcpkg-configuration.json`.

## Architecture

### DLL Lifecycle (src/entry.cpp)

1. `DllMain(DLL_PROCESS_ATTACH)`: opens a Detours transaction, constructs the `Subsystems`
   aggregate (each member attaches its detours in its ctor), commits.
2. `DllMain(DLL_PROCESS_DETACH)`: opens a Detours transaction, destroys the `Subsystems` aggregate
   (each member detaches its detours in its dtor), commits.

`Subsystems` holds only the **hook-installing** subsystems — `FileHookSystem`,
`d3d8::D3D8CaptureSystem`, `MusicSystem`, `DebugSystem`, `GUISystem`, `InputFixSystem`,
`VersionTextSystem`, `CustomMenuSystem`, `WindowPlacementSystem`, `ScriptQueueSystem`,
`gls::GlsSystem`, `CustomLevelSystem`, `MapLightSystem`, `image::ImageCodecSystem`, `ScriptSystem`.
`FileHookSystem` is deliberately **first**: it patches gl.exe's file imports, and assets
loaded during `WinMain` (before any other hook can fire) have to pass through it for a mod
to replace them. `D3D8CaptureSystem` is second, and before `GUISystem` for a reason: it wraps
the `IDirect3DDevice8` the game creates, and `GUISystem` reads that same global expecting
whatever `CreateDevice` handed back. Destruction is reverse order, so it also unpatches last — and per
`game_defects_notes.md` §4 a fault in an earlier destructor can prevent that entirely, so
nothing there may depend on running.
Everything else is struct-only
or a native-API wrapper that resolves its offsets lazily per call (`GetObjectAtOffset` is cheap
because `GetBaseAddress()` caches), so those subsystems need no lifecycle object.

### Native API and struct headers

Each subsystem is a header of decompiled structs/enums plus free-function declarations over the
game's own functions and globals, implemented in the matching `.cpp` (the same split `src/GLS.h` /
`src/GLS.cpp` has always had). Examples: `gk::GetActorById` / `gk::GetActorsTable` (Actors.h),
`gk::GetRoleByName` / `gk::SpawnRole` / `gk::AITypeName` (Roles.h), `gk::GetCurrentMap` /
`gk::MapSpawn` / `gk::MapToWorld` (Map.h), `gk::GoToMenu` / `gk::ResourceString` (Menu.h),
`gk::Print` / `gk::RegisterConsoleCommand` (Console.h), `gk::GetGameMode` / `gk::GetSettings`
(Misc.h). A native-API function resolves its offset per call — there are no cached module-owned
pointers to keep alive.

Adding a subsystem: write the `.h` (structs + `static_assert`s + API decls) and `.cpp` (bodies),
and add the `.cpp` to `CMakeLists.txt`. Only add a member to `Subsystems` in `entry.cpp` if the
subsystem installs a detour that must live for the process lifetime.

### Function Calling Convention (src/Core.h)

Game functions are called via typed function pointers resolved at runtime:

```cpp
// Calling convention typedefs
CDecl<Ret, Args...>      // __cdecl
StdCall<Ret, Args...>    // __stdcall
FastCall<Ret, Args...>   // __fastcall
ThisCall<Ret, Args...>   // __thiscall

// Resolution: adds base address to offset, stores in pointer
GetObjectAtOffset(funcPtr, 0x004d4b50);  // resolves Print function
```

Base address is computed once: `actualEntryPoint - 0x005e50c8`.

### Detour Hooking (src/DetourUtils.h)

Wrapper templates around `::DetourAttach`/`::DetourDetach` that handle member function pointer
casting via `memcpy`. Used for `__thiscall` hooks on game objects.

Pattern: store original as function pointer, attach hook in constructor, detach in destructor.

## Source Files

### Core Infrastructure

| File | Purpose |
|------|---------|
| `src/entry.cpp` | DllMain; constructs the `Subsystems` hook aggregate inside a Detours transaction |
| `src/Core.h/cpp` | `GetBaseAddress()`, `GetObjectAtOffset()`, `DebugWrite()`, the calling-convention aliases |
| `src/Field.h` | `union field` — a shared 4-byte value slot used by several struct mirrors |
| `src/DetourUtils.h` | Member function DetourAttach/DetourDetach wrappers |
| `src/List.h` | `List<T>` / `List_Member<T>` / `List_Member_Base<T>` — layout mirror of AvP's `list_tem.hpp`, with sentinel-safe `begin()`/`end()` |
| `src/HashTable.h` | `HashTableBase<T>` / `HashTable<T>` — layout mirror of AvP's `Hash_tem.hpp`, with bucket-walking `begin()`/`end()` |
| `src/Memory.h/cpp` | `pool_alloc`/`pool_free` and the `pool_unique_ptr`/`pool_string` ownership markers |
| `src/Json.h/cpp` | `gk::json::Classify` / `Quote` / `Envelope` / `OpenEnvelope` — the two queues' JSON. This file owns the `{kind, body}` envelope's *shape*; the vocabulary of kinds is `ScriptQueue.cpp`'s. **The codec is QuickJS** (`JS_ParseJSON` / `JS_JSONStringify`) on a **private `JSRuntime` behind a lock**, because this runs on both game threads and the host's runtime may only be used from one. No modules and no scripts in it, so nothing there is observable from a script. See the `JS_UpdateStackTop` rule in the QuickJS conventions — sharing the runtime across threads disarms its stack guard. Everything in it is UTF-8; codepages are `Encoding.h`'s job |
| `src/Encoding.h/cpp` | `Utf8FromGameText` / `GameTextFromUtf8` — CP_ACP ↔ UTF-8 via `MultiByteToWideChar`/`WideCharToMultiByte`. **The script queue's edges.** Everything the engine holds in a `char *` is ANSI (a `.gls` is read as bytes; `fopen` reads the codepage) and JSON is UTF-8, so a name is transcoded on the way into a payload and back on the way to `ExecuteCommandFile`. A conversion that fails returns its input unchanged |
| `src/Varint.h` | Variable-length integer encode/decode utility (currently no callers) |

### Subsystem sources (struct headers + native API)

Each pair is a header of decompiled structs/enums plus native free-function declarations, and a
`.cpp` implementing them (offset resolution + any behavioral hooks): `src/Actors`, `src/Roles`,
`src/Font`,
`src/Map`, `src/Vulnerability` (header-only), `src/Music`, `src/Math`, `src/Menu`, `src/Tokens`,
`src/World` (sun angle/brightness/direction, ambient light and the fog state behind `FogSystem`,
whose null-ness is the "is a level loaded" test),
`src/Triggers`, `src/Console`, `src/Misc`, `src/Camera`, `src/Debug`, `src/GUI`, `src/InputFix`,
`src/CustomMenu`, `src/ScriptQueue`, `src/CustomLevel`, `src/Script`, `src/WindowPlacement`
(the game window clear of the taskbar, see below), `src/Font` (the text queue, see below),
`src/Session` (starting a
level without the menus, see below), `src/MakeRole` (native constructors, see below),
`src/FileHooks` (mod loading, see below), `src/Render` (the AWAPI renderer, see below).
`src/GLS.h/cpp` is the model the rest now follow. The behavioral-hook subsystems (`Music`, `Debug`,
`GUI`, `InputFix`, `CustomMenu`, `WindowPlacement`, `ScriptQueue`, `GLS`, `CustomLevel`, `Script`,
`FileHooks`, `Font`) expose a `*System` RAII class constructed by `entry.cpp`; the others are pure
struct + native-API.

`FileHookSystem` and `WindowPlacementSystem` are the two that do not resolve *offsets* at all.
`FileHookSystem` patches gl.exe's import table
and detours two functions in the exe's private CRT copy, and its lookup half (`src/Vfs`) touches no
game memory whatsoever. `WindowPlacementSystem` patches one more slot in the same table and reads
nothing out of the game at all.

`GlsSystem` is the odd one there: `src/GLS` is otherwise pure struct + native-API, and its single
detour (`PushFileToParserStack`) exists only so `gls::ParseSource` can hand the parser a **source
text instead of a file**. The parser's input is a source object with a
`{dtor, Read, GetFileName}` vtable, and `File::ReadFile` already tops up from an in-memory tail
after its `fread` — so a null `FILE *` plus that buffer is a complete parse, using the game's own
class. Full mechanism, and the three measurements it rests on (an unopenable root file is
harmless because `ParseErrorCount` has no readers; that detour is the only seam; the source name
is re-lexed inside a `# line` directive so it must not contain a quote) are in
`gls_system_notes.md` under "Parser input sources". It is what lets a script-defined level, a
`gls.probe` and a `gls.try_parse` all run with **nothing written to disk**.

### Script host and the REPL channel (`src/Script`, `src/Repl`)

One `JSRuntime`, one entry module (`<game dir>\gkplus\main.mjs`, booted from a detour on
`SetupMenus`), plus an optional loopback JavaScript REPL behind `GKPLUS_REPL_PORT`. **The REPL is
how you test anything in a running game** - see "Debugging the running game" above, and
`script_host_notes.md` for the protocol, the boot sequence and the five facts that pin the design.
### Custom menu items (`src/CustomMenu.h/cpp`)

Front-end menu items owned by GkPlus. `CustomMenuSystem` hooks `UpdateAndDrawMenuScreen` and
`OnMenuItemClicked`; the bodies are `ReconcileCustomMenu` and `DispatchCustomMenuClick`.

- **Registrations are never freed and never move** (`vector<unique_ptr<CustomMenuItem>>`).
  `Menu::AddItem` and `Menu::AddToggleItem` both store the label pointer with `label_is_static = 1`
  and bind a toggle's `int *` by address, and `ClearItems` frees neither — so the game reads our
  `std::string`'s characters and our `int` for as long as the item is on screen.
- **Items are applied lazily, at draw time, for the chosen menu only** — never at registration.
  That is what keeps them *after* the game's own items on the dynamically populated menus, whose
  populators run on transition. It also makes them survive a rebuild for free: reconcile matches by
  label *pointer* (exact, even for two items with the same text) and re-appends whatever is
  missing. The one menu out of reach is **11 (JoinGame)**, which re-enumerates its sessions from
  inside `UpdateAndDrawMenuScreen` every frame, after we have reconciled it.
- A custom toggle **flips its own `int`** in the dispatch, because the game has no generic toggle
  handler — `OnMenuItemClicked` mutates every bound variable explicitly.
- Dispatch plays `PlayUiSound(UiSoundMenuSelect)` itself, since it never reaches the game's
  handler, which opens with exactly that.

There is no removal: `Menu::ClearItems` is all-or-nothing and each item caches its own index, so
`ClearCustomMenuActions()` (used at host teardown) makes items inert rather than deleting them.
Only the **front-end** `Menus[36]` is covered; `InGameMenus[7]` has a separate dispatch
(`InGameMenu::OnItemActivated`) and is not wired up.

### Building game objects (`src/MakeRole`, `src/JsMake.cpp`, `src/JsGls.cpp`)

`MakeRole` is the `ToXxx` converters re-expressed over plain description structs, so a game object
can be built with no `ParsedThing` in sight; `make` exposes them to JS; `gls` keeps the three
things only the real parser can answer (`schema`, `probe`, `try_parse`). **Descriptions are in
`.gls` units** - degrees, seconds, metres, cycles/sec - and five of the conversions carry real
risk; the unit traps are listed under Conventions below. Full detail in `make_role_notes.md`.
### Both queues carry one JSON envelope (`src/ScriptQueue.h/cpp`)

Gunlok's script queue and its console-command queue both carry `{kind, body}` documents instead of
bare file names, so a trigger can deliver a *message* to a script. Ten hooks; the console queue is
read through the envelope but never written to it. **This is visible on disk and deliberately
breaking** - a save written by this build will not restore in an unpatched Gunlok. See
`script_queue_notes.md`, and `threading_model_notes.md` for which thread each hook runs on.
### Script-defined levels and programmatic starts (`src/CustomLevel`, `src/Session`)

A level with **no `.gls` and no `.gcs`** - only the `.rif` still comes off disk - registered with
`levels.add(title, module)` and startable with `levels.start(target)`, no menus and no briefing.
Nothing a custom level needs is ever written to disk. `custom_levels_notes.md` has the six facts
that pin the design; `level_loading_notes.md` sections 6.5 and 7 are the measurements.
### Mod loading (`src/Vfs.h/cpp`, `src/FileHooks.h/cpp`)

Archives and directories layered over Gunlok's data tree, mounted from `<Gunlok>\gkplus\mods`,
later name wins. The interception is **gl.exe's import table**, not Detours on kernel32, which is
what makes it non-recursive. `mod_loading_notes.md` has the five decisions that shape it and the
three that are easy to get wrong later; `file_io_notes.md` sections 1 and 5 are the measurement -
read those before touching either file.
### The game window and the taskbar (`src/WindowPlacement.h/cpp`)

`WinMain` @ 0x0046aef0 passes **literal 0, 0** as X/Y to both of the binary's `CreateWindowExA`
sites — not `CW_USEDEFAULT`, not a centering computation — so a windowed-mode game sits under a
taskbar docked to the left or top edge. The fix is one more IAT slot in the same table
`FileHookSystem` patches: the windowed creation (the captioned, non-topmost one, still at 0,0) is
redirected to the monitor's work-area origin, the fullscreen one is left alone, and the size is
untouched. Nothing in the game undoes it — user32's import set has no `MoveWindow`,
`SetWindowLongA`, `AdjustWindowRect` or `SetWindowPlacement` at all, and the single `SetWindowPos`
passes `SWP_NOMOVE`. The addresses are in `address_map.md` under "Window and video mode"; the
header carries the reasoning.

### The text queue and the version stamp (`src/Font.h/cpp`)

`Font_QueueText` @ 0x005782e0 **draws nothing** — it lays the string out and appends a
`TextDrawItem` to the font's own list, which `ScenePass_Overlay2D` drains once a frame. So text is
a **third retained path**, parallel to the render queue and invisible to anything hooking
`RenderQueue_Submit`/`Flush`, and **a queued string lives exactly one frame**. Main thread only —
the list has no lock. `rendering_notes.md` §4.2 is the chain; the addresses are in
`address_map.md` under "Text rendering".

Two things the wrapper does rather than pass through. `gk::QueueText` **truncates at 1027
characters**, because the engine copies the caller's string into a 1028-byte stack buffer with no
bound and smashes its own frame past that (`game_defects_notes.md` §1) — that defect is reachable
from script the moment this is exposed, so the clamp is not optional. And `max_chars <= 0` is
normalised to the whole string, since the engine's own clamp is an *unsigned* compare where a
negative reads as "no limit".

Ownership, measured rather than assumed: `Font_QueueText` **copies** the text
(`malloc(strlen+1)` + inline `strcpy` @ 0x005787f4) into a pool block the flush frees, so a caller
may pass a temporary or a literal. `color` and `alt_color` are dereferenced during the call.
**`target` is the exception** — it is stored raw and read again at flush time, so it must be one of
the game's own objects and never a GkPlus one.

`VersionTextSystem` detours `DrawVersionText` and replaces the game's `"v1.3 DX8"` with
`GkPlus - <renderer>` at both call sites (the Main menu and the splash frame), keeping each site's
own colour and position. The name comes from `d3d8::RendererName()`, which reports the **resolved**
mode — a `GKPLUS_RENDERER=d3d8` that could not load Windows' own runtime says `d3d9`, not what was
asked for. `GKPLUS_VERSION_TEXT=raw` restores the stock string.

### The renderer's struct mirror (`src/Render.h/cpp`)

Pure struct + native-API over AWAPI's own classes - no detours of GkPlus's own. It is the second
place in the codebase to use real multiple inheritance, and the vtable slot ordering is
load-bearing. `rendering_notes.md` is the analysis and now carries the mirror's own section too.
### JavaScript bindings (`src/Js*`) and type definitions (`types/`)

The `"gk"` QuickJS C module, 24 namespaces, built by `src/JsGk.cpp`'s `Namespaces` table; plus the
hand-written `.d.ts` that type-checks a plain `.mjs`. **`console_command_notes.md` is the map of
what is still missing** - all 280 registrations classified against the JS surface, with no gaps
left. `js_bindings_notes.md` has the collection scaffolding, the Actor prototype chain, the
native-vs-command-backed split, and the nine members that do not replicate.

The QuickJS rules that govern *writing* a binding are under Conventions below, not in that file -
they only help if they are read first.
### The Vulkan renderer (`src/D3D8Capture`, `src/Vk*`, `src/VertexFormat`)

A bindless Vulkan replacement for Gunlok's renderer: `GKPLUS_RENDERER=vulkan` draws the game, and
the whole-frame residual against the original is 0.13/255. **`vulkan_renderer_plan.md` is where to
start** - status, next steps, how to test, and the subsystem guide moved out of this file;
`vulkan_renderer_notes.md` is the design record and every measurement behind it. Read the plan
before touching any of it.

The one fact that shapes everything: **the seam is `Direct3DCreate8`, not the AWAPI render
queue**, and the ground truth to compare against is `GKPLUS_RENDERER=d3d8` - the original runtime,
still shipped in SysWOW64.

It also draws two things the game never could, both keyed on a texture's `.rim` name:
`render.material_override` retextures, tints or hides every draw sampling one asset, and
**`src/VkLighting`** gives one a bump/metallic/roughness response from a companion
`<texture> lighting.dds` served by `src/Vfs` or the install - **R height, G highlight intensity,
B highlight sharpness**, the normal derived at draw time so the 48-byte canonical vertex is
unchanged. Nothing registers a lighting map; the interface is the file name, and `lightmap/`
generates one. Its defaults are
measurements (`vulkan_renderer_notes.md` §4.48) - Gunlok's lights author a **black specular** and
a **4.0 diffuse**, which is why the highlight follows the diffuse colour and `specular_scale`
defaults to 0.25.
### The Blender addon (`blender/`)

Import/export of `.rif` geometry for Blender, in **pure Python** — no compiled extension, and
it shares nothing with `d3d8.dll`. Because it is a separate world from the C++ side, its design
record lives in **`blender/CLAUDE.md`**, which loads automatically when you work in that
directory; `blender/README.md` is the user-facing half and `rif_chunk_format.md` is the format
reference. Test invocations are under "Running the test suites" above.

### Other

| File | Purpose |
|------|---------|
| `src/Dds.h/cpp` | DDS parsing, and nothing else — pure, no game memory, harness-testable. Two families. **DXT1/DXT3** take the engine's S3TC path and must **stop their mip chain at 4x4**, which is not a style choice: the row loop decrements by 4 and exits on exactly zero, so a 2x2 level makes it run past the locked surface. **Uncompressed 24/32-bit** is the **only way to get true 24-bit colour into Gunlok** — every other route (DXT, an uncompressed `.RIM`, a 24bpp BMP) lands on a 4-bit-per-channel surface; it works by reporting `alpha_bits = 8` so the candidate walk rejects everything and falls through to the 32-bit descriptor, which needs `Use32BitTextures` (forced by `src/ImageCodec`). It has no 4x4 floor. Refuses **by name** anything the engine cannot render: DXT2/4/5, DX10-extension, cubemap, volume, and any pixel layout that would need a swizzle |
| `src/ImageCodec.h/cpp` | The game-facing half: `EngineImage` (the engine's 24-slot image interface and its 0x30-byte base layout, `static_assert`ed field by field), a `DdsImage` that serves DXT blocks straight out of its own copy of the file, and `ImageCodecSystem`. **A registration, not a detour** — the image layer picks its decoder by magic bytes via `RegisterImageCodec` @ 0x005c8360, and nothing on the texture path reads a file extension, so `Ground\ground.dds` in a `BMPNAMES` entry just works. Registered from `FileHookSystem`'s **first intercepted open** rather than `DllMain` (the trie is built with `pool_alloc`, whose backing heap gl.exe's CRT has not initialised that early) and rather than a detour of its own (two subsystems detouring one target do not chain — see Conventions). That anchor is provably both late enough and early enough: the game only opens a file from `WinMain` onwards, and a file is always opened before its bytes can be sniffed. **Verified in the running game**, not just by inspection: a DXT1 `.dds` served in place of `Graphics\Bitmaps\Main Menu 01.RIM` renders. The whole contract — slots, base layout, the row-streaming loop, the 4x4 floor, the `RimLoadErrorCode` failure channel, and the `Seek(0)` a codec must issue before its first read — is in `file_io_notes.md` §4 |
| `src/Rif.h/cpp` | Reading `.rif` chunk files: the `REBCRIF1` container (via `huffman/`), the tree walk, and `LIGHTSET`/`STDLIGHT`/`AMBIENCE` — **the light rig that baked each level's per-vertex colours, which the shipped engine loads and never reads** (`rif_chunk_format.md`). **Pure**: bytes in, records out, no game memory and no VFS, which makes it the only `src/` file with a test that runs without Gunlok — `utils/riflights` over all 563 shipped files against `blender/io_scene_rif`, 3,794 lights, every field exact |
| `src/MapLights.h/cpp` | `MapLightSystem` — the game-facing half of the above: where the level's `.rif` is, what scale it is in, and when to reload. **It has to be a hook**, because neither the path nor the rif object survives the load: `LoadLevel` frees the rif right after `ConvertParsedObjects` and `Map` retains only the *shadow* object's rif name. So it detours `LoadOrGetRifFile` @ 0x004ae960 — the one seam all three of `ToMap`'s routes pass through — records the path and the unit scale, and parses lazily on the first read after a level change. Deriving the path from the `.gls` name instead was measured and **fails on 4 of 32 shipped levels** |
| `src/Vfs.h/cpp` | The mod filesystem: mount, case-folded index, lookup, read, `Materialize`. Pure lookup — touches no game memory, so it is the half a harness can exercise. See "Mod loading" above |
| `src/FileHooks.h/cpp` | `FileHookSystem` — the nine IAT patches and the two static-CRT detours that make the engine consult `src/Vfs`, plus the virtual-handle table and the `mods.served`/`mods.recent` diagnostics |
| `src/Repl.h/cpp` | The loopback JavaScript REPL: `StartRepl` / `PumpRepl` / `StopRepl`, owned by `BootScriptHost` rather than by `Subsystems` (it installs no detour). Off unless `GKPLUS_REPL_PORT` is set. See "The REPL channel" above |
| `src/Session.h/cpp` | `StartLevel` / `QueueLevelStart` / `QueueReturnToMainMenu` — a level start with no menus and no briefing, deferred to the message loop. Installs no detour and has no `*System`: it registers `SetMessageLoopCallback` on first use. See "Starting a level programmatically" above |
| `src/Font.h/cpp` | The engine's text layer: `GetFont` / `LineHeight` / `QueueText`, and `VersionTextSystem`. `Font` is deliberately left **incomplete** - its 0xb18 layout is measured but nothing here needs a field out of it, and an unchecked mirror would only go stale. See "The text queue and the version stamp" above |
| `src/InputFix.h/cpp` | `InputFixSystem` - hook-only. Detours `AcquireDInputDevice` to suppress the vestigial DirectInput keyboard acquire and its `WH_KEYBOARD_LL` hook (see `input_notes.md`) |
| `src/WindowPlacement.h/cpp` | `WindowPlacementSystem` - hook-only. Patches one IAT slot (`user32!CreateWindowExA`) so the windowed-mode window is created at the monitor's **work area** origin instead of the hardcoded 0,0 it would otherwise sit at, under a left- or top-docked taskbar. `GKPLUS_WINDOW_PLACEMENT=raw` restores the stock behaviour. See "The game window and the taskbar" below |
| `src/ActorClasses.inc.h` | X-macro listing the 15 Actor subclasses: `GK_ACTOR_CLASS(Name, Parent, Predicate, Kind)`. Drives the JS class table, `kind`, the RTTI ladder and the prototype chain. **Must list every class before its own base** |
| `src/Menus.inc.h` | X-macro listing all 36 Gunlok menus: `GUNLOK_MENU(Name, Id, TitleResourceId, "English title")`. There are no gaps - ids 11 and 14-20 are identified in `menu_system_notes.md`. Also counted into `gk::MenuCount` |
| `imgui-quickjs/` | Static library: the ImGui bindings, linked into `d3d8.dll`. **Not a QuickJS module** — `js_imgui_new_namespace(ctx)` builds a plain object the host passes to `draw_gui`, since an ImGui call outside that frame does not work. `JS_SetPropertyFunctionList` handles the whole export list, `JS_DEF_CGETSET` included (see the QuickJS conventions) |
| `examples/main.mjs` | A working entry module, JSDoc-annotated against `types/`. Install it as `<Gunlok>\gkplus\main.mjs`; `examples/jsconfig.json` is what type-checks it. **It imports `levels/` which imports `headers/`, so install all three** — a module the host cannot find takes the whole entry module with it and registers no hooks at all, so the symptom is that nothing happens rather than that one level is missing |
| `examples/render-panel.mjs` | Every `render` knob as ImGui, in collapsing sections, drawn into the caller's window. Its own module because it is longer than the rest of the example put together and is the piece most worth copying. **Its one rule is write-on-`changed`**: `draw_gui` runs every frame, and `lighting_maps = true` re-reads every file while `map_shadow_rate` re-bakes the shadow atlas |
| `examples/levels/arena.mjs` | A working level module for `levels.add` — `map` + `includes` + `define` + `populate` + `setup` |
| `examples/headers/` | `bug.gsh` and part of `defaults.gsh` re-implemented with `gls`, as the worked example of translating a header |
| `types/` | `.d.ts` for the `"gk"` module and the `ImGui` interface, the generator for the latter, and `typecheck.ts`. See "Type definitions" above |
| `blender/` | The Blender `.rif` import/export addon and its seven test harnesses. Pure Python, unrelated to `d3d8.dll`. Design record in `blender/CLAUDE.md`. **Its decoders are imported by `pbr/` and by `lightmap/`** — the one thing outside this directory a change here can break |
| `pbr/` | `gkpbr` — a uv project that generates PBR map sets (color/roughness/metallic/normal/emissive/height) from the 365 `.RIM` a `BMPNAMES` table names, **per UV region rather than per sheet**, because most of them are atlases: a vision model decides what each material is, arithmetic decides every pixel. Pure Python, unrelated to `d3d8.dll`, and it imports `blender/io_scene_rif`'s decoders rather than reimplementing them — so an addon change breaks it silently, which is what `pbr/tests/test_addon_boundary.py` exists to catch. Design record in `pbr/README.md` |
| `lightmap/` | `gklightmap` — a second uv project, and **`pbr/` with the intelligence removed on purpose**: one `.RIM` in, three prompts to an image-editing model over **OpenRouter** (`OPENROUTER_API_KEY`, env or a file of that name at the repo root), one `<stem> lighting.dds` out. No segmentation, no classification, no gates, no cache. The three channels are `src/VkLighting.h`'s and two do not mean what their names mean elsewhere — R is a *height field*, G is highlight *intensity* (not a metal switch), B is highlight sharpness — so `gklightmap/prompts.py` spells all three out. Writes **uncompressed 24-bit + a full mip chain**, not DXT1: two channels are masks and a block's endpoints would smear them, and there is no S3TC compressor here. $0.20 a texture, measured. Design record in `lightmap/README.md` |
| `huffman/`, `utils/rifutil` | The C++ REBCRIF1 codec and its CLI. The Python port in `blender/io_scene_rif/rif.py` is decode-only; this is the only compressor |
| `utils/rendertest` | The PowerShell harness for driving Gunlok through the REPL and capturing frames — launch, dismiss the briefing, wait for the camera to come to rest, screenshot, and bisect `render.draw_hide` for the draw behind a pixel. What every renderer comparison should use; its README is the list of things that waste a run otherwise |
| `utils/rimutil` | `.RIM` <-> PNG CLI over spng + libsquish, both directions, both image forms. `compress` takes `--format dxt1\|dxt3\|body` (default **dxt3**) and `--raw`; **dxt5 is refused by name**, because `TextureFormatCandidates` @ 0x006ac348 lists only DXT1/DXT3 and `SurfaceDesc_SetCompressedFormat` @ 0x005c6820 drops any other fourcc *silently* — a DXT5 file renders with garbage alpha rather than failing. `body` is exactly lossless **on disk** and needs no DXT compressor, and `check_lossless` re-derives every pixel before writing — but **it is not lossless in the engine, and it refuses graded alpha for that reason**: Gunlok ignores the `ALPH` chunk a palettized image carries alpha in, so such a texture loads fully opaque (measured — see `rif_chunk_format.md`, "The engine does not honour an `ALPH` you write"). It picks `masking 2`, the one palettized alpha that works, when every transparent texel shares one RGB; a cut-out that cannot (several RGBs under transparency, so an `ALPH`) is warned about rather than refused, because that case is presumed broken and not measured. Format details in `rif_chunk_format.md`; tests in `utils/rimutil/tests` |

## Reverse Engineering Reference

### Detailed Documentation Files

**GkPlus's own subsystems** — these were sections of this file until it outgrew the context it is
loaded into. Each is the design record for one subsystem, and the rules that govern *writing* the
code still live here under Conventions:

- `address_map.md` - the binary's segment layout, every named global and function address, the Actor class hierarchy and subclass sizes, `TriggerKind`, and the `Role`/`Map` struct offsets
- `script_host_notes.md` - the QuickJS host (`src/Script`) and the loopback REPL (`src/Repl`): boot point, the four facts that pin the design, the REPL protocol and its limits
- `js_bindings_notes.md` - the `"gk"` module's 24 namespaces (`src/Js*`) and `types/`: the collection scaffolding, the Actor prototype chain, the native-vs-command-backed split, and the nine members that do not replicate
- `make_role_notes.md` - `src/MakeRole` and the `make` / `gls` namespaces: one `Make*` per GLS section, the five conversions that carry real risk, and what only the parser can answer
- `script_queue_notes.md` - the `{kind, body}` envelope on both of Gunlok's queues, its ten hooks, and why the console queue is read through it but never written to it
- `custom_levels_notes.md` - levels with no `.gls` and no `.gcs` (`src/CustomLevel`), and starting one with no menus (`src/Session`)
- `mod_loading_notes.md` - the PhysicsFS VFS (`src/Vfs`) and the IAT patching that makes the engine consult it (`src/FileHooks`)
- `harness_testing_notes.md` - building a throwaway 32-bit harness so the script layers can be exercised outside Gunlok
- `pbr/README.md` - the PBR map generator (`pbr/`, its own uv project, not part of `d3d8.dll`): why classification is per UV region, what the render-state profile harvested from the running game is and is not allowed to decide, what the stage-1 cache fingerprints and what it deliberately does not, how much baked lighting the set actually carries, and what a generated map looks like on screen

**The game, reverse engineered:**

- `actor_vtable_notes.md` - Actor class hierarchy, all 83+ vtable slots, subclass sizes, constructor addresses
- `trigger_system_notes.md` - 22 trigger types, data structures, console command syntax, function addresses
- `gls_system_notes.md` - GLS/GSH script parser: pipeline, ParsedThingBase layout, per-section field tables (types/ranges/defaults), ToXxx converters, C++ API is `src/GLS.h`
- `level_loading_notes.md` - How a level is built: `BeginLevelSession` -> `LoadLevel` -> `ToMap`, the `.cut`/`.map`/`.opt`/`.loc` sidecar caches, the `LevelMeshHeader` geometry format, the `use <role> in team <n> for "<rif object>"` placed-object binding hash on `ParsedMap+0x1b60`, both spawn factories, the three seams for replacing the `.gls` path with a native level builder, and (§5.5) **the navmesh — which is just the level's own polygons**: a face is walkable iff `(flags & 0x100) == 0 && normal.y < 0`, where 0x100 is both an authored blocker and the loader's verdict on a **45° slope limit**
- `role_system_notes.md` - `Role` (0xc0) field-by-field: the entity hash table (0x007b48f0), lifecycle (`CreateRole`/`ToRole`/`RoleDtor`/`DestroyRoles`), the two embedded 16-byte list headers (vulnerabilities @ 0x68, sever points @ 0xac), the `flags` bitfield, `InventoryInfo`, pickup classification via `character->aggression*10`, the `ai` -> Actor-subclass dispatch (`CreateActor`), the spawn path (`SpawnRole`), three `ToRole` defects, and (§10) the whole vulnerability subsystem - `Vulnerability` (0x1c), `VulnerabilityType`, the 0xc-byte list sentinel, and the four population paths
- `role_subobjects_notes.md` - the four `Role` sub-objects: `Character` (0xb8, with `ToCharacter`'s unit conversions), `Projectile` (0x20), `ParticleGenerator` (0xd4: GLS fields, the five 0x18-byte `PGenChannel` records, the `ParticleType` enum, and the template -> emitter map from `ParticleEmitter_Ctor`), and the 3-variant `Destructibility` family (base 0x8 / `FragData` 0x24 / `ReplaceDestructibility` 0x10, dispatched on the `+0x04` tag by `Frag` @ 0x0052e220)
**The gameplay layer** - recovered from the binary against the shipped manual, which documents
several features the retail build cannot actually execute:

- `ai_behaviour_notes.md` - enemy perception and the alert state machine: the **vision cone is a swept ray**, not a cone (it detects only on the tick it crosses a target's bearing), the five alert states on `Actor+0x80`, alert propagation whose radius collapses over its own window, and the orange "reacquiring" state that **nothing in the binary can enter**
- `combat_notes.md` - there is **no hitscan**; every shot builds a `ProjectileActor`. The damage arithmetic end to end, the ballistic solve and the SHIFT lob, `aim` as a spread, armour as a flat absorb threshold, splash with no falloff, and the ammo/weapon compatibility test
- `orders_notes.md` - the 0x28-byte `PendingOrder` FIFO and its 11 kinds (**nothing on the client ever pushes one** - the executor builds it from a wire command), standing orders, selection, Active Pause, and the 71-action key-binding table. **Formations have no geometry**
- `navigation_notes.md` - flat A\* over the level's own triangles with *squared* distance as both cost and heuristic, so paths are non-optimal; a 200-500 node budget that emits a partial path; no smoothing; and walkability that is **per-agent**, not global
- `inventory_notes.md` - the `Inventory` container and its 0x38-byte items, the slot model, the deny-bitmask behind "only Frend can use the heaviest weapons", and modules whose effects are re-derived rather than accumulated
- `stealth_and_fog_notes.md` - crouch and concealment as two separate flags, camouflage keyed on a scrap pile's proximity or nav-polygon flag 0x800 (the water), concealment as a **hard skip** in every acquisition loop, and a client-only fog grid the executor never reads
- `gadgets_notes.md` - mines as ordinary `ai mine` characters whose trigger radius *is* the perception system, the decoy, scrap-pile scavenging, and laser fences matched by coordinate rather than by name

- `threading_model_notes.md` - Two game threads (main "client" + executor "server"), loopback message queues (full `MsgQueue`/`MsgQueueList`/`MsgQueueNode` layouts), pause handshake, per-thread clocks/RNG, which GkPlus hooks run on which thread, and the four script-execution entry points (all main-thread; host uses `ScriptQueue`, MP joiners use update `0x67`)
- `directplay_protocol_notes.md` - Multiplayer wire protocol: DirectPlay (`IDirectPlay4A`) COM/session setup, app GUID, SendEx/Receive framing & reliability, and the full command (client->server) and update (server->client) message-id tables with payload layouts, the `0x87` lock-step turn model, and update `0x67` (§8.11) which makes every client run a trigger script from its **own** local `Scripts\` copy
- `menu_system_notes.md` - Both menu systems (front-end `Menus[36]` + in-game `InGameMenus[7]`): `Menu`/`MenuListItem` layouts, the four item constructors and the 4 item types, the full 0-35 menu inventory with titles and populators, the (menu, item) -> action transition map, navigation/rendering/input, key bindings, and the localized string table
- `save_system_notes.md` - `.sav`/`.msv` savegame format: full field-by-field stream layout, the header-only "carry to next level" variant, the team carry-over roster, and why the console `SAVE`/`LOAD` commands are the demo system instead
- `input_notes.md` - Input subsystem: keyboard runs on Win32 `WM_KEYDOWN` (`MainWindowWndProc` -> `HandleKeyMessage` -> VK->DIK `VkToScanCodeTable` -> the universal `HandleKeyPress4` sink), mouse on Raw Input, and the DirectInput `SysKeyboard` is a vestigial acquired-but-never-read fossil whose `Acquire()` arms the `WH_KEYBOARD_LL` hook that lags system keyboard input under a debugger; the `InputFixSystem` (`src/InputFix.cpp`) detours `AcquireDInputDevice` to suppress it
- `vulkan_renderer_plan.md` / `vulkan_renderer_notes.md` - the bindless Vulkan renderer that
  replaces AWAPI's D3D8 usage. **Plan first** (status, next steps, how to test), notes for the
  design and every measurement behind it. Start here before `src/D3D8Capture` or `src/Vk*`
- `rendering_notes.md` - the high-level renderer, which is a **separate library, `AWAPI`**
  (`Code\awapi\`, and *not* in the AvP source drop, unlike the RIF layer). The frame is
  "submit everything, then drain once": ~100 producers push into one global `RenderQueue` via
  `RenderQueue_Submit`, and `RenderQueue_Flush` state-sorts them by material then texture, with a
  back-to-front list drawn last. There is a real but shallow class hierarchy
  (`AwFrame` -> `AwNode` -> `Renderable`, and `AwFrame` -> `SceneNode`, recovered from
  base-ctor vtable overwrites), and almost no other polymorphism: nearly every vtable in the
  render range is a one-slot `List_Member<T>` destructor, and the D3D8 COM interfaces do the
  virtual work. The one genuine interface is `LightSet` (6 slots), through which **lighting is
  state-sorted the same way materials are**. Ranked hook points in §4; §5 catalogues all 31
  producers (identified from the localized string ids they fetch out of `glres<lang>.dll`) and the
  traps they carry - the queue is flushed **more than once a frame** because
  `SubmitAndFlushMapGeometry` drains it itself, the inventory screen *replaces* the world submit
  rather than overlaying it, the shadow-quality setting changes which functions are producers, and
  the single biggest producer is **vtable slot 68 of the `Unit` hierarchy**, shared by sixteen
  subclasses across only four distinct bodies
- `rif_chunk_format.md` - the `.rif` asset format: 12-byte chunk header, `REBCRIF1` Huffman
  container, all 105 registered chunk types, the `.RIM` texture format (IFF + S3TC, the one
  asset format that is *not* RIF chunks), the **cutscene system** (all twelve chunks decoded -
  a Catmull-Rom camera path, a cast of participants, a tagged event stream, and the `camera track`
  GLS section without which none of it is reachable), **and** the AvP upstream mapping (see below)
- `game_defects_notes.md` - bugs in **Gunlok itself** that reproduce without GkPlus, so nobody
  re-blames our hooks for them. Also the debugging recipes, and the one that matters most: WER
  already writes a full dump to `%LOCALAPPDATA%\CrashDumps\gl.exe.<pid>.dmp`, and
  `cdb -z <dump> -c ".ecxr; k 40; q"` plus `llvm-symbolizer` on the printed `d3d8+0x...` RVAs
  resolves a crash to file and line - where **every live attach failed** to walk the stack at all. Currently: `Font_QueueText` @ 0x005782e0 smashing its stack on any
  string over ~1024 chars (which makes the training-level debrief fatal), the fact that the
  game's own `PrintParseWarning`/`PrintParseError` discard their output, and (§5)
  `PolygonAdjacencyTest` @ 0x0048ecf0 overflowing a 3-element buffer during
  `LoadOrBuildSectionAdjacency` on **level** geometry containing degenerate-after-weld
  triangles - a `/GS` fast-fail (0xc0000409), so it is not an AV and **a debugger suppresses
  the dump entirely**. Also collects what
  actually works for debugging Gunlok - where cdb lives, why `bp d3d8+0x...` silently resolves
  wrong, and which breakpoints make the game unplayable
- `console_command_notes.md` - every console command the game registers (280 registrations, 272
  distinct names, 255 handlers), how the registry and its longest-prefix dispatch actually work,
  and each command classified against the JS surface. §4 explains the native vs command-backed
  split, §4.1 lists the 27 broadcasters with their update ids. Includes the localized-command-name
  hazard, the inert `CommandCondition` gate, and the GkPlus mirror defects the inventory turned up
- `file_io_notes.md` - every way the game opens a file, for anyone building a virtual filesystem
  or mod loader: the four properties that make one possible (all I/O through the IAT, no overlapped
  I/O at any of the 31 `CreateFileA` sites, no file I/O on the executor thread, whole-file reads for
  `.rif` and sound), the chdir-per-category `GLDir` scheme, the classified read/write site tables for
  both the Win32 and CRT `fopen` families, the two memory-source seams the engine already has (the
  GLS parser and the image loader) and the two it does not (Bink, `glres<lang>.dll`), the IAT slot
  map, and the PhysicsFS assessment in §6. §4 also has the **image-codec registry** — the image
  layer picks its decoder by *magic bytes* through an open registration function
  (`RegisterImageCodec` @ 0x005c8360), so a new image format is a registration rather than a
  detour — and the audit proving **nothing on the texture path reads a file extension**, which is
  what lets a `BMPNAMES` entry name something that is not a `.RIM`
- `gls.txt` - Game Level Structure file format quick field list (superseded by gls_system_notes.md)
- `<Gunlok>\html manual\manual.htm` - the shipped manual (Italian). Not RE, but the best
  inventory of what the gameplay layer is *supposed* to do. Treat it as claims to verify:
  formations have no geometry, flares are dead code, and the orange alert state is unreachable

### Shipped game data: the developers' own commented source

`<Gunlok>\scripts\*.gsh` and `*.gcs` are the game's own GLS headers and console scripts, and they
are **commented**. Read them before decompiling anything data-shaped: `defaults.gsh` names the
perception fields (`sight angle 45 // 45 degrees left or right` — a half-angle),
`body_slot_upgrades.gsh` names every pickup class behind `aggression * 10`, and `mine.gsh` shows a
mine is an ordinary `character` with `ai mine`. Field *values* across all headers are a free
cross-check on any recovered unit or enum; `grep -h '<field>' *.gsh | sort | uniq -c` is usually
enough. The comments are authoring intent, not measurement — two shipped roles have `max range` in
the wrong units — so confirm against the consumer, but they tell you what to look for.

### Upstream Source: Aliens vs Predator (1999)

Gunlok's asset layer is Rebellion's `3dc` chunk library, and the **published AvP Gold source**
is available locally at `D:\Documenti\GitHub\aliens-vs-predator\source\AvP_vc\3dc`. This is
verified, not assumed: AvP's `huffman.cpp` decompresses 563/563 shipped `.rif` files, and 88 of
Gunlok's 105 registered chunk ids appear in that source. Use it as ground truth for anything
chunk/RIF-shaped instead of decompiling — `rif_chunk_format.md` has the id -> class/file map.

The split is sharp and worth remembering:

- **Shared** — the chunk/RIF library, `List<T>` (`list_tem.hpp`, mirrored in `src/List.h`),
  `HashTable<T>` (`Hash_tem.hpp`, mirrored in `src/HashTable.h` — and it reaches further into
  the game layer than the "not shared" rule below suggests: the actors and roles tables are
  both this template),
  `Chunk`/`Chunk::Register` (`Chunk.hpp`), and the load-side consumers
  `avp/win95/Projload.cpp` + `Objsetup.cpp` (AvP's counterpart to `ToMap`).
  `list_tem.hpp` is the ground truth for every `{sentinel, count, cached_array,
  cache_valid}` header in Gunlok: the header is `List<T>`, nodes are
  `List_Member<T>` (`{vptr, prev, next, data}`, 0x10 for a pointer payload), and the
  **sentinel is a bare `List_Member_Base` of only 0xc bytes** — it has no `data`, so
  reading a node field off it is a heap over-read. Terminate on `cur != sentinel`, never
  on `cur->next != sentinel`. All three are mirrored in `src/List.h`; use them rather
  than open-coding the header again (see the convention below).
- **Not shared** — the entire game layer. Roles, Actors, GLS scripts, triggers, menus and the
  save format have no AvP counterpart; AvP's `STRATEGYBLOCK`/`MODULE`/behaviour blocks do
  **not** describe Gunlok structures. Don't map them across.

Two gotchas when searching that tree: most `win95/` files have **uppercase** extensions, so
`grep --include=*.cpp` silently misses them (use `find -iname`); and AvP's debug `fail()` is
`#define fail if (0)` under `NDEBUG`, which is why shipped code has no bounds checks
(e.g. `Menu::GetItemData`).

### Upstream Source: quickjs-ng

The exact `quickjs.c` for the pinned 0.15.1 is unpacked at
`vcpkg/buildtrees/quickjs-ng/src/v0.15.1-*/quickjs.c`. Read it rather than reasoning about QuickJS
semantics — it is what settled every rule in the QuickJS conventions below: the export-list
`abort()`, own-property-before-exotic lookup order, `find_atom`'s `[Symbol.iterator]` scan, and who
frees `val` in an exotic setter. The public header alone does not answer any of those.

### Ghidra Database Hygiene

Always leave the Ghidra database in a better state than you found it. Whenever you decompile
something, write your understanding back into the database instead of keeping it in the chat:

- Rename functions from `FUN_00xxxxxx` to descriptive names as soon as their purpose is clear
- Rename locals and parameters from `uVar1`/`param_1` to meaningful names, and set their types
- Define/refine structs and enums for the data being accessed, and apply them to the variables
  that use them (prefer this over leaving raw `*(int *)(param_1 + 0x50)` offset arithmetic)
- Name globals discovered along the way (`DAT_00xxxxxx` -> a real name)
- Add a plate comment on non-obvious functions summarizing what they do and their calling convention

Anything reusable (offsets, struct layouts, subsystem behavior) should also land in this file or
the relevant `*_notes.md`.

Read-only is **not enforced** on delegated work — a nested subagent renamed functions and globals
this session despite being told not to. If you fan out analysis, treat the constraint as advisory
and audit the DB afterwards rather than trusting it.

### Analysis Traps

- `StartExecutorThread` -> `ExecutorThreadProc` is a `CreateThread` **entry-point reference, not a
  call edge** — leave it in a caller-closure and every executor-only function falsely appears
  reachable from the main thread. Cut it; treat thread procs as roots.
- "No xrefs" often means the *referencing data was never defined* (vtables sitting as raw bytes).
  Scan for the little-endian pointer before concluding a virtual has no callers.
- Reachability and gate counts must be **transitive**: `CommandSpawn` looks ungated but delegates
  to `DoSpawn`, which holds the gate. Converges around depth 2.
- Read **disassembly** for computed sizes/arguments — the decompiler folds constants differently
  (`iVar + 0x48` vs the actual `LEA EDX,[EDI + 0x49]`).
- When extracting call arguments in bulk, take literals from the **same source line** as the
  buffer variable; pairing P-code operands positionally desyncs in multi-call-site functions.
- Bulk-recovering broadcast ids: scan each vtable slot body for the last `MOV dword [EBP+x], imm`
  before a `CALL BroadcastToPlayers` (0x00504bf0). Single-candidate sites are trustworthy — the
  scan independently reproduced every id already documented — but sites yielding 2+ candidates, or
  none, are computed (`0x41 + close_range`) and need disassembly.
- Verify against the Ghidra DB, not the `*_notes.md` — a claim in the notes is only as good as
  the measurement behind it, and not every one was measured.
- A **mistyped global pointer** makes the decompiler emit `Global[n].field_0xNN` shorthand whose
  real offset is `n * sizeof(wrong type) + 0xNN`. Retype the global before reading anything
  through it, and distrust notes written in `[n].field_` form.
- When sweeping accesses to a global struct, track the **offset carried in the register**
  (propagate through `MOV`/`LEA`/`ADD`/`SUB`, kill on other writes and on `CALL` for EAX/ECX/EDX).
  A plain `[reg+disp]` scan misses every field reached after an `ADD reg,imm` rebase. Sanity-check
  that all recovered offsets land inside the known struct size.
- Existing **names in the DB are not evidence either** — several shipped names described
  something the function does not do. Confirm a name against the body before building on it,
  and rename when it's wrong.
- A wrong name does not stay local. `save_system_notes.md` described the savegame `kind` field as
  "0 = player character" — inferred from a constructor name that does not exist in the binary —
  which made the format look impossible for any actor larger than 0x178. `kind` is just
  `IsProjectile()`. When a note calls something contradictory or broken, suspect the label first.
- A stub that decompiles to `return;` is **not necessarily a no-op**: check for `RET 0xN`. A
  non-zero operand means the function takes `N` bytes of stack arguments and discards them, which
  usually makes it a *setter* whose base implementation ignores the value. `Actor` slots 9 and 54
  were documented as per-tick callbacks for exactly this reason; both are `RET 0x4` and pair with
  getter slots 8 and 30.
- **`RET` vs `RET n` is the ground truth for a calling convention, and Ghidra's label is not.**
  A function with stack arguments ending in a bare `RET` is caller-clean (`__cdecl`); `RET n` is
  callee-clean (`__stdcall`, or `__thiscall`/`__fastcall` with stack args past the registers). The
  DB had `pool_free` @ 0x005715b0 as `__stdcall` when it ends in a bare `RET` at 0x0057166f and
  every game call site does `CALL free` … `ADD ESP,0x4`. `src/Memory.cpp` believed the label.
  **Getting this wrong does not fail where you can see it**: calling a `__cdecl` function through a
  `__stdcall` pointer leaks 4 bytes of stack per call (the compiler emits `sub esp,4` to undo a
  callee pop that never happens), so ESP drifts until some *later* frame's epilogue returns to
  garbage. It presents as a non-deterministic access violation with **EIP on the stack** and a
  faulting module of "unknown", nowhere near the real culprit, and it stays dormant until something
  calls the function often. Cross-check the convention of every wrapped function against its `RET`,
  and treat "it worked so far" as meaning "nothing called it in a loop yet".

  The same test applies to **arity**, not just convention, and it is worth running in bulk: `RET n`
  states exactly how many bytes of arguments a `__thiscall` callee pops, so a declaration with the
  wrong parameter count drifts ESP by the difference. Sweeping all sixteen Actor vtables (1,460 slot
  entries) against `src/Actors.h` found **nine wrong declarations** — see `console_command_notes.md`
  §6.3 for the table and the two false-positive sources (slot 0's hidden destructor flag, and a
  by-value `Vec3` being 12 bytes). Re-run it after adding any slot.

  `RET n` gives you the argument *bytes*, not the parameter count or their shapes — a `Vec3*`
  plus three dwords and a by-value `Vec3` plus a `bool` are both 0x10. What discloses a by-value
  class parameter is the **callee**: MSVC emits `_eh_vector_destructor_iterator_` on it, which is
  why `MobileActor` slot 86's stub has a body at all. Slot 90 has the same `RET 0x10`, takes a
  pointer, and dereferences it.
- The decompiler's `Class::Method` header line does **not** always match
  `FunctionManager.getParentNamespace()`. Query the namespace; never read ownership off the C output.
- For a vtable slot, the owning class is the **shallowest** class whose vtable contains that
  address — not the most-derived one that inherits it. 55 of 249 Actor-family functions were filed
  under a descendant. Fixing `setParentNamespace` also repairs the `this` parameter type for free.
- **There are two parallel class trees, one per thread** - the executor's `Actor` family and the
  client's `Unit` family - and a size, an offset or a slot index is only comparable *within* one.
  `role_system_notes.md`'s "MobileActor 0x230" and `level_loading_notes.md`'s "Mine 0x238"
  describe different objects and are both right. Slot 35 (`GetSize`) is the size oracle for the
  executor tree; `level_loading_notes.md`'s table is the client one, as its own header says.
- Vtable **slot indices are branch-local**. Two classes deriving from a common base number their own
  extension slots from the same index, so a "rename slot N everywhere" sweep silently clobbers an
  unrelated method in a sibling branch (`PickupActor` slot 85 vs `MobileActor` slot 85).
- The **last** vtable in an adjacent run has no successor to bound it. `PresidentActor`'s
  (0x00669380) runs to the string pool at 0x00669500 — 96 slots, not the 84 that "ends at the next
  vtable" implies. Bound the final table with the reference test, never with adjacency.
- A `ParsedThingBase` subclass may be **larger than 0x1b60**: check the `malloc` size in its
  `DoParseXxx`. `ParsedMap` is 0x1b78 - the extra 0x18 is the placed-object binding hash.
- A **`ToXxx` converter that only default-initialises a field can never tell you what it
  means** - constants are not evidence. When a struct is mostly `field0xNN`, check whether the
  only function with the type applied is the converter; if so the analysis simply never reached
  a consumer. `ParticleGenerator` sat that way until `ParticleEmitter_Ctor` was found.
- **Same size is not the same type.** `ParticleTypeInfos`' elements are 0xd4 bytes, exactly
  `sizeof(ParticleGenerator)`, and are reached through the same code - but their element ctor
  builds `Vec3`s at completely different offsets. Confirm with the ctor/dtor pair, not the size.
- A struct copied with **`MOVUPS`/`MOVQ` shows you its real record boundaries**. Five
  `ParticleGenerator` sub-records looked like `{vec4, int}` preceded by padding until the
  16-byte load from `+0x2c` (not `+0x30`) proved each record starts one dword earlier.
- A **table of N filled instances beats any single decompilation**. Diffing the 13 cases of
  `InitParticleTypeInfo` down a column named most of `ParticleTypeInfo` in one pass: the field
  that is 9.81 for snow/rain/sparks is gravity, the one that is 30 for rain and 9 for snow is
  fall speed. When a struct resists, look for the initialiser that fills every variant.
- **A shared epilogue after a switch is usually derived state.** Four of `ParticleTypeInfo`'s
  `Vec3`s are just the others divided by the tick rate; naming them as independent fields would
  have invented four physics parameters that do not exist.
- A helper reached only through subsystem X is **not necessarily X's**. `ParticleTypeInfo`'s
  `render_state` ctor/finalise looked particle-specific until a caller check showed the shadow
  renderer and five other subsystems using them. Check callers before baking a prefix into a name.
- **A register argument spilled in the prologue reads as an uninitialized local.** `LoadLevel`
  was documented as `StdCall<void>` because its `bool` parameter is `MOV [EBP-0x175],CL` at the
  third instruction: the decompiler shows a `local_179` with no assignment anywhere and every
  P-code def marked `INDIRECT`, i.e. "some call might have written it". A local that is *read*
  several times and *never written* is an argument until proven otherwise — check the first
  handful of instructions for a store from ECX/EDX, then check what each call site puts there
  (here `MOV CL,1` vs `XOR CL,CL`, which turned out to be new-level vs savegame-restore).
- **No `static_assert` means nothing is pinning the layout.** Before trusting a GkPlus struct
  mirror, check it actually has one - `ParticleGenerator` and `Projectile` had none despite
  this file claiming otherwise.

### Ghidra MCP Mechanics

- Tell a delegated `ghidra-analyst` **not to spawn subagents of its own**. The pool is 20 and
  nested fan-out saturates it; the failure surfaces as `Concurrent subagent limit reached` on an
  unrelated agent.
- `execute_command` runs in a **persistent** Jython context — globals survive between calls, so
  accumulate into a global and process in batches.
- 30 s timeout: >~15 decompilations per call times out. Batch with a `DONE` set so a timeout
  doesn't lose progress.
- `mem.getBytes()` into a Jython `bytearray` does not marshal back (silently returns zeros) — use
  `getInt()`/`getByte()` or `jarray`.
- `createLabel` replaces a dynamic `DAT_` symbol, deleting any `Symbol` handle fetched beforehand
  (`ConcurrentModificationException`) — re-fetch after.
- `findDataTypes` may return a pre-existing duplicate from another category; consolidate with
  `dtm.replaceDataType(old, new, False)` instead of leaving two definitions.
- Editing a struct field: `setFieldName` on an `undefined` component **silently does not persist**,
  and renaming a component *wider* than the field mislabels its neighbours (`MobileActor+0x187` was
  a 4-byte `int` spanning 0x188). Use `clearAtOffset` then
  `replaceAtOffset(off, dt, dt.getLength(), name, comment)`, and re-check `getLength()` afterwards —
  an unchanged struct size is the guard that the edit landed where you meant.
- For `__thiscall`, the `this` type comes from the function's **parent class namespace**
  (`setParentNamespace`), not `updateFunction`; a parameter literally named `this` binds to ECX.
- Ghidra's `__thiscall` puts **only** `this` in ECX, everything else on the stack. A function
  taking args in ECX *and* EDX is `__fastcall` — model it that way and check
  `getVariableStorage()` on each param afterwards. `updateFunction("__thiscall", ...)` also
  auto-inserts its own `this`, shifting your explicit params by one.
- After renaming in Ghidra, `grep` the `*.md` files for the old `FUN_`/`DAT_` name.
- A big function's decompilation can exceed the MCP result token cap (it auto-saves to a
  `tool-results` file needing chunked reads). Instead write it to the scratchpad via Jython
  (`open(p,'w').write(r.getDecompiledFunction().getC())`) then `Read` it — or hand that file to
  a subagent to summarize so a 1000+-line function never enters the main context.
- `getInt()`/`getBytes()` on an uninitialized `.data`/`.bss` global throws `MemoryAccessException`
  (zero-init globals aren't in the file image) — read meaning from writers/disassembly, not live bytes.
- `getInstructionAt` returns None for an address inside an instruction — use
  `getInstructionContaining` and walk `getPrevious()` to dump a window around a data reference.
- Walk to a vtable slot's body with `mem.getInt(vtbl + slot*4)`, masking `& 0xffffffff` (Jython ints
  are signed, so a high address comes back negative and `getAddress` rejects it).
- **Do not scrape constants out of the decompiler's local variables.** It reuses one local for
  several `.rdata` symbols, so a regex over `x = DAT_...;` then `field = x;` silently attributes the
  *last* binding to every use. Scan the disassembly instead, tracking register loads. For the GLS
  section constructors the pattern is SSE and two encodings matter: `MOVSD xmm,[const]` +
  `MOVSD [reg+disp],xmm` writes one 8-byte slot, while `MOVAPS xmm,[const]` + `MOVUPS [reg+disp],xmm`
  writes **two adjacent slots** from a 16-byte constant. Integers and booleans are plain
  `MOV byte/dword ptr [reg+disp], imm`. `XORPS xmm,xmm` then a store means zero.

### Addresses, struct layouts and enums

**`address_map.md`** - the binary's segment layout, every named global and function address, the
Actor class hierarchy and subclass sizes, the `TriggerKind` enum, and the `Role`/`Map` struct
offsets. Moved out of this file because it is looked up, not carried.

Before wrapping any function in it, read the `RET`-form and register-argument warnings under
Analysis Traps below - a wrong calling convention or arity drifts ESP and faults somewhere
unrelated, long after the call.
## Conventions

- Everything lives in the `gk` namespace
- Game addresses are always offsets added to base address (never hardcoded absolutes)
- **Hash tables are `HashTable<T>` / `HashTableBase<T>` from `src/HashTable.h`** — AvP's
  `Hash_tem.hpp` (`_base_HashTable`), separate chaining, power-of-two table, no rehashing.
  Gunlok uses **both** shapes and the difference is 4 bytes of offset on every field, so check
  which one you have: `HashTable<T>` carries the v1.1 three-slot node-allocation vtable
  (`NewNode`, `DeleteNode`, `NewNode(T, Node*)` — that declaration order is the slot order) and
  puts `n_entries` at 0x04; `HashTableBase<T>` has no vptr and starts at 0x00. `actors`
  @ 0x007ba0d8 is the former (its address is passed as `this`, and `HashTable_Remove`
  @ 0x0054f2b0 is the template's own `Remove`); `roles` @ 0x007b48f0 is the latter (nothing in
  `.text` even mentions 0x007b48ec, and every operation is inlined into `CreateRole` /
  `GetRoleByName` / `GetRoleById` / `DestroyRoles`). Note the node is `{d, next}` — payload
  **first**, the opposite of `List_Member<T>` — so a node holding a by-value payload is
  `sizeof(T) + 4`, which is what makes a `PlacedObjectBinding` node 0x18 for a 0x14 payload.
  The table's `T` needs a `HashFunction(T)` overload; Gunlok's for `Actor*`
  (`HashFunction_Actor` @ 0x0054db10) just returns `actor->id`
- **A `{sentinel, count, cached_array, cache_valid}` group is `List<T>` from `src/List.h`** —
  embed it as one member instead of spelling out four fields, and model the node type as
  `List_Member<T>` rather than `{void *vtbl; N *prev, *next; T data;}`. Range-for over a
  `List<T>` terminates on the sentinel correctly by construction, which is the whole point;
  `entry_of()` is the escape hatch when you need the node itself, and it is only valid on a
  node you have already proved is not the head. `List<T>` is deliberately a trivially-copyable
  aggregate with no constructors — `RegisterTriggers` takes one **by value** — and it is
  standard-layout, so embedding it does not cost `-Winvalid-offsetof` warnings on the
  containing struct. Picking the right `T` is a real claim about the payload: `List_Member<T>`
  puts `data` at 0x0c for a pointer but at 0x10 for an 8-aligned value type, which is exactly
  what makes `MenuListItem` 0x78 rather than 0x10
- Detour hooks follow: resolve original -> attach in constructor -> detach in destructor
- **Two subsystems must never detour the same target.** `Subsystems` is constructed inside a
  single Detours transaction, and two `DetourAttach` calls against one address do not chain
  there — one hook silently stops running. Measured: adding a second `SetupMenus` detour
  beside `ScriptSystem`'s killed the script host with no diagnostic at all (the REPL listener
  simply never opened, and the game otherwise looked normal). If a subsystem needs to run at
  a point another already hooks, have that hook call it, or find a different anchor — the DDS
  codec registers from `FileHookSystem`'s first intercepted open for exactly this reason
- `static_assert` on struct sizes and offsets to catch layout mismatches
- Game vtables are modelled in `src/Actors.cpp` as **declaration-ordered pure virtuals**: the base
  `Actor` declares 83 (slot 0 is the destructor), and each subclass appends its own extension slots
  in vtable order. Adding a virtual there is how you record a new slot — it costs no object size
  (the vptr already exists), so the `static_assert(sizeof(...))` guards still hold and will catch a
  mistake. Cross-check slot numbers against `actor_vtable_notes.md`.
- **A struct with a vtable gets pure virtuals, never an explicit `void *vtbl` member.** Declare the
  slots as `virtual ... = 0;` in vtable order and let the implicit vptr occupy offset 0x00 — the
  first *data* member then starts at 0x04 (see `Actor`'s `unk1`, `Map`'s `lock`). An explicit vtbl
  field would double-count those 4 bytes and, worse, hides the slots. Slots whose purpose is not
  yet known follow the field convention: `StubN()` (as in `Actor::Stub27`).
- **A second vptr mid-struct means multiple inheritance — model it with real C++ inheritance**,
  not a `void *sub_vtbl` field. MSVC lays base subobjects out in declaration order, so the second
  vptr lands at exactly the right offset once the first base is sized correctly (`Map : MapBase,
  RefCountedBase` puts `RefCountedBase`'s vptr at 0xa4 because `sizeof(MapBase) == 0xa4`). Add a
  `static_assert(sizeof(...))` per base plus `offsetof` on a member of each — those are what
  prove the layout, and they fail loudly if the split is wrong.
  One deliberate exception: `gls::ParsedThingVtbl` models the vtable as a struct of typed
  function pointers *on purpose*, because GkPlus calls those slots rather than just describing them.
- Determining a vtable's **slot count**: MSVC emits every class's vftables adjacently in `.rdata`,
  so the table ends at the next address that is referenced *at all*. Walk the candidate slots and
  count references per dword: a real slot has **zero** (nothing points into the middle of a
  vtable), a boundary has some. `Map`'s secondary vtable at 0x00652828 is 2 slots — 0x0065282c has
  no refs, 0x00652830 has six from unrelated classes. "Looks like a function pointer" is not a
  slot test: past a table's end sit the one-slot vtables of the list/node helpers, then floats.
- Fields with a known offset but unknown meaning are named `field0xNN` / `unkN[...]` padding; a
  getter/setter of unknown purpose is named `GetField0xNN` / `SetField0xNN`
- **Owning pointers in a struct mirror are `pool_unique_ptr<T>` (`src/Memory.h`); owned strings
  are the `pool_string` alias.** There is only **one heap** on the game side — `pool_alloc` is a
  page sub-allocator over the CRT, and the game's `malloc`/`free`/`strdup` are JMP thunks into
  it — so a decompiled `free(x)` and a decompiled `free_sized(x, n)` are the same call and strings
  are pool memory like everything else. Do **not** add a CRT-flavoured deleter: this DLL's `/MD`
  UCRT heap is neither the pool nor the game's CRT heap, so calling our `::free` on any of these
  pointers would corrupt it. The deleter is empty, so the member stays pointer-sized and the
  existing `static_assert`s remain the proof; MSVC's `unique_ptr` is standard-layout, so this
  adds no `-Winvalid-offsetof` warnings either. Read the containing object's destructor before
  annotating, and keep the reason a sibling stayed raw in a comment — **refcounted** (per-type
  Release, or "decrement then slot 0"), **borrowed** (`GetResourceString` results and the roles
  hash are the two big sources), **conditional** (ownership gated on a sibling flag, as with
  `MenuItemData::label`), or **leaked** (allocated per-object, never released).
- Enum-like `int` fields use `enum class Name : int` (near its struct, or a header if shared across
  files) applied to the field; only encode values you've **verified** (the game's own enum, or a
  keyword->id function like `GetParticleIDFromName`) — leave `int` rather than guess a mapping.
- **A misleading name is renamed, not annotated around.** When a type, field, function, or global
  turns out to mean something other than its current name (yours or a shipped one), rename it to
  what it *is* — do not keep a wrong name behind a "the name is doubtful" comment. Renaming is free
  and expected; the whole point of the DB/mirror is that names carry the analysis. Do it in one
  pass across **all three surfaces** — the `src/*.cpp` mirror, every `*_notes.md`, and the Ghidra DB
  (type + field + the getter/setter/ctor/dtor that reach it) — then `grep` the old name to confirm
  nothing dangles. Leave a one-line breadcrumb of the prior name where external write-ups will still
  use it (a struct doc comment, or the notes' slot row). Example: the MobileActor `+0x200` object
  was modelled as `AIController` / `GetAIController`, but it is the actor's nav-mesh
  movement/collision agent, so it is now `NavAgent` / `nav_agent` / `GetNavAgent` with
  `CreateNavAgent`/`DestroyNavAgent`, and the getter's old name is noted in the `NavAgent` comment
  and the slot-24 row of `actor_vtable_notes.md`.

### QuickJS binding conventions (`src/Js*`)

Four rules, all verified against quickjs-ng 0.15.1's own source rather than assumed. The first is
the one that bites hardest, because its failure mode is silent.

- **Never let a `JS_CGETSET_DEF` reach `JS_SetModuleExportList`.** That switch ends in
  `default: abort()` (quickjs.c:40006) — the process dies with no diagnostic, no exception, no log.
  `JS_SetPropertyFunctionList` is the one instantiation path that honours `JS_DEF_CGETSET`. The
  binding layer sidesteps this structurally: it never calls `JS_SetModuleExportList` at all, every
  namespace is built object-first and handed to `JS_SetModuleExport` as a finished value, and every
  `JSCFunctionListEntry` array stays file-local to the TU that installs it.
- **A C module's named exports are values set once at instantiation, not live bindings.**
  `import { position } from "gk"` could only ever be a snapshot. Exporting *objects* and putting the
  accessors on them is what makes state live — which is why there is one `"gk"` module with six
  namespace objects rather than six `gk:*` modules of loose exports. `JS_DEF_OBJECT` nests safely
  (it routes through `JS_NewObjectProtoList` -> `JS_SetPropertyFunctionList`, quickjs.c:40081), so
  accessors are legal one level below an export and nowhere above it.
- **Own properties beat exotic handlers.** `JS_GetPropertyInternal` calls `find_own_property` before
  consulting `exotic->get_own_property` at each step of the prototype chain (quickjs.c:8734). That
  is what lets `actors.count` and `actors[Symbol.iterator]` coexist with `actors[12]`. Keep those
  own properties **non-enumerable**, or they show up in `Object.keys(actors)` beside the ids — the
  same reason `Array`'s `length` is hidden. Conversely `JSPropertyEnum::is_enumerable` is *ignored*
  for exotic objects (quickjs re-queries `get_own_property`), so that callback must return
  `JS_PROP_ENUMERABLE` or `Object.keys` comes back empty.
- **A bare specifier needs no module loader and no normalizer.**
  `js_default_module_normalize_name` passes through anything not starting with `.`, and resolution
  checks the already-loaded modules — where `JS_NewCModule` registers — before consulting a loader.
  A loader is only needed for the *script's* own files, and `src/Script.cpp` has the minimal one.
- **A runtime used from more than one thread needs `JS_UpdateStackTop` on every entry, or its stack
  guard is pointing at another thread's stack.** `rt->stack_top` is captured when the runtime is
  created; `js_check_stack_overflow` compares the current SP against it. Use that runtime from a
  second thread and the comparison is against an unrelated range, so the guard never fires and a
  deeply nested `JS_ParseJSON` recurses until the process dies — `0xC00000FD`, reachable from the
  network, since update `0x67` carries a payload any peer can author. The header says it plainly
  ("should be called when changing thread"); it is easy to read as advice about *migrating* a
  runtime rather than about alternating between threads. `src/Json.cpp` calls it under its lock on
  every operation and sets `JS_SetMaxStackSize` to 256 KB, because the default megabyte is more
  headroom than either game thread has left by then. A 200k-deep payload is in the harness.
- **A duplicated name in an export list must be grepped for, because nothing checks it any more.**
  `imgui-quickjs` listed `JS_ENUM_DEF(SortDirection)` twice, which cost nothing until something did
  `import * as ImGui from "ImGui"` — building a namespace object rejects duplicate exports, so the
  whole module failed to link and the host reported "duplicate exported name 'SortDirection'". That
  check is **gone**: `js_imgui_funcs` is now instantiated with `JS_SetPropertyFunctionList`, which
  happily sets the same property twice. A long hand-maintained `JSCFunctionListEntry` array
  therefore needs a duplicate check, not review: `awk` the array out of the file,
  `grep -oE 'JS_(CFUNC_DEF\("[A-Za-z0-9_]+"|ENUM_DEF\([A-Za-z0-9_]+)'`, `sort | uniq -d`.

JavaScript naming, which is not the C++ naming: `snake_case` for methods and functions
(`set_target`, `attack_position`), `PascalCase` for classes and types (the `JSClassDef::class_name`
strings — `Actor`, `Role`, `Actors`, `Roles`), `camelCase` for local variables in JS, and
`snake_case` for data properties and accessors (`max_distance`, `text_color`, `strength_ratio`).
`goto` keeps the engine's name for `MobileActor` slot 88; reserved words are barred as identifiers,
not as member names, so `actor.goto(dst, 1.0)` parses.

Do not put `src/` on an include path. `quickjs.h` does `#include <math.h>`, which resolves to
`src/Math.h` on a case-insensitive filesystem and produces a wall of errors from `<cstdlib>`. The
build never does this (sources are compiled by full path and include each other relatively); a
throwaway harness that adds `-I src` will.

## Git Workflow

**Commit directly to `main` when working in the primary checkout.** This overrides the default
"if on the default branch, branch first" behavior — do not create a branch and do not ask.

Only work on a separate branch when inside a dedicated git worktree, which will already be on
its own branch (see the worktree build notes: `VCPKG_ROOT` is unset there, so reuse the main
build's `vcpkg_installed` with manifest install disabled).

Committing still happens only when explicitly asked.

### Shell quoting for commit messages

The Bash and PowerShell tools need different multi-line quoting, and mixing them up produces a
partially-executed command rather than a clean error:

- Bash: `git commit -F - <<'MSGEOF' … MSGEOF`
- PowerShell: `git commit -m @' … '@` with the closing `'@` at column 0

Also: prefix `python3` with `PYTHONIOENCODING=utf-8` when printing non-ASCII on Windows, and use
forward-slash paths inside Bash heredocs (`'\\'` gets collapsed). `/tmp` does not exist — use the
scratchpad directory.

Use the `Edit` tool for text substitution in files, never a PowerShell read-replace-write:
`Set-Content -NoNewline` rejected its own parameter in this shell, and the round-trip rewrites line
endings across the whole file.

Stage with a path glob (`git add src/Js*`), not a hand-typed file list — a 17-file list silently
dropped `src/JsGk.cpp` and would have committed a JS binding layer with no module registration.
