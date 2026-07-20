# Gunlok DirectPlay / Multiplayer Wire Protocol - Reverse Engineering Notes

## Overview

Gunlok's multiplayer runs over **DirectPlay** (`IDirectPlay4A`). The game is architecturally a
client/server simulation (see `threading_model_notes.md`): the **host** runs the authoritative
*executor* ("server") thread; every node (host and joiners) also runs a *client* on its main
thread. The two sides exchange **fixed-format binary messages**, each a raw DirectPlay
player-to-player payload whose first dword is an application **message id**.

The single-player game uses the *same message code paths* through in-process loopback queues
(`ServerDPlay`/`ClientDPlay` are `NULL`); this document covers the **networked** path, i.e. what
actually goes on the wire.

Two logical message streams share one framing but use **separate, direction-specific id spaces**:

| Stream | Sender | Receiver | Transport | Dispatcher | id range |
|--------|--------|----------|-----------|------------|----------|
| **Commands** | client (`SendToServer`) | host executor | `SendEx` idTo=1 | `ExecutorThreadProc` switch @ `0x005092a4` | `0x04`-`0x3b` |
| **Updates** | host executor (`BroadcastToPlayers`) | every client | `SendEx` per-player | `ApplyUpdateMessage` switch (jmp @ `0x004fdec4`) | `0x00`,`0x01`,`0x03`,`0x1e`,`0x37`-`0xcb` |

> **The same numeric id means different things in each direction.** e.g. `0x1e`, `0x3b`, `0x3d`
> are movement/getter *commands* to the server but position/death *updates* from it. Always
> interpret an id together with its direction.

---

## 1. DirectPlay object model & COM setup

The interface is created lazily by `FUN_005116a0` (called before host/join/enumerate):

```c
CoCreateInstance(CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER,
                 IID_IDirectPlay4A, &MultiplayerActive);
```

| Constant | GUID | Address |
|----------|------|---------|
| `CLSID_DirectPlay` | `{D1EB6D20-8923-11D0-9D97-00A0C90A43CB}` | `0x0066b910` |
| `IID_IDirectPlay4A` (ANSI) | `{0AB1C531-4745-11D1-A7A1-0000F803ABFC}` | `0x0066b920` |
| **Gunlok application GUID** | **`{C1CB8A00-1293-11D2-8A89-0000E8CAE446}`** | `0x00667ad0` |

`MultiplayerActive` (`0x007b9e74`) holds the `IDirectPlay4A*` for the whole multiplayer session
(NULL outside MP; `IsMultiplayerActive` @ `0x005116f0` just tests non-NULL). Teardown
(`FUN_00511700`) `Release`s it and clears the pointer.

### IDirectPlay4A vtable slots used (offset from vtable base)

| Off | Method | Used by |
|-----|--------|---------|
| `+0x04` | AddRef | `StartExecutorThread` (retain server session) |
| `+0x08` | Release | teardown |
| `+0x14` | CreateGroup | `StartExecutorThread` (server broadcast group) |
| `+0x18` | CreatePlayer | server player + local client player |
| `+0x34` | EnumSessions | `FUN_00512160` (session browser) |
| `+0x60` | Open | host / join |
| `+0x64` | Receive | `DPlayReceive` |
| `+0x68` | Send | executor reply (command `0x07`) |
| `+0x8c` | EnumConnections | `FUN_005119f0` (service-provider list) |
| `+0x98` | InitializeConnection | `FUN_005119a0` / `FUN_00511b50` |
| `+0xc4` | **SendEx** | `SendToServer`, `BroadcastToPlayers` |
| `+0xc8` | **GetMessageQueue** | backpressure checks |

*(The 4 IDirectPlay4-only slots GetGroupFlags/GetGroupParent/GetPlayerAccount/GetPlayerFlags sit
at `+0xb4..+0xc0`, which is why SendEx is at `+0xc4`, not `+0xb4`.)*

---

## 2. Service providers & connection addresses

`FUN_005119f0` calls `EnumConnections` to list installed service providers; the game supports the
standard DirectPlay SPs (TCP/IP, IPX, modem, serial). `FUN_00512080` serialises a compound
DirectPlay address (`DPADDRESS` of `{GUID, size, data}` chunks) using these element GUIDs:

| GUID | Address | Meaning |
|------|---------|---------|
| `{07D916C0-E0AF-11CF-9C4E-00A0C905425E}` | `0x0066b7f0` | `DPAID_ServiceProvider` |
| `{1318F560-912C-11D0-9DAA-00A0C90A43CB}` | `0x0066b800` | `DPAID_TotalSize` |
| `{78EC89A0-E0AF-11CF-9C4E-00A0C905425E}` | `0x0066b7d0` | `DPAID_Phone` (modem dial string) |
| `{44EAA760-CB68-11CF-9C4E-00A0C905425E}` | `0x0066b8d0` | modem address element (`DPAID_Modem` family) |
| `{F6DCC200-A2FE-11D0-9C4F-00A0C905425E}` | `0x0066b7b0` | serial/modem address element (`DPAID_ComPort` family) |

*(Last two GUID names are best-effort; the GUIDs themselves are exact. `DPAID_Phone` implies
dial-up modem play is supported.)*

---

## 3. Session lifecycle

The `DPSESSIONDESC2` (0x50 bytes) built for both Open and EnumSessions:

```
+0x00 dwSize            = 0x50
+0x04 dwFlags           = 0xA0C0   ; KEEPALIVE | NODATAMESSAGES | DIRECTPLAYPROTOCOL | 0x2000
+0x08 guidInstance      = {0} (host: assigned; join: copied from browsed session, param_1[0..3])
+0x18 guidApplication   = {C1CB8A00-1293-11D2-8A89-0000E8CAE446}
+0x28 dwMaxPlayers      = 0
+0x2c dwCurrentPlayers  = 0
+0x30 lpszSessionName / dwReserved / dwUser1..4 = 0
```

