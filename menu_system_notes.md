# Gunlok Menu System

Gunlok has **two independent menu systems** that share the same `Menu` / `MenuListItem`
container types but nothing else:

| | Front-end menus | In-game menus |
|---|---|---|
| Array | `Menus[36]` @ `0x007b76d0` | `InGameMenus[7]` @ `0x007b7578` |
| Current menu | `ChosenMenu` @ `0x007b732c` | `InGameMenuIndex` @ `0x007b7270` |
| Current item | `ChosenMenuItem` @ `0x006a7d6c` | `InGameMenuSelectedItem` @ `0x006a89b4` |
| Built by | `SetupMenus` @ `0x004e95e0` | one `OpenInGameXxxMenu` per menu, on demand |
| Activation | `OnMenuItemClicked` @ `0x004ecf10` | `InGameMenu__OnItemActivated` @ `0x00563c30` |
| Lifetime | built once at startup, persists | built on open, freed by `CloseInGameMenu` |
| Rendering | front-end renderer | HUD renderer inside `FUN_0055fbd0` |

`InGameMenus` ends at `0x007b76ac`, i.e. immediately before `Menus`; the two arrays are
adjacent but unrelated. GkPlus historically only knew about `Menus`.

## Core types

### `Menu` — 0x2c bytes

| Offset | Type | Name | Meaning |
|--------|------|------|---------|
| 0x00 | `MenuListItem*` | `sentinel` | sentinel node of the circular doubly-linked item list |
| 0x04 | `int` | `numNodes` | number of linked nodes actually allocated |
| 0x08 | `int*` | `cachedItems` | lazily-built index -> item lookup array |
| 0x0c | `bool` | `cacheValid` | whether `cachedItems` is up to date |
| 0x10 | `MenuListItem*` | `currentItem` | list cursor used while iterating/rendering |
| 0x14 | `Menu*` | `itemsOwner` | points back at the owning `Menu` (list-header idiom) |
| 0x18 | `int` | `scrollOffset` | index of the first visible item |
| 0x1c | `int` | `parentMenuId` | menu to return to on "back" |
| 0x20 | `GL_RESOURCE_ID` | `titleResourceId` | string id for the menu title |
| 0x24 | `int` | `nItems` | logical item count (incremented by every Add*Item) |
| 0x28 | `GL_RESOURCE_ID` | `firstItemResourceId` | first auto-generated item id, `GL_START` if none |

> The `Menu` struct GkPlus previously used in `src/Menu.cpp` (`int a,b,c,d,e,f; char *label; ...`)
> was a guess. Only `nItems` @ 0x24 was right; the field it called `label` @ 0x18 is
> actually `scrollOffset`.

### Constructor — `Menu::Menu` @ `0x004f94f0`

```c
void __thiscall Menu::Menu(Menu *this, GL_RESOURCE_ID firstItemId, int nLabels,
                           GL_RESOURCE_ID titleId);
```

Resets the list cursor and, when `firstItemId != GL_START`, auto-appends `nLabels` plain
items whose labels are `GetResourceString(firstItemId + 0)` .. `GetResourceString(firstItemId + nLabels - 1)`.
This is why so many menus are declared with just a first-id and a count: their labels are a
contiguous run in the string table.

The sentinel is **not** allocated here — it already exists when the constructor runs, so an
"empty" menu (`firstItemResourceId == GL_START`, `nItems == 0`) is one that `SetupMenus`
only reset, to be filled in later at runtime.

### `MenuListItem` — 0x78 bytes

Really `ListNode` (0x10: vtable/prev/next/pad) + **`MenuItemData` (0x68) at +0x10**.
`MenuListItemVtable` has **exactly one slot** (the scalar deleting destructor) — there is no
virtual draw or update; all polymorphism is the `itemType` int.

| Node off | Data off | Type | Name | Meaning |
|---|---|---|---|---|
| 0x00 | — | `void**` | `vtable` | `&MenuListItemVtable`, 1 slot |
| 0x04 | — | `MenuListItem*` | `prev` | |
| 0x08 | — | `MenuListItem*` | `next` | |
| 0x0c | — | | padding | data needs 8-byte alignment for the `int64`s |
| 0x10 | 0x00 | `int*` | `toggleValuePtr` | type 2 only. Dereferenced as **`int*`**, not `bool*` |
| 0x14 | 0x04 | `char*` | `valueText` | type 1 only: right-hand string |
| 0x18 | 0x08 | `bool` | `valueTextOwned` | **1 = heap, free it** |
| 0x1c | 0x0c | `int*` | `multiValueIndexPtr` | type 3 only |
| 0x20 | 0x10 | `GL_RESOURCE_ID**` | `multiValueLabels` | type 3 only, **double indirect** |
| 0x24 | 0x14 | `bool` | `isCurrentValue` | draws the row red instead of green |
| 0x28 | 0x18 | `char*` | `label` | primary left-aligned text |
| 0x2c-0x38 | 0x1c-0x28 | `float[4]` | `rectLeft/Top/Right/Bottom` | normalized screen coords |
| 0x3c | 0x2c | `int` | `itemType` | see below |
| 0x40 | 0x30 | `bool` | `labelIsStatic` | **0 = heap, free it** (opposite polarity to `valueTextOwned`) |
| 0x44 | 0x34 | `int` | `itemIndex` | = `Menu::nItems` at insert time |
| 0x48 | 0x38 | `float` | `glowLevel` | ramps 0..0.5 while selected; `/0.75` gives the bracket ambient colour |
| 0x4c | 0x3c | `float` | `revealRate` | random, chars per ms for the typewriter effect |
| 0x50 | 0x40 | `float` | — | random, **written by `InitAnimation` and never read** |
| 0x54 | 0x44 | `int` | `labelCharsRevealed` | clamped to `strlen(label)` |
| 0x58 | 0x48 | `int` | `valueCharsRevealed` | |
| 0x60 | 0x50 | `int64` | `labelRevealStartTime` | |
| 0x68 | 0x58 | `int64` | `valueRevealStartTime` | re-stamped to now each frame until the label finishes |
| 0x70 | 0x60 | `void*` | `extraOwnedBuffer` | `free()`d by `Menu::ClearItems` |

