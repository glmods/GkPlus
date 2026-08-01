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
python blender/tests/test_rim.py "<Gunlok dir>"         # all 513 textures, ~20 min
```

Two need Blender itself, and take the scene round trip through a real `.blend`:

```bash
blender --background --python blender/tests/test_scene.py -- "<Gunlok dir>" [N|all]
blender --background --python blender/tests/test_authoring.py -- ["<Gunlok dir>"]
```

`test_scene.py` defaults to a sample rather than all 563 — pass `all` for the full run.

The `rimutil` pair takes the built exe first:

```bash
python utils/rimutil/tests/test_decode.py <rimutil.exe> "<Gunlok dir>"
python utils/rimutil/tests/test_encode.py <rimutil.exe> ["<Gunlok dir>"]
```

Lint is `uv run --group dev ruff check .` from `blender/`; the type check is
`npx -y -p typescript tsc -p types/tsconfig.json` (see "Type definitions").

### Runtime-testing outside the game

Nothing in `src/` can be exercised outside Gunlok: `GetBaseAddress()` derives from the host exe's
entry point, so every native-API call faults in a standalone process. To runtime-test a layer that
does not itself touch game memory (the `src/Js*` bindings and the script host are the cases in
point), compile it into a throwaway 32-bit exe alongside a stub TU that replaces the `gk::` natives
with fakes:

```
clang-cl -m32 /EHsc /MD -clang:-std=c++23 -Wno-invalid-offsetof -Wno-deprecated-declarations \
  "/Ibuild/vcpkg_installed/x86-windows-static-md/include" "/I." "/Iimgui-quickjs" \
  main.cpp stubs.cpp src/JsCommon.cpp src/JsMenus.cpp src/CustomMenu.cpp src/Script.cpp \
  imgui-quickjs/imgui-quickjs.cpp /Feharness.exe /link \
  build/vcpkg_installed/x86-windows-static-md/lib/{qjs,imgui,detours}.lib