Note `DPSESSION_CLIENTSERVER` (`0x100`) is **not** set - at the DirectPlay level this is a peer
session; the client/server split is entirely the game's own (the server *player* carries
`DPPLAYER_SERVERPLAYER`).

| Step | Function | Call |
| ------ | ---------- | ------ |
| Browse | `FUN_00512160` | `EnumSessions(desc, 0, cb=FUN_00512b90, 0, 0x91)` — `AVAILABLE\|ASYNC\|RETURNSTATUS` |
| **Host** | `HostMultiplayerSession` @ `0x00512540` | `Open(desc, 0x82)` = `CREATE\|RETURNSTATUS`, retry while `DPERR_CONNECTING`, then `StartExecutorThread(MultiplayerActive)` |
| **Join** | `JoinMultiplayerSession` @ `0x00512370` | `Open(desc, 0x81)` = `JOIN\|RETURNSTATUS`; **does not** start the executor |
| Server player | `StartExecutorThread` @ `0x00502db0` | `CreatePlayer(&id, NULL, hEventMsgAvailable, 0,0, 0x100)` — `0x100`=`DPPLAYER_SERVERPLAYER` -> **DPID 1**; then `CreateGroup(&group,…)` |
| Local client player | `CreateLocalClientPlayer` @ `0x005126b0` | `CreatePlayer(&ClientPlayerId, &dpname, 0,0,0,0)`, then `InitClientRouting` |

**Player IDs:** the server is always **`DPID_SERVERPLAYER` = 1**; each node's own player id is in
`ClientPlayerId` (`0x007b9d64`). Only the **host** runs the executor thread; a joiner runs only its
main thread and reaches the world purely through commands out / updates in.

The server's `CreatePlayer` registers `hEventMsgAvailable` (`0x007b9df4`); DirectPlay signals it
when a message is queued for the server player, waking the executor's
`WaitForMultipleObjects(…, 50ms)`.

---

## 4. Transport layer

### 4.1 Client -> server: `SendToServer` @ `0x004fdbc0`

`__fastcall(void *buf, int size, uint flags)` where `flags` low byte = *guaranteed*.

```c
if (ClientDPlay == NULL) MsgQueue_Push(buf, size);          // single-player loopback
else {
    GetMessageQueue(0,0, DPMESSAGEQUEUE_SEND, &pending, &bytes);  // backpressure
    if (pending < 29 || guaranteed) {
        SendEx(idFrom = ClientPlayerId,
               idTo   = 1,                                   // DPID_SERVERPLAYER
               dwFlags = 0x200 | guaranteed,                // DPSEND_ASYNC [| DPSEND_GUARANTEED]
               lpData = buf, dwDataSize = size,
               dwPriority = guaranteed,
               dwTimeout  = guaranteed ? 0 : 10000,         // 10 s TTL for unreliable
               0, 0);
    }
}
```

Unreliable commands are **dropped when 29+ messages are already queued** to the server.

### 4.2 Server -> clients: `BroadcastToPlayers` @ `0x00504bf0`

`__fastcall(void *buf, int size, byte guaranteed, Vec3f coords)` - runs on the executor thread.

```c
if (ServerDPlay == NULL) MsgQueue_Push(buf, size);         // single-player loopback
else for each player p != server:
    if (coords == sentinel(0,0,0)  ||  p is last  ||  IsPositionRelevant(coords)) {
        backlog = ++PlayerMsgBacklog[p];
        if (backlog < 20-Difficulty  ||  guaranteed)                   send;
        else if (backlog < 25-Difficulty && (rng()>>1)%10 == 1)        send;   // ~1/10 sample
        // else drop (unreliable only)
        SendEx(idFrom = 1, idTo = p_id,
               dwFlags = 0x200 | guaranteed,                            // ASYNC [| GUARANTEED]
               lpData = buf, dwDataSize = size,
               dwPriority = guaranteed,
               dwTimeout  = guaranteed ? 0 : 3000,                      // 3 s TTL for unreliable
               0, 0);
    }
```

Key transport behaviours:

- **`coords`** is the world position used for *spatial relevance culling*. Callers that must reach
  everyone pass `(0,0,0)` (the sentinel `FloatZero` @ `0x007f5f40`, zero in BSS); position updates pass the
  entity's real position so far-away players can be skipped.
- **Reliability** = the `guaranteed` byte -> `DPSEND_GUARANTEED`. Reliable messages ignore the
  backlog throttle.
- **Backpressure/throttle**: per-player `PlayerMsgBacklog` counter; when a player is backed up,
  unreliable messages are dropped except a random ~1-in-10 sample (per-thread RNG). Difficulty
  shifts the thresholds. This is the shipped mechanism behind the game's known MP jitter.

### 4.3 Receiving: `DPlayReceive` @ `0x00502c80`

Wraps `Receive` with `DPRECEIVE_TOPLAYER`: a `PEEK` (flags `0xA`) to size the buffer, `malloc`,
then a real receive (flags `0x2`). Returns `NULL` on `DPERR_NOMESSAGES` (`0x887700BE`), grows the
buffer on `DPERR_BUFFERTOOSMALL` (`0x8877001E`). Used by both:

- executor: `DPlayReceive(ServerDPlay, 1)` in the thread loop, then the command switch;
- client: `ClientReceivePump` @ `0x004fdc70` (once/frame; ≤50 msgs/frame **in MP only** — loopback
  drains `UpdateQueue` until empty), then `ApplyUpdateMessage`.

---

## 5. Message frame format

Every message is a raw DirectPlay payload; there is **no game-level header beyond the id**:

```
offset  size  field
0x00    u32   message id            (switch selector)
0x04    ...   id-specific payload   (little-endian; ints, IEEE-754 floats, Vec3f, Vec4f quats)
```

Common field conventions in payloads:

- **actor id** - `u32`, resolved with `GetActorById` (`0x0044e0b0`).
- **Vec3f** position = 3×`f32` (x,y,z); **Vec4f** orientation = 4×`f32` quaternion (x,y,z,w).
- **time** = `f32` game-clock timestamp (executor clock; see `threading_model_notes.md`).
- Strings are fixed-max, null-terminated, placed at a fixed offset.

