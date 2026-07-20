# Gunlok Save System

Reverse-engineered from `SaveGame` @ `0x00507a80` and `LoadGame` @ `0x00505730`. The two functions
are exact mirrors of each other; everything below was cross-checked against both sides.

## Overview

Gunlok savegames are **flat, unversioned, uncompressed binary dumps** written with raw `WriteFile`
calls. There is no magic number, no header struct, no checksum and no seeking — the file is a
strict append-only stream and the loader consumes it in exactly the same order.

Two file kinds share one format:

| Extension | Used when | Chosen by |
|-----------|-----------|-----------|
| `.sav` | single player | `MenuSaveGame` / `MenuLoadGame` |
| `.msv` | `GameMode == Cooperative` | same |

Files live in the game directory (every entry point calls `SetCurrentDirectory` first), and the
save "slot" name is just the file's basename — the load menu is built by globbing `*.sav` and
stripping the extension.

There are **no autosaves and no checkpoints**. The only writers are the save menu and the
level-transition path.

### Two flavours of save

`SaveGame(LPCSTR path /*ECX*/, bool full /*DL*/)` is `__fastcall` and behaves differently
depending on `full`:

- **`full = true`** — a real savegame. Called from `MenuSaveGame` @ `0x004e6d30`.
  Writes the header and then the entire world state.
- **`full = false`** — a *header-only* save. Called from `CommandNextLevel` @ `0x004f6ba0`.
  Writes the header, sets the header's `headerOnly` field to 1, closes the file and returns.
  This is how the game carries the squad's health and inventory from one level to the next.

`LoadGame` branches on that same field: if `headerOnly` is set it restores the settings block and
carry-over roster, then jumps straight to the briefing screen for the new level instead of
rebuilding a world.

> Note: `SaveGame` returns a `char` success flag (`MenuSaveGame` tests it to pick between
> `GL_TEXT_GAME_SAVED` and `GL_ERROR_SAVE_FAILED`), but on most of its failure paths it simply
> `CloseHandle`s and falls out — a truncated save is left on disk rather than deleted.

### Versioning

There is no version number. The closest thing is the settings block: the loader compares its
stored **size** against the hard-coded `0x98` and prints `GL_ERROR_DIFFERENT_SAVE_VERSION` on a
mismatch — then carries on and reads that many bytes anyway. Any other layout change between
builds silently corrupts the load.

## Entry points

| Offset | Name | Role |
|--------|------|------|
| `0x00507a80` | `SaveGame` | `__fastcall(path /*ECX*/, bool full /*DL*/)` — the writer |
| `0x00505730` | `LoadGame` | `__fastcall(path /*ECX*/)` — the reader |
| `0x005055e0` | `PeekSaveGameScriptName` | Reads only the first string (the `.gls` level script) to find out which level a save belongs to |
| `0x004e6d30` | `MenuSaveGame` | Builds `<slot>.sav`/`.msv`, calls `SaveGame(path, true)` |
| `0x004e6be0` | `MenuLoadGame` | Builds the name, checks existence, shows the loading screen, calls `PeekSaveGameScriptName` |
| `0x004fba00` | `SetupLoadGameMenu` | Populates `Menus[8]` by globbing `*.sav`; falls back to `GL_TEXT_EMPTYSAVESLOT` |
| `0x004e6e10` | `EnsureFileExtension` | `(char *buf /*ECX*/, const char *ext /*EDX*/, int bufSize)` — appends the extension unless already present |
| `0x004ecf10` | `OnMenuItemClicked` | Dispatches the save/load menu items |

The filename is assembled into `SaveFileNameBuf` @ `0x007b6ef0`, a **0x29-byte** buffer.

> **`SAVE` and `LOAD` in the console are not this system.** `CommandSaveDemo` @ `0x00446b40` and
> `CommandLoadDemo` @ `0x00446cf0` save and replay *input demos* (alongside `RECORD` / `PLAYBACK`).
> Their format is: `u32 len + ScriptFileName`, `u32 len + ConsoleFileName`, `u32 frameCount`,
> `frameCount * 0x18` bytes of input records, `frameCount * 4` bytes of a parallel table.

## Related globals