### Item types

| Value | Name | Right-hand text | Rect | 
|-------|------|-----------------|------|
| 0 | `PlainLabel` | none | full row, x ∈ [0.15, 0.90] |
| 1 | `LabelWithValue` | `valueText` | full row |
| 2 | `Toggle` | `GL_TEXT_ON` / `GL_TEXT_OFF` by `*toggleValuePtr` | tight text rect |
| 3 | `MultiValue` | `GetResourceString((*multiValueLabels)[*multiValueIndexPtr])` | tight text rect |

Only 0-3 exist — verified by scanning every immediate stored to the `itemType` slot. There is
no slider, no dedicated text-entry type and no greyed-out state. Text entry is a *mode* layered
on a type-0 item (`MenuBeginTextEdit` @ `0x004f7b60` starts it; `MenuHandleTextOrKeyBind`
rewrites the item's `label` in place). `ComputeLayout` branches on `itemType <= 1` (full-width
row) vs `>= 2` (measured tight rect), which is counter-intuitive but confirmed.

There is **no generic toggle/cycle handler** — `OnMenuItemClicked` mutates every backing
variable explicitly. The `int*` bindings are read by the renderer only.

### Item constructors

| Address | Signature | `itemType` |
|---------|-----------|------------|
| `0x004f7a60` | `Menu::AddItem(const char *label)` | 0 |
| `0x004f7ae0` | `Menu::AddValueItem(const char *label, const char *valueText, bool labelIsStatic, bool valueTextOwned)` | 1 |
| `0x004f7950` | `Menu::AddToggleItem(const char *label, int *value)` | 2 |
| `0x004f79d0` | `Menu::AddMultiValueItem(const char *label, int *index, GL_RESOURCE_ID **labels)` | 3 |

All four build a stack `MenuItemData` template, call `MenuListItem::ComputeLayout` @ `0x004f7ba0`
and `MenuListItem::InitAnimation` @ `0x004fa1f0` on it, then hand it to
`Menu::AppendItemNode` @ `0x004fbf10`, which `malloc(0x78)`s a node, installs the vtable, copies
0x68 bytes to `node + 0x10`, links it before the sentinel, bumps `numNodes` and invalidates
`cachedItems`.

**Offset trap:** the template base aliases `MenuListItem + 0x10`, so every field offset inside
`ComputeLayout`, `InitAnimation` and `Menu__GetItemData` is shifted down by 0x10. Ghidra's
field names in those functions are misleading for this reason.

`MenuListItem::InitAnimation` seeds the per-item reveal animation from the per-thread RNG rings
described in `threading_model_notes.md`.

Ghidra's decompiler **silently drops** several of the template stores; the disassembly is
authoritative. For example `004f7a93 MOV dword ptr [EBP + -0x3c],0x0` (`itemType = 0`) is
invisible in the C output for `AddItem`.

### Item lookup

`Menu`'s first four fields *are* an intrusive list header `{sentinel, count, cache, cacheValid}`,
a template instantiated three times (also `LevelList` @ `0x007b74dc` and
`MultiplayerLevelList` @ `0x007b76b0`).

`Menu__GetItemData` @ `0x004f7750` — `__thiscall MenuItemData *(Menu *, int index)` — is the
"get item N" accessor:

```c
if (!this->cacheValid) {
    free(this->cachedItems);
    this->cachedItems = malloc((this->numNodes + 1) * 4);
    for (n = sentinel->next, i = 0; n != sentinel; n = n->next, i++)
        this->cachedItems[i] = (int)n + 0x10;   // -> MenuItemData*
    this->cacheValid = true;
}
return (MenuItemData *)this->cachedItems[index];
```

**No bounds check** — `index` is trusted. Only `OnMenuItemClicked` uses it; the draw, hit-test
and navigation paths instead walk the list with `Menu::currentItem` as a mutable cursor.
`Menu::itemsOwner` exists so one menu can render another menu's item list.

`Menu__ClearItems` @ `0x004f7cd0` frees, per item, `extraOwnedBuffer`, `label` **if
`labelIsStatic == 0`**, and `valueText` **if `valueTextOwned != 0`**, then calls vtable slot 0
with `1`.

## Localized strings

Menu labels never hold literal text; they hold `GL_RESOURCE_ID` values resolved through
`GetResourceString` @ `0x00579000` (`__fastcall`, ECX = `&LocalizedStrings`, EDX = id).

