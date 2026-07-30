# Gunlok Level Loading - Reverse Engineering Notes

How a level goes from `.gls` + `.rif` to a running world, and where the seams are for
replacing the `.gls` half with a native level builder.

Companion documents: `gls_system_notes.md` (the script parser), `rif_chunk_format.md`
(the geometry container), `trigger_system_notes.md` (the `.gcs` half),
`save_system_notes.md` (`LevelLoadReason`), `threading_model_notes.md` (which thread runs what).

## 1. The three files a level is made of

| File | Chosen by | Consumed by |
|------|-----------|-------------|
| `<level>.gls` | `ScriptFileName` @ 0x007b6dcc | `LoadGLS` inside `LoadLevel` |
| `<level>.rif` | the `map` section's `file` field | `ToMap`, indirectly via `LoadOrGetRifFile` |
| `<level>.gcs` | `ConsoleFileName` @ 0x007b6dd0 | `ExecuteCommandFile`, multiplayer only |

Plus three **derived cache/sidecar files**, all optional and all regenerated when stale:

| File | Built by | Contents |
|------|----------|----------|
| `<ScriptFileName>.cut` | `ToMap` | baked mesh (`level01.cut`, `prison.cut`, ...) |
| `<ScriptFileName>.map` | `LoadOrBuildSectionAdjacency` | per-section adjacency graph |
| `<level rif>.opt` / `.loc` | shipped with the rif | optimisation data / locators |

### `.cut` is NOT a cutscene file

The extension invites the assumption, and the RIF format does have `CUTSHEAD` /
`CUTTRACK` / `CUTEVENT` / `CUTPOINT` chunks. It is wrong. Evidence:

- **Cutscenes live inside `.rif` files as chunks**, looked up by *name*, exactly like
  shapes and hierarchies. The console command is `PLAY CUTSCENE <name>` ("Requires the
  name of the cutscene, e.g. PLAY CUTSCENE intro"), and the only cutscene *file* string
  in the binary is `sound\cutscene_bink\%s.bik` for the Bink audio.
- There is **no `.cut` string literal anywhere in the binary** - the extension is never a
  constant. `ToMap` builds it three bytes at a time, which is why a text search misses it:

  ```
  0047f258  CALL strdup                            ; strdup(ScriptFileName)
  0047f28a  MOV word ptr [EDX+EAX-0x3], 0x7563     ; 'c','u'
  0047f291  MOV byte ptr [EDX+EAX-0x1], 0x74       ; 't'
  0047f296  CALL CreateFileA                       ; GENERIC_READ, OPEN_EXISTING
  ```

- The on-disk bytes match the reader documented below exactly - `level01.cut` starts with
  an 8-byte FILETIME, then `ff ff 7f 7f` x3 (= `{FLT_MAX, FLT_MAX, FLT_MAX}`, the
  compressed-body marker), then a `REBCRIF1` Huffman header.
- `level01.cut` and `level01.map` begin with **byte-identical FILETIMEs**
  (`c2 6f dd 67 12 80 d8 01`): two independent caches stamping the same source
  `level01.rif`. A cutscene asset would have no reason to do that.

(Speculative, but the mesh is split by `max vertices per section` inside `Map_Ctor`, so
"cut" plausibly means the level cut into sections rather than anything cinematic.)

`ScriptFileName` is written by `OnMenuItemClicked` (menu level pick), `LoadGame`,
`PeekSaveGameScriptName`, `CommandNextLevel`, `LoadDemoFile`, `EnterMainMenuScreen`
and `ApplyUpdateMessage` (a multiplayer client being told which level to load).

## 2. Entry points

```
WinMain
  +- switch (GameState @ 0x006b02b4)
       +- WinMainState_RunFrameLimited -> RunGameFrame -> PumpQueuedConsoleCommand
       ...                                                (per-frame, NOT loading)

BeginLevelSession(bool doLoad)            @ 0x004e2560
  |   callers: CommandEndBriefing & ShowBriefingOrDebriefScreen (front end, see 2.1),
  |            LoadGame (savegame restore), FUN_004fb850 (multiplayer briefing),
  |            FUN_0046c170 (attract-mode demo)
  +- first call only: StartExecutorThread + InitClientRouting (single-player too),
  |  register GAMESPEED/FOG* console commands, load movement_indicator2.rif
  +- GameState = 0x12
  +- if (doLoad) LoadLevel(freshStart = 1)   @ 0x004e0980   (MOV CL,1 @ 0x004e26d9)
```

### 2.2 `LoadLevel` takes an argument

`LoadLevel` is **`__fastcall(bool freshStart)`**, not `StdCall<void>` - the flag arrives
in CL and is stashed at `[EBP-0x175]` in the prologue (0x004e09ad), before anything
else touches ECX. It gates five things: the sun colour/direction, the
`ExecuteCommandFile(ConsoleFileName)` that queues the level `.gcs`, the
`ExecuteAllCommands()` that runs it, `FUN_00504500`, and the mission-stats reset in
SP/coop.

Only two call sites, and they disagree:

| Caller | CL | Meaning |
|--------|----|---------|
| `BeginLevelSession` @ 0x004e26d9 | `MOV CL,1` | a new level start |
| `LoadGame` @ 0x00505c86 | `XOR CL,CL` | restoring a savegame (right after `LevelLoadReason = 3`) |

So it is the savegame counterpart of `LevelLoadReason == 3`, carried separately: a
restore must not re-run the `.gcs`, because the save already holds everything that
script set up.

**The `.gcs` therefore runs in single player.** Earlier revisions of this file
annotated the `ExecuteCommandFile(ConsoleFileName)` in step 7 as multiplayer-only,
having mistaken this flag for the multiplayer one. `level01.gcs` is 22 KB of camera
bounds, fog, sun and trigger setup that single-player level 1 plainly needs.

### 2.1 From the front end: New Game / Choose Level -> briefing -> BeginLevelSession

The menus never call `LoadLevel` directly. Selecting a single-player level only sets
`ScriptFileName` / `ConsoleFileName`; the actual load is kicked off from the **briefing
screen**. The non-obvious hop is that difficulty selection goes to `GameState 0x10`
(briefing), and it is the briefing's `END BRIEFING` command (or a briefing with no scripted
content) that finally calls `BeginLevelSession`. Verified in `OnMenuItemClicked` @ 0x004ecf10
(jump table @ 0x004ef318) and the briefing driver:

```
Main(0) item0 ---------------------------> GoToMenu(7) Single Player
SinglePlayer(7) item0 "New Game" .........  strdup the CURRENT campaign level's .gls/.gcs
   |                                         out of LevelList (0x007b74dc): cache node
   |                                         +0x10 -> ScriptFileName, +0x14 -> ConsoleFileName
   |                                         -> GoToMenu(9) Difficulty
SinglePlayer(7) item3 "Choose Level" .....  GoToMenu(5)   (only if FlagChooseLevel 0x006b0173)
ChooseLevel(5) item n ....................  LevelList__GetTitlePtrAt(0x007b74dc, n) @0x004f7650,
   |                                         node +0x10/+0x14 -> ScriptFileName/ConsoleFileName;
   |                                         GameMode(0x007b9e28)==coop -> menu 15, else GoToMenu(9)
Difficulty(9) item 0/1 (Normal/Hard) .....  GameDifficulty(0x007b9cc4) = item+1;
   |                                         FUN_004f94c0, FUN_004e8dd0 (menu-screen teardown);
   |                                         ShowBriefingOrDebriefScreen(isBriefing=1) @0x004b1f60
   v
ShowBriefingOrDebriefScreen(1)            @ 0x004b1f60
  +- GameState = 0x10; global IsBriefing(0x007b68e0) = 1
  +- display briefing bitmap (GL_MISBRF_FILENAME); load robots.dat
  +- register briefing console cmds incl. END BRIEFING -> CommandEndBriefing @0x004b33c0
  +- ExecuteCommandFile("<ScriptFileName, ext -> .brf>")     // the level's briefing script
  +- if that script queued NO commands (NumCommandsToExecute==0):
  |       BeginLevelSession(load) inline  @0x004b23c5   -> LoadLevel     [no briefing shown]
  +- else the briefing screen stays up (GameState 0x10) until the player ends it:
          END BRIEFING -> CommandEndBriefing -> BeginLevelSession(load) @0x004b33df -> LoadLevel
```

Notes:

- **`IsBriefing` @ 0x007b68e0** `1` = a
  pre-level *briefing* is up, `0` = a post-level *debriefing*. `CommandEndBriefing` loads the
  next level (`BeginLevelSession`, `CL=1`) only when it is non-zero; the `0` path tears the
  debrief down without loading. `ShowBriefingOrDebriefScreen`'s parameter was likewise
  mislabelled `IsDebriefing` and is now `isBriefing` (`bool`, in `CL`): `isBriefing=0` shows the
  debrief screen (`GL_MISDEB_FILENAME`) and chains to the next briefing or `EnterMainMenuScreen`.
- The screen id is `GetResourceString((isBriefing ^ 1) + GL_MISBRF_FILENAME)`, i.e.
  `isBriefing=1` -> GL_MISBRF (briefing), `isBriefing=0` -> GL_MISDEB (debrief).
- **Neither script is specified anywhere; both names are derived.** The briefing is
  `ScriptFileName` with its last three characters overwritten with `brf` (guarded by
  `len > 4`), and the **debrief is the fixed literal `debrief.dbf`** - one file shared by
  every level in the game, not a per-level script. The single exception is
  `ScriptFileName == "Training Level.gls"`, compared with `_mbsicmp`, which takes a
  script-free path instead (`IsTrainingDebrief` + a 3-second fade). So a custom level whose
  `ScriptFileName` names no file gets no briefing and starts immediately - the
  `NumCommandsToExecute == 0` branch calls `BeginLevelSession` inline. Giving one a
  briefing means serving that `.brf` from memory through the `ExecuteCommandFile` hook;
  nothing does that yet.
- The debrief screen is only reached from `CommandNextLevel` @ 0x004f6ba0 (`NEXT LEVEL`),
  which first requires the current level *not* to be the last node in `LevelList` -
  otherwise it just sets `LevelLoadReason = 1` and returns. Since `AddLevel` appends,
  a single registered custom level makes `NEXT LEVEL` a no-op.
- Front-end call sites pass the `BeginLevelSession` load flag in **CL** (`MOV CL,1`) — arg 1 of
  the `__fastcall`, not DL.
- Training area (menu 21) and campaign-advance (`CommandNextLevel` @0x004f6ba0) reach the same
  `ShowBriefingOrDebriefScreen`; multiplayer uses FUN_004fb850 / FUN_004ef950 instead, and a
  savegame load uses `LoadGame` -> `BeginLevelSession` directly (no briefing screen).

## 3. `LoadLevel` @ 0x004e0980

Ordered outline. Loading-bar text comes from `GetResourceString(&LocalizedStrings, ...)`
then `ShowLoadingMessage` @ 0x004e2910; `FUN_004e2c20(pct)` advances the bar.

```
 1. briefing background: GL_MISBRF_FILENAME -> "bitmaps\<name>" -> LoadRimFile2 -> BriefingRIM
 2. SetAmbientLight, fog reset
 3. if (LevelLoadReason != 2) flush the texture cache
 4. next_entity_id = 0; NextInventoryItemId = 0            <-- id counters reset here
 5. GL_LOADING_AI_AND_UI    : AI + UI init; per-team-slot init over TeamSlots[]
 6. GL_LOADING_SOUNDS       : robots.dat, specials.dat, environ.dat from the Sounds dir
 7. GL_LOADING_LEVEL_DATA   :
        SetCurrentDirectoryToGLDir(GL_Scripts)
        list = LoadGLS(ScriptFileName, 1)     // parse .gls + every #include
        if (freshStart) { BattleNumber = 3; ExecuteCommandFile(ConsoleFileName) }
        SetCurrentDirectory()                 // cwd is back to normal from here on
        ConvertParsedObjects(list)            // <<< ToMap runs here, see section 4
        FreeParsedObjectList(list)            // pool-frees the header too
 8. world bounds -> 0x007f598c.. ; ShadowQuality==3 && LevelLoadReason!=3 -> shadow bake
 9. GL_LOADING_TEXTURES     : texture upload; free BriefingRIM
10. camera defaults (dist 20, roll 341 deg), then snap to the first team-1 actor
11. clocks resynced to the executor; music stopped; if (freshStart) ExecuteAllCommands() (the .gcs)
12. if (LevelLoadReason != 3) ApplyTeamCarryOverState()   // inventory carried between levels
13. LevelLoadReason = 0
14. multiplayer frag/time-limit trigger registered
```

