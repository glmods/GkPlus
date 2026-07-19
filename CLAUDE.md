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

### Module System (src/Module.h)

All game subsystems use `Module<Derived>` (CRTP). The constructor registers into Lua's
`package.preload[Derived::module_name]` so Lua code uses `require("gk.xxx")`.

A module that defines `Register(lua_State*)` gets auto-registered. Modules without `Register`
(like `ChunksModule`) only hook functions without exposing a Lua API.

### Lua Type System (src/LuaEngine.h)

Custom C++20 concepts-based Lua interop:

- **`LuaObject`**: Any type with `metatable_name` and `setup_metatable(L)` static members
- **`Lua::Create<T>(L, args...)`**: Allocates userdata, sets up metatable with `__gc`, `__tostring`, `__eq`, `__index`/`__newindex` (from `fields`)
- **Field descriptors** (used in `Fields<...>` type lists):
  - `Slot<Name, Member>` / `ROSlot` - direct member read/write or read-only
  - `Getter<Name, Method>` - calls a getter method
  - `GetterSetter<Name, Get, Set>` - getter + setter methods
  - `TableGetter<Name, Method>` - getter that returns a Lua table
  - `Function<Name, Type, Method>` - member function callable from Lua
  - `StaticSlot` / `StaticGetter` / `StaticFunction` - static/global variants

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
| `src/Varint.h` | Variable-length integer encode/decode (used for Lua refs in trigger scripts) |

### Game Object Wrappers (Lua userdata types)

| File | Lua type | Key fields |
|------|----------|------------|
| `src/Actors.h/cpp` | `Actor` | id, position, orientation, team_id, role, center, ai_type, vulnerabilities, health |
| `src/Roles.h/cpp` | `Role` | id, type (ai enum), name, vulnerabilities, spawn() |
| `src/Vulnerability.h/cpp` | `Vulnerability` | role, vulnerability_role, delay, duration, script, type |
| `src/Math.h/cpp` | `Vec3`, `Vec4` | x, y, z [, w] (read-only fields) |

### Lua Modules (`require("gk.xxx")`)

| File | Module name | Key API |
|------|-------------|---------|
| `src/Console.h/cpp` | `gk.console` | print(), set_text_color(), set_cursor_color(), execute(), register_command(), set_onprint(), set_onsetup() |
| `src/Menu.h/cpp` | `gk.menu` | See "Menu module API" below |
| `src/Tokens.h/cpp` | `gk.tokens` | Table-like: tokens.name = value, pairs(tokens), #tokens |
| `src/Actors.h/cpp` | `gk.actors` | actors[id] returns Actor, pairs(actors) iterates all |
| `src/Roles.h/cpp` | `gk.roles` | roles[id] or roles["name"], pairs(roles) iterates all |
| `src/Triggers.h/cpp` | `gk.triggers` | add_time_trigger(delay, callback) |
| `src/Misc.h/cpp` | `gk.misc` | game_mode, game_state, battle_number, game_difficulty, actor_under_cursor, foobar, parse_gls() |
| `src/Memory.h/cpp` | `gk.memory` | Direct memory read/write |
| `src/GUI.h/cpp` | `gk.gui` | ImGui integration |
| `src/Camera.h/cpp` | `gk.camera` | Camera control |
| `src/Debug.h/cpp` | `gk.debug` | Debug utilities |
| `src/AI.h/cpp` | `gk.ai` | AI system access |

### Other

| File | Purpose |
|------|---------|
| `src/Chunks.h/cpp` | ChunksModule - hooks chunk registration (debug logging only, no Lua API, currently commented out in entry.cpp) |
| `src/ImGuiBindings.h/cpp` | ImGui C++ to Lua/JS bindings via imgui-quickjs |
| `src/Menus.inc.h` | X-macro listing all 36 Gunlok menus: `GUNLOK_MENU(Name, Id, TitleResourceId, "English title")`. There are no gaps - ids 11 and 14-20 are identified in `menu_system_notes.md` |
| `imgui-quickjs/` | Static library: QuickJS bindings for ImGui |

### Menu module API (`require("gk.menu")`)

| Member | Purpose |
|--------|---------|
| `set_onsetup(cb)` | Runs after `SetupMenus`; `cb` receives `{get=fn}` |
| `menus` | Indexable by id **or** name (`menus[25]`, `menus.Audio`), `pairs`-iterable, `#menus` |
| `get(id_or_name)` / `get_ingame(0..6)` | A `Menu` object, or nil |
| `goto_menu(id_or_name[, remember_parent])` | Calls the game's `GoToMenu` |
| `chosen_menu()` / `chosen_item()` / `set_chosen_item(n)` | Front-end selection state |
| `ingame_menu_index()` / `ingame_selected_item()` | In-game (HUD) menu state |
| `is_ingame_menu_open()` / `close_ingame_menu(kind)` | In-game menu lifecycle |
| `resource_string(id)` | Resolve a `GL_RESOURCE_ID` through the active language DLL |
| `ids` / `names` / `titles` | name->id, id->name, id->English title |
| `item_type` / `pseudo_item` / `visible_rows` | `{plain,value,toggle,choice}`, `{none,back,scroll_up,scroll_down}`, `6` |

`Menu` objects: `id`, `in_game`, `name`, `title`, `title_id`, `parent_id`, `num_items`,
`num_nodes`, `scroll_offset` (rw), `items`, `item(i)`, `activate([remember])`,
`add_item(label[, cb])`, `add_value_item(label, value[, cb])`,
`add_toggle(label, initial[, cb])`, `add_choice(label, {resource_ids}[, initial[, cb]])`.

