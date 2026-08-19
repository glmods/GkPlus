# Address map

Every global, function address, struct offset and enum recovered so far, moved out of
`CLAUDE.md` so it is looked up rather than carried in every session's context. Addresses
are offsets from the image base; see `CLAUDE.md` for how `GetObjectAtOffset` resolves them,
and **read the calling-convention warning in `CLAUDE.md`'s Analysis Traps before wrapping
any function here** - a wrong `RET` form drifts ESP and faults somewhere unrelated.

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
| 0x007b6a54/58/5c/60 | Font* | **SmallFont / LargeFont / HudSmallFont / HeadingFont** (all built in `InitConsole`). Were `ConsoleSmallFont / ConsoleLargeFont / HudSmallFont / ConsoleLargeFont2`; three of those four names were wrong. Only `SmallFont` is a console font — the console draws through it exclusively, and `LargeFont`'s complete xref list (9 entries) contains nothing console-side. `LargeFont` and `HeadingFont` are **the same font twice**: same `large font.RIM`, same width table, same line height. All four are constructed with `line_height = 25`; `HeadingFont` measures 50-75 because `ScaleFontsForClientWidth` @ 0x004d79f0 writes `Font.scale` (+0xaf4) for that one only. Textures: `small font.RIM` / `large font.RIM` / `small font 2.RIM` / `large font.RIM`. **Construction order is not address order** — ctor #3 stores into 0x007b6a60, #4 into 0x007b6a5c |
| 0x007b6c3a | bool | **ConsoleInitialized** — 0 at load, set by `InitConsole` @ 0x004d5380 as its *last* act, cleared by `ShutdownConsole` @ 0x004d5620 after it frees and nulls all four fonts. The engine uses it as InitConsole's own idempotence guard; four references in the whole binary, all in that pair. `gk::ConsoleReady()`, and the gate `gk::Print` needs: `WinMain` reaches InitConsole at 0x0046bb81, *after* the engine's first file open, so anything on the first-open anchor is ahead of it and printing there faults on a null `SmallFont`. Re-read per call — it goes back to 0 at teardown |
| 0x007b6a70..0x007b6a7c | — | command **hash table**: NumRegisteredCommands, CommandTableNumBuckets, CommandTableMask, CommandTableBuckets (`CommandListElem**`) |
| 0x007b6aa8..0x007b6ab4 | List | command exec queue: CommandsToExecute (anchor), NumCommandsToExecute, cache, cacheValid — one popped per frame by `PumpQueuedConsoleCommand` |
| 0x007b6a80 / 0x007b6b38 | float | ConsoleTextScrollTarget / ConsoleSlidePos (open/close anim; -1=closed) |
| 0x007b6b3c | Renderable* | ConsoleBackdropSprite — a 0x1f0-byte `Renderable` over a 1024x1024 shape, built by `CreateConsoleBackdrop` @ 0x004d7b20, **not** a `Sprite` |
| 0x007b6ac8..0x007b6ad4 | RECTF | static text rect passed as `Font_QueueText`'s `rect` by the two console draws @ 0x004d7337 / 0x004d73d0. Four **floats** `{left, top, right, bottom}`, normalized 0..1 — was recorded as a "DrawText scratch arg block (X/Ctx/Scale/4)" before the callee was read |

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
proximity-activated subset — and it has exactly **one producer**, `BackgroundCreatureUnit`'s ctor
@ 0x004c9730, which appends a `List_Member` node (`pool_alloc(0x10)`, vtable 0x00666220, `data`
@ +0xc) and bumps the count at +0x693c; its dtor @ 0x004c9af0 unlinks. So every entry is a
`BackgroundCreatureUnit` or a `FlyingBackgroundCreatureUnit`.
`UnitsTable` @0x007b68f0 is a `HashTable<Unit*>` (vptr variant like
`actors`): n_entries=`NumUnits`@0x6f4, buckets=`UnitsTable_buckets`@0x6900. `CombatMusicKillCounter`
@0x007b68ec counts kind-2 entity deaths to escalate battle music (UpdateBattleMusic). The six scattered
`VestigialFloat_*` (0x6898/6904/6914/6924/6948/6a94 = 1024/1024/60/120/1024/1024) are CRT-constructed
floats with **no readers**.

**Open question on `ObjectList`, not acted on.** The client-`Unit`-tree round argued it is really the
*local player's party roster* rather than a general object list: its only insert and remove
(`UnitList_Add` @ 0x004d0310 from slot 51, `UnitList_Remove` @ 0x004d0380 from slot 91) are both
gated on `team == LocalPlayerTeam`, and every reader is a select-character / friendlies /
HUD-portrait function — while `UnitsTable` is the general list `DrawUnits` walks. That is plausible
but **not measured**: it needs `SelectAllCharacters` and `HudItem_DrawByKind` read first, so the name
above and the DB name were both deliberately left as they are.

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
| 0x007ba1f0 / 0x007ba1f4 | HudWidget** | InGameDialogA / InGameConfirmDialog — the same two objects as `InGameMenuPanels[5]` and `[6]` |
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
| 0x006a7d2c | int[] | PlayerTeam — indexed by player index; its only two accessors are `GetPlayerTeam` @ 0x004fc0e0 (`MOV EAX,[ECX*4 + 0x6a7d2c]`) and `SetPlayerTeam` @ 0x004fc340 |
| 0x007b70dc | int[6] | MaxUnitsPerTeam — `ApplyUpdateMessage` copies `msg+0x04..+0x18` into it as one run (0x004fe221-0x004fe24c); indexed 1..5 when a team assignment is validated |
| 0x006abe24 | int* | BandwidthUse — 0..9, an **index into the relevance-radius table** `IsRelevantToPlayer` uses, not a byte count: radius 120, 110, 100, 90, 80, 70, 60, 50, 40, 30 units (the ten `FILD` immediates at 0x00511284-0x00511365 are the squares 14400 … 900), fetched as `table[BandwidthUse + 4]`. Higher setting = larger radius = more traffic |

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
`TrainingAreaIndex` (0x9d14), and the **mission-stats** counters (reset by `ResetMissionStats`
@ 0x004fcc30, read by `CommandStatsScreen`, and carried to clients at debrief as update `0xa2`, 35
packed bytes. `ApplyMissionStatsUpdate` @ 0x005029d0 is the **client applier**, not the sender: it
contains no `BroadcastToPlayers`, guards on `msg[0] == 0xa2`, copies the block out only when
`!IsExecutorRunning()`, and sets `StatsScreenClientsReady`. It is reached from the client pump at
0x004fdd60, not through `ApplyUpdateMessage`, whose byte map sends `0xa2` to the default arm):
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
| 0x007b68e4 | int* | NextClientActorId (client actor id counter) |
| 0x007b6dc0 | float* | LoadingProgressFraction (`DrawLoadingProgressBar` clamps a percent into it) |
| 0x007b6dd9 | bool* | LevelSessionLoaded — `UnloadLevel`'s early-out guard, cleared at its end. Distinct from `LevelSessionStarted` @ 0x007b6dd8 |