- `LocalizedStrings` @ `0x00725664` is a single `ResourceEntry*`.
- `ResourceEntry` is 0x14 bytes: `{ GL_RESOURCE_ID id; char *text; int unused1; int unused2; int isLast; }`.
- `LoadResourceStringTable` @ `0x00578f30` is called once from `WinMain` @ `0x0046b355` with
  `firstId = GL_START (0)`, `lastId = GL_END (0x7532)` — 30003 records, 600060 bytes. Every
  string is `LoadStringA`'d out of a satellite DLL into a heap copy, then the DLL is
  `FreeLibrary`'d immediately.
- Lookup is a **linear scan** comparing `+0x00` to the id, with **no bounds check**. Because
  the table covers the whole contiguous id space, record index == id in practice.
- Missing strings have `text == NULL` and resolve to `EmptyResourceString` @ `0x007c14b4` (`""`).
- Freed by `FreeResourceStringTable` @ `0x00579020` from `MainWindowWndProc`'s `WM_DESTROY`.

`LoadResLibrary` @ `0x00578f10` picks the DLL from `GetUserDefaultLangID()`:

| Language | DLL | `LanguageIndex` (`0x007b5d10`) |
|----------|-----|-------------------------------|
| German | `glresgr.dll` | 2 |
| French | `glresfr.dll` | 3 |
| Italian | `glresit.dll` | 4 |
| Spanish | `glressp.dll` | 5 |
| default | `glreseng.dll` | 0 |

`LanguageIndex` is written at all five sites and **never read** anywhere in the binary.

All five DLLs ship 1527 strings with identical id sets. The `GL_RESOURCE_ID` enum has 1560
names over 1552 distinct values; 25 ids have no string in any DLL and render as `""`.
Eight values carry two names each; the five that matter for menus are the CTF/Team-Score
options block (`0x05e5`-`0x05e9`), where `MPCTFMNU_*` and `MPTSMNU_*` alias the same ids, so
both multiplayer options menus necessarily display identical labels.

Localized *art* is selected through the same table: `GL_MISDEB_FILENAME` (0x5fa) =
`"english debriefing screen.rim"`, `GL_MENU_LOADSAVEFILENAME` (0x5fb) = `"english load save.rim"`.

## Front-end menu inventory

Every index of `Menus[36]`, its title, and how it is populated.

| # | Name | Title resource | Title text | Population |
|---|------|----------------|------------|------------|
| 0 | `Main` | `GL_MENUTITLE_MAIN` | Main Menu | static |
| 1 | `Options` | `GL_MENUTITLE_OPTIONS` | Options | static |
| 2 | `ScreenMode` | `GL_MENUTITLE_SCREENMODE` | Screen Mode | dynamic |
| 3 | `GfxCard` | `GL_MENUTITLE_GFXCARD` | Graphics Card | dynamic |
| 4 | `Keyboard` | `GL_MENUTITLE_CONTROLS` | Controls Menu | static (8 category items) |
| 5 | `ChooseSinglePlayerLevel` | `GL_MENUTITLE_MISSION` | Mission Menu | dynamic |
| 6 | `MouseControls` | `GL_MENUTITLE_MOUSE` | Mouse Menu | static |
| 7 | `SinglePlayer` | `GL_MENUTITLE_SINGLEP` | Single Player Menu | static |
| 8 | `LoadSinglePlayerGame` | `GL_MENUTITLE_LOADGAME` | Load Game | dynamic |
| 9 | `NewSinglePlayerGame` | `GL_MENUTITLE_DIFFICULTY` | Difficulty Menu | static |
| 10 | `Multiplayer` | `GL_MENUTITLE_MSERVICE` | Connection type | dynamic |
| 11 | `JoinGame` | `GL_MULTIPLAYER_JOINGAME` | Join game | dynamic |
| 12 | `JoinIPGame` | `GL_MENUTITLE_MIPADDR` | IP Address | dynamic |
| 13 | `HostIPGame` | `GL_MULTIPLAYER_CREATEGAME` | Create game | dynamic |
| 14 | `MultiplayerLevel` | `GL_MENUTITLE_MISSION` | Mission Menu | dynamic |
| 15 | `MultiplayerOptions` | `GL_MPSOMNU_TITLE` | Multiplayer game options | dynamic |
| 16 | `MultiplayerPlayers` | `GL_MENUTITLE_MPLAYERS` | Select your Team by Clicking on Tabs: | dynamic |
| 17 | `MultiplayerGameType` | `GL_MULTIPLAYER_GAME_TYPE0` | Cooperative | dynamic |
| 18 | `LoadGameInGame` | `GL_MENUTITLE_LOADGAME` | Load Game | dynamic |
| 19 | `Preferences` | `GL_MENUTITLE_PREF` | Preferences | static |
| 20 | `ConfirmScreenMode` | `GL_MENUTITLE_CONFIRMSCRMODE` | Confirm Screen Mode | static (1 item) |
| 21 | `TrainingLevel` | `GL_MENUTITLE_TRAINING` | Training Area Menu | static (5 areas) |
| 22 | `Video` | `GL_MENUTITLE_VIDEO` | Video Menu | static (2 items) |
| 23 | `Controls` | `GL_MENUTITLE_CONTROLS` | Controls Menu | static (2 items) |
| 24 | `GraphicDetails` | `GL_MENUTITLE_GRAPHIC` | Graphic Detail Menu | static |
| 25 | `Audio` | `GL_MENUTITLE_AUDIO` | Audio Menu | static (4 volumes) |
| 26 | `GamePreferences` | `GL_MENU_OPTIONS6` | Game Preferences | static |
| 27 | `IPGame` | `GL_MULTIPLAYER_PLAY_AND_HOST` | Play and Host | static (3 items) |
| 28 | `Camera` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 29 | `Mines` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 30 | `Character` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 31 | `Gameplay` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 32 | `ActivePause` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 33 | `Recon` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 34 | `Formations` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |
| 35 | `Music` | `GL_MENUTITLE_CONTROLS` | Controls Menu | dynamic (key bindings) |