| Offset | Name | Purpose |
|--------|------|---------|
| `0x007b6ef0` | `SaveFileNameBuf` | 0x29-byte current save/load path |
| `0x007b6d70` | `TeamCarryOverState` | Per-role squad snapshot carried across levels |
| `0x007b6d68` | `TeamCarryOverStateAux1` | Optional second roster (pointer, may be NULL) |
| `0x007b6d64` | `TeamCarryOverStateAux2` | Optional third roster (pointer, may be NULL) |
| `0x007b9c88` | `SaveSettingsBlock` | 0x98-byte settings/cheats block; its size is the de-facto version |
| `0x007b6e48` | `NextInventoryItemId` | Running inventory-item id counter |
| `0x007b9cf0` | `LevelLoadReason` | Set to 3 by `LoadGame` so `LoadLevel` skips carry-over reapply |
| `0x007b9d24` | `GlowNodeNameList` | Model dummy-node names that get glow effects attached |
| `0x007b9d28` | `NumGlowNodeNames` | Count for the above |
| `0x007b9e88` | `WallEffectList` | Textured wall/curtain effects; kinds `0x0b` and `0x0c` persist |
| `0x007b9ebc` | `WorldEffectList` | Electricity / light cylinders / laser fences; kind `5` persists |
| `0x006b0144` | *(level object)* | `+0x1c` fog-of-war grid, `+0xa0` grid dimension |

## File layout

Notation: `u32`/`f32` are little-endian 4-byte; `str` means `u32 length` immediately followed by
that many bytes (**length includes the NUL terminator**, since it comes from `strlen + 1`-style
allocation on the read side — the loader `malloc`s exactly `length` and reads into it).

### Header — always present

| # | Field | Type | Notes |
|---|-------|------|-------|
| 1 | `ScriptFileName` | `str` | The level's `.gls` script |
| 2 | `ConsoleFileName` | `str` | The matching `.gcs` console script |
| 3 | `gameTime` | `f32` | Per-thread game clock in seconds, from `GetGameTimeSeconds` @ `0x00571b10` |
| 4 | `headerOnly` | `u32` | `!full` — 1 for a level-transition save, 0 for a real savegame |
| 5 | `TeamCarryOverState` | *roster* | See below; always written |
| 6 | `hasAux1` | `u8` | 1 if `TeamCarryOverStateAux1 != NULL` |
| 6a | *aux1 roster* | *roster* | Only if `hasAux1` |
| 7 | `hasAux2` | `u8` | 1 if `TeamCarryOverStateAux2 != NULL` |
| 7a | *aux2 roster* | *roster* | Only if `hasAux2` |
| 8 | `settingsSize` | `u32` | Always `0x98`; version check |
| 9 | `settings` | `u8[settingsSize]` | Verbatim copy of `SaveSettingsBlock` @ `0x007b9c88` — difficulty, camera limits, cheat flags |
| 10 | `ambientLight` | `LightInfo` (0x18) | From `GetAmbientLight` @ `0x00579fb0`; type `2`, RGBA at `+4..+0x10` |

**If `headerOnly != 0` the file ends here.**

Otherwise `LoadGame` now sets `LevelLoadReason = 3` and calls `LoadLevel`, which spawns the level
fresh *without* applying the carry-over roster (that state is about to be overwritten by the
per-actor blobs). Everything below is read after the level exists.

### Roster block (`TeamCarryOverState`)

Written by `WriteTeamCarryOverState` @ `0x004dad40`, read by `ReadTeamCarryOverState` @ `0x004da980`.

```
u32  entryCount
entryCount x {
    str  roleName          // key: Role::name
    f32  unknown           // written as 0 by CaptureTeamCarryOverState; ctor default is -1.0
    f32  shield            // Actor::GetShieldValue()
    f32  strength          // Actor::strength
    u32  itemCount
    itemCount x {
        u32  field0        // item +0x18
        u32  field4        // item +0x28
        u32  slot          // owner's equip-slot index, or -1
        u32  field8        // item +0x2c
        u32  ammoTypeOrM1  // -ammoType if the item is ammo, else -1
        str  roleName      // present ONLY when ammoTypeOrM1 == -1
    }
}
```

Note the on-disk field order (`field0, field4, slot, field8, ammoTypeOrM1`) does not match the
in-memory order — both sides agree, but a naive struct dump will be wrong.

This roster is what actually survives a level change: `CaptureTeamCarryOverState` @ `0x004da350`
snapshots every actor whose team is carry-over-eligible (`DAT_006a67b8[team_id]`), and
`ApplyTeamCarryOverState` @ `0x004da4a0` re-applies it after the next `LoadLevel`.

### World state — only when `headerOnly == 0`

#### Actors

Two independent actor lists are stored, matching the game's two-thread design (see
`threading_model_notes.md`): the executor/"server" list (`actors`) and the client-side list
(`DAT_007b68f0`, whose objects are `Unit`s built by `0x004b4620`, not `Actor`s).

```
u32  num_actors            // the global next-actor-id counter
u32  serverActorCount
serverActorCount x {
    u32  size              // actor->vtbl->GetSize()
    u32  kind              // actor->vtbl->IsProjectile()  (slot 38) as 0/1
    u8[size]  blob         // the raw actor object, memcpy'd
    u32  roleId            // actor->role->id
    u32  actorId           // actor->id
    ...fixups...           // variable length, see below
}
u32  clientActorCount      // filtered: entries with actor[+0x24] != 0 are skipped
clientActorCount x { ...same, with the client-side fixup writer... }
```