Two flags gate large parts of it:

- `LevelLoadReason` @ 0x007b9cf0 - `3` = restoring a full savegame, which suppresses the
  shadow bake, the team carry-over **and the entire placed-object spawn** (the savegame
  has its own actor list). `2` suppresses the texture flush.
- `GameMode` - `SinglePlayer` / `Cooperative` vs the multiplayer modes.

## 4. `ToMap` @ 0x0047f160 - the actual level builder

`toGameObject` (vtbl slot 7) of a parsed `map` section. ~1500 decompiled lines. Full
phase breakdown is in the Ghidra plate comment; the structure that matters:

```
if (TheMap != NULL) {                       // <<< THE HOOK
    rif = AcquireLevelRifForLocators(...);   // geometry already exists, skip to phase B
} else {
    ... produce (sceneObject, origin, LevelMeshHeader*) ...   // phase A
    SetMaxVerticesPerSection(field 0x7a);
    TheMap = Map_Ctor(malloc(0x18c), scene, &origin, mesh);
    Map_PostConstruct();
}
... decorate TheMap ...                      // phase B
... populate the world ...                   // phase C
```

### Phase A - produce the geometry triple

Two existing implementations converge on the same `Map_Ctor` call:

**Cold path** (no cache, or `.cut` older than the `.rif`):

```
LoadOrGetRifFile()                                    @ 0x004ae960  - no args, global rif
RifFindObjectByName(rif, field 0x00 /*name*/, &origin) @ 0x005aa5c0
mesh = rifObject[0x8c]; rifObject[0x8c] = NULL        // ownership stolen
CreateSceneObjectFromRifObject(rifObject, 1)          @ 0x00599f80  - malloc(0x1a0)
... then serialise mesh to <ScriptFileName>.cut ...
```

Note the `.rif` named in the `file` field is opened here **only for its FILETIME** - the
rif itself was already loaded globally before `ToMap` ran.

**Warm path** (`.cut` FILETIME matches the `.rif`):

```
read 8-byte FILETIME, then a 0xc-byte marker
if marker == {FLT_MAX,FLT_MAX,FLT_MAX}  body is Huffman-packed -> HuffmanDecompress
else                                    those 12 bytes were the origin Vec3f
mesh = malloc(0x18); fill it by hand; fix up indices -> pointers
CreateSceneObjectFromCachedMesh()                     @ 0x0059da90
```

`.cut` body layout as written:

```
+0x00  FILETIME of the .rif                     8
+0x08  {FLT_MAX, FLT_MAX, FLT_MAX} or origin    0xc
+0x14  origin Vec3f (compressed form only)      0xc
       vertexCount                              4
       vertices                                 n * 0xc
       triangleCount                            4
       quadCount                                4
       per triangle: 3 x int32 index, 0xc bytes plane, 4 bytes flags
       per quad:     4 x int32 index, 0xc bytes plane, 4 bytes flags
```

`LevelMeshHeader` (0x18, a named struct in the Ghidra DB):

| Off | Field |
|-----|-------|
| 0x00 | `tri_count` |
| 0x04 | `tris` -> `LevelMeshTri[0x1c]` |
| 0x08 | `quad_count` |
| 0x0c | `quads` -> `LevelMeshQuad[0x20]` |
| 0x10 | `vert_count` |
| 0x14 | `verts` -> flat `Vec3f[0xc]` |

`LevelMeshTri` (0x1c): `v0/v1/v2` are **absolute pointers** into the vertex array
(`verts + idx*0xc`), not indices; `+0x0c` 12-byte plane; `+0x18` flags.
`LevelMeshQuad` (0x20): four pointers, `+0x10` plane, `+0x1c` flags.

### Phase B - decorate `TheMap`

`TheMap` @ 0x00739090 is a 0x18c-byte object. These writes are flat and independent:

| Parsed field | Destination |
|--------------|-------------|
| `bitmap` 0x02 | `Map->bitmap` @ 0xcc (owned `char*`) |
| `camera plane` 0x50 | locator lookup -> `InitialCameraState` @ 0x007b4e18 |
| `max camera focus height` 0x52 | locator `+0x48` -> `Map+0x150` -> `MaxCameraFocusHeight` @ 0x007b3ea8 |
| `min camera focus height` 0x53 | locator `+0x48` -> `Map+0x144` -> `MinCameraFocusHeight` @ 0x006a574c |
| `shadow object rif` 0x54 | `Map->shadow_object_rif` @ 0x160 |
| `shadow object name` 0x55 | `Map->shadow_object_name` @ 0x164 |
| - | `Map->rif_time_low/high` @ 0x158/0x15c = the `.rif` FILETIME |
| - | `Map->sky_object` @ 0x188 = sky object (0x1f0, `FUN_0059c0f0`) |

### The `Map` object (0x18c)

Mirrored in C++ as `gk::Map` in `src/Map.cpp`, with `static_assert`s on every
offset below. Built by `Map_Ctor` @ 0x00470f20.

The object has **two vptrs** (0x00 and 0xa4), i.e. multiple inheritance, and the
C++ mirror reproduces it as `Map : MapBase, RefCountedBase`:

- `MapBase` @ 0x00, **0xa4 bytes**. Its whole chain declares exactly one virtual —
  vtables 0x00663e5c <- 0x00652818 <- 0x00652824 are each a single slot. Destructor
  `FUN_00489ae0` (called with `this+0x00` at the tail of the Map dtor) frees the 0x24
  list and the arrays at 0x34/0x38/0x3c and 0x7c/0x80/0x84/0x88.
- `RefCountedBase` @ 0xa4, **8 bytes** = `{vptr, refcount}`. A base shared across the
  engine: root vtable 0x006522e8 is referenced by ~28 classes, and the same pair sits
  at +0x9c/+0xa0 on the scene objects Map holds at 0xc8 and 0x188. Chain 0x006522e8
  (1 slot) <- 0x0065281c (2, slot 1 `__purecall`) <- 0x00652828 (2 slots).
- Everything from 0xac on is `Map`'s own.

`Map_ClearReferencesTo` @ 0x00473f70 is that single primary slot; it is never called
directly, only virtually.

