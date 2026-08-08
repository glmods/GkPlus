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
| 0x007b6a70..0x007b6a7c | — | command **hash table**: NumRegisteredCommands, CommandTableNumBuckets, CommandTableMask, CommandTableBuckets (`CommandListElem**`) |
| 0x007b6aa8..0x007b6ab4 | List | command exec queue: CommandsToExecute (anchor), NumCommandsToExecute, cache, cacheValid — one popped per frame by `PumpQueuedConsoleCommand` |
| 0x007b6a80 / 0x007b6b38 | float | ConsoleTextScrollTarget / ConsoleSlidePos (open/close anim; -1=closed) |
| 0x007b6b3c | Sprite* | ConsoleBackdropSprite (FUN_004d7b20) |
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
| 0x007c1288 / 0x007c128c | int* | ClientWidth / ClientHeight (18 readers, all pixel comparisons — `DrawHud`, `DrawInventoryItemPanel`, `ApplyShadowQuality`) |

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
| 0x005780d0 | ThisCall<void, Font*> | Font_Dtor — **tail `JMP` to 0x00579170, no `RET` of its own**. 4 call sites, all `FUN_004d5620` |
| 0x004f72e0 | FastCall<void, uint*> | DrawVersionText — draws the literal `"v1.3 DX8"` bottom-left. Two callers: the Main menu (gated `ChosenMenu == 0`) and the splash frame. Was `DrawVersionNumber?` |
| 0x007c14a0 | List | the font registry `Font_Ctor` appends to and `ScenePass_Overlay2D` walks |
| 0x00667434 | char[9] | `"v1.3 DX8"` — the on-screen stamp. **Unrelated** to `CommandVersion` @ 0x0043f1a0, which reports `"00.08 Built on Jun 24 2019"` @ 0x00651b2c |
| 0x00667440 | unsigned | GREEN_TEXT_COLOR 0xff00e500 (the Main-menu version stamp; channel order unverified) |
| 0x006ab100 | unsigned | TEXT_OUTLINE_COLOR 0xff000000 — read only under `TF_Outline` |
| 0x007c1478 | int | TextAnchorAdjustDisabled — suppresses `TF_AnchorBottom`'s per-line y decrement. Cleared/set by `FUN_00484e40`; **why is not established** |
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
