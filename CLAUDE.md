# GkPlus - Gunlok Modding Framework

A 32-bit Windows DLL mod for the game **Gunlok** (2000). Built as a `d3d8.dll` proxy that hooks
into the game via Microsoft Detours. It is a **native C++ reverse-engineering library**: the
decompiled game structs live in headers, a typed native API wraps the game's own functions and
globals, and a handful of behavioral hooks (music volume fix, input fix, debug redirect, ImGui/D3D
overlay) run at load. The game binary is actively being reverse engineered in Ghidra, accessible
via MCP.

On top of that sits a **QuickJS scripting layer**: `src/Script.cpp` boots a runtime during the
game's `SetupMenus` and runs `<game dir>\gkplus\main.mjs`, which can import the `"gk"` and
`"ImGui"` C modules, add items to the game's front-end menus and draw an ImGui overlay. See
"Script host" below.

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

### Dependencies (vcpkg.json)

| Package | Purpose |
|---------|---------|
| imgui (dx9-binding, win32-binding) | In-game GUI overlay |
| d3d8to9 | Direct3D 8 to 9 translation layer |
| detours | Microsoft Detours - function hooking |
| quickjs-ng | QuickJS JavaScript engine |
| dear-bindings | ImGui language bindings |

Custom vcpkg ports in `ports/` for: d3d8to9, detours, quickjs-ng, dear-bindings.
Overlay configuration in `vcpkg-configuration.json`.

## Architecture

### DLL Lifecycle (src/entry.cpp)

1. `DllMain(DLL_PROCESS_ATTACH)`: opens a Detours transaction, constructs the `Subsystems`
   aggregate (each member attaches its detours in its ctor), commits.
2. `DllMain(DLL_PROCESS_DETACH)`: opens a Detours transaction, destroys the `Subsystems` aggregate
   (each member detaches its detours in its dtor), commits.

`Subsystems` holds only the **hook-installing** subsystems — `MusicSystem`, `DebugSystem`,
`GUISystem`, `InputFixSystem`, `CustomMenuSystem`, `ScriptSystem`. Everything else is struct-only
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
| `src/Varint.h` | Variable-length integer encode/decode utility (currently no callers) |

### Subsystem sources (struct headers + native API)

Each pair is a header of decompiled structs/enums plus native free-function declarations, and a
`.cpp` implementing them (offset resolution + any behavioral hooks): `src/Actors`, `src/Roles`,
`src/Map`, `src/Vulnerability` (header-only), `src/Music`, `src/Math`, `src/Menu`, `src/Tokens`,
`src/Triggers`, `src/Console`, `src/Misc`, `src/Camera`, `src/Debug`, `src/GUI`, `src/InputFix`,
`src/CustomMenu`, `src/Script`.
`src/GLS.h/cpp` is the model the rest now follow. The behavioral-hook subsystems (`Music`, `Debug`,
`GUI`, `InputFix`, `CustomMenu`, `Script`) expose a `*System` RAII class constructed by
`entry.cpp`; the others are pure struct + native-API.

### Script host (`src/Script.h/cpp`)

One `JSRuntime`, one `JSContext`, one entry module. The entry module is `GKPLUS_SCRIPT` if that
environment variable is set, otherwise **`gkplus\main.mjs` next to `d3d8.dll`** (i.e. in Gunlok's
directory). A missing file logs the path it looked for and leaves the game unmodified.
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
- **`ImGui` and `gk` are handed to the script as arguments** as well as being importable. Getting
  a usable namespace object for a C module requires the module to have been *linked*, which only
  happens when something imports it, so `BootScriptHost` evaluates a three-line internal module
  (`import * as ImGui from "ImGui"; import * as gk from "gk"; export {ImGui, gk};`) and reads its
  namespace. Same object identity as the script's own `import * as`, and it fails loudly at boot
  rather than inside the user's first import.
- **Module evaluation returns a promise even without top-level await**, so `Await()` pumps
  `JS_ExecutePendingJob` until it settles, and reports a stuck pending promise instead of spinning.