Indices 11 and 14-20 were previously undocumented gaps in `src/Menus.inc.h`.

Two menus deserve a footnote:

- **Menu 19 (`Preferences`) is unreachable.** `ChosenMenu` is written by exactly two functions
  (`GoToMenu` and the reset in `EnterMainMenuScreen`), and no `GoToMenu` call site passes 19.
  Its handler duplicates menu 24's toggles plus two extras. Dead code superseded by 24 + 26.
- **Menu 16 (`MultiplayerLobby`) never contains any `MenuListItem`s.** `nItems` stays 0; the
  whole screen is custom-drawn each frame by `UpdateAndDrawMenuScreen` and `MultiplayerLobbyTick`
  @ `0x004fbb20`. Its "item indices" `0x104`-`0x110` are synthetic slot ids, not list nodes.

### Which function fills each dynamic menu

| Menu | Populator |
|------|-----------|
| 2 ScreenMode, 3 GfxCard | `RebuildDisplayModeMenus` @ `0x004efa30` |
| 5 ChooseSinglePlayerLevel | `AddLevel` @ `0x004efcc0`, incrementally, as levels register |
| 8 LoadSinglePlayerGame | `SetupLoadGameMenu` @ `0x004fba00` (`*.sav`) |
| 10 Multiplayer | inline in `OnMenuItemClicked` @ `0x004ed9f0` (DirectPlay providers) |
| 11 JoinGame | `RefreshJoinGameSessionList` @ `0x004fbda0`, **every frame** |
| 12 EnterIPAddress | inline in `OnMenuItemClicked` @ `0x004edaad`, once |
| 13 CreateGame | `SetupCreateGameMenu` @ `0x004f8500`, auto-invoked by `GoToMenu` |
| 14 ChooseMultiplayerLevel | `SetupChooseMultiplayerLevelMenu` @ `0x004f8850` + `AddMultiplayerLevel` @ `0x004efe00` |
| 15 MultiplayerGameOptions | `SetupMultiplayerGameOptionsMenu` @ `0x004f8bd0` |
| 17 CooperativeGame | `SetupCooperativeGameMenu` @ `0x004f8490` |
| 18 LoadMultiplayerGame | `SetupLoadMultiplayerGameMenu` @ `0x004fb910` (`*.msv`) |
| 28-35 key bindings | `RebuildKeyBindingMenus` @ `0x004f80e0` |

`LevelList` @ `0x007b74dc` is seeded by `EnterMainMenuScreen` with the 15-mission campaign
(`level01`, `prison`, `level02`-`level07`, `level10`, `level09`, `junkyard`, `level11`,
`level12`, `cityruins`, `level15`) and can be extended from the console with
`ADD MISSION <script.gls> <console.gcs>` (`CommandAddMission` @ `0x004402b0`).
`MultiplayerLevelList` @ `0x007b76b0` has the equivalent `ADD MULTI MISSION <name> <script>`
(`CommandAddMultiMission` @ `0x00440390`).

Menu 4's eight items appear to map 1:1, in order, onto menus 28-35 (Camera, Mines, Character,
Gameplay, ActivePause, Recon, Formations, Music) — the `SetupMenus` `AddItem` order matches
that menu-index order exactly. Note the resource ids are *not* contiguous in that order
(`GL_CTRLMENUS_MUSIC` is 0x48a but is added last), so the correspondence comes from the add
order, not from the string table.

### Static menu contents (from `SetupMenus`)

| Menu | Items, in order |
|------|-----------------|
| 0 Main | Single Player, Multiplayer, Options Menu *(auto, `GL_MENU_MAIN1` x3)*, Credits, Exit Game |
| 1 Options | Video, Controls, Graphic Detail, Audio *(auto, `GL_MENU_MAIN_OPTIONS1` x4)*, Game Preferences |
| 4 Keyboard | Camera, Mines, Character control, Gameplay, Active Pause, Recon mode, Formations, Music |
| 7 SinglePlayer | New Game, Training Level *(auto, `GL_MENU_SINGLE3` x2)*, Load Game, and Choose Level **only if `FlagChooseLevel` @ `0x006b0173`** |
| 9 Difficulty | Normal, Hard *(auto, `GL_MENU_DIFF2` x2)*, then toggle Health bars |
| 20 ConfirmScreenMode | "Click here to keep screen mode." |
| 21 Training | Training Level Area 1..5 *(auto, `GL_MENU_TRAIN1` x5)* |
| 22 Video | Change Screen Mode, Change Graphics Card |
| 23 Controls | Edit Keyboard Controls, Edit Mouse Controls |
| 27 IPGame | Host New Game, Play LAN game, Manual IP address entry |

Note menu 9 starts at `GL_MENU_DIFF2` ("Normal"), so **"Easy" (`GL_MENU_DIFF1`, 0x3f1) is
never offered** by the difficulty menu even though the string exists.