```

Do **not** put `src/` on the include path (see the include collision under Conventions) — put the
**repo root** there instead and include as `"src/Menu.h"`, which is what `-I .` above is for; the
`src/*.cpp` files still resolve their own siblings through quoted includes. The real project flags
live in `build/CMakeFiles/impl-Debug.ninja` (`FLAGS =`) if they drift. Always add a
deliberately-failing assertion once and confirm the harness reports it — a harness that cannot fail
proves nothing.

Two things make the script host reachable at all:

- The harness must **not construct the `*System` objects** — their ctors resolve offsets off a fake
  base and hand them to Detours. Both subsystems therefore expose their hook bodies as ordinary
  functions (`BootScriptHost`, `ReconcileCustomMenu`, `DispatchCustomMenuClick`) and the `*System`
  ctor is only the detour. Drive those directly.
- Supply the harness's own `gk::js::RegisterGkModule` and simply do not compile `JsGk.cpp`. The
  trimmed module registers just the namespace under test, which is what keeps the actor/role/token
  stubs (and the whole pure-virtual scrape below) out of a menus-and-host harness. Building nodes
  for a `List<T>` also needs `template <typename T> List_Member_Base<T>::~List_Member_Base() {}` —
  the pure virtual dtor is declared but never defined in the DLL, because nothing there ever
  constructs a node.

Four more things that cost time the first time round:

- **Run the compile from the PowerShell tool, not Bash.** MSYS rewrites every `/`-prefixed flag into
  a path (`/EHsc` becomes `C:/Program Files/Git/EHsc`), and clang-cl silently forwards the wreckage
  to the linker as input files — so the build *appears* to work until it links, and `/EHsc` `/MD`
  were never applied. Include the src files by **absolute** path from the harness directory; quoted
  includes still resolve each header's own siblings.
- **Stub actors need every pure virtual.** Generate them: scrape `virtual … = 0;` out of
  `src/Actors.h` into per-class `#define`s of `override` bodies, paste those into each stub class.
  Emit the parameter list verbatim — both `char *script` and a bare `float *` are legal in a
  definition, so there is nothing to rename. The generated slot counts are a free cross-check
  against `actor_vtable_notes.md` (82 + dtor / 12 / 5 / 5 / 3).
- **`Actor::~Actor()` is pure and undefined**, and `Inventory` is only ever forward-declared. Both
  are fine in the DLL because nothing there instantiates or destroys an actor; a harness that does
  must supply `gk::Actor::~Actor() {}` and a harness-local `struct Inventory {}` (otherwise
  `pool_unique_ptr<Inventory>`'s deleter fails to instantiate on an incomplete type).
- Have the stub hierarchy **restate the class tree independently** of `src/ActorClasses.inc.h`
  rather than including it. If the two disagree the kind/`instanceof` tests fail, which is the
  point of testing the ladder at all.

### Debugging the running game

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
`CustomMenuSystem`, `ScriptQueueSystem`, `gls::GlsSystem`, `CustomLevelSystem`,
`ScriptSystem`.
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
`src/Map`, `src/Vulnerability` (header-only), `src/Music`, `src/Math`, `src/Menu`, `src/Tokens`,
`src/World` (sun angle/brightness/direction, ambient light and the fog state behind `FogSystem`,
whose null-ness is the "is a level loaded" test),
`src/Triggers`, `src/Console`, `src/Misc`, `src/Camera`, `src/Debug`, `src/GUI`, `src/InputFix`,
`src/CustomMenu`, `src/ScriptQueue`, `src/CustomLevel`, `src/Script`, `src/Session` (starting a
level without the menus, see below), `src/MakeRole` (native constructors, see below),
`src/FileHooks` (mod loading, see below), `src/Render` (the AWAPI renderer, see below).
`src/GLS.h/cpp` is the model the rest now follow. The behavioral-hook subsystems (`Music`, `Debug`,
`GUI`, `InputFix`, `CustomMenu`, `ScriptQueue`, `GLS`, `CustomLevel`, `Script`, `FileHooks`) expose a
`*System` RAII class constructed by `entry.cpp`; the others are pure struct + native-API.

`FileHookSystem` is the one that does not resolve *offsets* at all: it patches gl.exe's import table
and detours two functions in the exe's private CRT copy, and its lookup half (`src/Vfs`) touches no
game memory whatsoever.

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

### Script host (`src/Script.h/cpp`)

One `JSRuntime`, one `JSContext`, one entry module. The entry module is `GKPLUS_SCRIPT` if that
environment variable is set, otherwise **`gkplus\main.mjs` next to `d3d8.dll`** (i.e. in Gunlok's
directory). A missing file logs the path it looked for and leaves the game unmodified. An
**empty** one does not: it loads, evaluates, exports nothing, and looks exactly like a working
host that happens to do nothing — check the file is non-empty before debugging the bindings.
`examples/main.mjs` is a working starting point.

```js
export function setup_menus(menus) { menus.Main.add_item("Hi", (item) => {}); }
export function draw_gui(ImGui)    { if (ImGui.Begin("x")) { … } ImGui.End(); }
```

Four facts pin the design, in decreasing order of how expensive they were to find:

- **The boot point is a detour on `SetupMenus` @ 0x004e95e0**, not `DllMain` and not the first
  frame. `WinMain` calls it exactly once, after `LoadResourceStringTable` (0x0046b355) and
  `InitConsole` (0x0046bb81) and before the frame loop (0x0046be47) — so at boot the localized
  strings and `gk::Print` both work, and the game's own menus are already populated. That last
  part is load-bearing: `OnMenuItemClicked` switches on the item *index*, so a script item added
  before the game's would shift every index in the dispatch table.
- **`ImGui` is not a module — it is only `draw_gui`'s argument.** Every call in it is valid only
  between `NewFrame` and `Render`, which is exactly the window `draw_gui` runs in, so
  `js_imgui_new_namespace` (imgui-quickjs) builds a plain object with `JS_SetPropertyFunctionList`
  and `BootScriptHost` hands it over. Making it importable would let a script call it from module
  scope or `setup_menus`, where it cannot work. The cost is that a duplicate name in
  `js_imgui_funcs` is now *silent* — see the export-list convention below, where the `grep`
  recipe is the only remaining guard.
- **`menus` is the same story** — it is `setup_menus`' argument and not a `"gk"` export, because
  the game's own items must already be in place when a script adds one. The host builds it with
  `js::NewMenusNamespace`.
- **`LinkGkModule` imports `"gk"` at boot purely as a check**, and keeps nothing. A C module is not
  linked until something imports it, and its export list is only validated then — a duplicated name
  fails the link with a `SyntaxError`. Doing the first import at boot reports that as
  `gkplus bootstrap` rather than against whichever script imported `"gk"` first. (It used to also
  fetch the namespace for `menus`; if you ever need a C module's namespace from C, note that
  `JS_GetModuleNamespace` on an un-imported module does not return undefineds —
  `js_build_module_ns` falls through to the module's unset `func_obj` and crashes.)
- **Module evaluation returns a promise even without top-level await**, so `Await()` pumps
  `JS_ExecutePendingJob` until it settles, and reports a stuck pending promise instead of spinning.
- **The entry module's name uses forward slashes.** QuickJS's default normalizer resolves a
  relative specifier by scanning the *importing module's name* for `/`; with `C:\…\main.mjs` it
  finds none and `import "./x.mjs"` silently resolves to `x.mjs` in the process's cwd.

**The host loads exactly one module, and the bindings load none.** `levels.add` used to take a
module *path* and pull the file in itself, through a `LoadScriptModule` host service; it now takes
the description object, and a script reaches a level module with an ordinary
`import * as arena from "./levels/arena.mjs"` — the namespace is already the right shape. That
deleted the last binding-side module load, so `Script.h` exports only `BootScriptHost`. If a
binding ever does need to load a script file from C, the answer is
`Await(ctx, JS_LoadModule(ctx, EntryPath, specifier))` — QuickJS's own C-side `import()`, which
normalizes, runs the loader, links, evaluates and resolves with the namespace — with the **entry
module's path** as the base name, which is what makes a relative specifier resolve next to
`main.mjs` rather than against the cwd the game keeps moving. Do **not** open-code it by evaluating
a synthetic wrapper module that does the import; that is what the deleted version started as.

Two seams in `src/GUI.h` carry it: `SetOverlayDrawCallback` (inside the ImGui frame, F11 only)
runs `draw_gui`, and `SetFrameCallback` (once per `PresentScene`, overlay or not) drains the job
queue and then pumps the REPL channel. Both are installed by `BootScriptHost`, not by the
`ScriptSystem` ctor, so they can never call into a context that does not exist yet. Everything runs
on the main thread.

One runtime, but not necessarily one context: `StartRepl` adds a second one for the debug channel
(see "The REPL channel"). It is started *before* `LoadEntryModule`, because a REPL is most useful
exactly when `main.mjs` is missing or throws — both of which make `BootScriptHost` return early.

Nothing a script throws reaches game code: every seam ends at `gk::js::ReportException`, which
prints the message and stack to the console and the debugger. A `draw_gui` that throws is
**disabled for the session** — once per frame forever is a flood, not a diagnostic, and a script
that threw mid-frame has usually left ImGui's stack unbalanced.

**There are no host globals at all.** QuickJS core provides no `console` (that is quickjs-libc,
which this port does not install) and GkPlus deliberately does not add one — `log`/`info`/`warn`/
`error`/`debug` live on the `"gk"` module's `console` beside the game's own `print`, colours and
`execute`, so there is exactly one console object and a script reaches it the way it reaches
everything else. `js::Log` is still the C-side sink for all of it, including `ReportException`.

### The REPL channel (`src/Repl.h/cpp`)

A loopback socket that evaluates JavaScript in the running game. **Off unless
`GKPLUS_REPL_PORT` names a port**, and bound to `127.0.0.1` only — it executes
arbitrary code in the game process, and the game already takes attacker-authored
payloads off the network (update `0x67`), so a wildcard bind would be remote code
execution on whoever is playing. One line of NDJSON each way, UTF-8:

```
-> {"code": "actors.count", "id": 7}       `id` optional, echoed back
<- {"ok": true, "value": "37", "id": 7}
<- {"ok": false, "error": "TypeError: ...", "stack": "..."}
```

#### Using it

Set the port in the environment the game inherits, and launch `gl.exe` directly —
Steam will not pass the variable through:

```
GKPLUS_REPL_PORT=9222 "<Gunlok>/gl.exe"
```

The listener opens during `SetupMenus`, about a second in, and logs
`repl: listening on 127.0.0.1:9222` to the game console. Then anything that speaks
TCP will do:

```
printf 'actors.count\n' | nc 127.0.0.1 9222
```

**A line that is not a JSON object with a string `code` is treated as source**, so
one-liners work with no quoting ceremony. Multi-line source has to ride in the
object form, because a newline is the frame delimiter:

```
{"code": "for (const a of actors) if (!a.alive) console.print(a.id);\nactors.count", "id": 3}
```

Everything the `"gk"` module exports is already a global — all 22 namespaces,
enumerated at boot, plus the default export as `gk` — so there is nothing to
import and no host object to reach for:

```
actors.count                       // 158
[...actors].filter(a => a.alive).length
roles["gunlok"].id
game.simulation_running            // the authority test, false on a joining client
levels.start("level01.gls")        // into a level from the menu, no keystrokes
console.print("hello")             // goes to the game's own console
```

Evaluation is **global scope, not module scope**: the expression's value comes
straight back, and `var`/`function` persist between lines, so a session
accumulates state. The cost is that `import` is a syntax error there — which
costs little, since the prelude has already put everything importable in scope.

**Do not expect a relative dynamic `import()` to work from the REPL.** Not
measured, but it follows from the normalizer rule documented under "Script host":
QuickJS resolves a relative specifier by scanning the *importing* script's name
for `/`, and the REPL evaluates under the name `<repl>`, which has none — so
`import("./x.mjs")` should land against the process's current directory, which
the game moves around during a level load, rather than next to `main.mjs`. A
bare specifier needs no normalizer and should be fine. If you need this, measure
it first and replace this paragraph with what you find.

Worth knowing before leaning on it:

- **A snippet runs on the game's own thread, inside the frame callback.** It can
  see and mutate live game state, and while it runs the game does not advance. A
  `JS_SetInterruptHandler` deadline throws after 5 s so a runaway loop cannot wedge
  the process — but it **cannot interrupt a native call**, only JavaScript.
- **`ImGui` is not reachable**, by construction: the frame callback runs outside
  the ImGui frame, so those calls would be invalid. `draw_gui` is where they live.
- **It shares the runtime with `main.mjs`** but has its own context, so `actors`
  at the socket is the very collection the entry module sees, while globals seeded
  here never leak into the script's `globalThis`.
- Limits, all of them there so a wedged client degrades into a dropped connection
  rather than an unbounded allocation: 4 concurrent connections, a 1 MiB cap on a
  single un-terminated line, 8 MiB of unread replies, and results over 64 K
  characters are elided with a note.
- **Replies are formatted, not round-trippable.** `value` is display text —
  `JSON.stringify` where that works, `[object Object]` where it throws (a circular
  structure, a getter that raises). Do not parse it.

Verified against a running game in all four states — front-end menu and in-level,
window focused and unfocused — and offline by the harness described at the end of
this section.

Five things pin the design:

- **No thread, because the frame callback already exists.** `StartRepl` only opens
  the listener; `PumpRepl` accepts, reads, evaluates and writes from `OnFrame`,
  on the thread that owns the runtime. That is why none of `src/Json.cpp`'s
  locking or `JS_UpdateStackTop` applies here. A blocking `accept` on a worker
  would buy nothing — every snippet would still have to be marshalled back to the
  main thread to run — and would cost a second thread on the host `JSRuntime`,
  which is the hazard `Json.cpp` exists to work around.
- **`PresentScene` is not a sufficient heartbeat, because Gunlok stops running
  frames entirely while its window is inactive** — which is exactly the state a
  debug channel is used in, since you are typing in a terminal. All of this is
  measured on a running game, not inferred:
  - `OnActivateApp` @ 0x0046f400 clears `HasWindowFocus` @ 0x006a3744 on focus
    loss and calls `FUN_00574960`, which releases the D3D resources and clears
    `DAT_007c1230`;
  - `RenderSceneAndPresent` @ 0x00574c50 wraps its whole body — scene,
    `EndScene`, the `PresentScene` call — in `if (DAT_007c1230 != 0)`, so the
    frame hook never fires;
  - yet the process still spins a full core and `SendMessageTimeout(WM_NULL)`
    answers in 7 ms, so the main thread is in a loop that pumps messages.

  **How much else stops depends on where you are, and only the gate is
  constant.** At the front-end menus both per-thread clock accumulators
  (`DAT_007c07e8` main, `DAT_007c07b8` executor) read **+0 over four seconds** —
  the game is not merely unrendered, it is not running. In a loaded level the
  same two clocks keep advancing (~10,300 per 3 s each, measured unfocused), so
  the simulation continues and only rendering is gated. `PresentScene` is
  unreachable either way, which is the part that matters for the heartbeat; do
  not generalise the menu measurement into "the game pauses on focus loss".

  A message is therefore the only way in. `SetFrameWakeupEnabled` (GUI.h) puts a
  **WM_TIMER** on the game window, handled in the existing `HookedWndProc`, which
  runs `RunFrameCallback` on the main thread ~50×/s regardless of focus. Verified
  A/B: with `HasWindowFocus` 0 and the gate 0, the previous build timed out and
  this one answers 12/12 at 10–37 ms. **WM_TIMER rather than a thread posting
  `PostMessage`**, because `StopRepl` runs from `DllMain(DLL_PROCESS_DETACH)` and
  waiting on a worker thread under the loader lock deadlocks; `KillTimer` is a
  plain USER32 call and is safe there.

  Two traps cost most of an afternoon here, both worth keeping:
  **reachability is not execution** — the front-end frame `FUN_0046eae0` *does*
  call `RunGameFrame()`, so a caller-graph search says the front end reaches
  `PresentScene`, when the call inside is gated off; read the guard, don't trust
  the edge. And **the state that looks like "front end vs in-game" was window
  focus all along** — the same menu screen answers or doesn't depending only on
  whether the window is active, so sample the condition rather than the scenario.
- **Its own `JSContext` on the host's runtime.** Same runtime is the same object
  graph — `actors` at the socket is the collection `main.mjs` sees — while the
  separate context keeps the globals it seeds off the entry module's
  `globalThis`, so "there are no host globals" stays true for scripts. The cost is
  per-context prototypes: `RegisterGkModule` runs again on the REPL context
  (`RegisterClass` in `JsCommon.cpp` is already a no-op for the runtime half), and
  a wrapper is branded by whichever context minted it.
- **Global-scope eval, not module.** It hands the expression's value straight back
  and lets `var`/`function` persist between lines, which module scope does neither
  of. `import` is the thing given up, and the prelude covers it by enumerating
  `import * as ns from "gk"` onto `globalThis` — enumerated, not listed, so a
  namespace added to `JsGk.cpp`'s table appears without anyone remembering.
- **One rule decides what a line is**: an object with a string `code` is a
  request, anything else is source. That keeps `nc 127.0.0.1 <port>` usable for a
  one-liner (`actors.count` is not JSON at all; `1` is JSON but not an object)
  while multi-line source rides as `{"code": "..."}` with its newlines escaped.
  The one ambiguity is a JS object literal that happens to have a string `code`
  property.
- **`JS_SetInterruptHandler` on a deadline**, armed only around a REPL eval, so a
  runaway loop throws instead of freezing the game. It cannot interrupt a *native*
  call that hangs — only JavaScript.

`JSON.stringify` **throws** on a circular structure or a getter that raises rather
than returning `undefined`; the formatter needs both fallbacks, and a test
asserting only `ok: true` passes on the error path too.

This is one of the layers that *can* be exercised outside Gunlok (see
"Runtime-testing outside the game"): `Repl.cpp` reaches only `js::RegisterGkModule`,
`Log`, `ReportException` and `ReleaseCallbacks`, so a harness supplying those four
plus a one-namespace `"gk"` module drives the whole protocol over a real socket
with `PumpRepl` standing in for the frame hook.

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

### Native constructors (`src/MakeRole.h/cpp`)

The `ToXxx` converters re-expressed over plain description structs, so a game object can be
built with no `ParsedThing` in sight. One `Make*` per GLS section type that produces an
object — thirteen of them, covering every converter except `ToMap` (which `CustomLevel`
drives) and `ParseGLDirs` (which sets game directories rather than building anything):

| GLS section | Converter | Native |
|---|---|---|
| shape / hierarchy | `ToShape` / `ToHierarchy` | `MakeShape` / `MakeHierarchy` — pure `.rif` lookups |
| light | `ToLight` | `MakeLight` |
| projectile | `ToProjectile` | `MakeProjectile` |
| pgenerator | `ToParticleGenerator` | `MakeParticleGenerator` |
| character | `ToCharacter` | `MakeCharacter` |
| destructibility / frag data / *replace* | slot-8 trio | `MakeDestructibility` / `MakeFragData` / `MakeReplaceDestructibility` |
| role | `ToRole` | `MakeRole` |
| ammo / ammo info | `ToAmmo` / `ToAmmoInfo` | `MakeAmmo` / `MakeAmmoInfo` — write the global tables |
| camera track | `ToCameraTrack` | `MakeCameraTrack` — needs a loaded level, and both `name` and `file` |

**Descriptions are in `.gls` units** (degrees, seconds, metres, cycles/sec), because that is
what a `ParsedThing` holds: `CheckValue` only range-checks and stores, and every conversion
lives in the converter. Each `Desc`'s defaults are its section constructor's own, read out of
the `.rdata` constants rather than transcribed.

Five conversions carry real risk and are the reason this is RE work rather than field copying:

- **Angles are BAM** — 4096 to a turn, what indexes the sin/cos tables — and `ToCharacter`
  uses **two association orders** for the same conversion (`(d/360)*4096` for scan angles,
  `(d*4096)/360` for aim/sight/yaw/elevation). Same value mathematically, not always the same
  float, so `MakeRole.h` exposes both.
- **`walking_speed` is 16.16 fixed point rounded twice** — `round(cycles * 65536)`, then if
  the character turns, that *already-rounded integer* goes back to float, divides by `size`,
  re-scales and rounds again. Rounding is `FISTP` under the default control word: nearest-even.
- **A GLS `radius`/`height` of 0 means "use the model's bounding box"**, and `ToRole` — not
  `ToCharacter` — computes it. Skipping it yields characters with no collision extents.
- **`Role::flags` packs ten booleans** in a fixed bit order, and `alpha_fogging` forces
  `per_vertex_fogging` off rather than reporting a conflict.
- **Particle TTL is converted at the *calling thread's* clock rate** — client and executor
  keep separate ones.

Two things deliberately not reproduced: `ToRole`'s leak on the beam-script error path
(`MakeRole` refuses up front instead), and the converters' habit of leaving allocations
partly uninitialised — everything here zeroes first, which is also required because several
mirrors carry `pool_unique_ptr` members that cannot start from garbage.

### Building game objects from script (`src/JsMake.cpp`, the `make` namespace)

The `"gk"` module's `make` namespace is the native constructors above, exposed to JS. It is
how the shipped `.gsh` headers get re-implemented as `.mjs` modules — no `ParsedThing`, no
parser, and a definition costs a few dozen bytes rather than 0x1b60:

```js
const role = make.role({
  identifier: "bug",
  hierarchy: { rif: "units\\bug.rif", object: "bug", hotspot: "head" },
  character: { walking_speed: 1.5, strength: 1, aggression: 0.1, weapon: "enemy laser weak" },
  ai: "background creature",
  destructibility: { kind: "explode" },
});
```

Four things decide the shape:

- **One call builds a whole role.** A Character, Light, Projectile, pgen or Destructibility
  becomes *owned* by the Role (`RoleDtor` pool-frees all six), so handing the same one to two
  roles would double-free it at level teardown. Describing them inline makes that
  unrepresentable rather than merely discouraged. Shapes and hierarchies are the exception —
  the rif cache owns those — so `make.shape` / `make.hierarchy` hand back reusable handles.
- **`make.role` registers as it builds**, and `DestroyRoles` clears the hash between levels,
  so a header module exports *functions* and a level's `define` hook calls them once per load.
  There is no `register()` step and no conversion cache to reset.
- **Enum fields take keywords**, resolved through the tables recovered by probing (`ai`,
  `weapon`, `secondary weapon`, `ammo type`, `weapon type`, `action on death`, `resistance`,
  particle `type`). A field whose table is not recovered — `interface beam effect` — says so
  and takes the number.
- **Ranges are still checked against the game's own bounds.** `gls::FindField` reads
  `min_values`/`max_values` off the section constructor, so `make` reports the same limits
  `CheckValue` would have, without going through it.

GLS inheritance has no equivalent and needs none: `child : parent` becomes object spread,
which does the merge at authoring time instead of inside the game. `abstract` likewise — a
description is a plain object until something calls `make.role` on it.

### What only the parser can answer (`src/JsGls.cpp`, the `gls` namespace)

Everything else moved to `make`; `gls` keeps the three things a reimplementation cannot
provide, all of which run the real parser:

- **`gls.schema(section)`** — the field table each section constructor declares *about
  itself*: `field_types`, `field_names` (the GLS keyword), `field_satisfied` (false =
  required) and `min_values`/`max_values`. `gls::SectionFields` builds it by constructing a
  throwaway instance and reading it, so no hand-maintained table can drift from the binary.
- **`gls.probe(section, field, names)`** — what integer a GLS enum keyword stands for.
- **`gls.try_parse(source)`** — does this text parse, for bisecting one that does not.

All three inherit the parser's hazards: destructive global state (never during a level load)
and the poisoning described above.

**The enum keyword tables were recovered by asking the parser.** `ai bot` and friends are
compiled into the flex DFA — not stored as strings, absent from every shipped header — so
`gls.probe(section, field, names)` builds a one-field section per keyword in memory, parses it, and
reads `parsed_values[field]` straight back. `ai`, `weapon type`, `ammo type`,
`action on death` and `resistance` now accept names (tables in `src/Roles.cpp`); `type` still
takes a number, because that id is shared by `destructibility` (0..1) and `pgenerator`
(0..12) and the binding cannot tell which section it is being asked about.

Two independent checks passed: `ai` reproduced all 21 values of `AIType` in order, and
`destructibility type` reproduced `DestructibilityKind`. Full tables, declared bounds and
what is still unknown are in `gls_system_notes.md`.

**One parser fact that outranks the rest: a syntax error poisons `LoadGLS` for the whole
process.** It resets its error counter, `ParsedObjList` and the symbol tables on entry, but
not the file stack, and nothing afterwards recovers — a verbatim copy of a shipped section
fails identically. Anything making repeated `LoadGLS` calls (`gls.probe`, `gls.try_parse`,
a level's `includes`) has to treat the first failure as terminal.

### Both queues carry one JSON envelope (`src/ScriptQueue.h/cpp`)

Gunlok has exactly one channel for "something happened, react to it", and it moves **file names**:
a trigger fires, `QueueScriptExecution` @ 0x00505080 pushes a `.gcs` name onto `ScriptQueue`
@ 0x007ba35c *and* broadcasts it as update `0x67`, and every machine runs its own local copy of
that file. It has a second, unrelated queue for console commands: `CommandsToExecute` @ 0x007b6aa8,
holding one strdup'd line per node, popped one per frame. `ScriptQueueSystem` redefines **both**
payloads — and the fields the first comes from — as one JSON object:

```json
{"kind": "file" | "command" | "message", "body": <contents>}
```

- **file** — `body` names a `.gcs`, run exactly where and when the engine would have run it.
- **command** — `body` is a console command line. Every entry on `CommandsToExecute` is one of
  these; on the script queue it is a command replicated to every machine.
- **message** — `body` is anything, handed to `SetScriptMessageHandler`'s handler instead of to
  the file system. `CustomLevel` installs `DispatchCustomLevelMessage`, which is what makes a
  script level's `message_received` fire. The handler receives the **body**, so the envelope is
  invisible from JS.

A kind this build does not know is still a well-formed envelope: reported and dropped, never
opened as a file.

**The envelope is why the format is an object rather than a bare value.** It used to be "a JSON
string is a file name, anything else is a message", which mis-read a `.gcs` literally called `123`.
The test is now "is this an object with a string `kind` and a `body`?" (`gk::json::OpenEnvelope`),
and a colliding file name would have to contain `"` and `:` — both illegal in a Windows path. What
is not an envelope is a bare name, with no residual doubt. `gls.probe`-style content inspection is
gone, and so is the marker byte that preceded it.

**This is visible on disk and deliberately breaking**: `SaveGame` serialises a trigger's script name
verbatim *and* walks `CommandsToExecute` writing each pending line, so a save written by this build
carries envelopes in both and will not restore correctly in an unpatched Gunlok (see
`save_system_notes.md`). Reading an older save still works, through the residual path below.

**The wrapping happens where the value is written, not where it is queued.** Nine hooks: four
*writers* that wrap the bare name the engine hands them, so a script-name field holds an envelope
from the moment it is set, and five on the two queues.

| Hook | Address | Role |
|---|---|---|
| `RegisterTriggers` | 0x0043e240 | writer: `TriggerData::script_name`. Covers all 23 game-side registrations — 21 branches of `CommandAddTrigger`, plus `LoadLevel` and `Frag` |
| `PickupActor::Associate` | 0x005469f0 | writer: `associated_script`. The only implementation that stores — `Actor::Associate` @ 0x0054e640 is a `RET 0x8` stub |
| `ToRole` | 0x0047cc20 | writer: `Role::interface_beam_script`, which `AddInterfaceBeamVulnerability` later copies *by pointer* into `Vulnerability::script` |
| `ToReplaceDestructibility` | 0x0047eaa0 | writer: `ReplaceDestructibility::script` |
| `CommandBatchAndBroadcast` | 0x00448400 | **replaced**: five calls reproduced, name wrapped |
| `MultiplayerRespawnRole` | 0x0050c8b0 | **replaced**: wraps its `.gcs` name, drops the 15-byte leak. `FUN_00511600` is `__thiscall` on `RespawnRoleList` @ 0x007b9d98 — the decompiler hides the ECX `this` |
| `CommandVulnerability` | 0x0044a600 | wrapped, then every `Vulnerability::script` swept and wrapped |
| `QueueScriptExecution` | 0x00505080 | guards the invariant for what the above do not cover |
| `RunQueuedScript` | 0x00505310 | host consumer: arms the payload window, runs the original |
| `ApplyUpdateMessage` | 0x004fde70 | joiner consumer: same, for the `case 0x67` arm inside it |
| `ExecuteCommandFile` | 0x0043f250 | consumes a script-queue payload; also **sweeps** whatever it just appended to `CommandsToExecute` into `command` envelopes |
| `PumpQueuedConsoleCommand` | 0x004d6120 | **replaced**: the console queue's consumer, see below |

**The console queue is read through the envelope, never written to it.** Its nodes keep the plain
lines the game put there, and the replaced consumer treats a bare line as `kind: "command"` — the
branch that had to exist for old savegames anyway. So both queues share one set of meanings and one
code path, while `CommandsToExecute` keeps byte-for-byte the representation vanilla Gunlok gives it.

- **An earlier design rewrote each node into a literal envelope, and it crashed the game.** After
  every `ExecuteCommandFile` the queue was swept, replacing each payload via the game's `strdup`
  and `pool_free`. It faulted as a wild call — EIP on the stack, so WER blamed "module: unknown" —
  in roughly two runs out of three, from inside the sweep's own `pool_free`. The lesson is not
  "that had a bug": a subsystem that rewrites another allocator's live objects on a hot path buys
  very little and risks everything, and all the rewrite bought was making the bytes on the queue
  *look* like the bytes on the wire.
- **Not writing to it costs nothing**, because the `#` directives, front-insertion, `CLEAR BATCH`,
  `NumCommandsToExecute` and `SaveGame`'s serialisation of pending lines all keep working by being
  left alone — and a save written by this build stays readable by vanilla Gunlok.
- **Consuming still had to be a replacement**, and that is a measurement, not a preference.
  `PumpQueuedConsoleCommand` copies a node's string into `ConsoleCommandLine` (`char[252]`
  @ 0x007b6958) with an **unbounded** byte loop at 0x004d61f0, and the next global is
  `ConsoleSmallFont` @ 0x007b6a54 with nothing in between. An envelope adds ~28 characters to a
  line `fgets` already allows 249 of — `level06.gcs` ships one 399 characters long — so letting
  the game pop an envelope would write JSON through a font pointer. The replacement decodes first
  and puts only the *body* in that buffer, truncating (with a note) if it still does not fit.
- **The replacement reproduces all three gates exactly** — the `WAIT` deadline, `REAL WAIT` with
  its TAB cancel, and `WAIT FOR` with the `REPTXT` re-print — plus the `LevelLoadReason == 3`
  bypass. Every comparison in the original is a signed test on the high dword then an unsigned one
  on the low (`JL`/`JG`/`JC`), which is what a signed 64-bit `<` compiles to, so they are written
  as ordinary comparisons. It differs from the original in one way, and it is a fix: it returns on
  an **empty** queue, where the original walks into the sentinel and reads a payload off a
  0xc-byte `List_Member_Base` that has no `data` — harmless only because both of its callers test
  `NumCommandsToExecute` first.
- A queue entry that is **not** an envelope is the normal case, not an exception: it is a plain
  line the game queued, run as the console command it always was. Old savegames land here by the
  same rule rather than through a special case.

Six things decide the shape of the script queue's half, in decreasing order of how much they
constrain it:

- **The script queue's consumers are not reimplemented — they are bracketed.** Both pop the payload themselves and
  hand it straight to `ExecuteCommandFile`, so each is wrapped in a *window* that marks provenance
  and the `ExecuteCommandFile` hook interprets whatever arrives inside one. Replacing either body
  instead would mean duplicating `MsgQueue_Pop`, the `SetCurrentDirectoryToGLDir(GL_Scripts)` dance
  and the `pool_free` of the popped buffer — the payload comes from `malloc` @ 0x005e3f72, which is
  the pool thunk, so `RunQueuedScript` frees it with the pool `free` @ 0x005e3f7b. That the window
  is unambiguous is *measured*: `RunQueuedScript` is 13 instructions with one
  `ExecuteCommandFile` call, and `ApplyUpdateMessage` contains exactly one (0x004ff971), reachable
  only from `case 0x67`, with none of its 164 direct callees reaching it transitively
  (`directplay_protocol_notes.md` §8.11). `ExecuteCommandFile` has six callers in all, and the
  other four — `LoadLevel`'s level `.gcs`, the console's own local `BATCH`, and two briefing-screen
  ones — are outside both windows. The window is also **one-shot**, so a payload that goes on to
  run a batch file with `#EXECUTE IMMEDIATELY` cannot have that file re-read as a payload.
- **Neither side guesses what it is holding.** A game-side write is always a bare name — a console
  argument or a GLS field — and GkPlus's own writes arrive inside an `EncodedPayloadScope`, a
  call-scoped "already a document, pass it through" that `triggers.create` and `actor.associate`
  wrap their engine calls in. That replaced an earlier **marker byte** in the stored value: once
  every writer encodes, there are no longer two representations to tell apart, so the marker had
  nothing left to do. Content inspection was the design before *that*, and it is wrong in a way no
  care fixes — a `.gcs` legitimately called `{a}.gcs` reads as an object.
- **`ToRole`'s cache check is load-bearing.** It early-returns the `Role` it cached at
  `parsed+0x1b60` (its 13th instruction, 0x0047cc50 — 0x1b60 is `sizeof(ParsedThingBase)`), and
  nested conversions *do* call it again, since `ToFragData` builds `role` and `replace_role` through
  it. Reading that slot before calling the original is what stops the field being quoted once per
  call. `ToReplaceDestructibility` needs no such guard: it pool-allocs a fresh record every time.
- **A site that queues without storing has to be replaced, not hooked.** `CommandBatchAndBroadcast`
  (5 calls) and `MultiplayerRespawnRole` (3, plus an error path) are small enough to reproduce, so
  they are — encoding the name and dropping the allocations both originals leaked.
  `CommandVulnerability` is 1369 bytes of argument parsing that also fans one `Vulnerability*` out
  to every actor of a role, so it is wrapped and its result **swept** instead: any vulnerability
  left holding a non-document gets encoded. A sweep rather than a before/after diff because the
  diff would have to assume the pool never returns the address it just freed, which that command
  does free; "is it encoded yet?" holds regardless of how the value got there.
- **The engine's own events still queue their stock `.gcs`** — `CTFRespawn.gcs`, `RTPRespawn.gcs`,
  `CaptureFlag_team<N>.gcs` — now properly encoded. GkPlus sends no messages of its own; a script
  hears from the queue only what a script put there. `MultiplayerRespawnRole`'s replacement must keep
  appending to `RespawnRoleList` @ 0x007b9d98 exactly where the original did, because that is how the
  queued `.gcs` finds the actor to equip (see `threading_model_notes.md`).
- **`OnFlagCaptured` is left alone on purpose.** Its 760 bytes drive two `BroadcastToPlayers`
  payloads, five `TeamSlots` writes, a vtable getter/setter pair and a "Hark" special case;
  duplicating its team-to-script switch a few instructions earlier, to encode a name the queue hook
  encodes anyway, would only add a way to pick the wrong file. So it is the one *local* source that
  still reaches `ScriptQueuePayload` bare — alongside old savegames and peers without GkPlus, which
  this process cannot encode at all.
- **`ScriptQueuePayload` is that decision as a pure function**, and **its return value is always a
  valid JSON document** — the invariant everything else rests on. Being pure is what lets the
  harness assert it on every case, including the one documented ambiguity: a residual bare name that
  happens to *be* a document (a file literally called `123`) is passed through as one.
- **The payload is UTF-8; the engine is ANSI; `Encoding.h` is the seam.** Every `char *` the game
  holds is CP_ACP — a `.gls` is read as bytes, and `fopen` reads the codepage — so a name is
  transcoded on the way into a payload and back on the way to `ExecuteCommandFile`. Carrying the raw
  bytes instead *looks* like it works, because `gk::json`'s own decoder is byte-exact, and it was the
  first design; it breaks as soon as a name is embedded in a **message**, since QuickJS decodes that
  document and silently turns every invalid sequence into U+FFFD (the `UTF8_HAS_ERRORS` throw is
  commented out in `JS_NewStringLen`), handing the script a path that opens nothing. Transcoding also
  makes a *script*-authored non-ASCII path work for the first time — it used to reach `fopen` as
  UTF-8.
- **QuickJS is the codec throughout.** The script host's context writes a message
  (`ToScriptPayload`) and reads one (`OnMessage`); `gk::json` does the queue's own encoding on a
  **private runtime behind a lock**, because the producer and the four writer hooks are executor-side
  while the consumer is main-side, and one `JSRuntime` may only be used from one thread at a time.
  That private runtime is what a hand-written parser used to buy — the trade is a lock and
  `JS_UpdateStackTop` per operation, against a grammar nobody has to maintain.
- **The producer runs on the executor thread, the consumers on the main thread.** That is what makes
  interning atoms in the host's runtime a race rather than a nicety, and what makes calling into
  QuickJS from the handler safe. It holds by call graph: `RunQueuedScript` has one caller (the in-game
  tick) and `ApplyUpdateMessage` has one (`ClientReceivePump`), whose own three callers are the
  tick, the multiplayer lobby pump and `UpdateAndDrawMenuScreen` — all main-thread. That last one
  means an update can be applied **from the front end**, so a message can arrive with no level
  loaded; it is reported undelivered rather than held. The producer hook is therefore pure string
  work: no console printing, no game locks.
- **The message inherits the engine's delivery semantics exactly**, because it *is* the engine's
  delivery. One dispatch per machine: the host from its own queue, joiners from `0x67` (whose
  `!IsExecutorRunning()` gate is what stops the host running it twice). The host is throttled to
  **one payload per frame**; a joiner is not. `QueueScriptMessage` is the way in from native code
  and calls the *trampoline*, deliberately not the raw address — after `DetourAttach` that entry
  point is the wrap, which would turn a message into a file name.

### Script-defined levels (`src/CustomLevel.h/cpp`)

A level with **no `.gls` and no `.gcs`** — only the `.rif` still comes off disk.
`import * as arena from "./levels/arena.mjs"; levels.add("Test Arena", arena)` registers one;
the module exports `map` (the GLS map section, field for field), `includes` (the `.gsh` files
its roles come from, or nothing at all if they are built with `gls` instead), `define(level)`,
`populate(level)`, `setup(level)` and `message_received(msg, level)`. The whole thing lands in
Choose Level.

**`add` takes an object, never a path.** A module namespace already *is* a description object
with the map under `map`, so the host has no reason to load the file itself — the script's own
`import` does it, statically, with the editor and the module cache both in play. An inline
description is the same object with the map fields flat instead of nested, which is the only
thing `ResolveDescription` has to tell apart, and it can: no `CustomLevelMap` field is called
`map`.

The three load hooks split the way the two script files split: `define` is the `.gls`'s `#include`
block — it registers roles, and runs per *load* before the map is converted, because the roles
hash is cleared between levels — `populate` is its `use ... for ...` clauses, and `setup` is
the whole `.gcs`.

`message_received` is the fourth slot and the odd one out: it fires **during play**, off the script
queue (see the section above), so `DispatchCustomLevelMessage` keys it on `LevelForCurrentScript()`
rather than on `CurrentCustomLevel()` — the latter is null outside a load. That is also why the
level arrives as the hook's **second** argument: with `levels.current` null while it runs, a
module-level function would otherwise have no way to reach its own `Level`, and putting the message
first keeps the documented signature `message_received(msg)`.

```js
export const map = { rif: "levels\\level01.rif", object: "Land", camera_plane: "camhund" };
export const includes = ["defaults.gsh", "gunlok.gsh"];
export function populate(level) {
  for (const spot of level.locators("Goodie A"))   // the `for "<rif object>"` clause
    level.spawn("Rol_GunLok", 1, spot, { as: "gunlok" });   // ... and the `as` clause
}
export function setup(level) {
  console.execute("sunangle 140");                                  // the .gcs
  triggers.create({ kind: triggers.kind.death, targets: ["elint"],  // data, not a file
                    script: { kind: "unit_lost" } });
}
export function message_received(msg, level) {                      // ... arrives here
  if (msg.kind === "unit_lost") level.send({ kind: "mission_failed" });
}
```

Six facts pin the design; the full reasoning is `level_loading_notes.md` §6.5 and §7.

- **`ConvertParsedObjects` @ 0x004747b0 is the main hook**, not `LoadGLS`: it lets the game
  build and free its own list, with GkPlus only calling the map's `to_game_object` slot
  afterwards. `FreeParsedObjectList` is hooked for the null guard both need — `LoadGLS`
  returns null for a script that defined nothing and the game dereferences it — and
  `LoadGLS` itself only for the no-`includes` case below. The fourth hook,
  `EnterMainMenuScreen`, has nothing to do with loading: it is where these levels reach
  `LevelList`, and it has to be *after* the campaign seed — see "getting into the game's
  LevelList" below.
- **The map section is built by `gls::Create(SectionType::Map)` at registration time**, not
  at load time, so a bad field is reported at boot through the game's own `CheckValue`
  rather than halfway through a level load. The object is kept and re-converted on every
  load; `to_game_object` neither takes nor drops a reference.
- **Nothing a custom level needs is ever written to disk, and its `ScriptFileName` names no
  file.** That name is a virtual `gkplus\<slug>.gls` (the title with every non-alphanumeric
  character folded to `_`), and a level that names `includes` gets the `#include` list as a
  **source text**, parsed from memory through `gls::ParseSource` — the parser takes its input
  through a source object, so a null `FILE *` plus a text buffer is a complete parse. One
  source rather than one `LoadGLS` per include, because the multiple-inclusion guards only
  hold within a single call (`ClearParseSymbolTables` runs per call). The `#include` lines
  still resolve, because the parser opens one with a bare `fopen` against the *current*
  directory, which `LoadLevel` has set to Scripts. It always ends with a filler `shape` —
  its `name` + `file` are the two strings the map already requires — because an empty script
  is the null-list case above.

  With no `includes` there is nothing to parse at all, so `LoadGLS` hands back an empty
  `ParsedObjectList` built the way `ParseGSH` builds its own. **Letting the parse fail
  instead is not an option**: a failed parse poisons the parser for the rest of the process,
  so the next *game* level to load would fail too.

  **The name's shape is dictated by four consumers, not chosen.** A three-letter extension
  and more than four characters, because `ToMap` and `LoadOrBuildSectionAdjacency` overwrite
  the last three in place for `.cut`/`.map` (both optional caches, and they land in
  `<Gunlok>\gkplus`); no double quote, because `PushFileToParserStack` puts the name in a
  `# line` directive that pass 2 re-lexes; and **machine-independent**, which the absolute
  `%TEMP%` path it replaced was not — `SaveGame` serialises it verbatim and
  `ApplyUpdateMessage` `strdup`s it off a network payload on a joining client, so a path
  under one user's profile made a custom-level save unportable and a multiplayer join match
  no registration at all.
- **`setup` hooks `ExecuteAllCommands` @ 0x004d62c0, which has exactly one call site** —
  `LoadLevel` @ 0x004e1e00, step 11, behind the very `freshStart` byte that gates the
  `ExecuteCommandFile(ConsoleFileName)` queueing the `.gcs` at step 7. So reaching the hook
  already means "a fresh level start is at the point its `.gcs` would take effect", and a
  savegame restore skips it for free — no flag of our own, and none of `LoadLevel`'s own
  callers to disambiguate. It runs the callback **before** the original, because the original
  loops until the queue is empty, so anything `setup` queues drains in that same call rather
  than trickling out one per frame through `PumpQueuedConsoleCommand`.
- **The loading level is identified by `ScriptFileName`, not remembered from the menu**, so
  Choose Level, `ADD MISSION`, a savegame restore and a multiplayer client all work alike.
- **Menu 5 needs its own way in.** The game's "Choose Level" item exists only when
  `FlagChooseLevel` @ 0x006b0173 is set, `WinMain` sets it from the `-chooselevel` switch,
  and `SetupMenus` reads it once — long before any script runs. So the first registered
  level appends a "Choose Level" item to Single Player through `CustomMenu`.

**Both paths are verified in a running game** (`level_loading_notes.md` §6.5.1): 140 roles from an
in-memory prelude of six `.gsh` files with nothing on disk, 6 actors from `populate`, fog and sun
from `setup`, and 7 actors on the no-`includes` path where `LoadGLS` never runs. Two naming traps
came out of that run and cost a load each: **a script spawns a role by its GLS `identifier`, not by
the section symbol** (`"gunlok"`, never `"Rol_GunLok"` - `examples/levels/arena.mjs` had it wrong,
which is how it went unnoticed), and **`actor.name` is a reverse token lookup that loses to any
numeric token sharing the id's value** (actor 0 reads as `RES`, not `gunlok`).

Placed objects go through `gk::MapSpawn` after `ToMap` rather than the binding hash at
`ParsedMap+0x1b60` (which would need a forged field-9 `ParsedField`), so a script can spawn
at arbitrary coordinates as well as at rif locators. `gk::LevelRifLocators` supplies the
locator half and is only valid while a load callback is running — `populate` being the window
`ToMap` spawns a `.gls`'s placed objects in, after the roles are registered and before the
camera settles. `CurrentCustomLevel()` marks all three windows, and doubles as the
re-entrancy guard for both hooks, so a callback that parses a `.gls` itself cannot make the
level build twice.

### Starting a level programmatically (`src/Session.h/cpp`)

`levels.start(target, {difficulty})` puts the game in a level with no menus and no briefing
screen. `target` is a `Level` from `levels.add`, a title from `levels.startable`, or
`{script, console}` for a `.gls` that was never registered; `levels.quit()` is the way back.

```js
levels.start("Test Arena", { difficulty: "hard" });          // by title
levels.start({ script: "level01.gls", console: "level01.gcs" });
arena.start();                                                // on the Level itself
```

This exists because driving the front end is not viable: menu activation runs from the
**window procedure**, the front end does not run a frame at all while the window is
unfocused (see `SetFrameWakeupEnabled` in `src/GUI.h`), and `GoToMenu` leaves
`ChosenMenuItem` at `0x100` = "nothing selected", so synthetic keystrokes depend on focus
*and* on a selection the game deliberately clears. Full sequence and its three hazards are
in `level_loading_notes.md` §7.5; the two that cost a crash each:

- **`LeaveFrontEndScreen` @ 0x004e8dd0 is once per front-end session.** Its first two
  branches are null-guarded and the rest is not — it releases a reference on ~40 menu
  sprites and zeroes each global on the way out, so a second call faults. `SpriteScrollUp`
  @ 0x007b7d0c is the "is the front end up?" predicate.
- **`ClearTeamCarryOverState` @ 0x004da230 is `__thiscall`** and takes the global
  `TeamCarryOverState` in ECX. A missing register argument pops exactly as many bytes as a
  correct call, so the `RET`-form sweep is blind to it — this is what the companion
  ECX/EDX-read check is for.

**The load is deferred by one turn of the message loop, and that is structural.** `LoadLevel`
may not run inside the renderer, and the script host's frame callback is driven from inside
`HookedPresentScene` whenever a level is up. So `Session.cpp` queues the request and drains
it from a private `WM_APP` message handled in `HookedWndProc` — `SetMessageLoopCallback` /
`PostMessageLoopWork` in `src/GUI.h`, which is the seam to reuse for anything else that
reloads the world. Everything wrong with the *request* (unknown level, bad difficulty, a
quote in the path, a multiplayer session, a start already pending) still throws at the call;
only the load itself is asynchronous, so poll `game.state` or `actors.count`.

**`levels.startable` is the game's one `LevelList`**, which holds the campaign, anything
`ADD MISSION` added, and — because `AddCustomLevel` registers through the same `AddLevel` —
every script-defined level. One lookup serves all three.

**Registering a level and listing it are separate acts, and the order is load-bearing.**
`EnterMainMenuScreen` seeds its fifteen campaign missions only when `LevelList` is empty
(`CMP dword [0x007b74e0],0` then `JNZ` past all fifteen `AddLevel` calls *and* the block
that seeds `ScriptFileName`/`ConsoleFileName` from the first entry), and `levels.add` runs
during `SetupMenus`, which is earlier. Calling `AddLevel` at registration time therefore
cost the player the whole campaign — Choose Level held only the script-defined levels, menu
7's "new game" launched the first of *those* (it starts `LevelList.sentinel->next`), and the
default script name was never set.

So `CustomLevel.cpp` holds its levels back and appends them from a detour on
`EnterMainMenuScreen`, after the original has had the empty list it insists on
(`ReconcileLevelList`). The result is campaign 0-14 then script-defined levels from 15,
which is also the Choose Level order, and it is stable: `LevelList` is cleared only by
`ShutdownMenuSystem` at process exit and `EnterMainMenuScreen` never touches `Menus[5]`, so
returning to the menu neither re-seeds nor duplicates. A level registered *after* the first
main-menu entry — from the REPL, say — is appended immediately instead. **Idempotency is a
per-level `listed` flag, not a re-read of the list**, because two levels may legitimately
share a title. `CustomLevelByTitle` covers the window in between, so
`levels.start("My Level")` works from the moment `levels.add` returns.

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

### The renderer's struct mirror (`src/Render.h/cpp`)

`rendering_notes.md` is the analysis; `src/Render` is the mirror, and it is pure struct +
native-API — no `*System`, because the renderer installs no detour of GkPlus's own (the
D3D-side hooks live in `src/GUI.cpp`).

Three things about it that are not obvious from the header alone:

- **It is the second place in the codebase to use real multiple inheritance**, after
  `Map : MapBase, RefCountedBase`. `AwNode : AwFrame, AwRefCounted` puts the refcounted
  subobject's vptr at +0x9c and its `refcount` at +0xa0 purely because `AwFrame` is 0x9c bytes —
  and +0xa0 is exactly the word `RenderQueue_Add` increments. The `static_assert`s on
  `offsetof(AwNode, refcount)` and `sizeof(AwFrame)` are what prove the split reproduces the
  original.
- **`AwRefCounted` and `Map.h`'s `RefCountedBase` are the same 8 bytes and are deliberately not
  merged.** They model different points on the same chain: the root vtable 0x006522e8 has one
  slot, which is what `AwRefCounted` declares, while Map's second base sits two levels above it
  (0x006522e8 <- 0x0065281c <- 0x00652828) and `Map.h` folds the middle base's extra slot into its
  own declaration. Merging them would make one of the two wrong about its slot count; the size is
  identical either way, which is all either `static_assert` pins.
- **`AwFrame` has no virtual destructor and `SceneNode` adds one as slot 2.** That ordering is
  load-bearing: `AwFrame`'s vtable (0x0066da08) is exactly `{EnsureMatrix, EnsureInverseMatrix}`,
  and `SceneNode`'s (0x0066da34) is those two *then* the deleting destructor. Declaring the
  destructor first — the reflex — would put it in slot 0 and silently mis-model every class in
  the tree. `AwNode` and `Renderable` add nothing to the primary table at all; their destructor
  lives on the secondary vtable at +0x9c.

Four field-level findings shape the rest of the header, and `rendering_notes.md` §6 has the
evidence:

- **`AwFrame`'s two virtual slots return the matrix**, they are not `void` — each early-outs on
  its cache bit and hands back a pointer, and *which* matrix comes back is the entire point of
  `AwNode`'s override (`&matrix` vs `&world`). That return value is what reaches `SetTransform`.
- **`LightSet`'s 0x44-byte tail is a `D3DMATERIAL8`** — `SetD3DMaterial` passes `this + 0x18` to
  `IDirect3DDevice8::SetMaterial`, and `0x08 + 0x10 + 0x44` is 0x5c exactly. The engine's "ambient
  light" is this object's Emissive term.
- **`AwMaterial` ends in `AwTextureStage stages[8]`** — `AwMaterial_Compile` walks
  `this + 0x3c + num_stages * 0x30` downwards, and `0x3c + 8 * 0x30 == 0x1bc` is the object.
- **`DrawItem+0x0c` is a timestamp and `+0x24` a LOD index**, neither of which looks like it at
  the call sites; the first drives the animation controllers at `Renderable+0x1e0`.

`SubmitDrawItem` wraps `RenderQueue_Submit`, whose nine stack arguments (`RET 0x24`) are **not**
in the order their fields land in — the mapping was read off the prologue, and swapping the
camera and the light set would not fault, it would just render with the wrong one. `AwTexture`
deliberately carries no `sizeof` assert — its size is not established, and the 0x34-byte record
`AcquireRimTexture` caches is a different, earlier object. It is also **not** one of the
`AwRefCounted` family, which an earlier revision had it as: `AwMaterial_ApplyStage` takes the D3D
texture as `**stage`, straight off offset 0, so there is no vptr and no refcount at +0x04.

### JavaScript bindings (`src/Js*`)

**The console command registry is the map of what is still missing, and it is measured**:
`console_command_notes.md` has all 280 registrations (272 distinct names) classified against the
JS surface — 7 replaced by plain JavaScript, 33 already reachable, **223 bound**, 9 dev-only, and
**no gaps**. Every registered command is now reachable through a typed binding. Read it before adding a binding; §1 has the recipe for re-extracting the
registry from the binary if the classification ever needs rebuilding.

Two facts from it that change how bindings get written:

- **Fifteen command names live in `glres<lang>.dll`, not in the exe.** `EXIT`, `QUIT`, `MENU`,
  `HELP`, `LINES`, `CONSOLE APPEAR`, `SAY`, `TIME`, `DATE`, `LIST COMMANDS`, `CLEAR HISTORY
  BUFFER`, `HISTORY BUFFER SIZE`, `HISTORY_BUFFER_LENGTH`, `QUEUE SIZE` and `QUEUE LENGTH` are
  registered under `GetResourceString` results, so `console.execute("QUIT")` is a no-op on a
  non-English install. `console.commands` enumerates the registry so those are discoverable.
- **What makes a command hard to bind is its broadcast, not its setter.** Almost every
  world-mutating handler is `IsExecutorRunning` → `SuspendExecutor` → mutate →
  `BroadcastToPlayers(id, …)` → `ResumeExecutor`, and the update id and payload are per-command.
  §4.1 of the notes lists all 27 broadcasters with their recovered update ids and payload sizes.
  **But a broadcast is only wire-format work for a *native* binding.** Dispatching the command runs
  the handler, which broadcasts itself — which is why all 25 broadcasting gaps closed for free once
  the command-backed surface existed. They are also safe to call unguarded: 24 of the 25 check
  `IsExecutorRunning`, so a joining client no-ops and takes the host's update instead.
- **There are two kinds of binding here, and the split is load-bearing.** *Native* wrappers
  (`camera`, `game`, `world`, the `console` colours and registry) exist wherever there is **state
  to read back**. *Command-backed* namespaces (`fx`, `light`, `objectives`, `music`, `screen`,
  `units`, `inventory`, `tracks`, `demo`, `script`, the interpolated `camera` moves, the `console`
  administration) format a console command line and run the game's own handler — because every one
  of those handlers **is** an argument parser whose defaults come from the map bounds or the cursor,
  so dispatching it is faithful by construction where a reimplementation would silently diverge.
  What they add over a raw string: typed arguments, locale-independent numbers, names that survive a
  translated install, a length check the engine does not do, and per-command whitespace rules.
  `src/JsCommands.cpp`'s header comment is the full argument.
- **`ExecuteCommand` @ 0x004d6090 has no length check.** It copies into `ConsoleCommandLine`
  (`char[252]`) with an unbounded byte loop, and `ConsoleSmallFont` @ 0x007b6a54 is next. `fgets`
  caps a batch line at 249 so the game cannot reach it, but a script can — `gk::ExecuteCommand`
  therefore refuses anything over `kConsoleCommandLineMax` and returns false.
- **Nine binding members do not replicate, and the ones next to them do.** `actor.armor`,
  `actor.shield`, `actor.set_position`, `actor.set_team`, `actor.mine`, `actor.goto`,
  `turret.turret_enabled`, `pickup.set_required_item` and `tokens[name] = value` all reach no
  broadcast, while `actor.health`, `frag`, `remove`, `die`, `set_target`, `associate`, the attack
  methods and `set_pickup_enabled` do. The console commands beside them broadcast *around* the same
  setters, which is what makes the difference invisible in single player. `set_team` is the sharpest
  case: `Actor::ChangeOwnerAndTeam` @ 0x00530470 replicates and `SetTeamId` does not, and the
  binding took the latter — now fixed, and it was two bugs, because bare `SetTeamId` also left the
  actor on its **old team's actor list**. **Check the setter, not the command** —
  `console_command_notes.md` §6 is the full table.
- **The `RET`-form sweep never covered the free functions a mirror resolves by address**, and the
  first in-game run of `make` and `console.commands` found three defects sitting in that gap — a
  missing dereference in `GetCommandTable`, a `FastCall` that is really `__thiscall` with two stack
  args (`HierarchyResolveNamedPointPos`, which made every `make.role` with a `hotspot` fault), and
  `MakeCameraTrack`'s bind (0x005aa920 is `__fastcall(track /*ECX*/, rif /*EDX*/, name,
  Vec3 by value)` — all four wrong). `console_command_notes.md` §6.5 has all three and the recipe:
  for a declared arity, `__fastcall` puts the first two *integral* args in registers (floats never)
  and `__thiscall` only `this`, so the expected operand is `4 * stack_args`. That sweep has now been
  run over **every** `GetObjectAtOffset` in `src/` and is clean (§6.5.1) — read it before adding a
  native call, because two of its three traps (per-site declaration resolution, and the four
  tail-jump functions with no `RET` of their own) make a naive run report defects that are not there.
- **The `RET` test cannot see a register argument, so pair it with an ECX/EDX read check.** Calling
  `AcquireLevelRifForLocators` @ 0x00483da0 with *no* argument — which `MakeCameraTrack` did, on the
  belief that it falls back to the loaded level rif — pops exactly as many bytes as calling it
  correctly. It does not fall back: it `strlen`s ECX with no null check, and `ToCameraTrack` passes
  the section's own `file` field. The check is "does the target read ECX (or EDX) before writing it?"
  and it costs one pass over the first few instructions.
- **`ApplyDamage` takes three stack arguments and the mirror declared two.** Both overrides end in
  `RET 0xc`; `__thiscall` is callee-clean, so each call drifted ESP by 4 bytes — the exact failure
  the calling-convention warning above describes. The third is the attacker's team id (-1 for none),
  used for Deathmatch frag credit and as the 0x9b payload. The base and mobile versions are also
  genuinely different functions, not a base and an override that forgot to broadcast: see §6.2.
- **`triggers.create` is local, and that is correct.** Every machine runs the same `.gcs` and
  registers its own copy; the payload is what replicates when it fires. So register triggers in
  `setup` (which runs everywhere) and do *not* guard that with `game.simulation_running` — the
  opposite of the rule for world mutation.
- **A time trigger's `value` is a delay in seconds, not a tick deadline.** `RegisterTriggers` does
  `deadline = GetServerTime64() + ticks_per_second * value` at the calling thread's rate. `gk.d.ts`
  said "game-tick deadline" and was wrong on both counts.
- **The trigger is the only durable scheduling there is** (§6.1). `SaveGame` writes the trigger
  list, its payloads, the WAIT deadlines and the tokens — all of it a deadline plus a name, which is
  precisely why it can be saved. A JavaScript closure cannot be, so an in-heap timer is
  session-scoped by construction and a savegame load rewinds the clock under it.

**`game.simulation_running` is the authority test, and any script that mutates the world from a
message needs it.** It is `IsExecutorRunning` @ 0x00502da0 (`gk::IsSimulationRunning` in `Misc.h`) —
true in single player and on a multiplayer host, false on a joining client and before a level has
started one. A message is delivered on *every* machine, so a `message_received` that spawns
unguarded makes a joining client build a local ghost the host never hears about, and then receive
the host's copy as well. It is an accessor on a namespace object rather than a plain export because
a C module's named exports are fixed at instantiation — a top-level `simulation_running` could only
ever report what was true before any level existed.

The `"gk"` QuickJS C module, exposing 22 namespaces to scripts. `src/Js.h` is the public surface
— `RegisterGkModule`, plus `Log` / `ReportException` / `ReleaseCallbacks` for the host — and
`src/JsGk.cpp`'s `Namespaces` table builds them. Twelve come one per translation unit —
`JsCamera`, `JsConsole`, `JsActors`, `JsRoles`, `JsTokens`, `JsTriggers`, `JsLevels`, `JsMake`,
`JsGls`, `JsGame`, `JsWorld`, `JsMods` — and the remaining ten are the command-backed clusters
`JsCommands.cpp` supplies (`fx`, `light`, `objectives`, `music`, `screen`, `units`, `inventory`,
`tracks`, `demo`, `script`), over shared helpers in `src/JsBindings.h` / `src/JsCommon.cpp`.
**`JsMenus` is not in that table** — see below. `JsGk.cpp` also owns
`ReleaseCallbacks`, which fans out to the per-TU `Release*Callbacks` of the two namespaces that
hold script values (menus and levels).

```js
import gk, { camera, console, actors, roles, tokens, triggers, levels, make, gls } from "gk";
camera.distance = 900;                              // live accessors
for (const a of actors) if (a.alive) a.health = 50; // iterable
actors[12].frag();                                  // by id
actors["tbaa"].set_target(actors["hark"].id, 0);    // by token name
roles["gunlok"].spawn(0, {x: 0, y: 0, z: 0});
for (const [name, value] of Object.entries(tokens)) console.print(`${name}=${value}`);
console.log("actors:", actors.count);               // the host's own logging - no global console
console.execute("GOD ON");                          // and the game's command surface
tokens["score"] = 0;                                // upsert; actors/roles throw
for (const mod of mods) console.log(mod.priority, mod.name);   // what is mounted
console.log(mods.served, mods.recent[0]);           // ... and what it actually served
levels.add("Test Arena", arena);                    // `import * as arena` first
levels.start("Test Arena", {difficulty: "hard"});   // no menus, no briefing