- **The entry module's name uses forward slashes.** QuickJS's default normalizer resolves a
  relative specifier by scanning the *importing module's name* for `/`; with `C:\…\main.mjs` it
  finds none and `import "./x.mjs"` silently resolves to `x.mjs` in the process's cwd.

Two seams in `src/GUI.h` carry it: `SetOverlayDrawCallback` (inside the ImGui frame, F11 only)
runs `draw_gui`, and `SetFrameCallback` (once per `PresentScene`, overlay or not) drains the job
queue. Both are installed by `BootScriptHost`, not by the `ScriptSystem` ctor, so they can never
call into a context that does not exist yet. Everything runs on the main thread.

Nothing a script throws reaches game code: every seam ends at `gk::js::ReportException`, which
prints the message and stack to the console and the debugger. A `draw_gui` that throws is
**disabled for the session** — once per frame forever is a flood, not a diagnostic, and a script
that threw mid-frame has usually left ImGui's stack unbalanced. There is also a global `console`
(`log`/`info`/`warn`/`error`/`debug`), which QuickJS core does not provide.

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

### JavaScript bindings (`src/Js*`)

The `"gk"` QuickJS C module, exposing seven subsystems to scripts. `src/Js.h` is the public surface
— `RegisterGkModule`, plus `Log` / `ReportException` / `ReleaseCallbacks` for the host — and
`src/JsGk.cpp` builds the module from seven namespace objects, one per translation unit:
`JsCamera`, `JsConsole`, `JsActors`, `JsRoles`, `JsTokens`, `JsTriggers`, `JsMenus`, over shared
helpers in `src/JsBindings.h` / `src/JsCommon.cpp`.

```js
import gk, { camera, console, actors, roles, tokens, triggers, menus } from "gk";
camera.distance = 900;                              // live accessors
for (const a of actors) if (a.alive) a.health = 50; // iterable
actors[12].frag();                                  // by id
actors["tbaa"].set_target(actors["hark"].id, 0);    // by token name
roles["gunlok"].spawn(0, {x: 0, y: 0, z: 0});
for (const [name, value] of Object.entries(tokens)) console.print(`${name}=${value}`);
tokens["score"] = 0;                                // upsert; actors/roles throw
menus.Main.add_item("Open console", (item) => {});  // menus[0], menus["main"] too
menus[1].add_toggle("Cheats", false, (item) => log(item.value));
```

`actors`, `roles`, `tokens` and `menus` are all **exotic-property collections** built by the same
scaffolding in `JsCommon.cpp`: indexable, `in`-testable, `Object.keys`/`values`/`entries`-able, and
iterable over a snapshot. `NewCollection` gives every one of them a non-enumerable `count` and
`Symbol.iterator`; a namespace only supplies per-collection extras (`roles.ai_types`,
`menus.current`). Actors, roles and menus are keyed by **id** (names are a lookup convenience and
are not enumerated); tokens are keyed by **name** and resolve to the bare value, with `lookup_id`
left null so a token called `"5"` still works.

Every lookup mints a **fresh wrapper**, so `menus[0] !== menus[0]` and likewise for actors and
roles. Compare `.id`, not object identity. The one exception is a `MenuItem`: `add_item` returns
the same object the callback receives, because the binding holds it for the item's lifetime.