| Off | Field | Notes |
|-----|-------|-------|
| 0x000 | `vtbl` | primary vtable 0x00652824 (1 slot) |
| 0x004 | `lock` | embedded RWLock (0x20); `RWLock_Lock` @ 0x00579700 / `RWLock_Unlock` @ 0x005797c0 |
| 0x024 | list header | `{sentinel, count, cached_array, cache_valid}` |
| 0x088 | `sections` | section table |
| 0x08c | `num_sections` | bound for the `.map` adjacency sidecar |
| 0x094 | list header | |
| 0x0a4 | vptr | second base subobject; vtable 0x00652828 (2 slots) |
| 0x0a8 | `refcount` | `LoadGame` addrefs; `FUN_004e2090` releases, deletes at 0 |
| 0x0ac | `adjacency_built` | run-once gate in `LoadOrBuildSectionAdjacency` |
| 0x0b4 | Vec4 | `{1,1,1,1}` |
| 0x0c8 | `scene_object` | 0x1f0 bytes, `FUN_0059c3a0(sceneObject, 1)` |
| 0x0cc | `bitmap` | owned `char*` |
| 0x0fc | list header | |
| 0x10c | list header | |
| 0x11c | `neg_origin` | **negated** map origin (Vec3) |
| 0x128 | `bounds_min` | world bounds; `LoadLevel` reads the pair adjacently |
| 0x134 | `bounds_max` | |
| 0x140 | `camera_focus_min` | y (0x144) -> `MinCameraFocusHeight` |
| 0x14c | `camera_focus_max` | y (0x150) -> `MaxCameraFocusHeight` |
| 0x158 | `rif_time_low/high` | FILETIME of the level `.rif` |
| 0x160 | `shadow_object_rif` | owned `char*` |
| 0x164 | `shadow_object_name` | owned `char*` |
| 0x168 | `default_position` | Vec3, valid only when 0x174 is set |
| 0x174 | `has_default_position` | gates `ConsoleParsePosition`'s use of it |
| 0x178 | Vec3 | |
| 0x188 | `sky_object` | lazily built; `ToMap` tests then sets |

`0x024..0x088` and `0x08c..0x0a4` remain unmapped: they are only reached through
`__thiscall` methods invoked directly on `TheMap` (`FUN_0048cf50` and the
`0x00472xxx` / `0x0048xxxx` cluster), so a global-reference sweep does not see
them. Sweeping `FUN_0048cf50` is the highest-yield follow-up.

**The origin is stored negated.** `Map_Ctor` XORs each component of its `origin`
argument with `0x80000000` before storing it at 0x11c, and `ToMap` *adds* that
field to scaled rif locator coordinates. The net effect on a placed object is

```
pos = rif_locator_pos * RifUnitScale(rif) - origin
```

`gk::MapOrigin` flips the sign back, returning the origin the map was actually
built with.

**`max camera distance` (0x51) is parsed, range-checked and then never read.**
`MaxCameraDist1 = MaxCameraDist2` comes from globals only.

If `camera plane` is absent the `.loc` entry with the extreme value of `locator+0x64`
is used instead.

### Phase C - populate the world

Positional sounds first: `MapAuxObjectList` @ 0x00739098 is drained; each node carries a
text blob at `+0x38`, and nodes whose first line is `Sound` are turned into 3D sounds
(`V` volume int, `P` pitch float, `R` range float, `I` inner float; hand-rolled atof).
Every node is freed (0x3c bytes each) regardless.

Then the placed objects - see the next section.

## 5. Placed objects: the `use ... for ...` binding table

This is the part a native level builder most wants to replace, and it is cleanly isolated.

### Syntax

```
map
{
    file "levels\level01.rif"
    name "Land"
    bitmap "bitmaps\\LEVEL01.rim"
    camera plane "camhund"
    max camera distance 60
    max camera focus height "max focus height"
    min camera focus height "min focus height"
    shadow object rif "levels\level01_shadow.rif"
    shadow object name "Land"
    max vertices per section 250

    use Rol_GunLok in team 1 for
        "Goodie A" as "gunlok"
    extreme use Rol_Hark in team 1 for
        "Goodie C" as "hark"
    use Rol_Corkscrew_F in team 0 for
        "Corkscrew F" and "Corkscrew FB" and "Corkscrew FC"
}
```

The quoted names are **names of objects inside the `.rif`**, not coordinates. The rif
supplies position and orientation; the script only says "whatever object is called
*this* in the rif, make it an actor with *that* role on *that* team".

### Storage

`ParsedMap` is **0x1b78 bytes, not the 0x1b60 of every other section type**. The extra
0x18 is a `PlacedObjectBindingMap` embedded at `+0x1b60`, constructed by
`PlacedObjectBindingMap_Ctor(this+0x1b60, 6)` -> 64 buckets:

| Off | Field |
|-----|-------|
| 0x00 | vtbl (0x0066328c) |
| 0x04 | count |
| 0x08 | capacity (64) |
| 0x0c | mask (63) |
| 0x10 | bucket array |

Hash = `sum of toupper(c)` over the object name; lookup compares with `__stricmp`.

`PlacedObjectBinding` (0x18):

| Off | Field | Meaning |
|-----|-------|---------|
| 0x00 | `char* object_name` | RIF object name (owned copy) |
| 0x04 | `char* token_name` | the `as "..."` clause, NULL if absent |
| 0x08 | `ParsedThingBase* role` | the parsed `role` section, `ref_count++` |
| 0x0c | `int team` | index into `TeamSlots[]` |
| 0x10 | `bool overridable` | 0 => duplicate is an error |
| 0x14 | `next` | bucket chain |

### How it gets filled

Field id **9** is the pseudo-field for the whole `use` clause, handled by the ParsedMap
override `CheckValue_Map` @ 0x0047efa0 (everything else falls through to the shared
`CheckValue`). The grammar hands it an extended `ParsedField`:

```
+0x00  id (= 9)
+0x04  list of {char* object_name, char* token_name}   // the "for A and B and C" list
+0x0c  ParsedThingBase* role
+0x10  int team
```

For each pair: `CreatePlacedActorEntry(&tmp, object_name, team, role, token_name)`
@ 0x0047eb20, then `PlacedObjectBinding_CopyCtor` into a `malloc(0x18)` hash node.
A duplicate object name produces `"object '%s' has already been used"` unless the
existing entry is `overridable`.

### How it gets consumed

The tail of `ToMap`, gated on `LevelLoadReason != 3`:

```c
for (binding in map->bindings) {
    int team = binding->team;
    if ((unsigned)team < NumTeamSlots && TeamSlots[team].active != 0) {
        RifFilterObjectsByName(&matches, rif, binding->object_name); // 0x005aaac0
        for (O in matches) {                 // node: next @ +0x8, payload @ +0xc
            Role *role = ToRole(binding->role);
            Vec3f pos  = (Vec3f){ (float)(int)O[0x44], (float)(int)O[0x48],
                                  (float)(int)O[0x4c] };   // FILD: signed int32
            pos *= *(float *)rif;                        // the rif's own unit scale
            pos += TheMap->neg_origin;                   // 0x11c, NEGATED origin
            Vec4f quat = { O[0x50], O[0x54], O[0x58], O[0x5c] };

            int id = -1;
            if (IsExecutorRunning())     id = ServerSpawnActorForTeam(team, role, &pos, &quat);
            if (IsClientRoutingActive()) id = ClientSpawnActorForTeam(team, role, &pos, &quat);
            if (binding->token_name)     SetOrCreateToken(&Tokens, binding->token_name, (float)id);
        }
    }
    PlacedObjectBindingMap_Remove(binding);   // consumed, one shot
}
```

Notes:

- Locator coordinates in the rif are **integers** (`FILD`, signed), converted to float
  and scaled by the rif's own unit scale, then offset by the map origin.
- **The unit scale is `*(float *)rif`** - the first float of the object
  `AcquireLevelRifForLocators` @ 0x00483da0 / `LoadOrGetRifFile` @ 0x004ae960 return,
  the same handle `ToMap` passes to `RifFilterObjectsByName` in EDX. It is **per-rif
  data; there is no world-unit-scale global or getter.** Earlier revisions of this file
  claimed `*GetWorldUnitScale()` at 0x005a9b40; that function is `CopyDword`, a
  `__fastcall(dest, src)` four-byte copy returning dest, and `ToMap`'s three calls to it
  (0x00480f65, 0x00481151, 0x00481898) are copying that float into a local before the
  `MULSS`. `gk::RifUnitScale` / `gk::MapToWorld` in `src/Map.h` take the rif for this
  reason.
- `TeamSlots` @ 0x007b3ec4 / `NumTeamSlots` @ 0x007b3ec0, stride 0xc4. A slot with
  `active` (`+0x69`) `== 0` means the whole team is skipped - this is how `extreme`-only
  and multiplayer-only teams are excluded without touching the bindings. The bound check
  is **unsigned** (`JNC`), so a negative team index is rejected too.
- `RifFilterObjectsByName` takes **three** arguments:
  `ThisCall<void, List *out, void *rif, const char *name>` (ECX = the output list,
  EDX = the rif, name on the stack).
- The token created by `as "..."` holds the **actor id as a float**, which is how `.gcs`
  trigger scripts refer back to script-placed actors.
- Both spawn calls run; on a listen host the executor and the client each get their own
  actor, and the id returned by the client call wins for the token.

### The two spawn functions

`ServerSpawnActorForTeam` @ 0x005035b0 (executor thread) - coop remaps team 1 for the
five named heroes based on `GetPlayerCount()`, then `CreateActor(team, role, pos, ori)`.

`ClientSpawnActorForTeam` @ 0x004fce90 (main thread) - the client actor factory,
switching on `role->ai` (`AIType`). Allocation sizes, which are the *client* mirror
classes and differ from the executor-side sizes in `actor_vtable_notes.md`:

| AIType | size | | AIType | size |
|--------|------|-|--------|------|
| Bot / default | 0x2e0 | | Pickup | 0x150 |
| default, `character->weapon == 0x21` | 0x238 | | BackgroundCreature | 0x178 |
| Mine | 0x238 | | FlyingBackgroundCreature | 0x190 |
| no character, no projectile | 0x130 | | Centipede / Centibody / Popup | 0x2e8 |
| Blocker | 0x140 | | Node / NodeWaiting | 0x240 |
| TrackObject | 0x1d0 | | President | 0x248 |
| Tumbleweed | 0x148 | | Turret | 0x2f0 |

Actor id source is `DAT_007b68e4`.

## 6. Sidecar caches

### `<ScriptFileName>.map` - `LoadOrBuildSectionAdjacency` @ 0x0044fef0

Run-once per level (`Map->adjacency_built` @ 0xac). Reads the `.map` file; if its leading
FILETIME matches `Map->rif_time_low/high` @ 0x158/0x15c (the `.rif` time) it replays the
cached per-section neighbour lists through vtbl slot 0x5c; otherwise it rebuilds them with
`FUN_0048aa00` (under `Map->lock` @ 0x04) and writes the file back.

Format: FILETIME, then per section `{neighbourCount, sectionId, neighbourCount x
neighbourSectionId}` over `Map->sections[0 .. Map->num_sections)` (0x88/0x8c).

### `<level rif>.opt` / `.loc`

Loaded by `FUN_005b03b0` from the RIFs directory during the cold path only. `.loc`
holds the named locators used for the camera plane and the focus-height fields;
entries named `"sky"` are filtered out of the geometry list.

## 6.5 Registering a level: `AddLevel` and `LevelList`

`AddLevel` @ 0x004efcc0 - `__fastcall(char *title, char *scriptFile, char *consoleFile)` -
is the game's own level registration, and the only way anything gets into Choose Level.
It strdups all three strings into a 0x18-byte node appended to `LevelList` @ 0x007b74dc,
**and appends a plain item to `Menus[5]` (ChooseSinglePlayerLevel) in the same call** - so
menu 5 is populated incrementally as levels register, and menu 5's dispatch mapping item
*n* onto list entry *n* holds by construction.

`LevelList` is `List<LevelInfo>` (the same template as `Menu::items`), so a node is the
0xc-byte `List_Member` base plus:

| Off (node) | Field |
|------------|-------|
| +0x0c | `char *title` - also the menu item's label, stored by pointer |
| +0x10 | `char *script` -> `ScriptFileName`, the level `.gls` |
| +0x14 | `char *console` -> `ConsoleFileName`, the level `.gcs` |

Nothing frees them; there is no `RemoveLevel`. Callers: `EnterMainMenuScreen` (the
15-mission campaign, 15 calls) and `CommandAddMission` (`ADD MISSION <gls> <gcs>`).
Mirrored as `gk::LevelInfo` / `gk::LevelList` / `gk::AddLevel` in `src/Menu.h`.