export function setup_menus(menus) {                // `menus` is not an export
  menus.Main.add_item("Open console", (item) => {});  // menus[0], menus["main"] too
  menus[1].add_toggle("Cheats", false, (item) => log(item.value));
}
```

**`menus` is the argument to `setup_menus` and nothing else** — `NewMenusNamespace` is declared in
`src/Js.h` rather than `JsBindings.h`, and `JsGk.cpp`'s `Namespaces` table deliberately omits it.
Adding a front-end item is a boot-time act (the game's own items must already be there, or every
index in `OnMenuItemClicked` shifts), so the object is scoped to the callback that runs at that
moment. It is a plain collection otherwise, and `menus.current` / `menu.open` work whenever — a
script that wants them later keeps the argument in a module-level variable.

`actors`, `roles`, `tokens`, `menus` (the argument), `levels` and `mods` are all **exotic-property
collections** built by the same scaffolding in `JsCommon.cpp`: indexable, `in`-testable,
`Object.keys`/`values`/`entries`-able, and iterable over a snapshot. `NewCollection` gives every one
of them a non-enumerable `count` and `Symbol.iterator`; a namespace only supplies per-collection
extras (`roles.ai_types`, `menus.current`, `levels.add` / `levels.current`). Actors, roles, menus
and levels are keyed by **id** (names are a lookup convenience and are not enumerated) — for
`levels` that id is registration order, which is also the Choose Level order; tokens are keyed by
**name** and resolve to the bare value, with `lookup_id` left null so a token called `"5"` still
works.

Every lookup mints a **fresh wrapper**, so `menus[0] !== menus[0]` and likewise for actors and
roles. Compare `.id`, not object identity. The exceptions are `MenuItem` and `Level`: `add_item`
and `levels.add` return the same object the callback receives, because the binding holds it for the
registration's lifetime — and `levels.current` returns that same object too.

`menus` covers the front end only (`Menus[36]`, keyed 0-35 and by the `Menus.inc.h` name, matched
case-insensitively). A `Menu` wrapper has `id`, `name`, `title` (localized), `count`, `items` (a
snapshot of the game's own entries as `{index, label, type}`), `add_item`, `add_toggle` and
`open(remember)`. Unlike the Actor and Role wrappers, `Menu` and `MenuItem` hold pointers that
outlive every context — into the `.data` array and into a never-freed registration — so neither
needs a finalizer and neither can dangle.

**Every field that held a `.gcs` name now takes a message object too**, through one helper:
`js::ToScriptPayload` in `JsCommon.cpp`. A **string** becomes a `file` envelope, so a field written
from a script and one written from a `.gls` end up identical; **anything else** is
`JS_JSONStringify`d into a `message` envelope. Both go through `FileScriptPayload` /
`MessageScriptPayload` in `ScriptQueue.h`, which exist so that nothing outside `ScriptQueue.cpp`
spells a kind. `JSON.stringify` refusing a value (a function, a symbol) is a `TypeError` at the
call, not a payload named "undefined".

**Four fields have it, and that number comes from the call-site inventory rather than from
guessing.** `threading_model_notes.md` traces the argument of all seven `QueueScriptExecution`
callers; every one of them that reads a script-writable field is covered —
`triggers.create({script})` → `TriggerData+0x54`, `actor.associate(script)` →
`PickupActor+0x134`, `make.role({interface_beam_script})` → `Vulnerability+0x10`, and
`make.role({destructibility: {kind: "replace", script}})` → `ReplaceDestructibility+0x08`. The
last was missed on the first pass, which is the argument for doing the inventory: it is spelled
`script` here although GLS spells the field `name`, because a queue push is its only reader.

`ToScriptPayload` returns a **complete envelope**, which is also exactly what the queue wants, so
`Level.send(msg)` — the one caller that skips the field — hands it to `QueueScriptPayload` rather
than to `QueueScriptMessage`. That split matters: `QueueScriptMessage` takes a message *body* and
wraps it, so passing an envelope to it would bury the kind and turn `send("wave2.gcs")` into a
message whose body is an envelope. Anything else queueing directly has the same choice to make.

Writes go through `CollectionOps::assign`, which only `tokens` supplies — `tokens["score"] = 0` is
an upsert. The handler **throws** rather than returning false: quickjs hands an exotic
`set_property` result straight back to the caller (quickjs.c:10209-10213) instead of converting
false into the strict-mode TypeError the ordinary read-only path raises, so returning false would
make `actors[1] = x` a silent no-op. Own properties resolve first (quickjs.c:10137 vs :10203), so
`tokens.count = 5` hits the read-only accessor and cannot reach the table.

`Actor`/`Role` wrappers hold the **raw game pointer**, so a wrapper kept past its object's
destruction reads recycled pool memory — `.valid` is the escape hatch, and `frag()`, `remove()` and
`die()` null the pointer because those are the destructions the binding can see. `Actor`'s surface
is the vtable, not its fields; `id` (our own snapshot) and `name` (a token lookup) are the two
documented exceptions. Tokens are not wrapped at all: an 8-byte `{name, value}` pair has no
identity to re-resolve against, so the collection yields plain values.

**The Actor hierarchy is a real JS prototype chain.** `src/ActorClasses.inc.h` is an X-macro of
`(Name, Parent, Predicate, Kind)` over all fifteen subclasses, and `JsActors.cpp` generates the
per-class `JSClassID`s, the `JSClassDef`s, the `kind` strings, the RTTI dispatch ladder and the
chained prototypes from it. `NewActorWrapper` runs the ladder once and picks the class, so
`JS_NewObjectClass` installs the right prototype:

```js
actors[4] instanceof actors.classes.MobileActor;   // true for a turret
typeof actors[5].goto;                             // "undefined" - a pickup cannot move
```

Three consequences worth knowing:

- **A subclass member is absent, not throwing.** `if (a.goto)` is a valid feature test, and walking
  a whole prototype chain reading every property is safe. Under the previous flat prototype
  `turret_enabled` sat on the *base*, so merely reading it on a character raised.
- **The chain does not replace the checked downcasts.** The prototype is chosen once, at wrap time,
  but the wrapper holds a raw pointer the game can free and recycle onto a different subclass, so
  `ResolveMobile`/`ResolveCharacter`/`ResolvePickup`/`ResolveTurret` still re-run the predicate on
  every call. A borrowed method (`MobileActor.prototype.goto.call(pickup, …)`) still throws.
- **One ordering rule drives everything**, and `JsActors.cpp` `static_assert`s it: every class is
  listed **before its own base**. The predicates are inherited (`IsMobile()` is true for a turret),
  so the ladder must test most-derived first; the chain is built by walking the list backwards,
  which needs each base's prototype to exist already.

Classes that add no JS surface (`PopupActor`, `CentibodyActor`, the background creatures…) still get
a prototype and a class id, so `instanceof` does not lie about a class an actor genuinely is.
`actors.classes` holds one non-enumerable handle per class purely for the brand check and for
`constructor.name`; calling one throws, since scripts cannot create actors. Like `count`, being an
own property means it shadows the indexer, so a token literally named `"classes"` is unreachable
through `actors[…]`.

`EnsureClass` has a six-argument overload for this: unlike the five-argument one, it *always*
creates a prototype (a class adding no members still needs one in the chain) and chains it to the
parent's, re-read per context because `JS_SetClassProto` is per-context.

### Type definitions (`types/`)

Ambient `.d.ts` declarations for everything a script can reach, so an editor type-checks a plain
`.mjs` with no build step. `types/README.md` is the user-facing half; what matters here is who
maintains what.

- **`types/gk.d.ts` is hand-written and must be updated alongside `src/Js*.cpp`.** The bindings are
  the truth; nothing enforces the correspondence but `types/typecheck.ts`.
- **`types/imgui.d.ts` is generated** — re-run `python3 types/gen-imgui-dts.py` after touching
  `imgui-quickjs.cpp`. The generator reads the export list for names, each wrapper *body* for types
  (which `JS_To*` a parameter goes through, which `JS_New*` the result comes from, and the
  `JS_GetPropertyStr` keys of an options object), and the doc comment for parameter *names* only.
  It currently types all 197 functions and 28 enums with **zero `any`**, and prints the count of
  anything it could not infer — if that number stops being 0, the C++ grew a shape the generator
  does not know.
- **`npx -y -p typescript tsc -p types/tsconfig.json` is the check** — TypeScript is not a
  dependency of this repo and there is no `package.json`, so a bare `npx tsc` refuses to run
  ("This is not the tsc command you are looking for") and `-p typescript` is what fetches it.
  `examples/jsconfig.json` is the same invocation, and checks the shipped `.mjs` against the same
  declarations. `types/typecheck.ts` asserts in both
  directions: ordinary lines must compile, and every `@ts-expect-error` line must not. A vacuous
  declaration file (everything `any`) fails it, because all twenty-seven expect-error directives
  would go unused.

Three modelling decisions that took a round-trip through `tsc` to settle:

- **`Actor` is a discriminated union on `kind`, not an inheritance chain.** Interfaces cannot
  narrow an inherited property to a different literal, so `interface CharacterActor extends
  MobileActor` and `kind: "character"` are mutually exclusive. The members are therefore composed
  from mixin interfaces (`ActorBase`, `MobileMembers`, `CharacterMembers`…), each class gets its
  own literal `kind`, and `Actor` is the union of all sixteen — which is what makes
  `if (a.kind === "turret") a.turret_enabled` work. Users write `Actor`, never `ActorBase`.
- **`instanceof` narrows through a widened `prototype`.** `actors.classes.MobileActor` is typed
  `ActorClass<AnyMobileActor>`, not `ActorClass<MobileActor>`, because at runtime the test is true
  for every descendant; narrowing to the exact class would be *wrong*. `ActorClass<T>` extends
  `Function` and deliberately has **no construct signature**, so `new actors.classes.PickupActor()`
  is a type error — matching the runtime, which throws.
- **A collection is an intersection**, `Members & { readonly [key: string]: T | undefined }`.
  Declaring `count` beside a string index signature is illegal in a single interface (the property
  must conform to the index type); in an intersection the declared property still wins on lookup,
  so `actors.count` is `number` while `actors["hark"]` is `Actor | undefined`. `tokens` is the one
  whose index signature is not `readonly`.

### The Vulkan renderer (`src/D3D8Capture`, `src/Vk*`, `src/VertexFormat`)

A bindless Vulkan replacement for Gunlok's renderer, in progress. **`vulkan_renderer_plan.md`
is where to start** — status and next steps; `vulkan_renderer_notes.md` is the design record
and every measurement behind it. Read the plan before touching any of this.

The one fact that shapes everything: **the seam is `Direct3DCreate8`, not the AWAPI render
queue.** The queue looked obvious and is not total — `rendering_notes.md` §4.1 — so
`src/D3D8Capture.cpp` wraps the D3D8 device instead. It is a **state recorder, not a
translation layer**: it mirrors the fixed-function state, replays state blocks into that
mirror, reduces each draw to a material and a pipeline key, and forwards everything to
d3d8to9 unchanged. `GKPLUS_RENDERER=vulkan` then puts a real swapchain on the game's window.

Three things worth knowing before editing:

- **`src/D3D8Device.gen.inc.h` is generated** by `src/gen-d3d8-forwarders.py` from d3d8to9's
  `d3d8.hpp`. Re-run it after changing which methods are intercepted. Its
  `check_wrapped_params()` **fails the build** if a method taking or returning a wrapped
  interface is left forwarding — that is not a nicety: forwarding a wrapper to d3d8to9 makes
  it `static_cast` to its own concrete class and read a garbage proxy pointer, which surfaces
  as an access violation inside `d3d9.dll` with nothing pointing back. `ProcessVertices` was
  missed twice by reading before the check existed, and adding `IDirect3DSurface8` to
  `INTERFACES` enumerated all ten surface-carrying device methods — `SetCursorProperties`
  included, which the hand-written prediction had missed. The check covers **every** wrapped
  interface including `IDirect3DDevice8`, because a forwarded `GetDevice` on a resource is the
  same failure without the crash: it hands the game the unwrapped d3d8to9 device, after which
  every call it makes is invisible.
- **Vulkan is reached through volk, never the loader's import library**, and the ImGui Vulkan
  backend is vendored in `third_party/imgui_backends/` for the same reason. GkPlus *is*
  `d3d8.dll`; a load-time dependency on `vulkan-1.dll` would stop the game launching.
- **Every "must be 0" counter in `render.report` is a real invariant.** Each one exists
  because getting it wrong once cost a debugging session.
- **A counter says the plumbing ran, not that it moved the right bytes.**
  `render.verify_textures()` reads each texture image back off the GPU and compares it against
  the D3D texture — and it found four defects with every counter reading clean, two of them in
  the staging ring and predating textures entirely (notes §4.13). Check `render.validation` in
  the same breath as any readback: a verifier that is itself invalid reports its own mismatches
  as the code's, which cost an afternoon.

| File | Purpose |
|------|---------|
| `src/D3D8Capture.h/cpp` | The capture device: wraps `IDirect3D8`, `IDirect3DDevice8`, the two buffer types, `IDirect3DTexture8` and `IDirect3DSurface8`; shadow state, state-block replay, per-draw material/pipeline keys, residency, the texture pixel path — `LockRect` on a `SYSTEMMEM` staging texture then `CopyRects` into the `MANAGED` one, so the upload hangs off `CopyRects` (notes §4.12) — and the `AcquireRimTexture` hook that names every image by its `.rim` asset (§4.14) |
| `src/VkContext.h/cpp` | Instance, physical device, logical device, validation. Lazily initialized — **never from `DllMain`**, since volk calls `LoadLibrary` and that deadlocks under the loader lock |
| `src/VkRenderer.h/cpp` | Surface, swapchain, frames in flight, the ImGui backend, present |
| `src/VkResources.h/cpp` | VMA arenas, the staging ring, and the texture images (creation, format mapping, upload, readback verification). Nothing device-local is ever mapped |
| `src/VertexFormat.h/cpp` | Every FVF the game uses → one canonical 48-byte vertex. Pure CPU, no Vulkan and no D3D headers |
| `src/JsRender.cpp` | The `render` namespace: all of the above, readable from the REPL |

### The Blender addon (`blender/`)

Import/export of `.rif` geometry for Blender, in **pure Python** — no compiled extension, and
it shares nothing with `d3d8.dll`. Because it is a separate world from the C++ side, its design
record lives in **`blender/CLAUDE.md`**, which loads automatically when you work in that
directory; `blender/README.md` is the user-facing half and `rif_chunk_format.md` is the format
reference. Test invocations are under "Running the test suites" above.

### Other

| File | Purpose |
|------|---------|
| `src/Vfs.h/cpp` | The mod filesystem: mount, case-folded index, lookup, read, `Materialize`. Pure lookup — touches no game memory, so it is the half a harness can exercise. See "Mod loading" above |
| `src/FileHooks.h/cpp` | `FileHookSystem` — the nine IAT patches and the two static-CRT detours that make the engine consult `src/Vfs`, plus the virtual-handle table and the `mods.served`/`mods.recent` diagnostics |
| `src/Repl.h/cpp` | The loopback JavaScript REPL: `StartRepl` / `PumpRepl` / `StopRepl`, owned by `BootScriptHost` rather than by `Subsystems` (it installs no detour). Off unless `GKPLUS_REPL_PORT` is set. See "The REPL channel" above |
| `src/Session.h/cpp` | `StartLevel` / `QueueLevelStart` / `QueueReturnToMainMenu` — a level start with no menus and no briefing, deferred to the message loop. Installs no detour and has no `*System`: it registers `SetMessageLoopCallback` on first use. See "Starting a level programmatically" above |
| `src/InputFix.h/cpp` | `InputFixSystem` - hook-only. Detours `AcquireDInputDevice` to suppress the vestigial DirectInput keyboard acquire and its `WH_KEYBOARD_LL` hook (see `input_notes.md`) |
| `src/ActorClasses.inc.h` | X-macro listing the 15 Actor subclasses: `GK_ACTOR_CLASS(Name, Parent, Predicate, Kind)`. Drives the JS class table, `kind`, the RTTI ladder and the prototype chain. **Must list every class before its own base** |
| `src/Menus.inc.h` | X-macro listing all 36 Gunlok menus: `GUNLOK_MENU(Name, Id, TitleResourceId, "English title")`. There are no gaps - ids 11 and 14-20 are identified in `menu_system_notes.md`. Also counted into `gk::MenuCount` |
| `imgui-quickjs/` | Static library: the ImGui bindings, linked into `d3d8.dll`. **Not a QuickJS module** — `js_imgui_new_namespace(ctx)` builds a plain object the host passes to `draw_gui`, since an ImGui call outside that frame does not work. `JS_SetPropertyFunctionList` handles the whole export list, `JS_DEF_CGETSET` included (see the QuickJS conventions) |
| `examples/main.mjs` | A working entry module, JSDoc-annotated against `types/`. Install it as `<Gunlok>\gkplus\main.mjs`; `examples/jsconfig.json` is what type-checks it |
| `examples/levels/arena.mjs` | A working level module for `levels.add` — `map` + `includes` + `define` + `populate` + `setup` |
| `examples/headers/` | `bug.gsh` and part of `defaults.gsh` re-implemented with `gls`, as the worked example of translating a header |
| `types/` | `.d.ts` for the `"gk"` module and the `ImGui` interface, the generator for the latter, and `typecheck.ts`. See "Type definitions" above |
| `blender/` | The Blender `.rif` import/export addon and its seven test harnesses. Pure Python, unrelated to `d3d8.dll`. Design record in `blender/CLAUDE.md` |
| `huffman/`, `utils/rifutil` | The C++ REBCRIF1 codec and its CLI. The Python port in `blender/io_scene_rif/rif.py` is decode-only; this is the only compressor |
| `utils/rimutil` | `.RIM` <-> PNG CLI over spng + libsquish, both directions, both image forms. `compress` takes `--format dxt1\|dxt3\|body` (default **dxt3**) and `--raw`; **dxt5 is refused by name**, because `TextureFormatCandidates` @ 0x006ac348 lists only DXT1/DXT3 and `SurfaceDesc_SetCompressedFormat` @ 0x005c6820 drops any other fourcc *silently* — a DXT5 file renders with garbage alpha rather than failing. `body` is exactly lossless and needs no DXT compressor — it picks `masking 2` only when every transparent texel shares one RGB (otherwise an `ALPH` chunk, since the RGB *under* transparency is what bilinear filtering blends into neighbours), and `check_lossless` re-derives every pixel before writing. Format details in `rif_chunk_format.md`; tests in `utils/rimutil/tests` |

## Reverse Engineering Reference

### Detailed Documentation Files

- `actor_vtable_notes.md` - Actor class hierarchy, all 83+ vtable slots, subclass sizes, constructor addresses
- `trigger_system_notes.md` - 22 trigger types, data structures, console command syntax, function addresses
- `gls_system_notes.md` - GLS/GSH script parser: pipeline, ParsedThingBase layout, per-section field tables (types/ranges/defaults), ToXxx converters, C++ API is `src/GLS.h`
- `level_loading_notes.md` - How a level is built: `BeginLevelSession` -> `LoadLevel` -> `ToMap`, the `.cut`/`.map`/`.opt`/`.loc` sidecar caches, the `LevelMeshHeader` geometry format, the `use <role> in team <n> for "<rif object>"` placed-object binding hash on `ParsedMap+0x1b60`, both spawn factories, the three seams for replacing the `.gls` path with a native level builder, and (§5.5) **the navmesh — which is just the level's own polygons**: a face is walkable iff `(flags & 0x100) == 0 && normal.y < 0`, where 0x100 is both an authored blocker and the loader's verdict on a **45° slope limit**
- `role_system_notes.md` - `Role` (0xc0) field-by-field: the entity hash table (0x007b48f0), lifecycle (`CreateRole`/`ToRole`/`RoleDtor`/`DestroyRoles`), the two embedded 16-byte list headers (vulnerabilities @ 0x68, sever points @ 0xac), the `flags` bitfield, `InventoryInfo`, pickup classification via `character->aggression*10`, the `ai` -> Actor-subclass dispatch (`CreateActor`), the spawn path (`SpawnRole`), three `ToRole` defects, and (§10) the whole vulnerability subsystem - `Vulnerability` (0x1c), `VulnerabilityType`, the 0xc-byte list sentinel, and the four population paths
- `role_subobjects_notes.md` - the four `Role` sub-objects: `Character` (0xb8, with `ToCharacter`'s unit conversions), `Projectile` (0x20), `ParticleGenerator` (0xd4: GLS fields, the five 0x18-byte `PGenChannel` records, the `ParticleType` enum, and the template -> emitter map from `ParticleEmitter_Ctor`), and the 3-variant `Destructibility` family (base 0x8 / `FragData` 0x24 / `ReplaceDestructibility` 0x10, dispatched on the `+0x04` tag by `Frag` @ 0x0052e220)
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
  asset format that is *not* RIF chunks), **and** the AvP upstream mapping (see below)
- `game_defects_notes.md` - bugs in **Gunlok itself** that reproduce without GkPlus, so nobody
  re-blames our hooks for them. Also the debugging recipes, and the one that matters most: WER
  already writes a full dump to `%LOCALAPPDATA%\CrashDumps\gl.exe.<pid>.dmp`, and
  `cdb -z <dump> -c ".ecxr; k 40; q"` plus `llvm-symbolizer` on the printed `d3d8+0x...` RVAs
  resolves a crash to file and line - where **every live attach failed** to walk the stack at all. Currently: `DrawText?` @ 0x005782e0 smashing its stack on any
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
  map, and the PhysicsFS assessment in §6
- `gls.txt` - Game Level Structure file format quick field list (superseded by gls_system_notes.md)

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
- The decompiler's `Class::Method` header line does **not** always match
  `FunctionManager.getParentNamespace()`. Query the namespace; never read ownership off the C output.
- For a vtable slot, the owning class is the **shallowest** class whose vtable contains that
  address — not the most-derived one that inherits it. 55 of 249 Actor-family functions were filed
  under a descendant. Fixing `setParentNamespace` also repairs the `this` parameter type for free.
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

### Game Binary Layout

| Segment | Address Range | Purpose |
|---------|--------------|---------|
| .text | 0x00401000 - 0x0064cbff | Code (~2.3 MB) |
| .rdata | 0x0064d000 - 0x006a2bff | Read-only data |
| .data | 0x006a3000 - 0x0083aa97 | Initialized + uninitialized data |

### Key Global Addresses (offsets from base)

**Trigger System:**

| Offset | Type | Name |
|--------|------|------|
| 0x006af858 | Trigger** | FirstTrigger |
| 0x006af85c | int* | NumTriggers |
| 0x007b9d34 | Trigger** | DoorTriggers |
| 0x007b9d38 | int* | NumDoors |

**Actor System:**

| Offset | Type | Name |
|--------|------|------|
| 0x007ba0d8 | Actors* | actors — `HashTable<Actor*>`, so +0x00 is the vptr and `n_entries` is at +0x04 |
| 0x0054f2b0 | ThisCall (member) | HashTable_Remove (the template's `Remove`) |
| 0x0054db10 | FastCall<unsigned, Actor*> | HashFunction_Actor (returns `actor->id`) |

**Role System:**

| Offset | Type | Name |
|--------|------|------|
| 0x007b48f0 | Roles* | roles — `HashTableBase<Role*>`, the **vptr-less** shape, so `n_entries` is at +0x00 |

**Token System:**

| Offset | Type | Name |
|--------|------|------|
| 0x007b6af8 | Tokens* | tokens |

**Tokens are also how the engine names actors.** A token is a `{name, float}` pair, and for an
actor name the float *is* the actor id: `ConsoleParseActorName` @ 0x004d6d90 does
`GetTokenValue -> ROUND -> GetActorById`, and `CommandGetActorName` @ 0x00446d30 inverts it with
`FindTokenWithValue((float)actor->id)`. `Actor` has no name field of its own. The three lookups are
`GetTokenValue` @ 0x004d3910, `SetTokenValue` @ 0x004d38a0 and `FindTokenWithValue` @ 0x004d3a60,
all `__thiscall` on the table; they are wrapped in `src/Tokens.h`. The class namespace was
`struct_unk1` in the DB and is now `Tokens`.

Two things the mirror cannot express. The global is a **`{List, RWLock}` pair** — every token
function locks an `RWLock` at `this + 0x10` (0x007b6b08) for the whole call, so they may only be
called with the real global, never a locally-built `Tokens`. And `GetTokenValue` compares names
with `_mbsicmp` (**case-insensitive**) and special-cases a `rand(N)` name as a pseudo-token that
skips the list entirely, drawing from the calling thread's PRNG for a uniform integer in `[0, N)`.

**Console System:** (the whole block 0x007b6950-0x007b6b41 is mapped in the Ghidra DB)

| Offset | Type | Name |
|--------|------|------|
| 0x007b6958 | char[252] | CommandLine (`ConsoleCommandLine`) |
| 0x007b6b40 | char[0xfc] | SavedConsoleCommandLine (ESC-stash of the line) |
| 0x007b6950 | unsigned | TextColor (`ConsoleTextColor`, "TEXT COLOR" cmd) |
| 0x007b6954 | unsigned | UITextColorLight 0xffccccd6 (scrolling msgs, briefing) |
| 0x007b6a64 / 0x007b6a68 | unsigned | UIColorDim 0xff595966 / UIColorYellow 0xffffef47 |
| 0x007c149c | unsigned | CursorColor (ARGB, init 0xffe5e5e5 in `InitConsole` @ 0x004d5380) |
| 0x007b6a54/58/5c/60 | Font* | ConsoleSmallFont / ConsoleLargeFont / HudSmallFont / ConsoleLargeFont2 (all built in `InitConsole`) |
| 0x007b6a70..0x007b6a7c | — | command **hash table**: NumRegisteredCommands, CommandTableNumBuckets, CommandTableMask, CommandTableBuckets (`CommandListElem**`) |
| 0x007b6aa8..0x007b6ab4 | List | command exec queue: CommandsToExecute (anchor), NumCommandsToExecute, cache, cacheValid — one popped per frame by `PumpQueuedConsoleCommand` |
| 0x007b6a80 / 0x007b6b38 | float | ConsoleTextScrollTarget / ConsoleSlidePos (open/close anim; -1=closed) |
| 0x007b6b3c | Sprite* | ConsoleBackdropSprite (FUN_004d7b20) |
| 0x007b6ac8/cc/d0/d4 | — | DrawText scratch arg block (X/Ctx/Scale/4) |

The console keeps text as parallel `List<T>` headers (0x10 bytes each: `{anchor_ptr, count,
cached_array, cache_valid}` — the anchor is a **pointer** to a heap sentinel, unlike the embedded
`List<T>` in `src/List.h`). Overlay (transient, console-closed, cap `DAT_006a66ac`): `ConsoleOverlayText`
@0x6a84 + `ConsoleOverlayColor` @0x6a98. Scrollback/history (cap `DAT_006a66b0`): `ConsoleHistoryText`
@0x6ab8 + `ConsoleHistoryColor` @0x6ad8 + `ConsoleHistoryTime` @0x6ae8. On-screen text-line layout
(anti-overlap, category @+0x30): `ScreenTextLineList` @0x6b28.

**Briefing / Debrief / Stats screen:** (globals 0x007b6890-0x007b68e1; driver `ShowBriefingOrDebriefScreen` @0x004b1f60)

| Offset | Type | Name |
|--------|------|------|
| 0x007b68e0 / 0x007b68e1 | bool | IsBriefing (1=brief,0=debrief) / IsTrainingDebrief |
| 0x007b6890 | byte | StatsScreenClientsReady (MP debrief handshake) |
| 0x007b6894 | int | CurrentBriefingTextIndex (GL_BRIEFING_0+n; -1=none) |
| 0x007b689c / 0x007b68ac / 0x007b68bc | List | BriefingBitmapList / BriefingSceneObjList / BriefingTextList (each 0x10-byte pointer-anchored list) |
| 0x007b68cc | int | BriefingFadeMode (0 none / 1 in / 2 out) |
| 0x007b68d0 / 0x007b68d8 | int64 | BriefingFadeStartTime / BriefingFadeEndTime |

**Client entity globals** (near the units block): `ObjectList` @0x007b6928 is the iterable
`List<Object*>` of every client game object (IsXxx @vtbl+0x18, name-ptr @+0xb8, teamslot @+0xb4 —
the named chars Gunlok/Elint/Hark/Frend/Maskelyn live here); `ProximityObjectList` @0x007b6938 is the
proximity-activated subset. `UnitsTable` @0x007b68f0 is a `HashTable<Unit*>` (vptr variant like
`actors`): n_entries=`NumUnits`@0x6f4, buckets=`UnitsTable_buckets`@0x6900. `CombatMusicKillCounter`
@0x007b68ec counts kind-2 entity deaths to escalate battle music (FUN_004e7230). The six scattered
`VestigialFloat_*` (0x6898/6904/6914/6924/6948/6a94 = 1024/1024/60/120/1024/1024) are CRT-constructed
floats with **no readers**.

**Menu System:** (see `menu_system_notes.md`)

| Offset | Type | Name |
|--------|------|------|
| 0x007b76d0 | Menu[36] | Menus (front-end) |
| 0x007b7578 | Menu[7] | InGameMenus (HUD/pause; ends at 0x007b76ac) |
| 0x007b732c | MenuIndex* | ChosenMenu |
| 0x006a7d6c | int* | ChosenMenuItem (0x100 none, 0x101 back, 0x102/3 scroll) |
| 0x007b7270 | int* | InGameMenuIndex |
| 0x006a89b4 | int* | InGameMenuSelectedItem |
| 0x007ba1dc | void*[7] | InGameMenuPanels |
| 0x007b74dc | LevelList | levelList — `List<LevelInfo>`; a node is 0x18 with `{title, script, console}` at +0x0c/+0x10/+0x14 |
| 0x007b74ec | float | MouseYNormalized |
| 0x007b74d0 | float | MouseXNormalized |
| 0x007b74f0 | LevelList[8] | KeyBindingCategories |
| 0x007b76b0 | LevelList | MultiplayerLevelList |
| 0x007b6f20 | LevelList | FileFindList (save-file enumeration) |
| 0x00725664 | ResourceEntry** | LocalizedStrings |

**Misc/Game State:**

| Offset | Type | Name |
|--------|------|------|
| 0x007b9e28 | int* | GameMode |
| 0x006b02b4 | int* | GameState |
| 0x007b68e8 | Actor** | ActorUnderCursor |
| 0x006abe04 | Settings* | Settings |
| 0x006a79b4 | int* | BattleNumber |
| 0x006a3001 | int* | EPWEnabled |
| 0x007b9cc4 | int* | GameDifficulty |
| 0x007b9c70 | Cheats* | Cheats |
| 0x007b9df0 | int* | Foobar |

**PRNG** (BSD `random()`, additive LFG DEG_3=31 / SEP_3=3; the generator is inlined at call
sites as `(*fptr += *rptr) >> 1`): there are **two** state tables selected per-call by
`GetCurrentThreadId() == ExecutingThread` — the client and executor threads each have an
independent RNG so server-side simulation stays deterministic (see `threading_model_notes.md`).

| Offset | Type | Name |
|--------|------|------|
| 0x006a3008 | int[31] | RandomState (main/client thread state table, 0x7c bytes) |
| 0x006a3084 | int[31] | RandomStateExecutor (executor/server thread copy, at +0x7c) |
| 0x006a3100 / 0x006a3104 | int* | RandomEndPtr / …Executor (= &state[31]) |
| 0x006a3108 / 0x006a310c | int* | RandomFrontPtr / …Executor (fptr, init &state[3]) |
| 0x006a3110 / 0x006a3114 | int* | RandomRearPtr / …Executor (rptr, init &state[0]) |

**Saved mission-state block** (0x007b9c88..0x007b9d20, the 0x98 bytes `SaveGame` snapshots — this
is the "SaveSettingsBlock", but its fields are ordinary live game state, not a settings struct):
the WAIT-command deadline (`0x9c88`/`0x9c8c` game-clock 64-bit; real-time twin at `0x9c90`/`0x9c94`,
click-cancel flags `0x9c98`/`0x9c99`), `GameDifficulty`, the sun color/direction
(`SunLightColor` 0x9cc8 / `SunDirection` 0x9ce0, set by SetSunBrightness/SetSunAngle), the
`TrainingAreaIndex` (0x9d14), and the **mission-stats** counters (reset by FUN_004fcc30, broadcast
to clients at debrief via message id `0xa2` in FUN_005029d0, read by CommandStatsScreen):
`MissionShotsFired` 0x9cf8 / `MissionShotsHit` 0x9cfc (accuracy = hit/fired, both count non-team-2
shooters) with team-2 copies `…Team2` at 0x9d00/0x9d04, `MissionTimeSeconds` 0x9d08, the resurrection
penalty 0x9d0c/0x9d10, and `DifficultyHealthToggle` 0x9cf4 (difficulty menu item 2).

**Networking / effect lists** (all `{anchor,count,cache,valid}` 0x10-byte headers with a
pointer-anchored heap sentinel, like the console lists): client `ClientOutgoingMsgList` @0x9d50 and
server `ServerOutgoingMsgList` @0x9ddc; the four DirectPlay enumeration snapshots
`DPlayGroupList`/`DPlayPlayerList`/`DPlayGroupDataList`/`DPlaySessionList` @0x9e30/0x9e40/0x9e54/0x9e64
(rebuilt from the `MultiplayerActive` COM object); `RespawnRoleList` @0x9d98; and the effect lists
`WallEffectList`/`WorldEffectList`/`LightEffectList` @0x9e88/0x9ebc/0x9ecc (light-cylinder & lightning
nodes are 0x80 bytes). `ScannerEffectSprite` @0x9f80 (bitmaps\scanner.rim) is the scanline
post-process; the other per-command effect sprites are `RayEffectSprite`/`LightCylinderSprite`/
`LaserFenceSprite`/`RingShockwaveSprite` (0x9f70/0x9f74/0x9f6c/0x9f60), and `EffectEmitterList` @0x9e98
(+ its geometry twin @0x9ea8) holds the steam/trail/explosion/sparks particle emitters. Executor thread
flags: `ExecutorThreadStarted`/`ExecutorKillFlag`/`ExecutorPauseAckFlag` @0x9df1-0x9df3.

A recurring **`VestigialFloat_*`** pattern shows up across `.data`: single `float` globals
CRT-constructed to `1024.0` with an atexit destructor and **no readers** (0x9c84/0x9d94/0x9e50/0x9eb8/
0x9f9c here; 0x6898/0x6904/0x6948/0x6a94 etc. in the console block). Treat them as dead/vestigial.

**Save System:** (see `save_system_notes.md`)

| Offset | Type | Name |
|--------|------|------|
| 0x007b6ef0 | char[0x29] | SaveFileNameBuf |
| 0x007b6dcc | char* | ScriptFileName (level `.gls`) |
| 0x007b6d70 | TeamCarryOverList | TeamCarryOverState |
| 0x007b6d68 | TeamCarryOverList* | TeamCarryOverStateAux1 (nullable) |
| 0x007b6d64 | TeamCarryOverList* | TeamCarryOverStateAux2 (nullable) |
| 0x007b9c88 | byte[0x98] | SaveSettingsBlock (size doubles as format version) |
| 0x007b6e48 | int* | NextInventoryItemId |
| 0x007b9cf0 | int* | LevelLoadReason (3 = loading a full savegame) |

**Level:** (see `level_loading_notes.md`)

| Offset | Type | Name |
|--------|------|------|
| 0x00739090 | Map** | TheMap (0x18c; non-NULL disables ToMap's geometry phase) |
| 0x00739098 | list | MapAuxObjectList (positional sounds etc.) |
| 0x007b3ec4 | TeamSlot* | TeamSlots (stride 0xc4; `active` @ 0x69 = slot active) |
| 0x007b3ec0 | int* | NumTeamSlots |
| 0x007b6dd0 | char* | ConsoleFileName (level `.gcs`) |
| 0x007b68e4 | int* | client actor id counter |

### Key Function Addresses (offsets from base)

**Console:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x004d4b50 | FastCall<void, const char*> | Print |
| 0x004d59e0 | FastCall<void, const char*> | ExecuteCommandLine |
| 0x004d6090 | FastCall<void, const char*> | ExecuteCommand |
| 0x004d5d50 | FastCall<void, const char*, const char*, TCallback, int> | RegisterConsoleCommand |
| 0x0043c800 | StdCall<> | SetupConsoleCommands |
| 0x004d62c0 | StdCall<void> | ExecuteAllCommands — `while (NumCommandsToExecute) PumpQueuedConsoleCommand()`. **Exactly one call site in the binary**: `LoadLevel` @ 0x004e1e00, step 11, behind the `freshStart` byte. That is what makes it the hook for a script level's `.gcs` replacement (see `src/CustomLevel.cpp`) |
| 0x004d6120 | StdCall<void> | PumpQueuedConsoleCommand — pops one queued line |
| 0x0043f250 | FastCall<bool, const char*> | ExecuteCommandFile — **queues**, does not run: each line is appended to `CommandsToExecute` and `PumpQueuedConsoleCommand` pops one per frame. `fgets` at 0xfa, `//` comments, and `#` directives (`ONLY IF SAFE` / `ONLY IF HINTS ON` / `CLEAR BATCH` / `EXECUTE IMMEDIATELY` / `NORMAL EXECUTION`). The declared `int` is a **bool in AL** (`MOV AL,1` / `XOR AL,AL`, upper 24 bits are fclose garbage) meaning "the file opened"; none of the six callers reads it |