`kind` is **not** an actor-class tag. `SaveGame` computes it at 0x00507e2f as a plain boolean:

```
MOV EAX,[EAX + 0x98]   ; ActorVtbl slot 38 = IsProjectile
CALL EAX
XOR ECX,ECX
TEST AL,AL
SETNZ CL               ; kind = IsProjectile() ? 1 : 0
```

so it distinguishes exactly one thing: *can this actor be rebuilt by the role factory, or not?*

On load the two branches are:

- **`kind == 0`** -> `ServerSpawnActorForTeam(team, role, coords, ori)` @ 0x005035b0, which calls
  `CreateActor` @ 0x00510760 and dispatches on `role->ai` to construct the **correct subclass**. A
  saved `TurretActor` comes back as a `TurretActor`, a `CharacterActor` as a `CharacterActor`, and
  so on. Nothing about this path is player-character-specific.
- **`kind == 1`** -> `malloc(0x178)` + `ProjectileActor::Ctor` @ 0x00542410 directly. Projectiles
  need the special case precisely because they are *not* produced by the `role->ai` factory - they
  are spawned by the weapon tick, the turret firing solution and `SpawnProjectileActor`.
- any other value aborts the load.

The `GetSize()` check against the stored `size` then validates that the reconstruction picked the
right class, and the id is forced back to `actorId` before the blob is applied over the object.

In Cooperative mode the team/player index is remapped for the five named player roles (`GUNLOK`,
`ELINT`, `HARK`, `FREND`, `MASKELYN`) before the spawn - and note `ServerSpawnActorForTeam`
performs the same five-way remap internally (0x0050360d..0x005036a1), so it happens twice on this
path.

> An earlier revision of this file described `kind` as "0 = player character, 1 = UnknownActor" and
> named the kind-0 constructor `CreatePlayerCharacterCoop`. Both were wrong: no such symbol exists
> in the binary, and reading kind 0 as "player character" made the format look impossible, since a
> 0x320 `TurretActor` could not have survived a 0x178 reconstruction plus the `GetSize` check. The
> contradiction was an artifact of the mislabelling, not of the format.

**The record does not end at `actorId`.** Each actor is followed by a variable-length *fixup*
section written by `WriteActorFixups` @ `0x00531cf0` and consumed by `ReadActorFixups` @
`0x00530900` (server side; `0x004b9c90` / `0x004b8d90` are the client-side pair). Its length
depends on the actor's dynamic type — the writer gates optional blocks on the virtual predicates
at vtable slots `0x94`, `0x98`, `0x9c` and `0xb4` — so the section **cannot be skipped without
interpreting it**.

### How the format survives pointers (and ASLR)

`gl.exe` is linked with `DYNAMICBASE` (`DllCharacteristics = 0x8140`) and ships a live `.reloc`
section (4884 relocations, `RELOCS_STRIPPED` clear), so Windows rebases the image on every system
boot. Absolute addresses are therefore **not** stable across boots — which is exactly why GkPlus
derives its base from `actualEntryPoint - 0x005e50c8` instead of assuming `0x00400000`.

The save format copes in three ways:

1. **Pointers are re-keyed to ids on the way out.** `WriteActorFixups` never writes a raw pointer:
   roles and entities go to disk as `id + 1` with `0` reserved for NULL, and strings as
   length-prefixed text with `-1` for NULL. The loader turns them back into pointers via
   `GetRoleById` / `GetActorById` against the freshly loaded level.
2. **The reader is a field-by-field copy, not a `memcpy`.** `ReadActorFixups` copies roughly 35
   individually named offsets out of the scratch blob and **never touches offset 0**. The saved
   vtable pointer is read into the scratch buffer and then silently dropped; the live vtable is
   whatever the constructor installed a moment earlier. The vtable is in the file, but nothing
   ever consumes it.
3. **Triggers never store one at all.** The 0x68-byte trigger record is a `TriggerData`, whose
   offset 0 is `int trigger_kind` — the vtable lives on the separate `TriggerBase` list node,
   which `TriggerInsertAlternate` @ `0x0050be10` allocates fresh and points at the current
   image's `TriggerBaseVtbl` / `TriggerVtbl2`.

So the stale-vtable hazard is real in the file but inert in practice. Note the failure mode it
*would* have had: Windows randomizes an image's base once per boot and reuses it for every
process launched from that image, so a bad vtable would have survived any number of quit-and-
reload cycles and only broken after a machine reboot.

#### Inventory

```
u32  actorsWithInventory
u32  NextInventoryItemId
actorsWithInventory x {
    u32  actorId
    u32  itemCount         // NOTE: written as (list count - 1); the last node is skipped
    itemCount x u8[0x38]   // raw inventory-item records
}
```