Sizes are **fixed per id** and are passed literally to `SendEx`; a receiver reads exactly the
fields its handler expects.

---

## 6. Command messages (client -> server)

Sent with `SendToServer` (idTo=1). Dispatched by the executor switch at `0x005092a4`
(`ExecutorThreadProc`, id = first dword). Handlers typically `GetActorById(arg0)` then invoke a
gameplay method; several re-`BroadcastToPlayers` a resulting update. `size` is the exact wire size.

| id | size | producer | meaning (server action) |
|----|------|----------|--------------------------|
| `0x04` | - | (pre-switch) | player-left / removal acknowledgement (marks session-list entry) |
| `0x06` | - | (pre-switch) | join/ready ack (`DAT_007b9df1` path) |
| `0x07` | - | (in-switch) | ready ping -> server replies via `Send` with a 4-byte `{2}` |
| `0x08` | - | (in-switch) | inventory/pickup spawn batch (iterates vulnerability list) |
| `0x09` | 12 | `FUN_004c0970` | pickup/equip object (`FUN_00539bd0`) |
| `0x0a`/`0x0c` | 24 | `FUN_004c3c90` | movement / move-to |
| `0x0b`/`0x0d` | 16 | `FUN_004c3da0` | enable/disable actor (2 actor ids) |
| `0x0e` | 16 | `FUN_004c3e40` | select secondary weapon |
| `0x0f` | 16 | `FUN_004c0bc0` | `FUN_00536ba0(arg1,arg2,1)` |
| `0x11` | 12 | `FUN_004c0c90` | equip object |
| `0x13` | 12 | `FUN_004c0cf0` | `FUN_00538240(arg1)` |
| `0x15` | 12 | `FUN_004c0dc0` | use/remove inventory item -> re-broadcasts update `0x80` |
| `0x17` | 24 | `FUN_004c0d50` | board / attach (escort) - actor id + carrier + slot |
| `0x19` | 12 | `FUN_004c0c30` | set movement state |
| `0x1a` | - | (in-switch) | set health (`SetHealth` vcall), arg1=`f32` |
| `0x1b` | 12 | `FUN_004bdd60` | `FUN_00538690(arg1)` |
| `0x1c` | 8 | `FUN_004c09d0` | destroy actor |
| `0x1d`/`0x1f` | 24 | `FUN_004c3e90` | query health into buffer |
| `0x1e`/`0x20` | 16 | `FUN_004c3fc0` | query center coords into buffer |
| `0x21` | - | (in-switch) | inventory/pickup spawn batch (variant of `0x08`) |
| `0x23` | 8 | `FUN_004be6f0` | `FUN_00539030` (mobile) |
| `0x24` | 8 | `FUN_004c9d70` | delete actor (`vtbl->Delete`) |
| `0x25`/`0x26` | 8 | `FUN_0046ef50` | team command -> re-broadcasts `0x65` |
| `0x27` | 16 | `FUN_004a4660` | Gunlok super-ability -> broadcasts `0x37`, projectiles |
| `0x28` | 259 | `FUN_004fcd00` | **set player name / chat text** (255-byte string) -> re-broadcast `0x8d`; in lobby (GameState 8) sets local name |
| `0x29` | 16 | `FUN_004fcdf0` | (world command) |
| `0x2a` | 16 | `FUN_004fce30` | score-matrix write (`DAT_007b70e4[arg0*5+arg1]=arg2`) |
| `0x2b` | 24 | `FUN_004c4040` | `FUN_00541170` (special) |
| `0x2c` | 8 | `FUN_004fbf90` | `FUN_004f6b00` (no-arg global) |
| `0x2d` | 8 | `FUN_004fc0f0` | set flag `DAT_007b7144[arg0]=1` |
| `0x2e` | 8 | `FUN_004fc400` | clear flag `DAT_007b7144[arg0]=0` (arg0 in 1..4) |
| `0x2f` | 12 | `FUN_004506c0` | clear team's mine detection |
| `0x30` | 12 | `FUN_00450770` | mine detect/defuse by state |
| `0x32` | 8 | `FUN_0046f590` | countdown control -> `0x9a`/`0x17` |
| `0x34` | - | `CommandStatsScreen` | stats screen (handled outside the switch) |
| `0x35` | 4 | `FUN_00496f70` | -> broadcast `0xa4` |
| `0x36` | 8 | `FUN_00502ab0` | timer sync -> broadcast `0xca` |
| `0x3b` | 56 | `FUN_004ad050` | movement/waypoint order: pos + orientation + waypoint (see §8.7) |

Additional ids consumed only in the pre-switch prologue: `0x2c`,`0x2d`,`0x2e`,`0x2a`,`0x28`
(lobby name), `0x34` (bypasses switch). Countdown/turn ids `0x31`,`0x32`,`0x33` are also handled
here and each re-broadcast a client update (`0x99`,`0x9a`,`0xcb`).

The canonical console-command producer pattern (e.g. `CommandOpenDoor` @ `0x00445f60`):

```c
if (IsExecutorRunning()) {
    BroadcastToPlayers(msg, size, guaranteed, coords);   // enqueue for all
    if (!IsClientRoutingActive()) ApplyDirectly();       // + apply locally when no routing
}
```

---

## 7. Update messages (server -> clients)

Broadcast by the executor via `BroadcastToPlayers`; applied by the client switch `ApplyUpdateMessage`
(valid ids `0..0xcb`; sparse jump via byte index-map `0x005028e8` -> pointer table `0x005026b8`).
**151 ids are handled.** The table below lists every id with a decoded producer, its wire size,
and the producing function (name conveys semantics). `R` = sent reliably (guaranteed) where known.