### Settings bound to menu items

Toggle items bind an `int*`; multi-value items bind an `int*` index plus a
`GL_RESOURCE_ID**` label array (the "labels array" column below is the *pointer* the item
holds — the ids themselves live one dereference further in, which is why `SetupMenus` ends
with all those `*DAT_007b74c8 = 0x2ee1;` stores):

| Menu | Label | Value global | Labels array |
|------|-------|--------------|--------------|
| 6 Mouse | Invert Mouse | `0x007b9cb0` | — |
| 6 Mouse | Right Mouse Scroll | `0x007b9cbc` | `0x007b76ac` (2) |
| 6 Mouse | Dragbox Edge Scroll | `0x007b9cb8` | — |
| 6 Mouse | Scroll Speed | `0x007b9cc0` | `0x007b7d04` (3) |
| 24 Graphics | Actor Detail | `0x007b9c9c` | `0x007b76c8` (2) |
| 24 Graphics | Texture Detail | `0x006abdf0` | `0x007b76c4` (4) |
| 24 Graphics | Particle FX | `0x006abe00` | `0x007b74d8` (4) |
| 24 Graphics | Game Shadows | `0x007b6fb8` | `0x007b74c8` (4) |
| 24 Graphics | Linear Mipmap | `0x006abdd4` | — |
| 24 Graphics | Anisotropic Filtering | `0x006abdd8` | — |
| 24 Graphics | Colour Depth | `0x006abdf8` | `0x007b74cc` (3) |
| 25 Audio | CD Music Volume | `CDMusicVolume` `0x006abe04` | `0x007b7574` (10) |
| 25 Audio | Battle Music Volume | `BattleMusicVolume` `0x006abe08` | `0x007b7574` (10) |
| 25 Audio | Sound Effects Volume | `SoundEffectsVolume` `0x006abe10` | `0x007b7574` (10) |
| 25 Audio | Cinematics Volume | `CinematicsVolume` `0x006abe0c` | `0x007b7574` (10) |
| 26 Prefs | Friendly Fire | `IsFriendlyFireOn` `0x006abe18` | — |
| 26 Prefs | Friendly Mines | `AreFriendlyMinesOn` `0x006abe1c` | — |
| 26 Prefs | Hints | `AreHintsOn` `0x006abe14` | — |
| 26 Prefs | Health bars | `0x007b9cf4` | — |
| 26 Prefs | Auto Crouch | `IsAutoCrouchOn` `0x006abe20` | — |
| 26 Prefs | Bandwidth Use | `0x006abe24` | `0x007b7d00` (10) |
| 19 Preferences | Triple Buffering | `0x006abddc` | — |
| 19 Preferences | 32-bit Textures | `0x006abde0` | — |
| 19 Preferences | Dynamic Lights | `0x006abdfc` | — |

The bandwidth label array `0x007b7d00` is the volume array `0x007b7574` **reversed**, and the
menu displays `9 - value`, so "bandwidth use" counts down where volume counts up.

Menu 19's particle-rate item passes the **hardcoded English literal `"Particle Rate"`**
instead of a `GL_RESOURCE_ID` — the only unlocalized menu label in the game.

## Front-end navigation, input and rendering

The front end is four cooperating pieces:

```
EnterMainMenuScreen  0x004e7e50   enter / load resources
UpdateAndDrawMenuScreen 0x004ea8e0   per-frame update + render
MenuScreenInputHandler  0x00470c70   input
OnMenuItemClicked    0x004ecf10   activate
GoToMenu             0x004fbfa0   transition
```

### `GoToMenu` @ `0x004fbfa0`

```c
void __fastcall GoToMenu(MenuId target /*ECX*/, bool pushToBackStack /*DL*/);
```

The **only** routine that writes `ChosenMenu` besides the reset in `EnterMainMenuScreen`.

1. Zeroes the outgoing menu's `scrollOffset` and every item's `glowLevel`.
2. Clears `IsTextEditModeActive`.
3. `ChosenMenuItem = 0x100` (nothing selected).
4. Restarts the incoming items' reveal animations and timestamps.
5. If `pushToBackStack`, `Menus[target].parentMenuId = ChosenMenu`. **This single parent link
   per menu is the entire back-stack** — not a real stack.
6. `ChosenMenu = target`; if `target == 13` also calls `SetupCreateGameMenu`.

### `ChosenMenuItem` pseudo-values

| Value | Meaning |
|-------|---------|
| `0x100` | nothing selected |
| `0x101` | Back |
| `0x102` | Scroll up |
| `0x103` | Scroll down |
| `0x104`-`0x110` | multiplayer-lobby slot hotspots (menu 16 only) |

The Up / Down / Back controls are a **circular dial**, hit-tested in polar coordinates by
`PointInPolarSector` @ `0x004f0fd0` (angle scaled to 0..4096, radius with a 0.5625 aspect
correction, centre `(0.25, 0.5)`). Sprites: `SpriteScrollUp` / `SpriteScrollDown` / `SpriteGoUp`
(`0x007b7d0c` / `10` / `14`), loaded from `User Interface/Main Menu.RIF` as "Scroll Up",
"Scroll Down" and "Go Up".

### Input map — `MenuScreenInputHandler` @ `0x00470c70`