**Getting to menu 5 is the catch.** Its only in-game entry point is item 3 of the
SinglePlayer menu, which `SetupMenus` adds **only if `FlagChooseLevel` @ 0x006b0173** is
set - and `WinMain` sets that from the `-chooselevel` command line switch
(`FlagChooseLevel |= strstr(cmdLine, "-chooselevel") != NULL`, 0x0046afe2). Since
`SetupMenus` reads it once, setting the flag later cannot conjure the item; anything
registering levels after boot has to supply its own way in.

## 7. Replacing the `.gls` path

The design already has the seams. In order of increasing ambition:

**(a) Keep the `.gls`, override the bindings.** The binding map is a plain hash on the
`ParsedMap` and the consumption loop is ~40 lines. Hooking `ToMap` and editing
`ParsedMap+0x1b60` before it runs gives full control over what spawns where, with zero
geometry work. `gk::gls` already knows how to build `ParsedRole`s programmatically.

**(b) Keep the `.rif`, drop the `map` section.** Reimplement the consumption loop in the
mod: `LoadOrGetRifFile` -> `RifFilterObjectsByName` -> read `O[0x44..0x5c]` ->
`ServerSpawnActorForTeam` / `ClientSpawnActorForTeam` -> `SetOrCreateToken` (formerly
`CreateToken`). This maps directly
onto the native API (`gk::MapSpawn` + `gk::SetOrCreateToken`); everything needed is a named
export already.

**(c) Supply geometry natively.** `TheMap != NULL` at the top of `ToMap` is the single
gate on the entire geometry phase - if something else has populated that global, `ToMap`
skips phases A entirely and runs B and C unchanged. The warm path is the template for a
third producer: it builds a `LevelMeshHeader` from scratch with plain `malloc`s, does its
own index -> pointer fixup, and never touches the rif. The required sequence is

```
SetMaxVerticesPerSection(maxVerts);
TheMap = Map_Ctor(malloc(0x18c), sceneObject, &origin, meshHeader);
Map_PostConstruct();
TheMap->rif_time_low/high = <some FILETIME>;   // 0x158/0x15c, or the .map cache thrashes
```

`CreateSceneObjectFromCachedMesh` @ 0x0059da90 is the only opaque piece on that side and
is the next thing to decompile if (c) is the goal.

### What GkPlus actually implements: (b), plus the map section natively

`src/CustomLevel.cpp` is (b) with the `map` section built through `gls::Create` rather
than parsed, which is what lets a level exist with no `.gls` and no `.gcs` at all. Five
decisions carry it, and each was picked over an alternative that does not work:

- **The map is built from a hook on `ConvertParsedObjects` @ 0x004747b0, not on
  `LoadGLS`.** Having `LoadGLS` return a fully synthetic list means hand-building a
  `ParsedObjectList`, and `FreeParsedObjectList` pool-frees the header *and* destroys the
  sentinel and every node through their vtables - the parser creates all of that inline
  inside `ParseGSH` (0x00478f6c: `pool_alloc(0x10)` header, `pool_alloc(0xc)` sentinel
  with vtable 0x00663064), so there is no helper to reuse. Hooking the *converter* instead
  means the game builds and frees its own list as usual and GkPlus only calls the map's
  `toGameObject` slot afterwards.

  `LoadGLS` **is** hooked, but only to decide what the parse reads: a source text for a
  level with `includes`, and for one without, the hand-built empty list above - which is
  the one case where reproducing `ParseGSH`'s layout is unavoidable, and it is 20 lines
  rather than a whole node graph.
- **The level's `ScriptFileName` names no file at all.** It is a virtual
  `gkplus\<slug>.gls`, where the slug is the title with every non-alphanumeric character
  folded to `_`, and nothing on any load path opens it. A level that names `includes` gets
  the `#include` list as a **source text** instead, handed to the parser by
  `gls::ParseSource` - the parser's input is a source object, not a path, so a null
  `FILE*` plus a text buffer is a complete parse (the mechanism, and the three
  measurements behind it, are in `gls_system_notes.md` under "Parser input sources"). The
  `#include` lines still resolve, because the parser opens an include with a bare `fopen`
  (0x00478c2c) against the *current* directory, which step 7 has already set to Scripts.
  One source text rather than one `LoadGLS` per include, because the multiple-inclusion
  guards only hold within a single call - `ClearParseSymbolTables` runs per call, so N
  calls would re-register every shared `.gsh`.

  **The shape of that name is dictated, not chosen.** It needs a three-letter extension
  and more than four characters, because `ToMap` and `LoadOrBuildSectionAdjacency`
  overwrite the last three characters in place to build `.cut` and `.map`; no double
  quote, because `PushFileToParserStack` puts the name in a re-lexed `# line` directive;
  and it must be **machine-independent**, which the `%TEMP%` absolute path it replaced was
  not. `SaveGame` serialises `ScriptFileName` verbatim and `ApplyUpdateMessage` @ 0x004fdf3b
  `strdup`s it out of a network payload on a joining client, so an absolute path under one
  user's profile made a custom-level savegame unportable and a multiplayer join match no
  registration at all - the client would load the geometry and run none of the callbacks.
- **The two derived cache names are legal, optional and land in `<Gunlok>\gkplus`.**
  `ToMap` restores the cwd to the game root (0x0047f24d) before deriving `.cut`, and the
  read is an `OPEN_EXISTING` with an explicit rebuild branch, so a missing directory costs
  a re-bake and nothing else. Note the cache is keyed on the level identity but *not* on
  the map description: changing `max_vertices_per_section` and reloading reuses the old
  sectioning until the `.rif` timestamp changes. That was equally true of the `%TEMP%`
  scheme.
- **The prelude source always ends with a filler `shape`.** A script that defines nothing
  leaves `ParsedObjList` null, `LoadGLS` prints "confused by earlier errors" and returns
  it, and `LoadLevel` dereferences it. A `shape` needs exactly the `name` + `file` pair
  the map section already requires, so it resolves to the very rif object `ToMap` loads
  next. A level with **no** `includes` skips the parser entirely and gets an empty
  `ParsedObjectList` built the way `ParseGSH` builds its own - letting the parse fail
  instead is not an option, since that poisons every later parse in the process.
