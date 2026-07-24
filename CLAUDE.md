# GkPlus - Gunlok Modding Framework

A 32-bit Windows DLL mod for the game **Gunlok** (2000). Built as a `d3d8.dll` proxy that hooks
into the game via Microsoft Detours, exposing game systems to Lua 5.4 scripting. The game binary
is actively being reverse engineered in Ghidra, accessible via MCP.

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

`cmake --build build` is incremental and quick. The `static_assert`s in `src/Actors.cpp` are the
check for any struct-layout or vtable edit — build after touching them rather than eyeballing.
`-Winvalid-offsetof` warnings on `Actor`/its subclasses and on `Map` are pre-existing and benign
(they are non-standard-layout due to virtuals); a clean build still links `d3d8.dll`. Every struct
modelled with pure virtuals produces them — that is the expected cost of the convention below, not
a signal to switch back to an explicit vtbl field.

### Dependencies (vcpkg.json)

| Package | Purpose |
|---------|---------|
| lua | Lua 5.4 scripting engine |
| imgui (dx9-binding, win32-binding) | In-game GUI overlay |
| d3d8to9 | Direct3D 8 to 9 translation layer |
| detours | Microsoft Detours - function hooking |
| quickjs-ng | QuickJS JavaScript engine |
| dear-bindings | ImGui language bindings |

Custom vcpkg ports in `ports/` for: d3d8to9, detours, quickjs-ng, dear-bindings.
Overlay configuration in `vcpkg-configuration.json`.

## Architecture

### DLL Lifecycle (src/entry.cpp)

1. `DllMain(DLL_PROCESS_ATTACH)`:
   - Resolves global pointers (triggers, doors) via `GetObjectAtOffset`
   - Initializes Lua VM (`Lua::Init()`, `luaL_openlibs`)
   - Opens a Detours transaction, constructs all modules (which attach hooks), commits
   - Loads and executes `main.lua`
2. `DllMain(DLL_PROCESS_DETACH)`:
   - Opens Detours transaction, destroys modules (which detach hooks), commits
   - Closes Lua VM

### Lua Layer (src/Module.h, src/LuaEngine.h)

The CRTP module base, the concepts-based type interop, the Lua userdata wrappers and the
per-module API surface are documented in `lua_notes.md`.

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
| `src/entry.cpp` | DllMain, module instantiation order, global pointer setup |
| `src/Core.h/cpp` | `GetBaseAddress()`, `GetObjectAtOffset()`, `DebugWrite()` |
| `src/Module.h` | CRTP module base - auto-registers in Lua `package.preload` |
| `src/LuaEngine.h/cpp` | Lua VM init/close, type interop (concepts, Fields, Create) |
| `src/DetourUtils.h` | Member function DetourAttach/DetourDetach wrappers |
| `src/List.h` | `List<T>` / `List_Member<T>` / `List_Member_Base<T>` — layout mirror of AvP's `list_tem.hpp`, with sentinel-safe `begin()`/`end()` |
| `src/HashTable.h` | `HashTableBase<T>` / `HashTable<T>` — layout mirror of AvP's `Hash_tem.hpp`, with bucket-walking `begin()`/`end()` |
| `src/Varint.h` | Variable-length integer encode/decode (used for Lua refs in trigger scripts) |

### Lua-facing sources

The Lua userdata wrappers (`src/Actors`, `src/Roles`, `src/Map`, `src/Vulnerability`,
`src/Music`, `src/Math`) and the `gk.*` modules (`src/Console`, `src/Menu`, `src/Tokens`,
`src/Triggers`, `src/Misc`, `src/Music`, `src/Memory`, `src/GUI`, `src/Camera`, `src/Debug`,
`src/AI`) are tabulated with their APIs in `lua_notes.md`.

### Other

| File | Purpose |
|------|---------|
| `src/Chunks.h/cpp` | ChunksModule - hooks chunk registration (debug logging only, no Lua API, currently commented out in entry.cpp) |
| `src/InputFix.h/cpp` | InputFixModule - hook-only, no Lua API. Detours `AcquireDInputDevice` to suppress the vestigial DirectInput keyboard acquire and its `WH_KEYBOARD_LL` hook (see `input_notes.md`) |
| `src/ImGuiBindings.h/cpp` | ImGui C++ to Lua/JS bindings via imgui-quickjs |
| `src/Menus.inc.h` | X-macro listing all 36 Gunlok menus: `GUNLOK_MENU(Name, Id, TitleResourceId, "English title")`. There are no gaps - ids 11 and 14-20 are identified in `menu_system_notes.md` |
| `imgui-quickjs/` | Static library: QuickJS bindings for ImGui |

## Reverse Engineering Reference

### Detailed Documentation Files