| id | size | producer(s) | meaning |
|----|------|-------------|---------|
| `0x00` | var | (HUD) | on-screen/objective string (string at payload `+0x10`, GameState==5) |
| `0x01` | var | (HUD) | on-screen/status string (string at payload `+0x10`) |
| `0x03` | 8 | (per-actor) | actor state update (`GetActorById(arg0)` -> `FUN_004fde10`) |
| `0x1e` | 16 | `SyncPositionAndBroadcast` | position/velocity update |
| `0x37` | 20 | (`ApplyDamage`/case 0x27) | health/armor/shield status `{actorId, strength, shield, armor}` |
| `0x39` | 80 | `FUN_00539540` | full actor spawn/state (large) |
| `0x3a` | 24 | `ExecutorThreadProc` | (session/turn-related state) |
| `0x3b` | 56 | `FUN_00539d40` | position + orientation + AI/waypoint update `R` |
| `0x3c` | 56 | `FUN_00539bd0` | position + orientation + AI/waypoint update |
| `0x3d` | 56 | `CommandTeleport`, `CommandTeleportAndOrientate`, `EvaluateTriggers`, `MobileActor::Die` | teleport / full move+orient (also destructible death) |
| `0x3e` | 12 | `FUN_00450f60`, `FUN_0045a640` | (short actor update) |
| `0x44` | 12 | `StopAttacking` | stop attacking |
| `0x45` | 8 | `SyncPositionAndBroadcast` | position keyframe (short) |
| `0x46` | 64 | `FUN_00503bd0` | **spawn projectile / weapon fire** `'F'` (see §8.6) |
| `0x48` | 56 | `MobileActor::Die` | **death** (non-destructible) `R` (see §8.5) |
| `0x49` | 8 | `Delete` | delete actor |
| `0x4d` | 9 | `FUN_005317b0` | (byte-flag update) |
| `0x4f` | 25 | `EquipObject`, `OnPickedUp`, `SyncPositionAndBroadcast` | equip + position |
| `0x50` | 12 | `ChangeOwnerAndTeam`, `CommandGiveControl`, `ReleaseFromOwner`, `SyncPositionAndBroadcast` | owner/team change |
| `0x51` | 16 | `EvaluateTriggers`, `SyncPositionAndBroadcast` | position (trigger-driven) |
| `0x52` | 8 | `EvaluateTriggers` | trigger event |
| `0x53` | 16 | `SyncPositionAndBroadcast` | position |
| `0x54` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x55` | 12 | `SetHealth` | **set health** `{actorId, f32 health}` `R` (see §8.2) |
| `0x56` | 16 | `SetTarget` | set attack target |
| `0x57` | 8 | `ClearTarget` | clear target |
| `0x58` | 20 | `ChangeOwnerAndTeam` | owner+team+extra |
| `0x59` | 8 | `ReleaseFromOwner` | release from owner |
| `0x5a`-`0x5d` | 12 | `FUN_00532d40`/`e80`/`fe0`/`OnFlagCaptured` | CTF/objective state updates; `0x5d` is **flag captured** `{actorId, playerIdx}` `R` |
| `0x5f` | 60 | `FUN_00539ed0` | large actor state |
| `0x60`/`0x61`/`0x63` | 8 | `FUN_00451220` | door/switch state variants |
| `0x62` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x65` | 8 | `ExecutorThreadProc` (case 0x26) | team notification |
| `0x66` | 8 | `FUN_004bdf90`, `FUN_00538930` | actor toggle |
| `0x67` | var | `QueueScriptExecution` | **run script file** `'g'` + filename `R` (see §8.11) |
| `0x68` | 16 | `FUN_00456c50`, `FUN_0045b620` | (world object update) |
| `0x6b` | 12 | `Frag` | frag/score event |
| `0x6e` | 20 | `FUN_0045a850` | (object update) |
| `0x6f` | 40 | `SyncPositionAndBroadcast`, `CommandTeleport` | **position+orientation state** `'o'` (see §8.1) |
| `0x70` | 40 | `CommandTeleportAndOrientate` | teleport + orientation `R` |
| `0x71` | 12 | `SyncPositionAndBroadcast` | position delta |
| `0x72` | 8 | `EquipObject` | unequip |
| `0x74`/`0x75` | 12/8 | `OnPickedUp` | animation/turret state |
| `0x78` | 12 | `FUN_00537db0`, `ReceiveObject` | receive/transfer object |
| `0x7d` | 12 | `CommandRemoveItem`, `EquipObject`, `FUN_00538240` | remove inventory item |
| `0x7e`/`0x7f` | 12/16 | `SyncPositionAndBroadcast` | position variants |
| `0x80` | 12 | `ExecutorThreadProc` (case 0x15), `SyncPositionAndBroadcast` | item removed / position |
| `0x81` | 24 | `FUN_00537db0` | object transfer variant `R` |
| `0x82` | 12 | `ExecuteSpecialAbility` | special ability |
| `0x83` | 12 | `OnMobileDamageReceived` | damage taken notification |
| `0x84` | 8 | `Associate` | associate objects |
| `0x85` | 8 | `FUN_00546240` | (turret/anim) |
| `0x86` | 8 | `FUN_00538700` | actor toggle |
| `0x87` | 4 | `ExecutorThreadProc` | **turn / step advance** (lock-step; see §9) `R` |
| `0x88`/`0x89` | 12/8 | `FUN_00505380` | (session/turn control) |
| `0x8a` | 12 | `InitPositionAndTiming` | initialise position + timing |
| `0x8b` | 16 | `PresidentActor::ApplyDamage` | death/gib effect `{actorId, seq, variant}` |
| `0x8c` | 8 | `OnPickedUp` | (turret state) |
| `0x8d` | 259 | `ExecutorThreadProc` (case 0x28), `EvaluateTriggers` | **chat / name broadcast** (255-byte string) |
| `0x8e` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x91` | 24 | `FUN_005046e0` | ownership/team transfer variant `R` |
| `0x92` | 20 | `FUN_00541170` | special-ability state |
| `0x93`/`0x95` | 12 | `CommandSetActorArmor`, `FUN_004da4a0` | armor/attribute set |
| `0x96` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x97` | 8 | `Dissociate` | dissociate objects |
| `0x98` | 32 | `ApplyTeamCarryOverState` | team carry-over roster entry `R` |
| `0x99` | 12 | `ExecutorThreadProc` (case 0x31) | countdown start `{index, timer}` `R` |
| `0x9a` | 4 | `ExecutorThreadProc` (case 0x32) | countdown tick/stop `R` |
| `0x9b` | 8 | `ApplyDamage`, `OnFlagCaptured`, `SyncPositionAndBroadcast` | deathmatch frag credit `{killerId}` `R` |
| `0x9c` | 10 | `FUN_00511150` | (player/session) |
| `0x9d`/`0x9e` | 20/16 | `EquipObject` | equip variants |
| `0x9f` | 8 | `FUN_004552a0` | (object update) |
| `0xa0` | 6 | `FUN_00508e70` | (compact update) |
| `0xa1` | 5 | `FUN_00508f60` | (compact update) |
| `0xa2` | 35 | `ExecutorThreadProc` | **game rules + scoreboard sync** (bytes: friendly-fire, mines, team scores) - client parse @ `0x005029d0` |
| `0xa3` | 12 | `FUN_00538240` | (actor update) |
| `0xa4` | 4 | `ExecutorThreadProc` (case 0x35) | (flag broadcast) `R` |
| `0xa5` | 8 | `DeleteTeam` | delete team |
| `0xa6` | 12 | `ApplyArmorDamage`, `ApplyShieldDamage` | armor/shield value update |
| `0xa7` | 16 | `ApplyShieldDamage` | shield damage |
| `0xa8` | 16 | `ApplyArmorDamage` | armor damage |
| `0xa9` | 73 | `CommandSetTrack` | set camera track: 4 parsed positions + carries-passengers flag `R` |
| `0xaa` | 56 | `CommandCameraTrack` | camera track path (parsed positions + float) |
| `0xab` | 12 | `CommandSetSpeed` | set track/animation speed |
| `0xac` | 12 | `CommandSetLoopTime` | set loop time |
| `0xaf` | 8 | `FUN_00548760` | (actor state) `R` |
| `0xb0`/`0xb1` | 8 | `FUN_005488f0`/`f0` | (actor state) |
| `0xb2` | 8 | `EvaluateTriggers` | trigger effect |
| `0xb4` | 8 | `CommandTrack` | camera track |
| `0xb5` | 8 | `CommandBoard` | board |
| `0xb6` | 24 | `EvaluateTriggers`, `FUN_0045e050` | trigger spawn/effect |
| `0xb7`/`0xb8` | 8 | `CommandDefogger`/`CommandFogger` | map defog/fog |
| `0xb9` | 12 | `FUN_0052f2d0` | (actor update) |
| `0xba` | 12 | `CommandAnim`, `FUN_005317b0`, `Frag` | play animation |
| `0xbb` | 8 | `CommandRemoveBB` | remove billboard |
| `0xbc` | 8 | `CommandOpenDoor` | **open door** `{doorId}` `R` (see §8.4) |
| `0xbd` | 8 | `CommandCloseDoor` | close door `R` |
| `0xbe` | 8 | `FUN_00448640` | (particle/effect) |
| `0xbf` | 8 | `CommandStopParticles` | stop particles |
| `0xc0` | 40 | `CommandAddLight` | add light (position + colour) |
| `0xc1` | 68 | `CommandAddBlinkingLight` | add blinking light |
| `0xc2` | 28 | `CommandAirstrike` | airstrike |
| `0xc3` | 8 | `CommandPlayerSelect` | player select |
| `0xc4` | 8 | `CommandShadow` | shadow toggle |
| `0xc8` | var | (client-only) | **full player-list / world resync** (player table, spawns) |
| `0xc9` | var | (client-only) | session control: `GL_MULTIPLAYER_YOU_ARE_REMOVED` / `_HOST_HAS_QUIT` |
| `0xca` | var | (client-only) | time-scale / clock adjustment |
| `0xcb` | var | (client-only) | clock sync (`FUN_00571a20`) |