- **Which level is loading is read off `ScriptFileName`, not remembered from the menu.**
  Every entry point writes that global before `LoadLevel` runs, so the test covers Choose
  Level, `ADD MISSION`, a savegame restore and a multiplayer client alike.
- **The `.gcs` replacement hooks `ExecuteAllCommands` @ 0x004d62c0, not `LoadLevel`.**
  That function has exactly one reference in the whole binary - the `CALL` at 0x004e1e00,
  step 11 - and it sits behind `CMP byte [EBP-0x175],0` / `JZ`, the same `freshStart` byte
  that gates the step-7 `ExecuteCommandFile(ConsoleFileName)`. So the hook needs no flag
  of its own and no way to tell `LoadLevel`'s two callers apart: being there *is* "a fresh
  level start has reached the point its `.gcs` takes effect", and `LoadGame`'s restore
  skips it exactly as it skips the real `.gcs`.

  The callback runs **before** the original, not after. `ExecuteAllCommands` is a loop -
  `while (NumCommandsToExecute) PumpQueuedConsoleCommand()` (0x004d62d0) - so a callback
  that *queues* commands (`ExecuteCommandFile` only appends) has them drained by this same
  call, which is what the level's own `.gcs` lines would have got. Running it afterwards
  would leave them to the once-per-frame pump instead.

### 6.5.1 Measured in a running game

Both paths were exercised end to end, in-game, with the REPL reading the state back:

| | |
|---|---|
| a level naming six `.gsh` `includes` | **140 roles** registered before `define`, from an in-memory prelude; nothing on disk |
| its `populate` | 6 actors spawned, and the `as:` clauses created the tokens the `.gcs` half names them by |
| its `setup` | fog 0.67 and sun 140 applied, i.e. the `ExecuteAllCommands` hook lands where the `.gcs` would |
| a level with **no** `includes` | 7 actors from one `make.role`-built role; `LoadGLS` never ran |
| the virtual `ScriptFileName` | `gkplus\Test_Level_Parsed.gls` matched on load; no file of that name exists |

Two traps that cost a run each, both about *names* rather than mechanism:

- **A script spawns a role by its GLS `identifier`, never by the section symbol.** `Rol_GunLok` is
  the symbol; the roles hash is keyed on the `identifier` field, so the name that works is `GUNLOK`
  (`GetRoleByName` @ 0x004ae030 compares with `__mbsicmp`, so case does not matter - the spelling
  does). `examples/levels/arena.mjs` had all three of `Rol_GunLok`/`Rol_Elint`/`Rol_Archore`, none of
  which can ever resolve, which is how this went unnoticed: nothing had run it.
- **`actor.name` is a reverse token lookup and it lies when values collide.** After a load, actor 0
  reported `'RES'`, not `'gunlok'` - `FindTokenWithValue(0.0)` finds the level's own `RES=0` token
  before the `gunlok=0` one that `as:` created. Any numeric token sharing a value with an actor id
  shadows that actor's name.

**Open, and not chased:** entries from a previous load appear to survive in the roles hash. Two
consecutive loads of the same script-defined level left four entries - `testbug`, `null`, `testbug`,
`null`, ids restarting at 0 because `LoadLevel` step 4 resets `next_entity_id` - where
`DestroyRoles` should have left one. A controlled single-load sequence enumerates exactly, so the
collection itself is fine; what is unclear is whether the stale entries are freed-and-recycled
`Role*`s the hash still holds.

Placed objects go through `gk::MapSpawn` after `ToMap`, not through the binding hash at
`ParsedMap+0x1b60`: the hash needs a forged field-9 `ParsedField`, and `MapSpawn` already
exists and accepts arbitrary coordinates. `gk::LevelRifLocators` supplies the `for "<rif
object>"` half by replaying `AcquireLevelRifForLocators` (ECX = the rif path, nothing in
EDX) -> `RifFilterObjectsByName` -> `MapToWorld`.

**Ordering constraint.** `next_entity_id` and `NextInventoryItemId` are reset in step 4
of `LoadLevel`, well before `ConvertParsedObjects`. Any native path that spawns actors
must run after that reset and before step 11 (`ExecuteAllCommands`, which runs the
`.gcs`), or tokens the trigger scripts expect will not exist yet.

## 7.5 Starting a level with no menus (`src/Session.cpp`)

The front-end route to a running level is a four-screen state machine, and none of it is a
single call:

| Screen | What its dispatch actually does |
|---|---|
| 7 SinglePlayer item 0 / 5 ChooseLevel item *n* | frees and re-`strdup`s `ScriptFileName` + `ConsoleFileName` from a `LevelList` entry, `ResetMissionStats(true)`, then `GoToMenu(9)` |
| 9 NewSinglePlayerGame items 0/1 | `GameDifficulty = item + 1`, `EnterSinglePlayerMode()`, `LeaveFrontEndScreen()`, `ShowBriefingOrDebriefScreen(true)` |
| the briefing | `BeginLevelSession(true)` - i.e. `GameState = 0x12` then `LoadLevel(freshStart = true)` |

**The shortcut is the game's own.** `LoadGame` @ 0x00505730, restoring a "carry to the next
level" header-only save, does `EndLevelSession(); BeginLevelSession(true);` with no menu and
no briefing at all. `StartLevel` reproduces that, having first set the state the two menu
screens would have set:

```
ResetMissionStats(true)                 // 0x004fcc30, __fastcall(char) in ECX
ClearTeamCarryOverState(&TeamCarryOverState)  // 0x004da230, __thiscall - see below
FreeAuxTeamCarryOverStates()            // 0x004dafd0
ScriptFileName / ConsoleFileName        // pool_free the old, game strdup the new
EnterSinglePlayerMode()                 // 0x004f94c0 - GameMode = SinglePlayer
GameDifficulty = 0..3
LevelLoadReason = 0                     // NOT 3; a previous LoadGame leaves it at 3
LeaveFrontEndScreen()                   // 0x004e8dd0 - ONLY when the front end is up
EndLevelSession()                       // 0x004e2710 - free when nothing is running
BeginLevelSession(true)                 // 0x004e2560
```

Three things this rests on, two of them found by crashing the game:

- **`LeaveFrontEndScreen` is once per front-end session, not idempotent.** Its first two
  branches are null-guarded, which is exactly as far as a quick read gets you; past the
  video-settings block it releases a reference on ~40 menu sprites with no guard at all
  (`MOV ECX,[SpriteScrollUp]; ADD ECX,0x9c; ADD [ECX+4],-1`) and zeroes each global on the
  way out. A second call faults at 0x004e8eb7, which is what starting a level from *inside*
  a level did. `SpriteScrollUp` @ 0x007b7d0c is the predicate: `EnterMainMenuScreen`
  @ 0x004e82be creates it and this function zeroes it, and they are its only two writers.
- **`ClearTeamCarryOverState` @ 0x004da230 is `__thiscall`.** `MOV EBX,ECX` is its second
  instruction and `[EBX+4]` its third; `LoadGame` does `MOV ECX,0x7b6d70` immediately
  before the call. Declared with no argument it faults at 0x004da242. `RET`-form checking
  cannot see this - a missing *register* argument pops exactly as many bytes as a correct
  call - which is the argument for the companion ECX/EDX-read check in
  `console_command_notes.md` §6.5.1. Run on these ten functions it flags this one and clears
  the other nine.
- **`EndLevelSession` and its coop twin are guarded by `LevelSessionStarted` @ 0x007b6dd8**
  and return early when no session was ever begun. That is what lets one code path serve
  both "start from the menus" and "switch levels mid-game".

**Where it may run.** `LoadLevel` must not run inside the renderer, and the script host's
frame callback is driven from inside `HookedPresentScene` whenever a level is up - so the
request is queued and drained from a private `WM_APP` message handled in `HookedWndProc`
(`SetMessageLoopCallback` in `src/GUI.h`). That is the same point in the message loop the
game reaches `OnMenuItemClicked` from. Everything wrong with the *request* is still reported
synchronously at the call; only the load is deferred.

Verified in a running game, one process throughout: a cold start from the menu, a switch
between two levels while one was running, `levels.quit()` back to the menu, a start after
that quit, a shipped `level01.gls`/`level01.gcs` (156 actors, 259 roles), `ADD MISSION` plus
start-by-title (`prison.gls`, 187 actors), and all five refusal paths. No WER record.

## 8. Address summary

| Offset | Name |
|--------|------|
| 0x004e2560 | `BeginLevelSession(bool doLoad)` |
| 0x004e0980 | `LoadLevel(bool freshStart)` - `__fastcall`, CL; see §2.2 |
| 0x004efcc0 | `AddLevel(title, scriptFile, consoleFile)` - list + menu 5 item |
| 0x004f7650 | `LevelList__GetTitlePtrAt` |
| 0x00474540 | `LoadGLS(file, mode)` - null on a script that defined nothing |
| 0x004747b0 | `ConvertParsedObjects(list)` |
| 0x00474870 | `FreeParsedObjectList(list)` - pool-frees the header too |
| 0x0043f250 | `ExecuteCommandFile(path)` - **queues** the `.gcs`, step 7; does not run it |
| 0x004d62c0 | `ExecuteAllCommands()` - drains the queue, step 11. One call site: 0x004e1e00 |
| 0x004d6120 | `PumpQueuedConsoleCommand()` - the one-per-frame pop the drain loops on |
| 0x00483420 | `List__Dtor` - empties a list and destroys its sentinel, keeps the header |
| 0x0047f160 | `ToMap` (ParsedMap `toGameObject`) |
| 0x0047efa0 | `CheckValue_Map` (the `use ... for ...` handler) |
| 0x0047eb20 | `CreatePlacedActorEntry(out, name, team, role, token)` |
| 0x0047ec40 | `PlacedObjectBinding_CopyCtor` |
| 0x0047ec00 | `PlacedObjectBinding_Dtor` |
| 0x00483f20 | `PlacedObjectBindingMap_Remove` |
| 0x00481cc0 | `PlacedObjectBindingMap_Ctor(this, log2Buckets)` |
| 0x00470f20 | `Map_Ctor(this, scene, origin, mesh)` |
| 0x004722d0 | `Map_PostConstruct` |
| 0x0059b4c0 | `SetMaxVerticesPerSection` |
| 0x00599f80 | `CreateSceneObjectFromRifObject` |
| 0x0059da90 | `CreateSceneObjectFromCachedMesh` |
| 0x004ae960 | `LoadOrGetRifFile` |
| 0x005aa5c0 | `RifFindObjectByName(rif, name, &origin)` |
| 0x005aaac0 | `RifFilterObjectsByName(list, rif, name)` |
| 0x00483da0 | `AcquireLevelRifForLocators(rifPath)` |
| 0x005a9b40 | `CopyDword(dest, src)` - `__fastcall` 4-byte copy returning dest. **Not** a world-unit-scale getter |
| 0x0044fef0 | `LoadOrBuildSectionAdjacency` (`.map` sidecar) |
| 0x005035b0 | `ServerSpawnActorForTeam(team, role, pos, ori)` |
| 0x004fce90 | `ClientSpawnActorForTeam(team, role, pos, ori)` |
| 0x004e2910 | `ShowLoadingMessage(const char*)` |
| 0x004e2c20 | loading-bar progress (percent) |
| 0x005af270 | `HuffmanDecompress` |

| Offset | Type | Name |
|--------|------|------|
| 0x00739090 | `Map*` (0x18c) | `TheMap` |
| 0x00739098 | list | `MapAuxObjectList` |
| 0x0073909c | int | `MapAuxObjectCount` |
| 0x007b3ec0 | int | `NumTeamSlots` |
| 0x007b3ec4 | `TeamSlot*` (stride 0xc4) | `TeamSlots` |
| 0x007b4e18 | 0x18 | `InitialCameraState` |
| 0x007b3ea8 | float | `MaxCameraFocusHeight` |
| 0x006a574c | float | `MinCameraFocusHeight` |
| 0x007b74dc | `LevelList` (0x10) | `LevelList` - the single-player campaign |
| 0x007b76b0 | `LevelList` (0x10) | `MultiplayerLevelList` |
| 0x006b0173 | byte | `FlagChooseLevel` - from `-chooselevel`, read once by SetupMenus |
| 0x00663c2c | vtable | `List_Member_Base_vftable` - the sentinel vtable list ctors install |
| 0x007b6dcc | `char*` | `ScriptFileName` (`.gls`) |
| 0x007b6dd0 | `char*` | `ConsoleFileName` (`.gcs`) |
| 0x007b9cf0 | int | `LevelLoadReason` (3 = full savegame restore) |
| 0x007b68e4 | int | client actor id counter |