| Event | Action |
|-------|--------|
| ESC | `MenuEscapePressed` @ `0x004f0ef0` |
| any key while `IsTextEditModeActive` | `MenuHandleTextOrKeyBind` @ `0x004f0910` |
| Del / Backspace | `MenuClearKeyBinding` @ `0x004f0e20` |
| Enter, or mouse click | `OnMenuItemClicked` |
| Cursor up / down | `MenuSelectPreviousItem` / `MenuSelectNextItem` |
| Wheel up / down | `MenuScrollUpOrSelectPrevious` / `MenuScrollDownOrSelectNext` |

Mouse position reaches the menu code as two normalized floats, `MouseXNormalized` @ `0x007b74d0`
and `MouseYNormalized` @ `0x007b74ec`, hit-tested against each item's `rect`.

`MenuSelectPreviousItem` / `MenuSelectNextItem` build a temporary **circular** list of currently
focusable pseudo-items — `[Up if scrolled] + [visible indices] + [Down if more below] +
[Back if not Main]` — and step through it with wrap-around.

**ESC does not go up one level**: on any menu other than Main it shuts down multiplayer and
jumps straight to `GoToMenu(Main, false)`. On Main it calls `exit(4)`.

### Rendering — `UpdateAndDrawMenuScreen` @ `0x004ea8e0`

Called only from the frame loop at `0x0046eaff`. It does **not** handle input.

- Row *i* (1..6) is drawn at `y = i * MENU_ROW_HEIGHT + MENU_FIRST_ROW_Y` = `i*0.05 + 0.30`.
  **Only 6 items are visible at once**; `Menu::scrollOffset` picks the first.
- Item rects are stored in *unscrolled* space (they use `itemIndex + 1`, not the visible row),
  so `MenuHitTestMouse` compensates by adding `scrollOffset * 0.05` to the mouse Y.
- The title is drawn three rows above row 0 (`y = 0.15`), right-aligned at `x = 0.9`, in
  `MENU_TITLE_TEXT_COLOR` (`0xff00ff00`), using the big font.
- Text colour: selected row pulses through `MenuSelectedColorRamp` (`0x0066744c`, 11 ARGB
  entries) indexed by `TitlePulseIndex`; otherwise green `0xff00e500`, or red `0xfff20000` when
  `isCurrentValue` is set.
- **Typewriter reveal**: `labelCharsRevealed = round(revealRate * (now - labelRevealStartTime))`
  clamped to `strlen(label)`. While the label is still revealing, `valueRevealStartTime` is
  re-stamped to now every frame, so the value only starts once the label finishes.
- Per-menu live refresh happens here too: menu 11 re-enumerates DirectPlay sessions every
  frame, and menu 20 reverts the video mode once `ScreenModeConfirmDeadline` passes.

Text helpers: `MeasureText` @ `0x004d7880`, `DrawTextClipped` @ `0x004d7620`,
`DrawTextRightAligned` @ `0x004d76c0`.

### Action dispatch — `OnMenuItemClicked` @ `0x004ecf10`

```
if (ChosenMenuItem == 0x100) return;
PlayUiSound(0x57);
0x102 -> scroll up      0x103 -> scroll down     0x101 -> back handler @0x004ed00f
switch (ChosenMenu)  ->  jump table @0x004ef318 (36 entries)
```

**A decompiler trap:** `LAB_004ef2f1` is `CALL GoToMenu` whose `ECX` (target menu) and `DL`
(push flag) are set at roughly 30 different `JMP` predecessors. Ghidra renders **every** menu
transition as a bare no-argument `FUN_004fbfa0()` and loses the destination entirely — read the
disassembly, not the C output. The table below was recovered that way.

The menu-13 jump table is typed at `0x004ef464`, with computed-jump references from the jump
site at `0x004edda9` to its five targets; that is what makes `0x004eddb0`-`0x004ee3d1`
disassemble as code.

Back handler specials (`0x004ed00f`): menu 20 restores the video mode; menus 11/12 shut down
DirectPlay; menu 13 clears `IsMultiplayerHost`; menus 14/16/17 use a **two-stage quit
confirmation** latched in `MenuQuitConfirmLatch` (`0x007b733c`) — the first Back prints
`GL_MULTIPLAYER_SERVER_QUIT_WARNING` / `..._CLIENT_QUIT_WARNING`, the second actually tears
down the session. Default: `GoToMenu(parentMenuId, false)`.

#### Menu transition map