Ids not individually decoded above but still handled by the client (from the jump table):
`0x38,0x3f-0x43,0x47,0x4a-0x4c,0x4e,0x5e,0x69,0x6c,0x6d,0x73,0x76,0x77,0x79-0x7c,0x8f,0x90,0x94,0xad,0xae,0xb3,0xc5-0xc7`
— these are additional actor/AI/effect updates in the same style (small fixed payloads, mostly
`{actorId, …}`), several produced by helpers whose id constant is set via non-standard codegen.
**Do not assume "small fixed payload" for anything on this list** — `0x67` sat here and turned out
to be a variable-length script-execution message (§8.11).

> **Producer names in this table are not unique.** Several are virtual overrides implemented once
> per actor subclass, so the same name maps to many distinct functions: `SyncPositionAndBroadcast`
> is **11** functions (`0x0052f8a0`, `0x00533720`, `0x0053d8d0`, `0x00544460`, `0x00546120`,
> `0x00547520`, `0x00549cd0`, `0x0054a060`, `0x0054a8f0`, `0x0054b000`, `0x0054d4c0`), and `Frag`
> (`0x0052e220`, `0x00548b00`) and `ApplyDamage` (`0x0052f3b0`, `0x00535ac0`) are two each. When a
> row matters, resolve the producer by **address**, not by name.

### How this table was verified

The whole table was cross-checked against an exhaustive automated sweep of **every** caller of
`BroadcastToPlayers` — 104 distinct functions, 183 reference sites, 163 parsed call sites — pulling
the message id, size and `guaranteed` flag from each. Result: **the extraction agreed with every
overlapping id already in this table**, and filled in 8 previously-undecoded ones
(`0x70`, `0x81`, `0x91`, `0x98`, `0xa9`, `0xaa`, `0xaf`, `0xc1`).