**The mouse cursor:** (the ten shapes are the developers' own `game_cursor.rif` hierarchy names)

| Offset | Type | Name |
|--------|------|------|
| 0x007b3f78 | CursorMode* | CursorMode — typed with the `CursorMode` enum, 7 values. **One writer in the image**, `SetCursorMode` @ 0x004a28e0 |
| 0x007b476c | CursorMode* | SavedCursorMode — where `SuspendCursorMode` stashes the prior mode |
| 0x007b4738 | void** | CurrentCursor — the selected hierarchy; written only by `SetCurrentCursor` @ 0x004ad1a0 |
| 0x007b4734 | int* | CursorVariant — 0/1/2, a **target-valid** flag, not a shape index |
| 0x007b470c .. 0x007b4730 | void*[10] | Cursor_Unselected, Cursor_Standard, Cursor_Attack, Cursor_AttackGround, Cursor_PickUp, Cursor_Activate, Cursor_Drop, Cursor_Pass, Cursor_CameraLock, Cursor_HealCharacter — consecutive dwords, all built by `LoadCursorHierarchies` @ 0x0049c4d0 |
| 0x007b4740 | int* | InventoryDragState — **PROPOSED**: written 0/1/2/3 by `FUN_004a5210`, read by `DrawInventoryItemPanel` |

**0x006a5b34 is deliberately left unnamed.** `UpdateCursorForMode`'s guard reads it, but its image
value is **1** and its only writer also writes 1, so that branch is **never taken**; six further
references take its **address**, and its neighbours are the constant 2 and pointers to
`"gunlok"`/`"elint"`/`"frend"`/`"maskelyn"`. So it is probably the head of a record rather than a
flag, and naming it as one would be a guess — **PROPOSED**.

**Window and video mode:** (see `src/WindowPlacement.h`)

| Offset | Type | Name |
|--------|------|------|
| 0x006b02b8 | HWND* | GameWindow — the main window; the only handle any of the six `ShowWindow` sites and the one `SetWindowPos` ever pass |
| 0x007c1244 | int* | ViewFlags — bit 0 set = **windowed**, bit 31 set by `-r`. Selects which of `WinMain`'s two `CreateWindowExA` sites runs |
| 0x007c1250 / 0x007c1254 | int* | ResolutionWidth / ResolutionHeight — what the window is actually sized from, and what `ResetD3D2` writes into the present parameters |
| 0x006a7d78 / 0x006a7d7c / 0x006a7d80 | int* | WindowedWidth / WindowedHeight / WindowedViewFlags — the persisted pair, copied into `Resolution*` by `ReadGLKeys` when `ViewFlags & 1` |
| 0x006a7d84 / 0x006a7d88 / 0x007b80a8 | int* | FullscreenWidth / FullscreenHeight / FullscreenViewFlags — the same, for the other branch |
| 0x007c1268 / 0x007c126c | int* | GameWindowX / GameWindowY — the client area's **origin** in screen space, filled by `WinMain` from `ClientToScreen(GameWindow, {0,0})` and maintained by the `WM_MOVE` bookkeeper. Nothing repositions the window from them: the one reader passes them to a `SWP_NOMOVE` `SetWindowPos`, which discards them |
| 0x007c1270 / 0x007c1274 | int* | ClientRight / ClientBottom — the client rect's **far corner**, `GameWindowX + width` / `GameWindowY + height`. Named `OrigX`/`OrigY` in the DB until the arithmetic was read; every consumer subtracts the origin back off to recover the size |
| 0x007c1278 | RECT | DesktopRect — `GetClientRect(GetDesktopWindow())` when windowed, `{0, 0, width, height}` otherwise. Its one reader takes only the width and height from it |
| 0x007c1288 / 0x007c128c | int* | ClientWidth / ClientHeight (18 readers, all pixel comparisons — `HudItem_DrawByKind`, `DrawInventoryItemPanel`, `ApplyShadowQuality`) |

### `.rdata` / `.data` landmarks

The bands that are structure rather than content — worth knowing before reading anything as a
pointer, and the source of most of the code that was undiscovered.

| Range | What |
|-------|------|
| 0x0064d000-0x0064d380 | the **import address table**, holding on-disk name-table RVAs; already typed |
| 0x0064d384 | `__guard_check_icall_fptr` — retyped `undefined4` -> `pointer`; its value is `_guard_check_icall` and it has 105 references. *Not* part of the array below |
| 0x0064d38c-0x0064dc47 | the **MSVC C++ static-initializer array, 559 entries**, bounded by `__xc_a` @ 0x0064d388 and `__xc_z` @ 0x0064dc48 (NULL sentinels, one reference each, from `_initterm` in `_cinit`). **309 of its targets were undiscovered code** |
| 0x0064dc50-0x0064dc64 | `.CRT$XI*` C initializers (6 pointers) |
| 0x0064dc78-0x0064dc80 | `.CRT$XP*` pre-terminators (3) |
| 0x0066e000-0x00672000 | the **RIF/chunk vtable band** — 245 sub-tables; see `rif_chunk_format.md` |
| 0x006a0000-0x006a2c00 | x86 `__except_handler4` **`_EH4_SCOPETABLE`** tables — 82 headers + 84 records |
| 0x006aabe8-0x006aade7 | a **128-entry** dispatch array (0x00571c10 -> 0x005745f0). **Nothing in the image references it** — the base is computed at runtime |

0x0064dc50-0x0064dc80 is the CRT initializer/terminator run above and **not** a SafeSEH
`SEHandlerTable`: no `SEHandlerTable` / `__safe_se_handler_table` symbol exists in the database at
all.

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
| 0x0043e240 | FastCall<void, TriggerKind, Vec3*, long long, TriggerList, const unsigned char*, int> | AddTriggerToGlobalList — registers **one** trigger (`pool_alloc(0x68)` `TriggerData` plus a 0x10-byte node onto `FirstTrigger`, `NumTriggers++`), copying each name out of `targets` into the trigger's own target list. `RET 0x20` = 8 + 0x10 (`TriggerList` by value) + 4 + 4, kind/coords in ECX/EDX. 23 callers: `CommandAddTrigger` x21, `LoadLevel`, `Frag`. `src/Triggers.h` still exports it as `RegisterTriggers` |
| 0x0050c400 | ThisCall (member) | RemoveTrigger |
| 0x0044c950 | ThisCall<TriggerList*, TriggerList*, TriggerList*> | CopyList |
| 0x0044ca10 | ThisCall<TriggerList*, TriggerList*> | `TriggerList::Ctor` — AvP `list_tem.hpp`'s `List<T>::List()`, instantiated for `char *`. Exported as `InitList` |
| 0x0044c900 | ThisCall<TriggerList*, TriggerList*, const char**> | `List<char*>::Ctor1` — `List<T>::List(const T &)`: sentinel, then one `add_entry`. The element is **not** an actor name; at all 12 call sites (`CommandAddTrigger` x10, `Frag` x2) it is the empty string at 0x0064dca0. Exported as `InitListWithActorName` |
| 0x0044e8c0 | ThisCall<ITrigger*, TriggerList*, const char**> | CreateTrigger |

**Menu:** (see `menu_system_notes.md`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x004e95e0 | StdCall<void> | SetupMenus (reads FlagChooseLevel @ 0x006b0173 **once**, so the Choose Level item cannot be enabled after boot; `WinMain` sets that flag from `-chooselevel`) |
| 0x004ecf10 | StdCall<void> | OnMenuItemClicked (action dispatch) |
| 0x004fbfa0 | FastCall<void, MenuIndex, bool> | GoToMenu (ECX=target, DL=push parent) |
| 0x004f94f0 | ThisCall<void, Menu*, unsigned, int, unsigned> | Menu::Populate (firstItemId, nLabels, titleId) — **not** a constructor; was `Menu::Menu` |
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
| 0x00579000 | FastCall<const char*, void*, unsigned> | GetResourceString (ECX=&LocalizedStrings). Opens `MOV EAX,[ECX]`, then scans in 0x14-byte steps **with no end test** — so a table that is not loaded does not fault, it walks `.data` until a dword happens to match the id |
| 0x00578f30 | FastCall<void, void*, int, unsigned, void**> | LoadResourceStringTable — `WinMain` @ 0x0046b355 passes (res dll, 0, 0x7532, 0x00725664) |
| 0x00725664 | void* | **LocalizedStrings** — the string table, null until the above. `gk::ResourceString`'s readiness test, for the same reason as ConsoleInitialized: it is filled after the engine's first file open |

**The mouse cursor:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00498140 | CDecl<void> | UpdateCursorForMode — per-frame cursor selector, was `FUN_00498140`. Bare `RET`; body `[0x00498140, 0x004985df]` = 1,184 bytes once its jump table was resolved |
| 0x004a28e0 | FastCall<void, CursorMode> | SetCursorMode(mode /*ECX*/) — the **sole** writer of `CursorMode` |
| 0x004a0d70 | — | SuspendCursorMode — saves the prior mode to `SavedCursorMode` and sets 6 |
| 0x004a0da0 | — | RestoreCursorMode |
| 0x004a0d50 | — | ResetCursorModeAndRefresh — `SetCursorMode(0)` then a tail `JMP UpdateCursorForMode` |
| 0x004ad1a0 | FastCall<void, void*> | SetCurrentCursor(cursor /*ECX*/) — stores to `CurrentCursor`, tail-jumps to the below |
| 0x004a2140 | — | ApplyWin32CursorShape — maps `CurrentCursor` to a `LoadCursorA` id, and does nothing else. **Was named `SetCursor`**, which is now the USER32 import thunk at 0x005712e1 |
| 0x0049c4d0 | — | LoadCursorHierarchies — builds all ten cursors from `user interface/game_cursor.rif` |
| 0x0049f340 | FastCall<char> | IsInventoryScreenOpen — `CMP dword [0x007b6e50],0; SETNZ AL; RET` |

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
| 0x0055a410 | ThisCall (member) | HudWidget_Ctor — writes `HudWidget_vtbl` and sets `+0x60` (the widget kind) from its first stack argument. The base constructor behind all 81 construction sites |
| 0x0055f8d0 | ThisCall (member) | HudWidget_Dtor |
| 0x005658d0 | ThisCall (member, RET 0x8) | HudMenuWidget_Ctor |
| 0x00565910 | ThisCall (member) | HudMenuWidget_Dtor — 2 instructions, tail-jumps to `HudWidget_Dtor` |
| 0x0056c280 | ThisCall (member) | InGameDialogButton_Ctor |
| 0x0056c380 | ThisCall (member) | InGameDialogButton_OnActivated — slot 4 of `InGameDialogButton_vtbl`, and the **actual** target of all three slot-4 dispatches in the image |
| 0x0056c5f0 | ThisCall (member) | InGameDialogLabel_Ctor |
| 0x0056c920 | ThisCall (member) | InGameDialogA_Ctor |
| 0x0056a2b0 | — | ActivateInGameDialogA_Default — reads `InGameDialogA`, calls slot 18, tail-jumps to slot 4 of the result |
| 0x0056a2d0 | — | ActivateInGameConfirmDialog_Default — the same shape on `InGameConfirmDialog` |

**Window and video mode:** (see `src/WindowPlacement.h`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x0046aef0 | StdCall (RET 0x10) | WinMain — registers the one window class (`"GLClass"`, resource id 0x2f07) and holds the binary's **only two** `CreateWindowExA` sites: windowed @ 0x0046b585 (`WS_EX_APPWINDOW`, style 0x90CF0000) and fullscreen @ 0x0046b5cf (`WS_EX_TOPMOST`, style 0x90080000). Both pass **X = 0, Y = 0** as literals; the game never asks Windows where a window may go |
| 0x004f6f10 | - | ReadGLKeys — the only thing that fills `Resolution*` before creation, from the `Windowed*`/`Fullscreen*` pair per `ViewFlags & 1` |
| 0x0046a0b0 | FastCall<int, int, int, int, int> (RET 0x8) | SetVideoMode(width ECX, height EDX, bpp, view_flags) — the single entry point for a mode change and the binary's **only** `SetWindowPos` caller (0x0046a183), which passes `SWP_NOMOVE\|SWP_NOZORDER`, so it resizes in place and its X/Y arguments are discarded |
| 0x0046a560 | FastCall<void, int, int> | OnClientSizeChanged(width ECX, height EDX) — the `WM_SIZE` bookkeeper (`lParam`'s packed LOWORD/HIWORD). **Resizes no window**: it recomputes `ClientRight`/`ClientBottom`/`Client*`/`DesktopRect` and a pair of 640x480-relative UI scale factors at 0x007c1290/0x007c1294. Was `WindowResize` in the DB, which cost real time to rule out |
| 0x00470dd0 | FastCall<void, int, int> | OnWindowMoved(x ECX, y EDX) — the `WM_MOVE` bookkeeper. **Moves no window**: it is the only writer of `GameWindowX/Y`, and keeps `ClientRight`/`ClientBottom` correct by the same delta. `WinMain` @ 0x0046b91b re-derives the origin from the live window via `ClientToScreen`, which is what keeps the mouse mapping correct wherever the window ends up. Was `WindowMove` |

gl.exe's user32 import set contains **no** `MoveWindow`, `SetWindowLongA/W`, `AdjustWindowRect(Ex)`,
`SystemParametersInfo`, `GetMonitorInfo`, `MonitorFromWindow` or `SetWindowPlacement` — so nothing
in the game can reposition or restyle the window after creation, and the non-client margin is
hand-rolled (`SM_CXFRAME`x2 horizontally, `SM_CXFRAME`x2 + `SM_CYCAPTION` vertically, missing
`SM_CXPADDEDBORDER`, which is where the undersized client area comes from).

**The shared vertex-buffer pool, the software-transform scratch and mouse picking:**
(see `vulkan_renderer_notes.md` §4.84 — this is where 84% of the Vulkan renderer's per-frame vertex
conversion was going, and none of it is geometry the game draws)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x005a2e40 | ThisCall<int, VertexBufferSet*, int count, int fvf, int flags, bool force_sysmem> (RET 0x10) | VertexBufferSet_Create — the binary's **only** `CreateVertexBuffer` caller. Stride is computed inline from the FVF (`address_map` note: XYZ +12, XYZRHW +16, normal +12, diffuse/specular +4 each, ntex\*8), pool is `MANAGED` unless `force_sysmem` or `ForceSystemMemVertexBuffers` |
| 0x005a2fc0 | ThisCall<int, VertexBufferSet*, void** out> (RET 0x4) | VertexBufferSet_Lock — locks the **whole** buffer with the flags fixed at creation. **Returns the cached pointer with no D3D call when `state == 1`**, so a re-entrant lock is invisible to any D3D-level hook |
| 0x005a3050 | ThisCall<int, VertexBufferSet*> | VertexBufferSet_Unlock |
| 0x005a30c0 | ThisCall<void, VertexBufferSet*, int mode> | VertexBufferSet_SetLockMode — jump table @ 0x005a3110; **mode 1 = `D3DLOCK_NOSYSLOCK\|D3DLOCK_READONLY` (0x810)**. Called once per set, from `AwScratchVB_CreateAll`, so the flags are constant for the process |
| 0x005a3140 | ThisCall<void*, VertexBufferSet*> | VertexBufferSet_GetD3DBuffer — the only route from a set to its `IDirect3DVertexBuffer8`; **4 callers**, which is what bounds the draw analysis |
| 0x005a31c0 | ThisCall<void, VertexBufferSet*, char*> | VertexBufferSet_SetDebugName — was `SetString?` in the DB, which described neither the argument nor the field |
| 0x005a1f30 | StdCall<void> | AwScratchVB_CreateAll — called once, from `CreateDirect3D` @ 0x00574b60. Creates the three scratch sets below |
| 0x005a3fa0 | FastCall<void, VertexBufferSet* dest, VertexBufferSet* src, int, int, int, int> | Aw_ProcessVertices — forces `D3DRS_SOFTWAREVERTEXPROCESSING` on and `LIGHTING`/`CLIPPING` off, binds **src** with SetStreamSource, calls `ProcessVertices` (device slot 74) into **dest**, restores. The dest is never a stream source, which is why FVF 0x004 appears in no draw |
| 0x005a7930 | — | Picking_TestNodeBoundingBox — the coarse pass: projects 8 bbox corners, once per drawn item from `DrawItem_RenderGeometry` |
| 0x005a70a0 | — | Picking_SelectPickersForNode — the same, once per `SceneMesh_Render` |
| 0x005a73c0 | — | Picking_PointInProjectedBox — cursor pixel against the six quads of a projected box |
| 0x005998f0 | — | SceneMesh_BuildHitTestVB — the mesh's position-only picking copy |
| 0x00582d10 | — | ParticleSystem_Render — writes `AwScratchVB_Particle`, the one scratch set the CPU fills |
| 0x00558cf0 | — | Spark_CreateVertexBuffer — the 512-vertex FVF 0x142 `"spark"` buffer, `MANAGED`+`WRITEONLY`, **drawn** from two sites (one gated on shadow quality) |

| Offset | Type | Name |
|--------|------|------|
| 0x00803d98 | VertexBufferSet* | AwScratchVB_Transform — `"transform"`, 4,096 x FVF 0x004 = 65,536 B, SYSTEMMEM, `DONOTCLIP`, lock mode 1 (READONLY) |
| 0x00803dfc | VertexBufferSet* | AwScratchVB_HitTest — `"hit test"`, 10,000 x FVF 0x004 = 160,000 B, same |
| 0x00803dd0 | VertexBufferSet* | AwScratchVB_Particle — `"particle"`, 4,096 x FVF 0x002 = 49,152 B, SYSTEMMEM, `WRITEONLY\|SOFTWAREPROCESSING\|DONOTCLIP`, lock mode 2 (**not** read-only) |
| 0x006ac62c | Picker* | MousePicker — +0x14/+0x18 the cursor pixel (`RunInGameFrame` @ 0x0046e839 writes it each frame), +0x1c the nearest hit, reset to `FLT_MAX` by `RunGameFrame` @ 0x0046e480 |
| 0x006ac628 | int* | MousePickingEnabled — ships as **1**; `ToggleReconMode` @ 0x004976d0 and `MenuLoadGame` write it |
| 0x00803e84 / 0x00803e88 | List* | PickerList / PickerCandidates |
| 0x006ab980 | int* | ForceSystemMemVertexBuffers |

**The mesh teardown cascade:** (see `vulkan_renderer_notes.md` §4.85 — ~3% of a settled level02
frame, and **none of it reachable from a renderer layer**: the hot instructions are cache-missing
linked-list chases over the game's own pool heap, touching no D3D object)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00599110 | ThisCall<void, void*> | SceneGraphNode_Release — refcount at +0x90, releases ≤10 children from the array at +0x44, then vtable slot 0. **70+ call sites**, so it does not identify what is being destroyed; it is the seam to hook if that question ever matters |
| 0x0059fee0 | ThisCall<void, SceneMesh*, int> | SceneMesh_DeletingDtor — vtable slot 0; `operator delete(p, 0x98)`, so **SceneMesh is 0x98** |
| 0x00598d10 | ThisCall<void, SceneMesh*> | SceneMesh_Dtor — destroys every `SubMesh` in the `List<SubMesh*>` at +0x34, **one `SubMesh_Dtor` per submesh**, which is what makes the cascade quadratic |
| 0x00595990 | ThisCall<void, SubMesh*> | SubMesh_Dtor — two O(n) list scans each: `List_RemoveEntry` on `SubMeshList`, and the `IndexBufferSet` unregister. `operator delete(p, 0x24)`, so **SubMesh is 0x24**. `SubMesh+0x08` is the shared per-FVF vertex-buffer pool (debug name `"AwSharedVB"`), which `rendering_notes.md` had left unidentified |
| 0x005a1440 | ThisCall<void, List<T>*, void*> (RET 0x4) | List_RemoveEntry — AvP's `list_tem.hpp` `List<T>::delete_entry`. `+0x17` is the branch after `CMP [ECX+0xc],EAX`, i.e. the load of `List_Member<T>::data` |
| 0x005a3330 | ThisCall<void, IndexBufferSet*> (bare RET) | IndexBufferSet_Dtor — `Release`s the D3D buffer, then linearly scans `IndexBufferSetList` for its own node. **`+0x43` is the branch after the `data` load**, and was 1.89% of all samples |
| 0x005a4300 | ThisCall<void, IndexBufferSet*, int> (RET 0x4) | IndexBufferSet_DeletingDtor — vtable slot 0 of the 2-slot vtable at 0x0066db34 |
| 0x005a3270 | ThisCall<void, IndexBufferSet*> (bare RET) | IndexBufferSet_Ctor — appends to `IndexBufferSetList` unconditionally, which is what makes the unregister O(n) |
| 0x005a33e0 | ThisCall<int, IndexBufferSet*, int, int, bool> (RET 0xc) | IndexBufferSet_Create — `CreateIndexBuffer`, always `D3DFMT_INDEX16`, `MANAGED` unless `force_sysmem` |
| 0x005a34d0 | ThisCall<void, IndexBufferSet*> (bare RET) | IndexBufferSet_Destroy |
| 0x00595510 | ThisCall<void, SharedVBPool*, void*> | SharedVBPool_ReleaseEntry — **unconditionally** calls the below, so tearing down N submeshes that share one pool is N Release+Create pairs, not one. Measured at **0 per frame** on a settled level02 camera, so it is a hazard to know about rather than a live cost |
| 0x00595550 | ThisCall<void, SharedVBPool*> | SharedVBPool_RecreateBuffer — destroys the pool's `IDirect3DVertexBuffer8` and creates a new one sized for the remaining vertices |

| Offset | Type | Name |
|--------|------|------|
| 0x00803e34 / 0x00803e38 | List* / int* | IndexBufferSetList and its count — **2,701 live** on a settled level02 camera, which is the length of the scan above |
| 0x00803c80 / 0x00803c84 | List* / int* | SubMeshList and its count |
| 0x00803d44 | int* | VertexBufferCreateCount |
| 0x00803d4c / 0x00803d50 | int* | IndexBufferCreateCount / IndexBufferReleaseCount — **+4 per frame** on a settled camera, which with the 2,701 above is the whole of that 1.89% |

**HUD and the 2D depth slices:** (see `rendering_notes.md` §4.4 and `game_defects_notes.md` §12;
`src/HudFix.cpp` hooks the first of these)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x0055fb20 | CDecl<void> | RenderHudItems — walks `HudItemList` @ 0x007ba250, makes `Camera_Hud` current, calls slot 2 on each item. **Exactly one call site** (0x0046e8c1) and zero literal references anywhere in the image |
| 0x0055fbd0 | ThisCall<void, HudItem*, int, int> | HudItem_DrawByKind — draws **one** HUD element, dispatched on `this->kind` (`+0x60`, 0..0x43; index table 0x00563928, jump table 0x005638f8). 11 `RenderQueue_Submit` sites, **all passing `Camera_Hud`**, then a run of immediate 2D quads. Was `DrawHud`, which described the caller rather than this |
| 0x0056a7b0 | ThisCall<void, HudItem*, int, int> | HudItem_Draw — slot 2 of the vtable at 0x006697a4, which is **`ParticleTester_vtbl`**, not a HUD-item table: its two writers agree on one class (`ParticleTester::Ctor` @ 0x0056a310 constructs it, `FUN_0056a6e0` is its destructor and chains to `HudWidget_Dtor`, so `HudWidget` is the **base** — calling this table `HudItem_vtbl` would file a derived table under its base). Object size 0x190, confirmed twice: `PUSH 0x190` in `SpawnParticleTester`, `free_sized(this, 400)` in slot 0. Forwards to the above |
| 0x005695a0 | CDecl<void> | Hud2D_BeginBatch — `RenderBatch_Begin` + bind `HudPlatesTexture`. 2 call sites, both in `RunInGameFrame` (0x0046e87a inventory / 0x0046e8b8 in-level) |
| 0x005695c0 | FastCall<void, float*, float*, uint, float> | Hud2D_DrawQuad — `(rect_px, uv, diffuse, z)`; 4 verts (stride 0x20) + 6 indices into `ImmediateBatch`. Writes the caller's `z` **verbatim** and `rhw = 1/z`. Wrappers: `Hud2D_DrawQuadNormalized` 0x00569e00, `Hud2D_DrawMeterBar` 0x00569ef0, `Hud2D_DrawNumber` 0x0056d390 |
| 0x00569ed0 | CDecl<void> | Hud2D_FlushBatch — `RenderBatch_End` + `RenderBatch_Draw(D3DPT_TRIANGLELIST, indexed)`. One call site (0x0046e8cf), no literal references |
| 0x00803d94 | RenderBatch* | ImmediateBatch — shared by the HUD 2D quads and `ParticleSystem_Render`. Was `ParticleRenderBatch`; the name understated who uses it |
| 0x007ba2b0 | void* | HudPlatesTexture — `units\plates 2 1024.rim`, the atlas holding both the panel plates and the meter/icon art |
| 0x007ba250 / 0x007ba254 | List / int* | HudItemList and its count |
| 0x004af4d0 | CDecl<void> | InitRenderCameras — carves the depth range into per-camera slices. Called from `WinMain` and `LoadLevel` |
| 0x00577490 | ThisCall<void, Camera*> | Camera_SetDeviceViewport — `SetViewport(this + 0x254)`. **The only `SetViewport` in the binary** |
| 0x00577550 | ThisCall<void, Camera*> | Camera_ApplyViewportAndZFunc — the above, plus `D3DRS_ZFUNC` from `this->+0x1d0`. Returns the previous ZFUNC in EAX; no caller reads it |
| 0x005774c0 | ThisCall<void, Camera*> | Camera_Apply — the three `SetTransform`s (world `+0xc4`, view `+0x84`, projection `+0x44`) and nothing else. **One** parameter: the DB had a second, which was an uninitialised-register artefact |
| 0x00576470 | ThisCall<Camera*, Camera*> | Camera_Ctor — vptr 0x0066cc9c, `sizeof(base Camera) == 0x26c`. Writes ZFUNC `D3DCMP_LESSEQUAL` to `+0x1cc`/`+0x1d0`, which nothing ever changes |
| 0x004b04e0 | ThisCall<void, Camera*, float*, float*> | Camera_SetOrthographic — clears `+0x250` (`is_perspective`) and fills `+0x240`..`+0x24c`. `InitRenderCameras` runs it over every camera except `Camera_World` and the sky camera |
| 0x004b0190 / 0x004b0450 | ThisCall | CameraData_Ctor / _Dtor — the derived class, vptr 0x006644a0, `sizeof == 0x2a0`. **Reached only from the C++ static-initializer array** at 0x0064d38c, which sat as undefined bytes — so neither had an xref at all until that array was defined |
| 0x007c146c / 0x007c1470 | Camera** / bool* | CurrentCamera / CurrentCameraIsPerspective |

Camera globals and their depth slices — the object lives *at* the address (the HUD submits push it
as the camera pointer), and a `D3DVIEWPORT8` sits at `+0x254` with `MinZ` at `+0x264`:

| Global | Name | MinZ..MaxZ | near/far |
|--------|------|------------|----------|
| 0x007f5c10 | Camera_Menu2D | 0.00 .. 0.02 | — |
| 0x007b5800 | Camera_Text | 0.02 .. 0.04 | 0 / 10 |
| 0x007b4e40 | Camera_Hud | 0.03 .. 0.04 | 0 / 10 |
| 0x007b4930 | (unidentified) | 0.06 .. 0.30 | 0 / 10 |
| 0x007b5320 | (unidentified) | 0.02 .. 0.03 | 0 / 10 |
| 0x007b4ba0 | Camera_World | 0.10 .. 1.00 | 1 / 200 |
| 0x007b5a70 | the sky/backdrop camera | 1.00 .. 1.00 | 1 / 1000 |
| 0x007b50b0 | (unidentified) | 0.02 .. 0.04 | 0 / 10 |
| 0x007b5590 | (unidentified) | 0.04 .. 0.06 | 0 / 10 |

**Text rendering:** (see `rendering_notes.md` §4.2 — text is its own queue, not the render queue)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x005782e0 | ThisCall<int, Font*, RECTF*, char*, uint*, void*, int, TextFlags, float, uint*, int> | Font_QueueText — lays out and **enqueues**, draws nothing. 39 call sites. Was `DrawText?`; carries the stack-smash defect, `game_defects_notes.md` §1 |
| 0x00578ee0 | StdCall<void> | ScenePass_Overlay2D — walks the font registry at 0x007c14a0 and flushes each. Sole caller `RenderSceneAndPresent` @ 0x00574ccd; sole *callee* of interest below. **The seam for suppressing all text in a frame** |
| 0x00578180 | ThisCall<bool, Font*> | Font_FlushQueuedText — drains `font+0xb08`, frees each `item.text`, then `RenderBatch_Draw(4,1)` |
| 0x00578a00 | ThisCall<bool, Font*, int, int, int, char*, int, uint, void*, TextFlags, float, uint> | Font_RenderTextItem — ten args = the ten `TextDrawItem` fields in order (`RET 0x28`) |
| 0x005792d0 | ThisCall<void, Font*, int, void*, float, uint*, int, int> | Font_EmitGlyphQuad — 4 verts / 6 indices per glyph; `depth` lerps the target camera's two z planes |
| 0x005782b0 | ThisCall<float*, Font*, float*> | Font_GetNormalizedLineHeight — `(font[+0xaf0] * font[+0xaf4]) / ResolutionHeightF`; returns the out pointer in EAX, and both callers rely on it |
| 0x00577c70 | ThisCall<Font*, Font*, void*, int, int*, int*> | Font_Ctor — 140 glyphs; registers the font into the 0x007c14a0 list. 4 call sites, all `InitConsole`, all passing `line_height = 25`. Normalizes glyph UVs with the **fixed literal 1/256**, not the texture size, so `large font.RIM`'s 512² sheet buys texel density rather than a bigger glyph box |
| 0x004d79f0 | CDecl<void> | ScaleFontsForClientWidth — called once from `InitConsole` right after the four constructions. Re-fills each font's advance table by `ClientWidth` band, and for `HeadingFont` **only** also writes `Font.scale` (2.0/2.5/3.0). That store @ 0x004d7b06 is the **only** write to `+0xaf4` in the binary outside `Font_Ctor` |
| 0x005789a0 | ThisCall<void, Font*, const int*, float> | Font_SetGlyphWidths — 140 entries at `font+0x8c0`, each `(int)(base[i]*scale + 0.5)`. Only caller is `ScaleFontsForClientWidth` |
| 0x00666640 / 0x00666870 | int[140] | SmallFontGlyphWidths / LargeFontGlyphWidths — differ in 14 entries, so `large font.RIM` is separately authored art rather than a resample |
| 0x00666aa0 | int[140] | FontGlyphSheetRows — shared by all four fonts |
| 0x005780d0 | ThisCall<void, Font*> | Font_Dtor — **tail `JMP` to 0x00579170, no `RET` of its own**. 4 call sites, all `ShutdownConsole` @ 0x004d5620 |
| 0x004f72e0 | FastCall<void, uint*> | DrawVersionText — draws the literal `"v1.3 DX8"` bottom-left. Two callers: the Main menu (gated `ChosenMenu == 0`) and the splash frame. Was `DrawVersionNumber?` |
| 0x007c14a0 | List | the font registry `Font_Ctor` appends to and `ScenePass_Overlay2D` walks |
| 0x00667434 | char[9] | `"v1.3 DX8"` — the on-screen stamp. **Unrelated** to `CommandVersion` @ 0x0043f1a0, which reports `"00.08 Built on Jun 24 2019"` @ 0x00651b2c |
| 0x00667440 | unsigned | GREEN_TEXT_COLOR 0xff00e500 (the Main-menu version stamp; channel order unverified) |
| 0x006ab100 | unsigned | TEXT_OUTLINE_COLOR 0xff000000 — read only under `TF_Outline` |
| 0x007c1478 | int | TextAnchorAdjustDisabled — suppresses `TF_AnchorBottom`'s per-line y decrement. Cleared/set by `UpdateReconCamera`; **why is not established** |
| 0x0066ccc0 | — | TextDrawNode_vtbl (one slot, dtor 0x00579240) |

`TextFlags` and the 0x28-byte `TextDrawItem` are modelled in the Ghidra DB. The `Font` layout
(0xb18+) is measured in `rendering_notes.md` §4.2 but deliberately **not** typed.

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
| 0x005e3f72 | — | `malloc` — `PUSH EBP` / `MOV EBP,ESP` / `POP EBP` / `JMP pool_alloc` (`55 8b ec 5d e9 f5 d4 f8 ff`). The prologue is exactly **stack-neutral** — ESP and EBP are both back at their incoming values by the `JMP` — so it is still a tail jump and still `__cdecl`, but it is *not* a bare `JMP` and Ghidra therefore does **not** mark it `isThunk()`, unlike `free` below. Thunk-following xref and call-graph queries do not traverse it |
| 0x005e3f7b | — | `free` — bare `JMP pool_free`; **every** `free` in game code goes here |
| 0x0044e1a0 | FastCall<char*, char*> | `strdup` — game-written, allocates via the malloc thunk |
| 0x00601f4a / 0x00601f2d | — | the *real* CRT malloc/free. Only pool_alloc/pool_free and a few file/rif paths (`ToMap`, `LoadOrGetRifFile`) call them — no field in any mirrored struct holds this memory |

**The IFF chunk registry:** (the IFF-side registry, distinct from `Chunk::Register` — see
`rif_chunk_format.md`)

| Offset | Signature | Name |
|--------|-----------|------|
| 0x005e1d00 | FastCall<void, RifRegEntryCreateFn*, uint32, uint32> (RET 0x8) | IffChunk_Register(create_fn /*ECX*/, container_id, chunk_id) — corrected from `unknown` with 2 stack params, per its `RET 0x8`. **PROPOSED** that the two stack dwords are separate arguments rather than one 8-byte by-value id struct: MSVC `__fastcall` puts no aggregate in a register and EDX carries no argument at any of the 13 call sites, which the struct reading would equally explain |
| 0x0043c530 / 0x0043c5c0 / 0x0043c670 / 0x0043c740 | — | IffChunkRegistry_AssertNotReadOnly_1..4 — four structurally identical guard stubs, `if (DAT_00838b08) { Error("IFF_READ_ONLY definition not consistent"); exit(-0x2d); }` |

**Misc:**

| Offset | Signature | Name |
|--------|-----------|------|
| 0x00474540 | FastCall<Parsed*, const char*, int> | LoadGLS — it `_fopen`s the script into a 0x14-byte `File`, pushes that on the parser stack and runs `ParseGSH` twice (both passes), so it opens as well as parses |

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
| 0x004ae960 | FastCall<void*, const char*, int> | LoadOrGetRifFile(name /*ECX*/, reuse_cached_and_prefer_opt /*DL*/) — bare `RET`. **The one seam every rif load passes through**, which is why `src/MapLights` hooks it rather than `AcquireLevelRifForLocators`: `ToMap` reaches it directly on the cold path (flag **0**) and through Acquire on both warm ones. The flag does two things — reuse a cache hit, *and* prefer `<stem>.opt` when newer |
| 0x004aead0 | CDecl<void> | RifCache_Clear — no arguments at all. `LoadOrGetRifFile` calls it **on every cache miss** before inserting, so the cache never holds more than one entry, and `LoadLevel` calls it again at 0x004e0e70 right after `ConvertParsedObjects`. **So the level's rif object is freed before the level is playable**, and nothing may retain the pointer `LoadOrGetRifFile` returns |
| 0x005a9b50 | FastCall<void*, const char*> | BuildRifFileObject — builds the 0x210-byte `RifFile`. `+0x00` unit scale (`ENVSDSCL`/1000, default **0.001f**; only 2 of 563 shipped files carry one, both 1.0), `+0x04`/`+0x08` the `BMPNAMES` table and its count, `+0x0c` the `REBINFF2` root, `+0x10` a 128-slot `INDSOUND` table. **It carries no filename** — the root `File_Chunk` does, at `+0x2c`, but `File_Chunk_WriteFile` @ 0x005b03b0 overwrites that with whatever it last wrote, so after a cold load it reads `<stem>.loc` rather than the authored `.rif` |
| 0x005aaa70 | FastCall<void, void*> | RifFile_Destroy — `free_sized(this, 0x210)` |
| 0x007b4918 | HashTable\<RifCacheEntry*\> | RifFileCache — 64 buckets; `{RifFile*, char* name}` entries, `{d, next}` nodes. Effectively single-entry and empty at play time, per RifCache_Clear above, so it is **not** enumerable for "which rifs are loaded" |
| 0x005aa5c0 | FastCall<void*, void*, char*, void*, void*, int> | RifFindObjectByName — **five** parameters, `RET 0xc`; the DB had three. The last, `merge_and_build`, gates the quad-merge pass and is 1 at only two of the eight call sites (`ToMap` @ 0x0047f926, `GetShape` @ 0x004ae6c4) |
| 0x005ab300 | FastCall<void, void*, void*, void*, unsigned char> | BuildShapeVertexBuffers — arg2 is a nullable `SHPVTINT`; the `SHPMRGDT` block runs only when `flags & 1` and null-checks the lookup |
| 0x005d7900 | FastCall<void, ChunkShape*, void*> | MergePolygonsInChunkShape — fuses triangle pairs into quads, replacing the shape's own poly/normal lists. Feeds the **navmesh**, never the renderer: the D3D buffers are built before the call and not rebuilt. **No bounds check anywhere, and AvP's planarity guard is absent**; see `rif_chunk_format.md`, "Merging polygons into quads" |
| 0x0048dc50 | ThisCall (member) | NavQuad_Ctor — 0x44 bytes, vtable 0x00663ecc, the 4-vertex sibling of `NavPolygon` |
| 0x0048f580 | ThisCall<int, NavQuad*, void*> | NavQuad_AdjacencyTest — slot 0x50 for a quad. Same unbounded append as `PolygonAdjacencyTest`, over a `Vec3[4]`, so the **fifth** match overruns |
| 0x00494d40 | ThisCall (member) | NavQuad_IsNeighbour — slot 0x58 |
| 0x0048e330 | ThisCall (member) | NavQuad_AddNeighbour — slot 0x5c |
| 0x00489d70 | ThisCall (member) | Map_AddNavGeometry — interleaves the tri and quad arrays into `MapBase::sections` (+0x88) and writes each one's `section_id` (+0x20), triangles first |
| 0x005d5e80 | FastCall<int, ChunkPoly*> | ChunkPoly_TextureIndex — `colour & 0xfff` |
| 0x005d5e60 | FastCall<int, ChunkPoly*> | ChunkPoly_UVListIndex — the 20-bit form, bits 12-15 folded in when set |
| 0x005d7590 | FastCall<int, ChunkPoly*, ChunkPoly*, ChunkPoly*, ChunkShape*> | TexMergePolys — textured pair (`5..7`, `0x14..0x18`) |
| 0x005d77e0 | FastCall<int, ChunkPoly*, ChunkPoly*, ChunkPoly*> | MergePolys — untextured pair |
| 0x005b97d0 | FastCall (this in ECX) | ShapeMergeDataChunk_FromData — `num_polys = payload >> 2`, and that count is never compared with `SHPPOLYS` |
| 0x005ba7b0 | FastCall (this in ECX) | ShapePolyChangeInfoChunk_FromData — loads `SHPPCINF`, which nothing reads |
| 0x005b5df0 | — | StripUnusedShapeChunks — deletes `SHPPCINF` once the shape is built |

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

### Recovered switch-table function bodies

Four functions whose real extent was hidden behind unresolved MSVC switch tables. Recorded with
their tables so a later byte census or reader can tell each one is now **complete** rather than
truncated. All four needed a **jump-table override** (`JumpTable(...).writeOverride`) before the
decompiler would render them as switches — creating `COMPUTED_JUMP` references is not sufficient;
see the trap in `CLAUDE.md`.

| Function | Body | Tables |
|----------|------|--------|
| `AiThink_Bot` @ 0x00451220 | `[0x00451220, 0x00455228]` = **0x4009 (16,393)** bytes, was 0x1f39 | 0x0045522c (9 entries, site 0x0045244b, `ai_state` remap); 0x00455250 (9, site 0x004524b1, behaviour dispatch); 0x00455274 (4, site 0x00452fdb); 0x00455284 (4, site 0x0045359a). Data map 0x00455229-0x0045529f: a 3-byte alignment NOP, the four tables, then 12 bytes of `cccccccc` up to `AiThink_Mine` @ 0x004552a0. See `ai_behaviour_notes.md` §2.1.2 |
| `InGameMenu__OnItemActivated` @ 0x00563c30 | `[0x00563c30, 0x0056495b]` = 3,372 bytes / 773 instructions, `void __thiscall(HudWidget *this)`, bare `RET` | five tables: outer pointer 0x0056495c (**23** slots, slot 22 a genuine NULL), byte index 0x005649b8 (66 bytes), inner 0x005649fc (7), 0x00564a18 (24), 0x00564a78 (6). They tile exactly up to `FUN_00564a90`. See `menu_system_notes.md` and `game_defects_notes.md` §17 |
| `UpdateCursorForMode` @ 0x00498140 | `[0x00498140, 0x004985df]` = 1,184 bytes | 0x004985e0, 7 entries, **no bound check** on the index, then a 4-byte `cccccccc` pad |
| `D3DFormatToString` @ 0x005a44b0 | `[0x005a44b0, 0x005a4600]` = 337 bytes, `char * __fastcall(int fmt)` | byte table 0x005a468c (103 bytes, indices 0..102) selecting pointer table 0x005a4604 (34 entries); 0x005a4601 is a 3-byte alignment NOP and 0x005a46f3-0x005a46ff is `0xcc`. **Zero callers in the image** — dead debug code; see `rif_chunk_format.md` |

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
@ 0xcc, `neg_origin` @ 0x11c, `bounds_max`/`bounds_min` @ 0x128/0x134 (**the larger corner is first** - measured, see src/Map.h),
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

### Small structures worth having by name

Three records that are passed around by pointer and are easy to mis-size:

- **`MotionSnapshot`, 0x30 bytes** — what `MobileActor::WriteMotionSnapshot` @ 0x0053bb00
  serialises and what four wire updates carry inline: `{float state_timestamp @0x00 (Actor+0xd8);
  Vec3 position @0x04 (Actor+0xa0); Vec4 orientation @0x10 (Actor+0xac); Vec3 velocity @0x20
  (nav_agent+0x1c); u32 nav_poly_id @0x2c}`. The last field is written **24 bits wide** — the
  top byte of the destination dword is preserved, and 0xffffff goes in when the agent has no
  nav polygon. Updates `0x39` (0x50 bytes), `0x3b`/`0x3c`/`0x3d` (0x38) all embed it after
  `{id, actor_id}`; see `directplay_protocol_notes.md`.
- **`Waypoint`, 0x18 bytes** — `pool_alloc(0x18)` = `{Vec3 pos; u32; u32}`, held in
  `MobileActor+0x204` as a `List<Waypoint *>` (count +0x208, cache +0x20c/+0x210, cursor +0x224).
  `MobileActor::PushRouteWaypoint` @ 0x0053a640 inserts at the **front**, which is what makes the
  A* backtrack come out in travel order; `ClearRouteWaypoints` frees each record with
  `free_sized(node->data, 0x18)`.
- **`Clock`, 0x24 bytes** — `{u64 ticks_at_calibration @0x00; DWORD timeGetTime_at_calibration
  @0x08; int ticks_per_sec @0x0c; float ticks_per_sec @0x10; float seconds_per_tick @0x14;
  u64 accumulated_ticks @0x18; int last_raw @0x20}`, built by `Clock::Calibrate` @ 0x005718b0.
  `MainClock` is 0x007c07d0 and `ExecutorClock` 0x007c07a0, so the **instance stride is 0x30**
  and the struct is 0x24 — a 0x30 figure quoted elsewhere is the stride, not the size. This is
  what pins the loose globals around them: `GetGameTimeSeconds` reads MainClock+0x14
  (0x007c07e4), and the two per-thread accumulators `src/GUI.h` names (0x007c07e8 main,
  0x007c07b8 executor) are each clock's +0x18.

### The client `Unit` hierarchy

Sixteen adjacent tables, bounded by the reference test (the last one by the string `"blobarrel"`
after it, **not** by adjacency). The base is 92 slots, so any offset >= 0x170 read off 0x006647ac
is inside the next class's table. Slot meanings and the ownership rules are in
`rendering_notes.md` §5.1; sizes come from **vtable slot 35, `GetSize()`**.

```
class                              vtable      slots  size   ctor        dtor
Unit                               0x006647ac    92   0x130  0x004b4620  0x004b5640
  MobileUnit                       0x0066491c   107   0x238  0x004ba050  0x004ba630
    CharacterUnit                  0x00664ac8   112   0x2e0  0x004c1100  0x004c1300
      CentibodyUnit                0x006656f0   112   0x2e8  0x004cbb90  0x004cbbd0
        CentipedeUnit              0x006658b0   112   0x2e8  0x004cbc70  0x004cbca0
      PopupUnit                    0x00665a70   112   0x2e8  0x004cc4f0  0x004cc530
        TurretUnit                 0x00665c30   112   0x2f0  0x004cc9a0  0x004cca10
    NodeUnit                       0x00665544   107   0x240  0x004cbb30  0x004cbb60
    PresidentUnit                  0x00665f60   108   0x248  0x004cdfe0  0x004ce060
  ProjectileUnit                   0x00664c88    94   0x180  0x004c47e0  0x004c4a50
  PickupUnit                       0x00664e00    93   0x150  0x004c6d70  0x004c6e60
  TrackObjectUnit                  0x00664f74    92   0x1d0  0x004c6f50  0x004c70b0
  TumbleweedUnit                   0x006650e4    92   0x148  0x004c8390  0x004c84b0
  BackgroundCreatureUnit           0x00665254    94   0x178  0x004c9730  0x004c9af0
    FlyingBackgroundCreatureUnit   0x006653cc    94   0x190  0x004cb530  0x004cb590
  BlockerUnit                      0x00665df0    92   0x140  0x004cd570  0x004cd620
```

`BlockerUnit`'s destructor @ 0x004cd620 carried the stray demangled name
`Concurrency::call<...>::~call<...>` until it was identified — the same failure mode
`actor_vtable_notes.md` records for `BlockerActor::Destructor`.

**The two client factories, and the class that is reachable from neither:**

| addr | name | role |
|---|---|---|
| 0x004fd450 | `CreateUnit` | the **network** path. `int __fastcall(int team /*ECX, consumed as a byte*/, Role * /*EDX*/, Vec3 *pos, Vec4 *quat, uint unit_id, uint owner_ref)`, `RET 0x10`. 17 allocation sites, **13** distinct sizes (14 sites inside the switch) — 13 and not 14 because it never allocates 0x180. Returns `Unit+0xc`, the unit id. `owner_ref` is an **owner unit id + 1**, 0 = none |
| 0x004fce90 | `ClientSpawnActorForTeam` | the **level-load / savegame** path. `__fastcall void *(int team /*ECX*/, Role * /*EDX*/, Vec3 *pos, Vec4 *quat)`, `RET 0x8`, jump table 0x004fd3fc. Takes its id from `NextClientActorId` @ 0x007b68e4; callers `ToMap` @ 0x00481a91 and `LoadGame` @ 0x00506831. A projectile role returns 0 |
| 0x004ae0d0 | `GetRoleById` | how update `0x64`'s handler @ 0x004ff934 turns a wire roleId into the `Role *` `CreateUnit` needs — no class tag is transmitted |
| 0x0044e070 | `GetUnitById` | the client units-hash lookup (mask 0x007b68fc, buckets 0x007b6900, key = `Unit+0xc`) |

`ProjectileUnit` is reachable from **neither** factory: it is built only by `ApplyUpdateMessage`
update `0x46` @ 0x004fec38 (gated on `role->projectile != 0`) and by `LoadGame` @ 0x00506373.

**Other client `Unit` addresses:**

| addr | name |
|---|---|
| 0x004b8c20 | `Unit_LeaveWorld` — slot 91, the base table's last slot; the inverse of slot 51 `Unit_EnterWorld` |
| 0x004b6530 | `Unit_Destroy` — slot 63 base body, in 11 tables, reached from update `0x49` |
| 0x004b5640 | `Unit::~Unit` |
| 0x004d0310 / 0x004d0380 | `UnitList_Add` / `UnitList_Remove` (both `ECX = 0x007b6928`, both gated on `team == LocalPlayerTeam`) |
| 0x006a58e0 | `LocalPlayerTeam` |
| 0x007b3f52 | `DecoyTargetPending` — one byte, immediately after `FlareModeActive` @ 0x007b3f51 |
| 0x006a6308 | `Gravity` (9.81f) |

### Imports

Key external libraries: BINKW32.DLL (video), STEAM_API.DLL, D3D8.DLL,
KERNEL32/USER32/GDI32/ADVAPI32/OLE32/WINMM (Windows API).

The import thunks live in **one contiguous run of 82 six-byte `JMP dword ptr [mem]` thunks at
0x0057115b-0x00571346** (492 bytes), all now defined as thunk functions carrying their bare
imported symbol names — KERNEL32 35, USER32 33, STEAM_API 5, ADVAPI32 5, OLE32 3, GDI32 1. That
matters beyond the DB: `utils/symdump/gl_symbols.py` exports database names as the profiler's symbol
map, so an import now resolves by name in a sampled stack instead of as hex. `SetCursor`
@ 0x005712e1 is one of them, which is why the game function that used to hold that name is now
`ApplyWin32CursorShape`.

Two lone 1-byte `RET` functions sit just past the run at 0x00571350 and 0x00571360, in `0xcc` fill,
with **0 references** each.
