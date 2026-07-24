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
  +- if (doLoad) LoadLevel()             @ 0x004e0980
```

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
        if (multiplayer) { BattleNumber = 3; ExecuteCommandFile(ConsoleFileName) }
        SetCurrentDirectory()
        ConvertParsedObjects(list)            // <<< ToMap runs here, see section 4
        FreeParsedObjectList(list)
 8. world bounds -> 0x007f598c.. ; ShadowQuality==3 && LevelLoadReason!=3 -> shadow bake
 9. GL_LOADING_TEXTURES     : texture upload; free BriefingRIM
10. camera defaults (dist 20, roll 341 deg), then snap to the first team-1 actor
11. clocks resynced to the executor; music stopped; ExecuteAllCommands() (the .gcs)
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
pos = rif_locator_pos * GetWorldUnitScale() - origin
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
            pos *= *GetWorldUnitScale();                 // 0x005a9b40
            pos += TheMap->neg_origin;                   // 0x11c, NEGATED origin
            Vec4f quat = { O[0x50], O[0x54], O[0x58], O[0x5c] };

            int id = -1;
            if (IsExecutorRunning())     id = ServerSpawnActorForTeam(team, role, &pos, &quat);
            if (IsClientRoutingActive()) id = ClientSpawnActorForTeam(team, role, &pos, &quat);
            if (binding->token_name)     CreateToken(&Tokens, binding->token_name, (float)id);
        }
    }
    PlacedObjectBindingMap_Remove(binding);   // consumed, one shot
}
```

Notes:

- Locator coordinates in the rif are **integers** (`FILD`, signed), converted to float
  and scaled by `*GetWorldUnitScale()`, then offset by the map origin.
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

## 7. Replacing the `.gls` path

The design already has the seams. In order of increasing ambition:

**(a) Keep the `.gls`, override the bindings.** The binding map is a plain hash on the
`ParsedMap` and the consumption loop is ~40 lines. Hooking `ToMap` and editing
`ParsedMap+0x1b60` before it runs gives full control over what spawns where, with zero
geometry work. `gk::gls` already knows how to build `ParsedRole`s programmatically.

**(b) Keep the `.rif`, drop the `map` section.** Reimplement the consumption loop in the
mod: `LoadOrGetRifFile` -> `RifFilterObjectsByName` -> read `O[0x44..0x5c]` ->
`ServerSpawnActorForTeam` / `ClientSpawnActorForTeam` -> `CreateToken`. This maps directly
onto the native API (`gk::MapSpawn` + `gk::CreateToken`); everything needed is a named
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

**Ordering constraint.** `next_entity_id` and `NextInventoryItemId` are reset in step 4
of `LoadLevel`, well before `ConvertParsedObjects`. Any native path that spawns actors
must run after that reset and before step 11 (`ExecuteAllCommands`, which runs the
`.gcs`), or tokens the trigger scripts expect will not exist yet.

## 8. Address summary

| Offset | Name |
|--------|------|
| 0x004e2560 | `BeginLevelSession(bool doLoad)` |
| 0x004e0980 | `LoadLevel()` |
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
| 0x005a9b40 | `GetWorldUnitScale` -> `float*` |
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
| 0x007b6dcc | `char*` | `ScriptFileName` (`.gls`) |
| 0x007b6dd0 | `char*` | `ConsoleFileName` (`.gcs`) |
| 0x007b9cf0 | int | `LevelLoadReason` (3 = full savegame restore) |
| 0x007b68e4 | int | client actor id counter |