`MenuItem` objects: `index`, `label`, `type`, `type_name`, `value_text`, `is_current_value`,
`rect`, `get_value()`, `set_value(v)`.

Strings and bound variables passed from Lua are copied into module-owned storage, because the
game stores label pointers with `label_is_static = 1` and never copies or frees them.

## Reverse Engineering Reference

### Detailed Documentation Files

- `actor_vtable_notes.md` - Actor class hierarchy, all 83+ vtable slots, subclass sizes, constructor addresses
- `trigger_system_notes.md` - 22 trigger types, data structures, console command syntax, function addresses
- `gls_system_notes.md` - GLS/GSH script parser: pipeline, ParsedThingBase layout, per-section field tables (types/ranges/defaults), ToXxx converters, C++ API is `src/GLS.h`
- `threading_model_notes.md` - Two game threads (main "client" + executor "server"), loopback message queues (full `MsgQueue`/`MsgQueueList`/`MsgQueueNode` layouts), pause handshake, per-thread clocks/RNG, which GkPlus hooks run on which thread, and the four script-execution entry points (all main-thread; host uses `ScriptQueue`, MP joiners use update `0x67`)
- `directplay_protocol_notes.md` - Multiplayer wire protocol: DirectPlay (`IDirectPlay4A`) COM/session setup, app GUID, SendEx/Receive framing & reliability, and the full command (client->server) and update (server->client) message-id tables with payload layouts, the `0x87` lock-step turn model, and update `0x67` (§8.11) which makes every client run a trigger script from its **own** local `Scripts\` copy
- `menu_system_notes.md` - Both menu systems (front-end `Menus[36]` + in-game `InGameMenus[7]`): `Menu`/`MenuListItem` layouts, the four item constructors and the 4 item types, the full 0-35 menu inventory with titles and populators, the (menu, item) -> action transition map, navigation/rendering/input, key bindings, the localized string table, and the Ghidra DB defects that had to be repaired first
- `save_system_notes.md` - `.sav`/`.msv` savegame format: full field-by-field stream layout, the header-only "carry to next level" variant, the team carry-over roster, and why the console `SAVE`/`LOAD` commands are the demo system instead
- `gls.txt` - Game Level Structure file format quick field list (superseded by gls_system_notes.md)

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
- Verify against the Ghidra DB, not the `*_notes.md`. Several long-standing claims in the notes
  were never measured and turned out wrong.

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
- For `__thiscall`, the `this` type comes from the function's **parent class namespace**
  (`setParentNamespace`), not `updateFunction`; a parameter literally named `this` binds to ECX.
- After renaming in Ghidra, `grep` the `*.md` files for the old `FUN_`/`DAT_` name.

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
| 0x007ba0d8 | Actors* | actors (hash table) |

**Role System:**

| Offset | Type | Name |
|--------|------|------|
| 0x007b48f0 | Roles* | roles (hash table) |

**Token System:**

| Offset | Type | Name |
|--------|------|------|
| 0x007b6af8 | Tokens* | tokens |

**Console System:**

| Offset | Type | Name |
|--------|------|------|
| 0x007b6958 | char* | CommandLine |
| 0x007b6950 | unsigned* | TextColor |
| 0x007c149c | unsigned* | CursorColor |
| 0x007b6aa8 | CommandList* | List (command list) |

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
| 0x007b74dc | LevelList | levelList (0x10-byte list header, not 8) |
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

**Misc:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00474540 | FastCall<Parsed*, const char*, int> | ParseGLS |

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
 +- UnknownActor (0x120 bytes)
```

Key Actor struct offsets (vtable ptr implicit at 0x00): `id` @ 0x0c, `vulnerabilities` @ 0x10,
`ai_type` @ 0x50, `position` (Vec3) @ 0xa0, `orientation` (Vec4) @ 0xac, `team_id` @ 0xbc, `role` @ 0xc0.

No RTTI - type checking uses virtual methods (IsCharacter, IsMobile, IsTurret, etc.).

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

Key fields: `name` @ 0x00, `shape` @ 0x18, `hierarchy` @ 0x1c, `hotspot` @ 0x44,
`character` @ 0x60, `inventory_info` @ 0x64, `vulnerabilities` @ 0x68,
`ai` @ 0x7c (determines Actor subclass), `armor` @ 0x94, `shields` @ 0x98, `id` @ 0xbc.

### Imports

Key external libraries: BINKW32.DLL (video), STEAM_API.DLL, D3D8.DLL,
KERNEL32/USER32/GDI32/ADVAPI32/OLE32/WINMM (Windows API).

## Conventions

- Everything lives in the `gk` namespace
- Game addresses are always offsets added to base address (never hardcoded absolutes)
- Hash tables (actors, roles) use bucket arrays with linked list chaining
- Linked lists (triggers, tokens) are doubly-linked with sentinel nodes
- Lua refs stored as varint-encoded bytes in script_name fields (high bit set = Lua ref, not filename)
- Detour hooks follow: resolve original -> attach in constructor -> detach in destructor
- `static_assert` on struct sizes and offsets to catch layout mismatches
- Game vtables are modelled in `src/Actors.cpp` as **declaration-ordered pure virtuals**: the base
  `Actor` declares 83 (slot 0 is the destructor), and each subclass appends its own extension slots
  in vtable order. Adding a virtual there is how you record a new slot — it costs no object size
  (the vptr already exists), so the `static_assert(sizeof(...))` guards still hold and will catch a
  mistake. Cross-check slot numbers against `actor_vtable_notes.md`.
- Fields with a known offset but unknown meaning are named `field0xNN` / `unkN[...]` padding; a
  getter/setter of unknown purpose is named `GetField0xNN` / `SetField0xNN`

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