| From | Item | To / action |
|------|------|-------------|
| 0 Main | 0 | menu 7 SinglePlayer |
| 0 Main | 1 | init DirectPlay, fill menu 10 with providers, go to 10 |
| 0 Main | 2 | menu 1 Options |
| 0 Main | 3 | play credits |
| 0 Main | 4 | `exit(3)` |
| 1 Options | 0..4 | menus 22, 23, 24, 25, 26 |
| 2 ScreenMode | *n* | apply mode, `GoToMenu(20, false)`, deadline = now + 5.0 s |
| 3 GfxCard | *n* | set adapter, recreate device, fall back to 640x480x16 if unsupported |
| 4 ControlCategories | 0..7 | menus 28..35 (Camera, Mines, Character, Gameplay, ActivePause, Recon, Formations, Music) |
| 5 ChooseLevel | *n* | load level; coop -> menu 15, else menu 9 |
| 6 MouseControls | 0..3 | toggle/cycle the mouse options in place |
| 7 SinglePlayer | 0 | load next campaign level, menu 9 |
| 7 SinglePlayer | 1 | menu 21 TrainingLevel |
| 7 SinglePlayer | 2 | menu 8, then `SetupLoadGameMenu` |
| 7 SinglePlayer | 3 | menu 5, only if `FlagChooseLevel` |
| 8 LoadGame | *n* | load `<label>.sav` |
| 9 Difficulty | 0,1 | `GameDifficulty = item + 1`, start game |
| 9 Difficulty | 2 | toggle health bars |
| 10 Multiplayer | *n* | select service provider, go to menu 27 |
| 11 JoinGame | *n* | join session, `SetupCreateGameMenu`, menu 13 |
| 12 EnterIPAddress | 0 | toggle IP text entry |
| 12 EnterIPAddress | >=1 | connect, enumerate sessions, menu 11 |
| 13 CreateGame | 0..4 | session name / player name / game type / server type / **Create or Join** |
| 13 CreateGame | create | host: coop -> menu 17, else menu 14; client -> menu 16 |
| 14 ChooseMPLevel | *n* | load map + team defaults, menu 15 |
| 15 MPGameOptions | *n* | adjust rules in place; Continue -> menu 16 |
| 16 MPLobby | `0x104`+ | team-slot / ready / kick / chat / start operations |
| 17 CooperativeGame | 0,1,2 | menu 18 / menu 15 / menu 5 |
| 18 LoadMPGame | *n* | load `<label>.msv`, menu 15 |
| 19 PreferencesLegacy | *n* | graphics toggles — **unreachable** |
| 20 ScreenModeConfirm | any | `GoToMenu(2, false)` |
| 21 TrainingLevel | 0 | `Training Level.gls`, difficulty 1 |
| 21 TrainingLevel | 1..4 | play `trainingarea{2..5}.tra` demos |
| 22 Video | 0,1 | menus 2, 3 |
| 23 Controls | 0,1 | menus 4, 6 |
| 24 GraphicDetails | 0..6 | toggle/cycle graphics settings in place |
| 25 Audio | 0..3 | cycle CD / battle / SFX / cinematic volume 0..9 |
| 26 GamePreferences | 0..6 | toggle/cycle preferences in place |
| 27 IPGame | 0,1,2 | host -> menu 13 / LAN -> menu 11 / manual IP -> menu 12 |
| 28-35 keys | any | set `IsTextEditModeActive` to capture the next key |

### Screen-mode confirmation

Applying a resolution goes to menu 20 and sets `ScreenModeConfirmDeadline = now + 5.0`.
`UpdateAndDrawMenuScreen` polls it and, on expiry, restores the previous mode and returns to
menu 2. **The timeout is 5 seconds**, not the 15 the genre usually uses.

### Key bindings

`KeyBindingCategories` @ `0x007b74f0` is 8 intrusive lists of 0x10 bytes each. A binding node
carries `+0x0c -> int[2]{scanCode, modifierMask}` and `+0x10 -> char *actionName`;
`scanCode == -1` means unbound, mask bit 1 = CTRL, bit 2 = ALT.

- `RegisterAllKeyBindings` @ `0x004effc0` installs ~70 defaults.
- `RebuildKeyBindingMenus` @ `0x004f80e0` walks menus `0x1c`..`0x23` in lockstep with
  categories 0..7, emitting one type-1 item per binding.
- Clicking a binding only sets `IsTextEditModeActive`; the actual rebind happens in
  `MenuHandleTextOrKeyBind`, which rejects duplicates within the same category with
  `GL_ERROR_KEY_ALREADY_IN_USE`.
- Persistence: `ReadGLKeys` @ `0x004f6f10` / `WriteGLKeys` @ `0x004fa670` use
  `Scripts\GLkeys.cfg` — repeating triples of lines `<actionName>` / `<scanCode>` /
  `<modifiers>`, matched by name across all 8 categories. A line equal to `"data"` switches the
  parser to a binary `_getw` block holding volumes, hints, friendly fire, and the windowed and
  fullscreen width/height/flags.

### `MENU` console command

`CommandMenu` @ `0x0043e940`, registered by `SetupConsoleCommands` as `MENU` with zero
arguments, help text *"Quits the current game and returns to the menu."*

```c
if (GameState != 5 && GameState != 6 && GameState != 7 && GameState != 0x12) {
    ConsolePrint(GL_ERROR_MAIN_GAME_ONLY);
    return;
}
LevelLoadReason = 1;
```

It does not touch `ChosenMenu`; it sets `LevelLoadReason` (`0x007b9cf0`) to 1, which the
level-load path reads as "tear down the level and return to the front end".

**`LevelLoadReason` values, consolidated** (see also `save_system_notes.md`):

| Value | Meaning |
|-------|---------|
| 1 | quit level, return to the front-end menus |
| 2 | restart the current level |
| 3 | load a full savegame |

## In-game (HUD) menu system

Seven menus in `InGameMenus[7]` @ `0x007b7578`, selected by `InGameMenuIndex` @ `0x007b7270`.
Each open menu also owns a panel widget in `InGameMenuPanels[7]` @ `0x007ba1dc`.