Two methodology notes for anyone repeating this:

- Take the size and `guaranteed` literals from the **same source line** as the buffer variable.
  An earlier pass read them from the decompiler's P-code and paired them to ids positionally,
  which desynchronised in the multi-site functions (`ExecutorThreadProc`,
  `SyncPositionAndBroadcast`, `EvaluateTriggers`) and produced 10 false "size disagreements" —
  all of which evaporated once the pairing was fixed.
- Where the size is a computed expression, read the **disassembly**, not the decompiler.
  For `0xa9`/`0xaa` the decompiler renders `iVar + 0x48` / `iVar + 0x37` while the asm shows
  `LEA EDX,[EDI + 0x49]` / `LEA EDX,[EDI + 0x38]` with `EDI == 0` (the same register is stored into
  all three coords slots), giving the true sizes 73 and 56.

26 call sites still have an unresolved id — the constant is not a simple literal store into the
buffer's first field. They cluster in `SyncPositionAndBroadcast`, `EquipObject`, `OnPickedUp`
(3 each) and 17 functions with one apiece.

---

## 8. Detailed payload layouts (key message families)

### 8.1 Position + orientation state - update `0x6f` ('o'), 40 bytes

`Actor::SyncPositionAndBroadcast` @ `0x0052f8a0`. **Unreliable.** The workhorse entity-state msg.

```
+0x00 u32     id = 0x6f
+0x04 u32     actorId
+0x08 f32     time            (executor game-clock timestamp)
+0x0c f32[3]  position (x,y,z)
+0x18 f32[4]  orientation quaternion (x,y,z,w)
```

### 8.2 Set health - update `0x55`, 12 bytes

`Actor::SetHealth` @ `0x0052dbc0`. **Reliable.**

```
+0x00 u32  id = 0x55
+0x04 u32  actorId
+0x08 f32  health
```

### 8.3 Spawn actor - update `0x64` ('d'), 48 bytes

`SpawnRole` @ `0x00503710` / `CreateActor`. **Reliable.**

```
+0x00 u32     id = 0x64
+0x04 u32     actorId          (new actor's id)
+0x08 u32     roleId           (role->id)
+0x0c u32     teamId           (low byte significant)
+0x10 u32     ownerRef         (0 = none; else spawn-owner handle)
+0x14 f32[3]  position
+0x20 f32[4]  orientation quaternion
```

### 8.4 Open/close door - update `0xbc`/`0xbd`, 8 bytes

`CommandOpenDoor` @ `0x00445f60` / `CommandCloseDoor`. **Reliable**, coords `(0,0,0)` = all players.

```
+0x00 u32  id = 0xBC (open) / 0xBD (close)
+0x04 u32  doorId
```

### 8.5 Death - update `0x48` (normal) or `0x3d` (destructible), 56 bytes

`MobileActor::Die` @ `0x0053a020`. `id = 0x48`, or `0x3d` when the entity is destructible
(`entity->destructibility != 0`). Reliable **only** for non-destructible deaths
(`guaranteed = destructibility==0`).

```
+0x00 u32     id = 0x48 / 0x3d
+0x04 u32     actorId
+0x08 ...     serialised final state (position/orientation/velocity; filled by FUN_0053bb00)
```

Related: `PresidentActor::ApplyDamage` also emits the death-effect `0x8b`
`{actorId, seq(num_actors++), variant}` (16 B, unreliable) and, in Deathmatch, the frag-credit
`0x9b` `{killerTeam/id}` (8 B, reliable).

### 8.6 Spawn projectile / weapon fire - update `0x46` ('F'), 64 bytes

`FUN_00503bd0` @ `0x00503bd0`. **Unreliable.**

```
+0x00 u32     id = 0x46
+0x04 u32     attacker actorId
+0x08 u32     team
+0x0c u32     projectile roleId
+0x10 ...     position + direction/velocity vectors + damage/params (to 0x40)
```

### 8.7 Movement / waypoint order - command `0x3b`/`0x3d` (server) & update `0x3b`/`0x3c`, 56 bytes

Command handler (executor cases `0x3b`/`0x3d`) reads:

```
+0x00 u32     id
+0x04 u32     actorId
+0x08 u32     flag                 (0x3b: clears +0xE0 flag & does door proximity; 0x3d: sets it)
+0x0c f32[3]  target position       -> SetPositionAndOrientation
+0x18 f32[4]  target orientation quat
+0x28 f32[3]  waypoint              -> AI controller +0x1c/+0x20/+0x24
+0x34 f32     delay / alarm
```

### 8.8 Chat / player name - command `0x28` & update `0x8d`, 259 bytes

`FUN_004fcd00` (send) / executor case `0x28` (re-broadcast as `0x8d`). **Reliable.**

```
+0x00 u32       id = 0x28 (to server) / 0x8d (to clients)
+0x04 char[255] null-terminated text (chat line or player name)
```

### 8.9 Game rules + scoreboard - update `0xa2`, 35 bytes

`ExecutorThreadProc` builds it from globals; client parses at `0x005029d0` (`if IsExecutorRunning`
skip, i.e. joiners apply it):

```
+0x00 u32   id = 0xA2
+0x04 u8    friendly-fire-related flag  -> 0x007b9cf4
+0x05 u8    -> 0x006abe18
+0x06 ...   further rule flags + per-team scores (0x007b9cf8..0x007b9d10), packed bytes/ints
```

### 8.10 Turn / step - update `0x87`, 4 bytes

`ExecutorThreadProc`. **Reliable**, broadcast to all. Payload is **just the id** (no body). Sent
once at executor start and each time all tracked objects report idle (lock-step advance; see §9).

### 8.11 Run script file - update `0x67` ('g'), variable length

`QueueScriptExecution` @ `0x00505080`. **Reliable**, broadcast to every player, and it reaches
all of them unconditionally — both of `BroadcastToPlayers`' drop mechanisms are bypassed:

- **Relevance filtering**: the caller passes `coords = {0,0,0}`, and the filter's first branch is
  `coords.x == FloatZero && .y == && .z ==`. `FloatZero` @ `0x007f5f40` lives in uninitialized `.data` and
  has exactly one writer in the binary (`FUN_0043ab80` @ `0x0043ab9d`, a static initializer doing
  `FILD 0` / `FSTP` / `MOVSS`), i.e. it is the shared **0.0f** sentinel. A zero vector therefore
  always matches and `FUN_00511250` is never consulted.
- **Backlog throttling**: the guard is `if ((backlog < limit) || (guaranteed != 0))`. This message
  is sent with `guaranteed = 1`, so it short-circuits past the per-player backlog counters and the
  ~1-in-10 random-sample path entirely. Script broadcasts are never dropped.

```
+0x00 u32     id = 0x67          // 'g' in byte 0, bytes 1-3 explicitly zeroed
+0x04 char[]  script filename    // NUL-terminated; length from strlen_plus1 (INCLUDES the NUL)
```

Total size = `strlen(name) + 1 + 4`.

**Only the filename travels; the file contents do not.** Every recipient resolves that name
against its *own* `Scripts\` directory. The handler in the client update applier
(`ApplyUpdateMessage`) is:

```c
case 0x67:
    if (!IsExecutorRunning()) {            // non-host only
      SetCurrentDirectoryToGLDir(GL_Scripts);
      ExecuteCommandFile((char *)(param_1 + 1));
      SetCurrentDirectory();
    }
    break;
