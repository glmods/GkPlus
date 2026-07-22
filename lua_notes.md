# GkPlus Lua Layer

How C++ game systems are exposed to Lua 5.4: the CRTP module base, the concepts-based type
interop, and the per-module API surface. The game-side reverse engineering lives in `CLAUDE.md`
and the `*_notes.md` files.

## Module System (src/Module.h)

All game subsystems use `Module<Derived>` (CRTP). The constructor registers into Lua's
`package.preload[Derived::module_name]` so Lua code uses `require("gk.xxx")`.

A module that defines `Register(lua_State*)` gets auto-registered. Modules without `Register`
(like `ChunksModule`) only hook functions without exposing a Lua API.

Adding a module means **three** edits beyond the new `.h/.cpp`: the source list in
`CMakeLists.txt`, the `#include` in `entry.cpp`, and *both* the member and the ctor init-list
entry in `entry.cpp`'s `Modules` struct.

Two module shapes: `Register` returns a `Lua::Create<State>` userdata when the module is a flat
set of dynamic scalars (`gk.misc`), or a plain `lua_newtable` of `lua_pushcfunction`s when it
needs **nested tables** with their own metamethods (`gk.menu`, `gk.roles`, `gk.map`). A `Fields<>`
userdata cannot carry a sub-table — its `__index` is a C function, not a table.

## Lua Type System (src/LuaEngine.h)

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

## Game Object Wrappers (Lua userdata types)

| File | Lua type | Key fields |
|------|----------|------------|
| `src/Actors.h/cpp` | `Actor` | id, position, orientation, team_id, role, center, ai_type, vulnerabilities, health |
| `src/Roles.h/cpp` | `Role` | id, type (ai enum), name, vulnerabilities, spawn() |
| `src/Map.h/cpp` | `Map`, `TeamSlot` | origin, bounds_min/max, camera_focus_min/max, bitmap, shadow_object_*, num_sections, to_world() |
| `src/Vulnerability.h/cpp` | `Vulnerability` | role, vulnerability_role, delay, duration, script, type |
| `src/Music.h/cpp` | `MusicTrack` | volume, looping, playing, play(path[, loop]), stop() |
| `src/Math.h/cpp` | `Vec3`, `Vec4` | x, y, z [, w] (read-only fields) |

## Lua Modules (`require("gk.xxx")`)

| File | Module name | Key API |
|------|-------------|---------|
| `src/Console.h/cpp` | `gk.console` | print(), set_text_color(), set_cursor_color(), execute(), register_command(), set_onprint(), set_onsetup() |
| `src/Menu.h/cpp` | `gk.menu` | See "Menu module API" below |
| `src/Tokens.h/cpp` | `gk.tokens` | Table-like: tokens.name = value, pairs(tokens), #tokens |
| `src/Actors.h/cpp` | `gk.actors` | actors[id] returns Actor, pairs(actors) iterates all |
| `src/Roles.h/cpp` | `gk.roles` | roles[id] or roles["name"], pairs(roles) iterates all |
| `src/Map.h/cpp` | `gk.map` | current(), world_unit_scale(), spawn(role, team, pos, ori), teams[i] / #teams / pairs(teams) |
| `src/Triggers.h/cpp` | `gk.triggers` | add_time_trigger(delay, callback) |
| `src/Misc.h/cpp` | `gk.misc` | game_mode, game_state, battle_number, game_difficulty, actor_under_cursor, foobar, parse_gls() |
| `src/Music.h/cpp` | `gk.music` | current (MusicTrack or nil), battle_volume (0..9 setting). Also owns the `MusicTrack_Ctor` hook that fixes the ignored-volume bug |
| `src/Memory.h/cpp` | `gk.memory` | Direct memory read/write |
| `src/GUI.h/cpp` | `gk.gui` | ImGui integration |
| `src/Camera.h/cpp` | `gk.camera` | Camera control |
| `src/Debug.h/cpp` | `gk.debug` | Debug utilities |
| `src/AI.h/cpp` | `gk.ai` | AI system access |

## Menu module API (`require("gk.menu")`)

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

## Conventions

- `lua_pushcfunction` is a **macro**: a lambda containing a comma at paren depth 0 (e.g.
  `Lua::PushMemberFunction<A, &A::b>`) is read as two macro args. Wrap the lambda in extra
  parens — `lua_pushcfunction(L, ([](lua_State *L) { ... }));` — as `Roles.cpp` does.
- Lua refs are stored as varint-encoded bytes in the game's `script_name` fields (high bit set =
  Lua ref, not filename); see `src/Varint.h`.