| Index | Panel kind | Opened by | Contents |
|-------|-----------|-----------|----------|
| 0 | 0x00 | `OpenInGamePauseMenu` @ `0x00567b60` | Resume Play, Options, Load Game, Save Game, Restart Level, Exit to Menu |
| 1 | 0x01 | `OpenInGameLoadMenu` @ `0x005686b0` | save-file list |
| 2 | 0x02 | `OpenInGameSaveMenu` @ `0x00568e40` | save-file list |
| 3 | 0x03 | `OpenInGameMultiplayerFailedMenu` @ `0x00567830` | Load Game, Restart Level, Exit to Menu |
| 4 | 0x41 | `OpenInGameOptionsMenu` @ `0x00567f00` | volumes, bandwidth, friendly fire/mines, hints, autocrouch, resume |
| 5 | 0x42 | — | (unidentified) |
| 6 | 0x43 | `OpenInGameConfirmDialog` @ `0x0056a120` | yes/no confirmation dialog |

- `IsAnyInGameMenuOpen` @ `0x00569550` scans the panel array.
- `CloseInGameMenu(kind)` @ `0x005691f0` maps kind `0/1/2/3/0x41/0x42/0x43` to index `0..6`,
  frees every `MenuListItem`, clears the panel slot, and resumes the executor thread once no
  in-game menu remains open (see `threading_model_notes.md`).
- `CloseInGameDialogs` @ `0x0056a230`; exposed to the console as `CommandClearDialogs` @ `0x0044c150`.
- `OpenInGameMultiplayerFailedMenu` is called from the multiplayer client message dispatch
  (`ApplyUpdateMessage` @ `0x0050037c`); it refuses to run in SinglePlayer or Cooperative mode.
- The save/load menus enumerate `*.msv` in Cooperative mode and `*.sav` otherwise, through
  `FUN_004e6840` into `SaveFileList` @ `0x007b6f20` (see `save_system_notes.md`).
- In-game options renders each value into the label buffers at `0x007ba2c4`..`0x007ba2e4`
  (`CDMusicVolumeLabel`, `BattleMusicVolumeLabel`, `SoundEffectsVolumeLabel`,
  `CinematicsVolumeLabel`, then bandwidth / friendly fire / friendly mines / hints /
  autocrouch) before building the items, so the menu shows a snapshot, not live values.

### In-game item dispatch — `InGameMenu__OnItemActivated` @ `0x00563c30`

`__thiscall`, `this` = the panel. A two-level sparse switch:

- **Outer** on `this->kind` (`panel + 0x60`): bytemap @ `0x005649b8` covering ids `0x00..0x41`,
  pointer table @ `0x0056495c` (23 slots). Ids `0x04`, `0x05`, `0x0d`-`0x0f` are unused.
- **Inner** on `InGameMenuSelectedItem` @ `0x006a89b4`:
  - kind `0x00` (pause) -> table @ `0x00564a78`, 6 entries
  - kind `0x41` (options) -> table @ `0x00564a18`, 24 entries
  - selected ids `0x100`+ -> table @ `0x005649fc`, 7 entries

Ghidra had left this whole region as undefined bytes; it needed explicit
`DisassembleCommand` on each jump-table target before it would decompile.

Pause-menu actions (kind `0x00`):

| Item | Label | Action |
|------|-------|--------|
| 0 | Resume Play | `FUN_004a0dc0` — close menus, resume |
| 1 | Options | `OpenInGameOptionsMenu`, unless already on index 4 |
| 2 | Load Game | `FUN_004a0e70` -> `OpenInGameLoadMenu` |
| 3 | Save Game | `FUN_004a0ea0` -> `OpenInGameSaveMenu` |
| 4 | Restart Level | sets `LevelLoadReason` @ `0x007b9cf0` = 2 |
| 5 | Exit to Menu | confirm dialog (see below) |

`LevelLoadReason` is the same global documented in `save_system_notes.md` (3 = loading a full
savegame); **2 = restart current level**.

### Confirm dialogs

```c
void __fastcall OpenInGameConfirmDialog(const char *text /*ECX*/,
                                        void *callbackA /*EDX*/,
                                        void *callbackB /*stack*/);
```

Creates `InGameMenuPanels[6]` (kind `0x43`) and sets `InGameMenuIndex = 6`. The "Exit to
Menu" call site @ `0x0056484d` is:

```asm
PUSH 0x567b60              ; OpenInGamePauseMenu
MOV  EDX,0x2b1c            ; GL_TEXT_CONFIRM_EXIT_TO_MENU
MOV  ECX,0x725664          ; &LocalizedStrings
CALL GetResourceString     ; -> "Are you sure you want to exit to the main menu?"
MOV  EDX,0x43e940          ; CommandMenu
MOV  ECX,EAX
CALL OpenInGameConfirmDialog
```

so the dialog gets the resolved prompt, `CommandMenu` @ `0x0043e940`, and
`OpenInGamePauseMenu`. Which of the two callbacks is "yes" and which is "no" is inferred
from the labels, not proven — `CommandMenu` returns to the front-end (yes) and
`OpenInGamePauseMenu` reopens the pause menu (no), but the argument order has not been
confirmed against `FUN_0056c9e0` @ `0x0056c9e0`, which actually builds the dialog.

`CommandConfirmDialogTest` @ `0x0044c0f0` exercises this path from the console.

## GkPlus API

`src/Menu.h` / `src/Menu.cpp` expose all of the above through `require("gk.menu")`. See the
module table in `CLAUDE.md` for the surface.