Each 0x38-byte record is patched up after reading: field `+0x1c` is a role id (or a negative ammo
id), and the loader re-resolves the item's hierarchy, shape and description pointers from
`Role::inventory_info` — falling back to the role's own `hierarchy`/`shape` — rather than trusting
the saved pointers. Negative ids index `AmmoInfos` instead.

#### Scripting and world objects

```
u32  numCommands           // NumCommandsToExecute
numCommands x str          // pending console command lines

u32  numTokens             // Tokens.token_count
numTokens x { str name; f32 value }

u32  numGlowNodes          // NumGlowNodeNames
numGlowNodes x str         // model dummy-node names

u32  numTriggers           // NumTriggers
numTriggers x {
    u8[0x68]  blob         // raw Trigger record
    str       script       // ONLY if blob[+0x54] != 0 (the trigger's script/Lua-ref field)
    u32       numNames
    numNames x str         // the trigger's actor-name list (field +0x44)
}
```

Tokens and pending commands are rebuilt through the normal `CreateToken` / `TriggerList::CreateTrigger`
APIs, so the linked lists are reconstructed rather than restored byte-for-byte.

#### Doors

Guarded by two readiness checks (`0x0056f450` / `0x0056e920` on save, `0x0056eca0` / `0x0056e840`
on load) — **if the door subsystem is not ready the whole remainder of the file is omitted**, and
the loader's matching check means it also stops reading. This is a real format fork, not just an
early-out.

```
u32  numDoors              // NumDoors
numDoors x {
    u32  doorId
    u32  triggerCount      // door +0x04
    triggerCount x u32     // associated trigger ids
    u32  isClosed          // door +0x10, widened from a byte
}
```

On load each door id is mapped through the level's door table before use, and `OpenDoor` /
`CloseDoor` is called — but only when client routing is inactive; otherwise the state change is
broadcast to players as an 8-byte message instead.

#### Camera

```
f32  yaw
f32  roll
f32  pitch
f32[3]  position           // CameraCoords
f32  distance              // CameraDistance2 (also assigned to CameraDistance1)
```

#### Fog of war

```
u32  gridDim               // level +0xa0
u8[gridDim * gridDim]  exploredGrid
```

If `gridDim` does not match the freshly loaded level, the loader still reads the bytes but
**discards them** (allocates a scratch buffer, frees it) — the map stays unexplored rather than
failing the load. On a match it also resets the grid's dirty rect to the full extent.

#### World effects

```
u32  wallCount0B           // WallEffectList entries of kind 0x0b
wallCount0B x {
    f32     radius
    u32     unknown        // read but discarded on load
    f32[3]  pointA
    f32[3]  pointB
    str     textureName
    u8      flagA
    u8      flagB
}

u32  wallCount0C           // same layout, kind 0x0c
wallCount0C x { ...same... }

u32  laserFenceCount       // WorldEffectList entries of kind 5
laserFenceCount x { f32[3] pointA; f32[3] pointB }

u8   visionConesEnabled
```

Note the two wall blocks write `pointA`/`pointB` in **opposite orders** (kind `0x0b` writes the
`local_3c` pair first, kind `0x0c` writes the `local_2c` pair first), and the loader mirrors that
— so the two blocks are not interchangeable despite having identical field types.

`visionConesEnabled` is the final byte of the file; the loader treats reaching it as the success
condition and only then calls `ResumeExecutor` on the happy path.

## Behavioural notes

- **The executor thread is suspended** for the duration of a load (`SuspendExecutor` /
  `ResumeExecutor` bracket the whole function) and the loopback message queues (`UpdateQueue`,
  `ScriptQueue`) are flushed before the world is handed back. In Cooperative mode the executor is
  resumed earlier, right after the actor lists are rebuilt.
- **Error handling is uniformly silent.** Every `ReadFile` is checked for both success and exact
  byte count, but every failure path does the same thing: `CloseHandle`, `ResumeExecutor`, return.
  The player gets no message and is left in a half-loaded world.
- **Multiplayer saves are client-authoritative.** In Cooperative mode the loader replays door state
  changes as broadcast messages rather than applying them locally, so peers converge through the
  normal update path.
- **Raw object blobs make saves build-specific.** Actor records are `memcpy`s of live C++ objects,
  so field offsets are baked into the file; the `GetSize()` equality check is the only guard, and
  a save from a differently-compiled binary is rejected at best and misinterpreted at worst.
  Cross-*run* safety is handled separately, by the id re-keying described above.
- **The blob leaks live addresses.** Because the writer dumps the whole object, saved files
  contain the run's vtable pointers and other heap addresses even though the loader ignores them.
  Harmless to the game, but it means a `.sav` discloses the process's ASLR base for the boot
  session in which it was written.
