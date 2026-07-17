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
cmake --preset default
cmake --build build
cmake --build build --target copy   # copies d3d8.dll to Gunlok's Steam directory
```

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
| `src/Menu.h/cpp` | `gk.menu` | set_onsetup(callback) - callback receives {get=fn} to get Menu objects; Menu:add_item(label, callback) |
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
| `src/Menus.inc.h` | X-macro listing all 33 Gunlok menus with IDs (0-35, gaps at 11,14-20) |
| `imgui-quickjs/` | Static library: QuickJS bindings for ImGui |

## Reverse Engineering Reference

### Detailed Documentation Files
- `actor_vtable_notes.md` - Actor class hierarchy, all 83+ vtable slots, subclass sizes, constructor addresses
- `trigger_system_notes.md` - 22 trigger types, data structures, console command syntax, function addresses
- `gls_system_notes.md` - GLS/GSH script parser: pipeline, ParsedThingBase layout, per-section field tables (types/ranges/defaults), ToXxx converters, C++ API is `src/GLS.h`
- `gls.txt` - Game Level Structure file format quick field list (superseded by gls_system_notes.md)

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

**Menu System:**
| Offset | Type | Name |
|--------|------|------|
| 0x007b76d0 | Menu[36]* | Menus |
| 0x007b732c | MenuIndex* | ChosenMenu |
| 0x006a7d6c | int* | ChosenMenuItem |
| 0x007b74dc | LevelList* | levelList |

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

**Menu:**
| Offset | Signature | Name |
|--------|-----------|------|
| 0x004f7a60 | ThisCall<void, Menu*, const char*> | AddMenu |
| 0x004e95e0 | StdCall<void> | SetupMenus |
| 0x004ecf10 | StdCall<void> | OnMenuItemClicked |

**Misc:**
| Offset | Signature | Name |
|--------|-----------|------|
| 0x00474540 | FastCall<Parsed*, const char*, int> | ParseGLS |

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
- `UNKNOWN_METHOD(name)` macro marks vtable slots with known position but unknown signature