**Actors:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x0044e0b0 | FastCall<Actor*, int> | GetActorById |

**Roles:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x004ae030 | FastCall<Role*, const char*> | GetRoleByName |
| 0x004ae0d0 | FastCall<Role*, int> | GetRoleById |
| 0x00503710 | FastCall<int, int, Role*, Vec3*, Vec4*, int> | SpawnRole |

**Tokens:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x004d35f0 | ThisCall<void, Tokens*, const char*, float> | SetOrCreateToken — an **upsert**, not a create; it overwrites in place when the name already exists (case-insensitively) and allocates only when it does not. Named `CreateToken` until the body was read |
| 0x004d3910 | ThisCall<bool, Tokens*, float*, const char*> | GetTokenValue — case-insensitive; a `rand(N)` name is a pseudo-token that skips the list |
| 0x004d38a0 | ThisCall<void, Tokens*, const char*, float> | SetTokenValue — update-only, **silent** for a token that does not exist |
| 0x004d3a60 | ThisCall<bool, Tokens*, float, char**> | FindTokenWithValue — reverse lookup; how an actor id becomes a name |

**Triggers:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x0043e240 | FastCall<void, TriggerKind, Vec3*, long long, TriggerList, const unsigned char*, int> | RegisterTriggers |
| 0x0050c400 | ThisCall (member) | RemoveTrigger |
| 0x0044c950 | ThisCall<TriggerList*, TriggerList*, TriggerList*> | CopyList |
| 0x0044ca10 | ThisCall<TriggerList*, TriggerList*> | InitList |
| 0x0044c900 | ThisCall<TriggerList*, TriggerList*, const char**> | InitListWithActorName |
| 0x0044e8c0 | ThisCall<ITrigger*, TriggerList*, const char**> | CreateTrigger |