`menus` covers the front end only (`Menus[36]`, keyed 0-35 and by the `Menus.inc.h` name, matched
case-insensitively). A `Menu` wrapper has `id`, `name`, `title` (localized), `count`, `items` (a
snapshot of the game's own entries as `{index, label, type}`), `add_item`, `add_toggle` and
`open(remember)`. Unlike the Actor and Role wrappers, `Menu` and `MenuItem` hold pointers that
outlive every context — into the `.data` array and into a never-freed registration — so neither
needs a finalizer and neither can dangle.

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
- **`npx tsc -p types/tsconfig.json` is the check**, and `types/typecheck.ts` asserts in both
  directions: ordinary lines must compile, and every `@ts-expect-error` line must not. A vacuous
  declaration file (everything `any`) fails it, because all thirteen expect-error directives would
  go unused.

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

### Other

| File | Purpose |
|------|---------|
| `src/InputFix.h/cpp` | `InputFixSystem` - hook-only. Detours `AcquireDInputDevice` to suppress the vestigial DirectInput keyboard acquire and its `WH_KEYBOARD_LL` hook (see `input_notes.md`) |
| `src/ActorClasses.inc.h` | X-macro listing the 15 Actor subclasses: `GK_ACTOR_CLASS(Name, Parent, Predicate, Kind)`. Drives the JS class table, `kind`, the RTTI ladder and the prototype chain. **Must list every class before its own base** |
| `src/Menus.inc.h` | X-macro listing all 36 Gunlok menus: `GUNLOK_MENU(Name, Id, TitleResourceId, "English title")`. There are no gaps - ids 11 and 14-20 are identified in `menu_system_notes.md`. Also counted into `gk::MenuCount` |
| `imgui-quickjs/` | Static library: the `"ImGui"` QuickJS C module, linked into `d3d8.dll` and registered by the script host. Its export list is `JS_CFUNC_DEF` + `JS_OBJECT_DEF` only, which is what makes `JS_SetModuleExportList` legal for it (see the QuickJS conventions) |
| `examples/main.mjs` | A working entry module, JSDoc-annotated against `types/`. Install it as `<Gunlok>\gkplus\main.mjs`; `examples/jsconfig.json` is what type-checks it |
| `types/` | `.d.ts` for the `"gk"` and `"ImGui"` modules, the generator for the latter, and `typecheck.ts`. See "Type definitions" above |

## Reverse Engineering Reference

### Detailed Documentation Files

- `actor_vtable_notes.md` - Actor class hierarchy, all 83+ vtable slots, subclass sizes, constructor addresses
- `trigger_system_notes.md` - 22 trigger types, data structures, console command syntax, function addresses
- `gls_system_notes.md` - GLS/GSH script parser: pipeline, ParsedThingBase layout, per-section field tables (types/ranges/defaults), ToXxx converters, C++ API is `src/GLS.h`
- `level_loading_notes.md` - How a level is built: `BeginLevelSession` -> `LoadLevel` -> `ToMap`, the `.cut`/`.map`/`.opt`/`.loc` sidecar caches, the `LevelMeshHeader` geometry format, the `use <role> in team <n> for "<rif object>"` placed-object binding hash on `ParsedMap+0x1b60`, both spawn factories, and the three seams for replacing the `.gls` path with a native level builder
- `role_system_notes.md` - `Role` (0xc0) field-by-field: the entity hash table (0x007b48f0), lifecycle (`CreateRole`/`ToRole`/`RoleDtor`/`DestroyRoles`), the two embedded 16-byte list headers (vulnerabilities @ 0x68, sever points @ 0xac), the `flags` bitfield, `InventoryInfo`, pickup classification via `character->aggression*10`, the `ai` -> Actor-subclass dispatch (`CreateActor`), the spawn path (`SpawnRole`), three `ToRole` defects, and (§10) the whole vulnerability subsystem - `Vulnerability` (0x1c), `VulnerabilityType`, the 0xc-byte list sentinel, and the four population paths
- `role_subobjects_notes.md` - the four `Role` sub-objects: `Character` (0xb8, with `ToCharacter`'s unit conversions), `Projectile` (0x20), `ParticleGenerator` (0xd4: GLS fields, the five 0x18-byte `PGenChannel` records, the `ParticleType` enum, and the template -> emitter map from `ParticleEmitter_Ctor`), and the 3-variant `Destructibility` family (base 0x8 / `FragData` 0x24 / `ReplaceDestructibility` 0x10, dispatched on the `+0x04` tag by `Frag` @ 0x0052e220)
- `threading_model_notes.md` - Two game threads (main "client" + executor "server"), loopback message queues (full `MsgQueue`/`MsgQueueList`/`MsgQueueNode` layouts), pause handshake, per-thread clocks/RNG, which GkPlus hooks run on which thread, and the four script-execution entry points (all main-thread; host uses `ScriptQueue`, MP joiners use update `0x67`)
- `directplay_protocol_notes.md` - Multiplayer wire protocol: DirectPlay (`IDirectPlay4A`) COM/session setup, app GUID, SendEx/Receive framing & reliability, and the full command (client->server) and update (server->client) message-id tables with payload layouts, the `0x87` lock-step turn model, and update `0x67` (§8.11) which makes every client run a trigger script from its **own** local `Scripts\` copy
- `menu_system_notes.md` - Both menu systems (front-end `Menus[36]` + in-game `InGameMenus[7]`): `Menu`/`MenuListItem` layouts, the four item constructors and the 4 item types, the full 0-35 menu inventory with titles and populators, the (menu, item) -> action transition map, navigation/rendering/input, key bindings, and the localized string table
- `save_system_notes.md` - `.sav`/`.msv` savegame format: full field-by-field stream layout, the header-only "carry to next level" variant, the team carry-over roster, and why the console `SAVE`/`LOAD` commands are the demo system instead
- `input_notes.md` - Input subsystem: keyboard runs on Win32 `WM_KEYDOWN` (`MainWindowWndProc` -> `HandleKeyMessage` -> VK->DIK `VkToScanCodeTable` -> the universal `HandleKeyPress4` sink), mouse on Raw Input, and the DirectInput `SysKeyboard` is a vestigial acquired-but-never-read fossil whose `Acquire()` arms the `WH_KEYBOARD_LL` hook that lags system keyboard input under a debugger; the `InputFixSystem` (`src/InputFix.cpp`) detours `AcquireDInputDevice` to suppress it
- `rif_chunk_format.md` - the `.rif` asset format: 12-byte chunk header, `REBCRIF1` Huffman
  container, all 105 registered chunk types, **and** the AvP upstream mapping (see below)
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
| 0x007b74dc | LevelList | levelList (0x10-byte list header `{sentinel, count, cache, cache_valid}`) |
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
| 0x0043f250 | FastCall<int, unsigned char*> | ExecuteCommandFile |

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
| 0x004e95e0 | StdCall<void> | SetupMenus |
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
| 0x005715b0 | StdCall<void, void*> | pool_free — returns an emptied page to the real CRT free |
| 0x005e3f64 | StdCall<void, void*, int> | `Dealloc?` (sized wrapper; discards the size, calls pool_free) |
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
| 0x004e0980 | StdCall<void> | LoadLevel |
| 0x0047f160 | ThisCall (member) | ToMap - builds TheMap and spawns placed objects |
| 0x0047efa0 | ThisCall (member) | CheckValue_Map - handles `use ... in team ... for ...` |
| 0x00470f20 | ThisCall<void, Map*, void*, Vec3*, LevelMeshHeader*> | Map_Ctor |
| 0x005035b0 | FastCall<int, int, Role*, Vec3*, Vec4*> | ServerSpawnActorForTeam |
| 0x004fce90 | FastCall<void*, int, Role*, Vec3*, Vec4*> | ClientSpawnActorForTeam |
| 0x005aaac0 | ThisCall<void, List*, void*, const char*> | RifFilterObjectsByName (ECX=out list, EDX=rif) |
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
`rif_pos * world_unit_scale - origin`. 0x24..0x88 and 0x8c..0xa4 are still unmapped -
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
  it — so a decompiled `free(x)` and a decompiled `Dealloc?(x, n)` are the same call and strings
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
- **A duplicated name in an export list is a latent `SyntaxError`.** `imgui-quickjs` listed
  `JS_ENUM_DEF(SortDirection)` twice, which cost nothing until something did
  `import * as ImGui from "ImGui"` — building a namespace object rejects duplicate exports, so the
  whole module failed to link and the host reported "duplicate exported name 'SortDirection'". A
  long hand-maintained `JSCFunctionListEntry` array needs a duplicate check, not review:
  `awk` the array out of the file, `grep -oE 'JS_(CFUNC_DEF\("[A-Za-z0-9_]+"|ENUM_DEF\([A-Za-z0-9_]+)'`,
  `sort | uniq -d`.

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