```

The `IsExecutorRunning()` guard stops the **host** running the script twice: in the same
function the host has already pushed the filename onto its local `ScriptQueue`, and runs it
from `RunQueuedScript` on its own main thread one-per-frame. Note the client path does **not**
go through `ScriptQueue` at all - it executes synchronously inside `ClientReceivePump`.

Consequence: **trigger scripts execute on every machine, each from its own local copy.** A
client whose `Scripts\` directory differs from the host's will diverge.

The blast radius is bounded by the `IsExecutorRunning()` gate in the `Command*` handlers, but
**that gate is far less comprehensive than it looks**. Counting transitively (a handler counts as
gated if it or anything it calls consults `IsExecutorRunning`, `IsClientRoutingActive` or
`IsMultiplayerActive`; converges at depth 2):

| | count |
|---|---|
| `Command*` handlers | 249 |
| gated | 97 |
| **ungated — execute fully on a joining client** | **152** |

The split tracks *actor/world authority*, not presentation:

- **Gated:** `CommandSpawn`, `CommandSpawnTeam`, `CommandGive`, `CommandOpenDoor`,
  `CommandTeleport`, `CommandSetActorArmor`, `CommandAddTrigger`
- **Ungated:** `CommandSet`, `CommandInc` (**tokens**), `CommandCompleteObjective`,
  `CommandDoor`, `CommandExplode`, and ~150 more

So authoritative actor state does still arrive only from the host's update stream, but a divergent
client script can freely mutate that client's **token table** — the script language's variable
system — and `CommandIf` reads tokens, so the client's subsequent script control flow diverges
too. `CommandSet` calls `SetTokenValue(&Tokens, …)` directly with no gate and no broadcast, behind
only a `LevelLoadReason != 3` check.

Those effects stay client-local: on a joiner `ServerDPlay == NULL`, so an ungated handler that
calls `BroadcastToPlayers` pushes onto the joiner's *own* loopback `UpdateQueue` and self-applies.
It never reaches the host.

> **Count gates transitively.** Counting only calls made *directly* inside each handler
> overstates the ungated set at 172 — `CommandSpawn` for instance looks ungated but delegates to
> `DoSpawn`, which holds the gate. The ungated remainder is not simply "the presentational half".

**`0x67` is the only update that can make a client execute a file, and `QueueScriptExecution` is
the only thing that sends one.** Both halves were established exhaustively rather than by
inspection:

- *Only `0x67` reaches `ExecuteCommandFile`.* `ApplyUpdateMessage` contains exactly one call to it
  (`0x004ff971`), and none of its 164 direct callees reaches `ExecuteCommandFile`,
  `ExecuteCommandLine`, `ExecuteCommand` or `QueueScriptExecution` transitively. Resolving the
  dispatch through the real jump table (index map `0x005028e8`, pointer table `0x005026b8`) puts
  that call in the case block `0x004ff95a`..`0x004ff980`, and **`0x67` is the only id that
  dispatches to it**.
- *Only `QueueScriptExecution` builds a `0x67`.* Of the 104 functions that call
  `BroadcastToPlayers`, exactly two contain an immediate `0x67`: `QueueScriptExecution`
  (`0x005050db`, the `'g'` id byte) and `SyncPositionAndBroadcast` @ `0x00533720`. The latter is a
  false positive twice over — one is `PUSH 0x67` into a virtual call `[ESI+0x11c]`, and the other
  writes `0x67` at offset `+0x08` of a buffer whose leading dword is `0x4f`, sent as
  `(id 0x4f, 25 bytes, unreliable)`. Neither is a message id.

So the effective trigger set is the **seven callers of `QueueScriptExecution`** listed in
`threading_model_notes.md`, not a broader family of messages.

**Scheduling differs between host and joiner even when the script files are identical.** Both run
the same tick `FUN_0046e6c0`, which calls `RunQueuedScript` @ `0x0046e6d4` *before*
`ClientReceivePump` @ `0x0046e7ba`:

| | Host / single-player | Joining client |
|---|---|---|
| Runs in | step 1, `RunQueuedScript` | step 2, inside `ClientReceivePump` |
| Source | `ScriptQueue` pop | `case 0x67` payload |
| Rate | **exactly one script per frame** | unthrottled within the batch (up to 50 msgs/frame) |
| Relative to that frame's updates | *before* them | *interleaved* with them |

So a host serialises scripts at one per frame and runs each before applying that frame's world
updates, while a joiner can run several in a frame, interleaved with the very updates the host
emitted alongside them. Ordering between a script's effects and neighbouring update messages is
therefore **not** guaranteed to match across machines.

---

## 9. Turn / lock-step model

The executor's simulation is turn-based on top of the message stream. Each iteration
(`ExecutorThreadProc`):

1. `WaitForMultipleObjects({kill, pause, msg-available}, 50ms)`.
2. Drain and dispatch all pending **commands** (§6): loopback `CommandQueue` pop (SP) or
   `DPlayReceive(ServerDPlay, 1)` (MP), through the id switch.
3. Per-actor updates + `EvaluateTriggers` (host-only), which emit **updates** (§7).
4. When every tracked object reports idle (`vcall +0x18`), broadcast **`0x87`** (turn advance) and
   compute the next deadline.

Clients apply updates in `ClientReceivePump`. The **≤50-per-frame cap is multiplayer-only**: with
a `ClientDPlay` session the pump reads the queue depth via vtable `+0xC8` and clamps it to `0x32`
before that many `DPlayReceive` calls. In loopback (`ClientDPlay == NULL`, i.e. single-player) it
instead calls `WakeExecutor` and then drains `UpdateQueue` **unbounded**, until `MsgQueue_Pop`
returns NULL. A joining client never evaluates
triggers and never runs the executor, and `IsExecutorRunning()` is false there, so its `Command*`
handlers no-op locally and only send to the host.

Trigger scripts, however, **do run on joining clients**. `QueueScriptExecution` both queues the
script locally *and* broadcasts update `0x67` carrying the filename (§8.11); each client then
runs `ExecuteCommandFile` on its **own local copy** of that file, synchronously inside
`ClientReceivePump` rather than via `ScriptQueue`. Only the host uses `ScriptQueue` - on a joiner
that queue is allocated but never populated, because all seven `QueueScriptExecution` callers are
host-side. See `threading_model_notes.md` for the caller inventory.

---

## 10. Key addresses summary

| Address | Type | Name |
|---------|------|------|
| `0x005116a0` | func | `CreateDirectPlay` (`CoCreateInstance`) |
| `0x00512160` | func | `EnumMultiplayerSessions` |
| `0x00512540` | func | `HostMultiplayerSession` (`Open` CREATE) |
| `0x00512370` | func | `JoinMultiplayerSession` (`Open` JOIN) |
| `0x005126b0` | func | `CreateLocalClientPlayer` |
| `0x00502db0` | func | `StartExecutorThread` (server player + group) |
| `0x00511700` | func | multiplayer teardown |
| `0x004fdbc0` | `FastCall` | `SendToServer` (client -> server, idTo=1) |
| `0x00504bf0` | `FastCall` | `BroadcastToPlayers` (server -> clients) |
| `0x00502c80` | func | `DPlayReceive` (Receive wrapper) |
| `0x004fdc70` | `StdCall` | `ClientReceivePump` (per frame) |
| `0x00509050` | thread | `ExecutorThreadProc` (command dispatch @ `0x005092a4`) |
| `0x004fde70` | `FastCall<void, u32*>` | `ApplyUpdateMessage` — client update dispatch (jmp `0x004fdec4`) |
| `0x00512890` | `FastCall<uint>` | `GetPlayerCount` (reads `0x007b9e44`) |
| `0x005128a0` | `FastCall<uint, uint>` | `GetPlayerIdByIndex` (lazy cache @ `0x007b9e48`) |
| `0x00505080` | `FastCall<void, char*>` | `QueueScriptExecution` (queues locally **and** broadcasts `0x67`) |
| `0x007f5f40` | float | `FloatZero` — 0.0f sentinel; zero coords bypass relevance filtering |
| `0x005028e8` | data | client-switch byte index-map (ids 0..0xcb) |
| `0x005026b8` | data | client-switch pointer table |
| `0x007b9e74` | `IDirectPlay4A*` | `MultiplayerActive` |
| `0x007b9dec` | `IDirectPlay4A*` | `ServerDPlay` (executor session) |
| `0x007b9d60` | `IDirectPlay4A*` | `ClientDPlay` (client session) |
| `0x007b9d64` | DPID | `ClientPlayerId` (local player) |
| `0x00667ad0` | GUID | Gunlok application GUID |
| `0x0066b910` / `0x0066b920` | GUID | `CLSID_DirectPlay` / `IID_IDirectPlay4A` |

Loopback queues, events, and thread details: see `threading_model_notes.md`.

## 11. Confidence & open items

- **High confidence**: COM/session setup, player IDs, SendEx/Receive framing, reliability &
  throttle logic, the message-frame format, the command id set, and the ~100 update ids with a
  named producer + exact size, plus the field layouts in §8. The producer/size/reliability columns
  have been **exhaustively verified** against every `BroadcastToPlayers` call site in the binary
  (104 functions / 183 references / 163 parsed sites) with zero disagreements — see
  "How this table was verified" at the end of §7. Every producer function also carries a plate
  comment in the Ghidra DB listing the update ids it emits.
- **Lower confidence / not fully decoded**: the semantic detail of update ids listed only in the
  jump table (§7 tail), the exact byte packing beyond the leading fields of the largest messages
  (`0x39` 80 B, `0x5f` 60 B, `0xc8` resync), and the two modem/serial `DPAID_*` GUID *names*
  (the GUID values are exact). The client-apply handlers live in an area Ghidra left as undefined
  bytes inside `ApplyUpdateMessage`; `disassemble` was applied to the ids inspected here, but a full
  sweep of every handler was not run.
- **The §7 tail list is not safe to gloss.** `0x67` sat in it, described as one more small
  fixed-payload actor update; it is in fact a variable-length message that makes every client
  execute a local script file (§8.11). Its omission also produced a wrong claim in §9 (that
  scripts run host-only) which stood until the handler was actually read. Treat the remaining
  undecoded ids as unknown-shape, not as assumed-small - and decode the handler before asserting
  anything about what does or does not cross the wire.