**Menu:** (see `menu_system_notes.md`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x004e95e0 | StdCall<void> | SetupMenus (reads FlagChooseLevel @ 0x006b0173 **once**, so the Choose Level item cannot be enabled after boot; `WinMain` sets that flag from `-chooselevel`) |
| 0x004ecf10 | StdCall<void> | OnMenuItemClicked (action dispatch) |
| 0x004fbfa0 | FastCall<void, MenuIndex, bool> | GoToMenu (ECX=target, DL=push parent) |
| 0x004f94f0 | ThisCall<void, Menu*, unsigned, int, unsigned> | Menu::Menu (firstItemId, nLabels, titleId) |
| 0x004f7a60 | ThisCall<void, Menu*, const char*> | Menu::AddItem (type 0) |
| 0x004f7ae0 | ThisCall<void, Menu*, const char*, const char*, bool, bool> | Menu::AddValueItem (type 1) |
| 0x004f7950 | ThisCall<void, Menu*, const char*, int*> | Menu::AddToggleItem (type 2) |
| 0x004f79d0 | ThisCall<void, Menu*, const char*, int*, unsigned**> | Menu::AddMultiValueItem (type 3) |
| 0x004f7750 | ThisCall<void*, Menu*, int> | Menu::GetItemData (cached; NO bounds check) |
| 0x004f7cd0 | FastCall<void, Menu*> | Menu::ClearItems |
| 0x004fbf10 | ThisCall<void, Menu*, void*> | Menu::AppendItemNode |
| 0x004ea8e0 | StdCall<void> | UpdateAndDrawMenuScreen |
| 0x0058cdd0 | FastCall<void, int> | PlayUiSound (id in ECX; 0x57 = menu activation) |
| 0x004d5380 | StdCall<void> | InitConsole (WinMain @ 0x0046bb81, right before SetupMenus) |
| 0x004e7e50 | StdCall<void> | EnterMainMenuScreen |
| 0x00470c70 | FastCall<void, void*> | MenuScreenInputHandler |
| 0x00579000 | FastCall<const char*, void*, unsigned> | GetResourceString (ECX=&LocalizedStrings) |

**In-game menus:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00563c30 | ThisCall (member) | InGameMenu::OnItemActivated |
| 0x00567b60 | StdCall<void> | OpenInGamePauseMenu |
| 0x00567f00 | StdCall<void> | OpenInGameOptionsMenu |
| 0x005686b0 | StdCall<void> | OpenInGameLoadMenu |
| 0x00568e40 | StdCall<void> | OpenInGameSaveMenu |
| 0x0056a120 | FastCall<void, const char*, void*, void*> | OpenInGameConfirmDialog |
| 0x005691f0 | FastCall<void, int> | CloseInGameMenu (kind 0/1/2/3/0x41/0x42/0x43) |
| 0x00569550 | FastCall<char> | IsAnyInGameMenuOpen |

**Particles:** (see `role_subobjects_notes.md` §3)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00580510 | ThisCall<void, void*, ParticleGenerator*, Vec3*, void*, char> | ParticleEmitter_Ctor (the template consumer) |
| 0x005828f0 | StdCall<void> | InitParticleSystem (allocates ParticleTypeInfos[13]) |
| 0x0057d220 | ThisCall<void, ParticleTypeInfo*, ParticleType> | InitParticleTypeInfo (13-case per-type defaults + per-tick precompute) |
| 0x00581180 | - | particle per-tick update (unnamed; reads gravity_per_tick2/ttl_seconds/spawn_velocity_range) |
| 0x00582d10 | - | particle renderer (unnamed; reads the uv rect, render_state, live_emitters) |
| 0x0044c340 | StdCall<int> | GetParticleIDFromName (console keyword -> ParticleType) |
| 0x007c1964 | ParticleTypeInfo* | ParticleTypeInfos (13 x 0xd4; **not** ParticleGenerator) |
| 0x007c1968 | void* | ParticlesRimTextures (`bitmaps\particles.rim`) |

**Memory:** (wrapped as `gk::pool_alloc` / `gk::pool_free` in `src/Memory.cpp`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00571470 | CDecl<void*, size_t> | pool_alloc — page sub-allocator; falls back to real CRT malloc for big blocks |
| 0x005715b0 | CDecl<void, void*> | pool_free — returns an emptied page to the real CRT free. **`__cdecl`, not `__stdcall`**: bare `RET` at 0x0057166f with one stack argument, and game call sites clean up themselves (`CALL free` then `ADD ESP,0x4`). Calling it through a `StdCall` pointer leaks 4 bytes of stack per call and eventually returns to garbage — see the comment in `src/Memory.cpp` |
| 0x005e3f64 | CDecl<void, void*, int> | `free_sized` (discards the size, calls pool_free). `__cdecl` for the same reason. Was named `Dealloc?` |
| 0x005e3f72 | — | `malloc` — bare `JMP pool_alloc` |
| 0x005e3f7b | — | `free` — bare `JMP pool_free`; **every** `free` in game code goes here |
| 0x0044e1a0 | FastCall<char*, char*> | `strdup` — game-written, allocates via the malloc thunk |
| 0x00601f4a / 0x00601f2d | — | the *real* CRT malloc/free. Only pool_alloc/pool_free and a few file/rif paths (`ToMap`, `LoadOrGetRifFile`) call them — no field in any mirrored struct holds this memory |

**Misc:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00474540 | FastCall<Parsed*, const char*, int> | ParseGLS |

**Level Loading:** (see `level_loading_notes.md`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x004e2560 | FastCall<int, char> | BeginLevelSession (CL != 0 -> also LoadLevel) |
| 0x004e0980 | FastCall<int, bool> | LoadLevel(freshStart) — **not** `StdCall<void>`: the flag arrives in CL, `BeginLevelSession` passes 1 and `LoadGame` passes 0, and it gates the sun setup, the level `.gcs` (`ExecuteCommandFile` + `ExecuteAllCommands`) and the mission-stats reset. The `.gcs` therefore *does* run in single player |
| 0x004efcc0 | FastCall<void, const char*, const char*, const char*> | AddLevel(title, script, console) — appends to `LevelList` **and** adds the Menus[5] item |
| 0x00474870 | FastCall<void, ParsedObjectList*> | FreeParsedObjectList — pool-frees the header too |
| 0x00483420 | FastCall<void, List*> | List__Dtor — empties a list and destroys its sentinel, keeps the header |
| 0x00483da0 | FastCall<void*, const char*> | AcquireLevelRifForLocators(rifPath) — ECX only, nothing in EDX |
| 0x0047f160 | ThisCall (member) | ToMap - builds TheMap and spawns placed objects |
| 0x0047efa0 | ThisCall (member) | CheckValue_Map - handles `use ... in team ... for ...` |
| 0x00470f20 | ThisCall<void, Map*, void*, Vec3*, LevelMeshHeader*> | Map_Ctor |
| 0x005035b0 | FastCall<int, int, Role*, Vec3*, Vec4*> | ServerSpawnActorForTeam |
| 0x004fce90 | FastCall<void*, int, Role*, Vec3*, Vec4*> | ClientSpawnActorForTeam |
| 0x005aaac0 | FastCall<void, List*, void*, const char*> | RifFilterObjectsByName — **`__fastcall`**, ECX=out list *and* EDX=rif (read at 0x005aaac8), name on the stack. Was declared `ThisCall`, which put the rif on the stack and left EDX garbage |
| 0x004ae960 | FastCall<void*, const char*, int> | LoadOrGetRifFile |

**Save System:** (see `save_system_notes.md`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00507a80 | FastCall<char, const char*, bool> | SaveGame (path in ECX, `full` in DL) |
| 0x00505730 | FastCall<void, const char*> | LoadGame |
| 0x005055e0 | FastCall<int, const char*> | PeekSaveGameScriptName |
| 0x004e6d30 | StdCall<void> | MenuSaveGame |
| 0x004e6be0 | StdCall<int> | MenuLoadGame |
| 0x004dad40 | ThisCall<int, void*, HANDLE> | WriteTeamCarryOverState |
| 0x004da980 | CDecl<int, HANDLE> | ReadTeamCarryOverState |
| 0x0044c8d0 | FastCall<int, const char*> | strlen_plus1 (length **includes** NUL) |

### Actor Class Hierarchy

```
Actor (0x120 bytes, vtbl @ 0x00667e30)
 +- MobileActor (0x230 bytes)
 |   +- CharacterActor (0x308 bytes)
 |   |   +- CentibodyActor (0x310 bytes)
 |   |   |   +- CentipedeActor (0x310 bytes)
 |   |   +- PopupActor (0x310 bytes)
 |   |       +- TurretActor (0x320 bytes)
 |   +- NodeActor (0x278 bytes)
 |   +- PresidentActor (0x240 bytes)
 +- PickupActor (0x150 bytes)
 +- TrackObjectActor (0x1b8 bytes)
 +- TumbleweedActor (0x120 bytes)
 +- BackgroundCreatureActor (0x120 bytes)
 |   +- FlyingBackgroundCreatureActor (0x120 bytes)
 +- BlockerActor (0x130 bytes)
 +- ProjectileActor (0x178 bytes) see actor_vtable_notes.md
```

Key Actor struct offsets (vtable ptr implicit at 0x00): `id` @ 0x0c, `vulnerabilities` @ 0x10,
`ai_type` @ 0x50, `flags` @ 0x7c, `position` (Vec3) @ 0xa0, `orientation` (Vec4) @ 0xac,
`team_id` @ 0xbc, `role` @ 0xc0, `armor_value` @ 0xf0, `strength` @ 0xf4, `is_dead` @ 0x115.

`NodeActor` derives from **`MobileActor`**, not `CharacterActor` - earlier revisions of this tree
indented it one level too deep, which cannot be right (0x278 is smaller than `CharacterActor`'s
0x308). `src/Actors.h` and `actor_vtable_notes.md` always had it correct.

The whole tree is also an X-macro, `src/ActorClasses.inc.h`, which is what the JS binding layer
generates its class table from; keep the two in step.

No RTTI - type checking uses virtual methods (IsCharacter, IsMobile, IsTurret, etc.). Slots 36-50
are that mechanism and all fifteen are mapped to a concrete class.

Vtable slot counts: Actor 83, MobileActor 95, CharacterActor 100, TurretActor 105,
PresidentActor 96, PickupActor 86, ProjectileActor 85; the other nine add nothing. **Slot indices are
only comparable within a branch** - `PickupActor`'s slot 85 and `MobileActor`'s slot 85 are
unrelated. A slot implementation belongs to the *shallowest* class whose vtable holds it.

### Trigger Types Enum (TriggerKind)

| Value | Name | Description |
|-------|------|-------------|
| 0 | Death | Fires when listed actors die |
| 1 | Location | Actor enters radius around coords |
| 2 | LocationSpecified | Specific actors enter location |
| 3 | LocationAll | All listed actors in location |
| 4 | LocationTimed | Location check with time component |
| 5 | InstantDeath | Immediate death trigger |
| 6 | InstantDisplace | Immediate displacement |
| 7 | Time | Fires after delay (game ticks) |
| 8 | Escort | Escort mission trigger |
| 9 | Proximity | Proximity-based |
| 10 | Door | Door interaction |
| 11 | DoorOnce | One-shot door |
| 12 | DoorsEither | Either of two doors |
| 13 | FourDoors | Four-door puzzle |
| 14 | LightUp | Light activation |
| 15 | Defog | Map reveal |
| 16 | Shot | Actor is shot |
| 17 | BeingAttacked | Actor under attack |
| 18 | FragScore | Frag count reached |
| 19 | TimeLimit | Time limit expired |
| 20 | TimeIfAlive | Time trigger if actor alive |
| 21 | BeenAlerted | AI alert state |

### Role Structure (0xc0 bytes)

Full field-by-field breakdown, lifecycle, hash table and spawn dispatch in
`role_system_notes.md`. Key fields: `name` @ 0x00 (from GLS `identifier` 0x47, **not**
`name`), `shape` @ 0x18, `hierarchy` @ 0x1c, `hotspot` @ 0x44, `character` @ 0x60,
`inventory_info` @ 0x64, `vulnerabilities` list @ 0x68 (`{sentinel,count,cache,flag}`),
`flags` @ 0x78 (10 packed booleans), `ai` @ 0x7c (determines Actor subclass),
`resistance_factor` @ 0x94, `armor_value` @ 0x98, `shields` @ 0x9c, `sever_points` list
@ 0xac, `id` @ 0xbc. The C++ mirror is `src/Roles.cpp`.

### Map Structure (0x18c bytes)

Full layout in `level_loading_notes.md`; the C++ mirror is `src/Map.cpp`, modelled as
`Map : MapBase, RefCountedBase` (see the vtable convention above). Key fields:
`lock` @ 0x04 (embedded RWLock), `sections`/`num_sections` @ 0x88/0x8c, the second
base subobject's vptr/`refcount` @ 0xa4/0xa8, `adjacency_built` @ 0xac, `scene_object` @ 0xc8, `bitmap`
@ 0xcc, `neg_origin` @ 0x11c, `bounds_min`/`bounds_max` @ 0x128/0x134,
`camera_focus_min`/`max` @ 0x140/0x14c, `.rif` FILETIME @ 0x158, `shadow_object_rif`/
`_name` @ 0x160/0x164, `default_position` @ 0x168, `sky_object` @ 0x188.

**The origin at 0x11c is stored negated** (`Map_Ctor` XORs each component with
0x80000000) and `ToMap` *adds* it, so a placed object lands at
`rif_pos * RifUnitScale(rif) - origin`, where the scale is **`*(float *)rif`** — the
first float of the object `AcquireLevelRifForLocators`/`LoadOrGetRifFile` return. It is
per-rif data; there is **no world-unit-scale global or getter**, and 0x005a9b40 (once
misnamed `GetWorldUnitScale`) is `CopyDword`, a `__fastcall(dest, src)` 4-byte copy.
0x24..0x88 and 0x8c..0xa4 are still unmapped -
they are reached only through `__thiscall` methods called directly on `TheMap`.

Roles are the "entity" hash @ 0x007b48f0 (`{num_entities, num_buckets, mask, buckets}`);
ids come from `next_entity_id` @ 0x007b48d4. `CreateRole` @ 0x004add90 allocates+inserts;
`ToRole` @ 0x0047cc20 converts a parsed `role`; `CreateActor` @ 0x00510760 dispatches
`role->ai` to the Actor subclass; `SpawnRole` @ 0x00503710 is the native `gk::SpawnRole`.

### Imports

Key external libraries: BINKW32.DLL (video), STEAM_API.DLL, D3D8.DLL,
KERNEL32/USER32/GDI32/ADVAPI32/OLE32/WINMM (Windows API).

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