- `lua_notes.md` - the Lua layer: `Module<Derived>` CRTP registration, the `LuaEngine` concepts/`Fields<>` interop, the userdata wrappers, every `gk.*` module's API, and the `gk.menu` surface
- `actor_vtable_notes.md` - Actor class hierarchy, all 83+ vtable slots, subclass sizes, constructor addresses
- `trigger_system_notes.md` - 22 trigger types, data structures, console command syntax, function addresses
- `gls_system_notes.md` - GLS/GSH script parser: pipeline, ParsedThingBase layout, per-section field tables (types/ranges/defaults), ToXxx converters, C++ API is `src/GLS.h`
- `level_loading_notes.md` - How a level is built: `BeginLevelSession` -> `LoadLevel` -> `ToMap`, the `.cut`/`.map`/`.opt`/`.loc` sidecar caches, the `LevelMeshHeader` geometry format, the `use <role> in team <n> for "<rif object>"` placed-object binding hash on `ParsedMap+0x1b60`, both spawn factories, and the three seams for replacing the `.gls` path with a native/Lua level builder
- `role_system_notes.md` - `Role` (0xc0) field-by-field: the entity hash table (0x007b48f0), lifecycle (`CreateRole`/`ToRole`/`RoleDtor`/`DestroyRoles`), the two embedded 16-byte list headers (vulnerabilities @ 0x68, sever points @ 0xac), the `flags` bitfield, `InventoryInfo`, pickup classification via `character->aggression*10`, the `ai` -> Actor-subclass dispatch (`CreateActor`), the spawn path (`SpawnRole`), three `ToRole` defects, and (§10) the whole vulnerability subsystem - `Vulnerability` (0x1c), `VulnerabilityType`, the 0xc-byte list sentinel, and the four population paths
- `role_subobjects_notes.md` - the four `Role` sub-objects: `Character` (0xb8, with `ToCharacter`'s unit conversions), `Projectile` (0x20), `ParticleGenerator` (0xd4: GLS fields, the five 0x18-byte `PGenChannel` records, the `ParticleType` enum, and the template -> emitter map from `ParticleEmitter_Ctor`), and the 3-variant `Destructibility` family (base 0x8 / `FragData` 0x24 / `ReplaceDestructibility` 0x10, dispatched on the `+0x04` tag by `Frag` @ 0x0052e220)
- `threading_model_notes.md` - Two game threads (main "client" + executor "server"), loopback message queues (full `MsgQueue`/`MsgQueueList`/`MsgQueueNode` layouts), pause handshake, per-thread clocks/RNG, which GkPlus hooks run on which thread, and the four script-execution entry points (all main-thread; host uses `ScriptQueue`, MP joiners use update `0x67`)
- `directplay_protocol_notes.md` - Multiplayer wire protocol: DirectPlay (`IDirectPlay4A`) COM/session setup, app GUID, SendEx/Receive framing & reliability, and the full command (client->server) and update (server->client) message-id tables with payload layouts, the `0x87` lock-step turn model, and update `0x67` (§8.11) which makes every client run a trigger script from its **own** local `Scripts\` copy
- `menu_system_notes.md` - Both menu systems (front-end `Menus[36]` + in-game `InGameMenus[7]`): `Menu`/`MenuListItem` layouts, the four item constructors and the 4 item types, the full 0-35 menu inventory with titles and populators, the (menu, item) -> action transition map, navigation/rendering/input, key bindings, and the localized string table
- `save_system_notes.md` - `.sav`/`.msv` savegame format: full field-by-field stream layout, the header-only "carry to next level" variant, the team carry-over roster, and why the console `SAVE`/`LOAD` commands are the demo system instead
- `input_notes.md` - Input subsystem: keyboard runs on Win32 `WM_KEYDOWN` (`MainWindowWndProc` -> `HandleKeyMessage` -> VK->DIK `VkToScanCodeTable` -> the universal `HandleKeyPress4` sink), mouse on Raw Input, and the DirectInput `SysKeyboard` is a vestigial acquired-but-never-read fossil whose `Acquire()` arms the `WH_KEYBOARD_LL` hook that lags system keyboard input under a debugger; the `gk.inputfix` module (`src/InputFix.cpp`) detours `AcquireDInputDevice` to suppress it
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

**Console System:** (the whole block 0x007b6950-0x007b6b41 is mapped in the Ghidra DB)

| Offset | Type | Name |
|--------|------|------|
| 0x007b6958 | char[252] | CommandLine (`ConsoleCommandLine`) |
| 0x007b6b40 | char[0xfc] | SavedConsoleCommandLine (ESC-stash of the line) |
| 0x007b6950 | unsigned | TextColor (`ConsoleTextColor`, "TEXT COLOR" cmd) |
| 0x007b6954 | unsigned | UITextColorLight 0xffccccd6 (scrolling msgs, briefing) |
| 0x007b6a64 / 0x007b6a68 | unsigned | UIColorDim 0xff595966 / UIColorYellow 0xffffef47 |
| 0x007c149c | unsigned* | CursorColor |
| 0x007b6a54/58/5c/60 | Font* | ConsoleSmallFont / ConsoleLargeFont / HudSmallFont / ConsoleLargeFont2 (all built in FUN_004d5380) |
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
| 0x004d35f0 | ThisCall<void, Tokens*, const char*, float> | CreateToken |

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
 |   |   |   +- TurretActor (0x320 bytes)
 |   |   +- NodeActor (0x278 bytes)
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
`role->ai` to the Actor subclass; `SpawnRole` @ 0x00503710 is `gk.roles` `Role:spawn`.

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
