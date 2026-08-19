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

The interface is created lazily by `EnsureDirectPlayInterface` @ `0x005116a0` (called before
host/join/enumerate):

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
(NULL outside MP; `IsMultiplayerActive` @ `0x005116f0` just tests non-NULL).

Teardown is `ShutdownMultiplayer` @ `0x00511700`, and it owns considerably more than the interface
pointer. In order: drain `DPlayGroupList` (per entry `FUN_00511b20` + `free_sized(payload, 0x1c)`),
drain `DPlaySessionList` (`FUN_00512360` + `free_sized(payload, 0x1c)`), drain `DPlayGroupDataList`
(`free(payload)`), drain `DPlayPlayerList` (`FUN_00502c70` + `free_sized(payload, 8)`) — each loop
also zeroing that list's count and cache-valid flag and freeing its cache — then
`StopClientRouting()` if client routing is active, `StopExecutorThread()` if the executor is
running, and only then `Release` through vtable `+0x08` and NULL the pointer. Callers:
`EndLevelSession`, `MenuEscapePressed`, `MultiplayerLobbyTick`, `OnMenuItemClicked`.

### IDirectPlay4A vtable slots used (offset from vtable base)

| Off | Method | Used by |
|-----|--------|---------|
| `+0x04` | AddRef | `StartExecutorThread` (retain server session) |
| `+0x08` | Release | teardown |
| `+0x14` | CreateGroup | `StartExecutorThread` (server broadcast group) |
| `+0x18` | CreatePlayer | server player + local client player |
| `+0x30` | EnumPlayers | `ReapDroppedPlayers` (rebuild the live-player hash) |
| `+0x34` | EnumSessions | `EnumerateDPlaySessions` (session browser) |
| `+0x60` | Open | host / join |
| `+0x64` | Receive | `DPlayReceive` |
| `+0x68` | Send | executor reply (command `0x07`) |
| `+0x8c` | EnumConnections | `EnumerateServiceProviders` (service-provider list) |
| `+0x98` | InitializeConnection | `TestServiceProviderConnection` / `InitializeDirectPlayConnection` |
| `+0xc4` | **SendEx** | `SendToServer`, `BroadcastToPlayers` |
| `+0xc8` | **GetMessageQueue** | backpressure checks |

*(The 4 IDirectPlay4-only slots GetGroupFlags/GetGroupParent/GetPlayerAccount/GetPlayerFlags sit
at `+0xb4..+0xc0`, which is why SendEx is at `+0xc4`, not `+0xb4`.)*

---

## 2. Service providers & connection addresses

`EnumerateServiceProviders` @ `0x005119f0` calls `EnumConnections` (vtable `+0x8c`, with
`dwFlags = 1 = DPCONNECTION_DIRECTPLAY` and the app GUID) to list installed service providers,
rebuilding `DPlayGroupList` from the survivors. It returns true only if the HRESULT is `S_OK`
**and** the list came back non-empty.

**Exactly two service providers are usable: TCP/IP and IPX.** The callback
`DPlayEnumConnectionsCallback` @ `0x00512a50` (`__stdcall`, `RET 0x18` — the 6-argument
`DPENUMCONNECTIONSCALLBACK`) accepts a provider only if its GUID equals one of two constants read
from the image, and silently drops everything else — modem, serial, and any third-party SP:

| GUID | Address | Identity |
|------|---------|----------|
| `{36E95EE0-8577-11CF-960C-0080C7534E82}` | `0x0066b8f0` | `DPSPGUID_TCPIP` |
| `{685BC400-9D2C-11CF-A9CD-00AA006886E3}` | `0x0066b900` | `DPSPGUID_IPX` |

Each survivor must additionally return `S_OK` from `TestServiceProviderConnection` @ `0x005119a0`,
which is the filter proper: it creates a **throwaway** `IDirectPlay4A` by `CoCreateInstance`, calls
`InitializeConnection(lpConnection, 0)` through vtable `+0x98`, `Release`s it, and returns that
HRESULT. So enumeration does a full COM create/destroy per provider. Accepted entries are appended
to `DPlayGroupList` as 0x1c-byte records — despite the name, that list holds enumerated
**service-provider connections**, not DirectPlay groups (`DPlayGroupDataList` is a different list).

`BuildModemCompoundAddress` @ `0x00512080` serialises a compound DirectPlay address (`DPADDRESS` of
`{GUID, size, data}` chunks) for a **modem** target specifically, in four chunks:

| Chunk | GUID | Address | Identity | Payload |
|-------|------|---------|----------|---------|
| 1 | `{1318F560-912C-11D0-9DAA-00A0C90A43CB}` | `0x0066b800` | `DPAID_TotalSize` | dword total size |
| 2 | `{07D916C0-E0AF-11CF-9C4E-00A0C905425E}` | `0x0066b7f0` | `DPAID_ServiceProvider` | the 16-byte GUID at `0x0066b8d0` = `{44EAA760-CB68-11CF-9C4E-00A0C905425E}` = `DPSPGUID_MODEM` |
| 3 | `{F6DCC200-A2FE-11D0-9C4F-00A0C905425E}` | `0x0066b7b0` | `DPAID_Modem` | string, the modem device name (`this+0x08`) |
| 4 | `{78EC89A0-E0AF-11CF-9C4E-00A0C905425E}` | `0x0066b7d0` | `DPAID_Phone` | string, the phone number (`this+0x0c`) |

All six GUID values above were re-verified byte-for-byte against the image.
**`BuildModemCompoundAddress` has no references of any kind** — not called, and in no vtable, so it
is not the slot-0 address provider `InitializeDirectPlayConnection` optionally consults either. That
is consistent rather than coincidental: the SP filter above can never yield a modem connection to
build an address for. Dial-up play is present in the code and unreachable in the shipped binary.

`InitializeDirectPlayConnection` @ `0x00511b50` is what actually commits a chosen provider:
`InitializeConnection` through `+0x98` with the connection blob at `ServiceProvider+0x10`, or with a
replacement blob returned by vtable slot 0 of an optional override argument. It returns true on
`S_OK` **or** on `DPERR_ALREADYINITIALIZED` (`0x88770005`).

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
| Browse | `EnumerateDPlaySessions` @ `0x00512160` | `EnumSessions(desc, 0, cb=DPlayEnumSessionsCallback, 0, 0x91)` — `AVAILABLE\|ASYNC\|RETURNSTATUS` |
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

Two details of the browse step that are easy to trip over. `EnumerateDPlaySessions` **hides the
game window** around the call — `ReleaseD3DResources()`, `EnableWindow(GameWindow, 0)`,
`ShowWindow(GameWindow, 0)`, then the restore plus `SetForegroundWindow` afterwards — because a
service provider may put up a modal dialog. It does this **only the first time**: the flag
`EnumSessionsWindowHidden` is latched to 1 at the end (and `InitializeDirectPlayConnection` writes
it too), so every later browse runs with the window up. It also tolerates `DPERR_USERCANCEL`
(`0x8877015E`) alongside `S_OK`. The callback `DPlayEnumSessionsCallback` @ `0x00512b90`
(`RET 0x10`, no register arguments — `LPDPENUMSESSIONSCALLBACK2`) returns 0 on a NULL descriptor to
stop enumeration, and otherwise appends a `pool_alloc(0x1c)` session record to `DPlaySessionList`.

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

**The `sender` column is the client-side function that puts the message on the wire, not the
handler that services it.** For the gameplay commands that sender is a **client `Unit` vtable
slot** — every `Unit_Send*` below is a virtual on the client-side `Unit` tree, whose slot number is
given where it was measured. The executor-side handler is a switch arm in `0x00509xxx`-`0x0050axxx`
and is named in the meaning column. Confusing the two is what produced most of the wrong meanings
this section has had to correct.

The executor's dispatch is a **byte-indexed jump table**: `EAX = id - 4`, range-checked against
`0x39`, then `MOVZX EAX, byte [EAX + 0x0050bae0]` and `JMP dword [EAX*4 + 0x0050ba3c]` — 41
jump-table slots, of which slot `0x28` (`0x0050addc`) is the shared default and is also where the
range check jumps. A handful of ids are serviced **before** that switch by an if-chain at
`0x005091b8`-`0x0050922f` (`0x28`, `0x2c`, `0x2d`, `0x2e`, `0x2a`, `0x06`, plus `0x34` at
`0x00509279`) and reach the default arm in the table itself; that is the normal arrangement for a
lobby message, since no executor is running then.

| id | size | sender (client) | meaning (server action) |
|----|------|-----------------|--------------------------|
| `0x04` | - | (pre-switch) | player-left / removal acknowledgement (marks session-list entry) |
| `0x06` | - | (pre-switch) | join/ready ack (`DAT_007b9df1` path) |
| `0x07` | - | (in-switch) | ready ping -> server replies via `Send` with a 4-byte `{2}` |
| `0x08` | - | (in-switch) | inventory/pickup spawn batch (iterates vulnerability list) |
| `0x09` | 12 | `Unit_SendStop` (slot 73) | **stop**: `MobileActor::StopAndBroadcast(actor, time)` then `ClearOrderQueue`, sets `Actor+0x12d`. Arm `0x00509ddc` |
| `0x0a`/`0x0c` | 24 | `Unit_SendMoveOrder` (slot 75) | **move/attack to position** -> `Actor` slot 97 `AttackPosition(&pos, time, 0, close_range)` then `ClearOrderQueue`. Arm `0x00509e91` |
| `0x0b`/`0x0d` | 16 | `Unit_SendAttackTargetImmediate` (slot 74) | **immediate attack target** -> `Actor` slot 96 `AttackTarget` then `ClearOrderQueue`. Arm `0x00509fae` |
| `0x0e` | 16 | `Unit_SendStopAttack` (slot 76) | **stop attacking** -> `Actor` slot 98 `StopAttacking` @ `0x00540f20`. Arm `0x0050a028` |
| `0x0f`/`0x10` | 16 | `Unit_SendInteract` (slot 100) | interact -> `MobileActor::EquipItemInSlot(arg1, arg2, 1)` |
| `0x11`/`0x12` | 12 | `Unit_SendEquip` (slot 101) | equip object |
| `0x13`/`0x14` | 12 | `Unit_SendDropItem` (slot 102) | **drop item** -> `MobileActor::DropItem` @ `0x00538240` |
| `0x15`/`0x16` | 12 | `Unit_SendUseItem` (slot 104) | use/remove inventory item -> re-broadcasts update `0x80` |
| `0x17`/`0x18` | 24 | `Unit_SendBoard` (slot 103) | board/give — see the layout note below. Arm `0x0050a22f` (immediate) / `QueueBoardOrder` @ `0x0050a2e8` |
| `0x19` | 12 | `Unit_SendSetAmmoType` | select ammo type, **immediate** |
| `0x1a` | - | (in-switch) | select ammo type, **queued** -> slot 85 |
| `0x1b` | 12 | `Unit_SendStandingOrder` | `MobileActor::SetStandingOrder(arg1)` |
| `0x1c` | 8 | `Unit_SendCrouchToggle` | crouch toggle |
| `0x1d`/`0x1f` | 24 | `Unit_SendAttackPosition` | attack ground -> slot 86 |
| `0x1e`/`0x20` | 16 | `Unit_SendAttackTarget` | attack target -> slot 87 |
| `0x21` | - | (in-switch) | inventory/pickup spawn batch (variant of `0x08`) |
| `0x23` | 8 | `Unit_SendCancelLastOrder` | `MobileActor::CancelLastOrder` (DELETE key) |
| `0x24` | 8 | `Unit_DeleteIfUnitInRange` (slot 92) | delete actor (`vtbl->Delete`). Arm `0x0050a540`. A **proximity trigger, not a UI action** — see below |
| `0x25`/`0x26` | 8 | `ActivateUnitAndResume` | hold-marker release / team push -> re-broadcasts `0x65` |
| `0x27` | 16 | `FUN_004a4660` | Gunlok super-ability -> broadcasts `0x37`, projectiles |
<!-- rows above corrected against the order system; see orders_notes.md -->

**Treat any row in this table whose meaning is not tied to a named handler with suspicion**, and
check it against the executor arm before building on it. That is a standing caution rather than a
general one: repeated passes over this section have found the *sender* column reliable and the
*meaning* column, where it was written from the id alone, invented. Ten rows have had their meaning
replaced by a measured one, and each now cites the arm address that pins it. `0x19`/`0x1a`, `0x1c`,
`0x1d`/`0x1f` and `0x1e`/`0x20` are the player order commands, not health/coordinate queries —
`orders_notes.md` has the dispatch.

### Paired ids: two different mechanisms, and only one of them is immediate/queued

Many rows carry two ids, and **the two spacings mean different things**. Getting this wrong is what
made every `+2` pair read as a queued order that does not exist.

- **`+1` pairs are immediate/queued.** The sender takes a trailing `bool queued` and picks the id
  with a literal: `Unit_SendInteract` does `CMP byte [EBP+0x10],0` then `MOV [EBP-0x14],0x10`
  (queued) or `0xf` (immediate), and the queued arm additionally calls a local order-queue helper.
  This holds for the `Unit` slots 100-104 (`0x0f`/`0x10`, `0x11`/`0x12`, `0x13`/`0x14`,
  `0x15`/`0x16`, `0x17`/`0x18`) and for `0x19`/`0x1a`. The odd member is immediate, the even one
  queues a `PendingOrder`.
- **`+2` pairs are one command, and the second id means `close_range`.** `0x0a`/`0x0c`,
  `0x0b`/`0x0d`, `0x1d`/`0x1f` and `0x1e`/`0x20` are each a *single* command; the sender has **no**
  `queued` argument at all and selects the id from `TEST byte ptr [KeyModifierState],0x2`, i.e.
  **CTRL held**. Both ids reach one executor arm, which recovers the flag from the id itself
  (`CMP EDI,0x0c ; SETZ CL` at `0x00509eab`, `CMP EDI,0x0d ; SETZ CL` at the `0x0b`/`0x0d` arm) and
  passes it on as the fourth argument of `Actor` slot 97 `AttackPosition` / slot 96 `AttackTarget`.
  The engine's own word for the flag is `close_range` (`actor_vtable_notes.md` slots 96/97:
  "broadcast id is computed: `0x41 + close_range`"), and on the client it also scales the unit's
  engagement range by `CloseRangeSpeedFactor` @ `0x00666234` = **0.65f**.

`KeyModifierState` @ `0x007b6ddc` is maintained bit by bit in `HandleKeyMessage` @ `0x004e3f20`,
set on `WM_KEYDOWN` and cleared on key-up: DIK `0x2a` LSHIFT -> bit `0x1`, DIK `0x1d` LCTRL ->
bit `0x2`, DIK `0x38` LALT -> bit `0x4`.

**Still unmapped in this direction**: `0x22` alone, the even twin of `0x21` and presumed its queued
form. `0x10`, `0x12`, `0x14`, `0x16` and `0x18` are each measured — every one is the id its own
sender writes when the trailing bool is set — and are listed with their odd partners above.
| `0x28` | 259 | `SendChatOrPlayerName` @ `0x004fcd00` | **set player name / chat text** (255-byte string, reliable) -> re-broadcast `0x8d`; in lobby (GameState 8) copies to the local name buffer `0x007b7150`. **Three unbounded `strcpy`s on this path — see §6.1** |
| `0x29` | 16 | `0x004fcdf0` | **dead on both ends.** No call site anywhere in the image (no code *or* data reference, so it is in no vtable), and the executor byte table routes `0x29` to the same default arm as a range-check failure while the pre-switch if-chain does not mention it. Payload word 2 (`[EBP-0xc]`) is never initialised, so it would leak 4 bytes of the sender's stack if it were ever sent |
| `0x2a` | 16 | `SendLobbyTeamAssignment` @ `0x004fce30` | lobby team assignment: `*(int*)(0x007b70e4 + (arg0*5 + arg1)*4) = arg2`, arm `0x00509217`. The formula is right; **there is no bounds check on any of the three — see §6.1**. Note the C argument order is not the wire order (`arg0` = EDX, `arg1` = ECX, `arg2` = the stack argument) |
| `0x2b` | 24 | `CharacterUnit::Unit_SendThrowDecoy` @ 0x004c4040 (slot 80) | **throw decoy at a position** -> `MobileActor::ThrowDecoy` @ `0x00541170`, which consumes one inventory item of pickup class 5 / weapon 3, spawns `decoy_projectile` and broadcasts update `0x92`. Arm `0x00509f85`. Payload `{0x2b, unit_number, f32 GetGameTimeSeconds, Vec3 pos}`. The sender is armed by `DecoyTargetPending` @ 0x007b3f52 and reached from `IssueMoveOrderToSelection` — and a **queued** decoy click sends nothing at all (`orders_notes.md` §8.3) |
| `0x2c` | 8 | `SendCycleTeamRequest` @ `0x004fbf90` | **cycle this player to the next free team slot** -> `CyclePlayerToNextFreeTeam(payload[0] - 1)` @ `0x004f6b00`. Unreliable. The handler is *not* the no-arg global it looks like — the argument is spilled (`MOV ECX,[EBX+4]; DEC ECX`) |
| `0x2d` | 8 | `SendSetLobbySlotFlag` @ `0x004fc0f0` | set lobby slot flag: `((byte*)0x007b7147)[arg0] = 1`, arm `0x005091eb`. **No bounds check on `arg0` — see §6.1** |
| `0x2e` | 8 | `SendClearLobbySlotFlag` @ `0x004fc400` | clear lobby slot flag: `((byte*)0x007b7148)[arg0-1] = 0`, arm `0x005091fc`, bounds-checked `arg0-1 < 4` with `___report_rangecheckfailure` |
| `0x2f` | 12 | `Unit_SendDetonateAllMines` @ `0x004506c0` | **detonate all of this team's remote mines** `{team, 0}`. Arm `0x00509cdd` snapshots the `actors` table and `MineDetonate`s every actor with `ai_type == 2 (Mine) && character->weapon == 1 && team_id == payload.team` |
| `0x30` | 12 | `Unit_SendDetonateUnitMine` @ `0x00450770` | **detonate (or, for a decoy, dismiss) the mine deployed by unit `slot` of team `team`** `{team, slot}`. Arm `0x00509c34` indexes `TeamSlots[team] + slot*4` (0xc4 team stride); `character->weapon == 1` -> `MineDetonate`, `== 3` -> `Decoy_Dismiss` @ `0x00450f60`. One key binding per unit slot, `slot` in 0..0xb |
| `0x32` | 8 | `FUN_0046f590` | countdown control -> `0x9a`/`0x17` |
| `0x34` | - | `CommandStatsScreen` | stats screen (handled outside the switch) |
| `0x35` | 4 | `FUN_00496f70` | -> broadcast `0xa4` |
| `0x36` | 8 | `SendClientTimeSync` @ `0x00502ab0` | **client clock sync**, `{0x36, f32 GetGameTimeSeconds()}`, unreliable -> broadcast `0xca`. Throttled to once every **2.0 s** of game time and gated on `ClientDPlay != 0` |
| `0x3b` | 56 | `FUN_004ad050` | movement/waypoint order: pos + orientation + waypoint (see §8.7) |

Additional ids consumed only in the pre-switch prologue: `0x2c`,`0x2d`,`0x2e`,`0x2a`,`0x28`
(lobby name), `0x34` (bypasses switch). Countdown/turn ids `0x31`,`0x32`,`0x33` are also handled
here and each re-broadcast a client update (`0x99`,`0x9a`,`0xcb`).

Ids whose byte-table entry points at the default arm, i.e. which the jump table itself ignores:
`0x05`, `0x06`, `0x29`, `0x2a`, `0x2c`, `0x2d`, `0x2e`, `0x34`, `0x37`, `0x38`, `0x39`, `0x3a`,
`0x3c`. All of them except `0x29` are covered by the pre-switch if-chain; `0x29` is covered by
neither, which is what makes the "dead on both ends" verdict for it solid rather than an absence of
evidence.

The lobby ready flags are the one place where the two halves of this document have not been made to
agree, and the disagreement is left standing on purpose. This section reads the `0x2d`/`0x2e`
target as `DAT_007b7144[1..4]`, i.e. bytes `0x7b7145`-`0x7b7148`; an independent read of the lobby
poll `StartMultiplayerGameWhenReady` @ `0x004ef950` has the same four flags at
`0x7b7148`-`0x7b714b` (`LobbyPlayerReady[4]`, also written as a run of four by
`ApplyUpdateMessage` @ `0x004fe1db`). The two ranges overlap in exactly one byte and cannot both be
right. **The Ghidra DB deliberately leaves that range untyped for this reason** — do not type it, or
name an array over it, until someone reads the lobby draw (`FUN_004f10b0`) and settles which base is
real.

What is not in doubt is the arithmetic each command performs, which is what §6.1 rests on: `0x2d`
and `0x2e` write **the same cell** (`0x7b7147 + arg0` and `0x7b7148 + (arg0 - 1)` are equal), so
they are set/clear of one flag and differ only in that `0x2e` bounds-checks and `0x2d` does not.
Note also which way that leans — `0x2e`'s own check admits `arg0` in 1..4, i.e. cells
`0x7b7148`-`0x7b714b`, which is the poll's range rather than this section's. That is a reason to
expect the poll to be right; it is **not** a measurement of the base, because a bounds check
constrains the index the code accepts and not the address the array was declared at. Read
`FUN_004f10b0` before acting on it.

### 6.1 Unchecked writes and unbounded copies reachable from the wire

These are defects in **Gunlok itself**, not in GkPlus, and they are recorded here because this is
where the wire format is documented. Each is reachable by any peer in a session, on a game from 2000
with no vendor to notify. The fuller defect write-ups belong in `game_defects_notes.md`.

- **Command `0x2d` writes one byte at a wire-controlled 32-bit offset with no bounds check.** The
  arm at `0x005091f0` is `MOV byte ptr [EAX + 0x7b7147],1` with `EAX` taken straight from
  `payload[0]`. The evidence that this is an omission rather than a deliberate wide range is its
  sibling: `0x2e` at `0x005091fc` does `DEC EAX ; CMP EAX,4 ; JNC ___report_rangecheckfailure`
  before writing the *same* cell. Both arms run in the pre-switch if-chain, so neither is covered by
  the jump table's range check on the id.
- **Command `0x2a` writes four bytes at a wire-controlled offset with no check on any of its three
  arguments.** `*(int*)(0x007b70e4 + (arg0*5 + arg1)*4) = arg2` at `0x0050921c`-`0x00509228`; both
  indices and the stored value come from the payload.
- **Three unbounded inline `strcpy`s on the `0x28` / `0x8d` chat-and-player-name path.** The wire
  form carries a 255-byte string field in a 259-byte message, and every copy terminates only on a
  NUL byte: the sender `SendChatOrPlayerName` @ `0x004fcd00` copies into a `SUB ESP,0x108` frame at
  `[EBP-0x104]` and smashes its own frame past ~256 bytes (the same family as `Font_QueueText`,
  `game_defects_notes.md` §1); the executor's lobby-name arm copies to the fixed buffer
  `0x007b7150` at `0x005091bd`-`0x005091d2`; and the `0x8d` re-broadcast at `0x00509734` copies it
  again into a stack frame. A peer that sends 259 bytes with no NUL in the text field overruns the
  latter two.

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
@ `0x004fde70`. The dispatch is the same **two-level** shape the executor uses for commands, and
reading it is the only reliable way to tell a handled id from an ignored one:
`CMP ESI,0xcb ; JA 0x00502695` (the default arm), then
`MOVZX EAX, byte [ESI + 0x005028e8]` — a byte index-map covering ids `0..0xcb` — and
`JMP dword [EAX*4 + 0x005026b8]`, the pointer table. An id whose byte-map entry selects the default
arm is *not* handled here even though it is inside the range check; update `0xa2` is exactly that
case, and is applied from the client pump instead (see its row).
**151 ids are handled.** The table below lists every id with a decoded producer, its wire size,
and the producing function (name conveys semantics). `R` = sent reliably (guaranteed) where known.

| id | size | producer(s) | meaning |
|----|------|-------------|---------|
| `0x00` | var | (HUD) | on-screen/objective string (string at payload `+0x10`, GameState==5) |
| `0x01` | var | (HUD) | on-screen/status string (string at payload `+0x10`) |
| `0x03` | 8 | (per-actor) | **unit is now live on this client** -> `ClientActivateUnit` @ `0x004fde10`: sets `Unit+0x210 = 1`, and on the *first* one (`ObjectList_count == 0` and not single-player) also snaps `CameraCoords` onto `Unit+0x98` and `CameraDistance1 = CameraDistance2` |
| `0x1e` | 16 | `SyncPositionAndBroadcast` | position/velocity update |
| `0x37` | 20 | (`ApplyDamage`/case 0x27) | health/armor/shield status `{actorId, strength, shield, armor}` |
| `0x39` | 80 | `MobileActor::SetMoveDestinationAndBroadcast` @ `0x00539540` | **move order issued**: `{id, actorId, MotionSnapshot[0x30], Vec3 destination, Vec3 current_waypoint}` (see §8.12). Unreliable |
| `0x3a` | 24 | `ExecutorThreadProc` | (session/turn-related state) |
| `0x3b` | 56 | `MobileActor::BroadcastStopAtPosition` @ `0x00539d40` | **stop at own position**: `{id, actorId, MotionSnapshot[0x30]}`. Unreliable |
| `0x3c` | 56 | `MobileActor::StopAndBroadcast` @ `0x00539bd0` | **stop**: `{id, actorId, MotionSnapshot[0x30]}`. Unreliable |
| `0x3d` | 56 | `CommandTeleport`, `CommandTeleportAndOrientate`, `EvaluateTriggers`, `MobileActor::Die` | teleport / full move+orient (also destructible death) |
| `0x3e` | 12 | `Decoy_Dismiss` @ `0x00450f60`, `Mine_OnDeployed` @ `0x0045a640` | decoy dismissed / expiry reset `{actorId, 0x7fffffff}` |
| `0x41`/`0x42` | 16 | (`Actor` slot 96 `AttackTarget`) | **attack target**, `0x42` = `close_range`. The client applies it in `ApplyUpdateMessage` @ `0x004fe735`: `CMP dword [EDI],0x42 ; SETZ CL` for the flag, `GetActorById(msg+0xc)` for the target, then `Unit` slot 108 `Unit_SetAttackTarget`. Matches `actor_vtable_notes.md` slot 96's computed id `0x41 + close_range` |
| `0x44` | 12 | `StopAttacking` | stop attacking |
| `0x45` | 8 | `SyncPositionAndBroadcast` | position keyframe (short) |
| `0x46` | 64 | `SpawnProjectileActor` @ `0x00503bd0` | **spawn projectile / weapon fire** `'F'` (see §8.6) |
| `0x48` | 56 | `MobileActor::Die` | **death** (non-destructible) `R` (see §8.5) |
| `0x49` | 8 | `Delete` | delete actor |
| `0x4d` | 9 | `Actor_FixupAfterLoad` @ `0x005317b0` | what a **restored** actor publishes: `{u32 0x4d, u32 actorId, u8 is_concealed}` (`MobileActor+0x186`), emitted only when `is_crouched` (`MobileActor+0x187`) is set. Unreliable |
| `0x4f` | 25 | `EquipObject`, `OnPickedUp`, `SyncPositionAndBroadcast` | equip + position |
| `0x50` | 12 | `ChangeOwnerAndTeam`, `CommandGiveControl`, `ReleaseFromOwner`, `SyncPositionAndBroadcast`, `ReapDroppedPlayers` | owner/team change `{actorId, newTeam}`; `ReapDroppedPlayers` emits one per actor it transfers off a dropped player's team, paired with `0x91` |
| `0x51` | 16 | `EvaluateTriggers`, `SyncPositionAndBroadcast` | position (trigger-driven) |
| `0x52` | 8 | `EvaluateTriggers` | trigger event |
| `0x53` | 16 | `SyncPositionAndBroadcast` | position |
| `0x54` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x55` | 12 | `SetHealth` | **set health** `{actorId, f32 health}` `R` (see §8.2) |
| `0x56` | 16 | `SetTarget` | set attack target |
| `0x57` | 8 | `ClearTarget` | clear target |
| `0x58` | 20 | `ChangeOwnerAndTeam` | owner+team+extra |
| `0x59` | 8 | `ReleaseFromOwner` | release from owner |
| `0x5a` | 12 | `MobileActor::AssignToTeamSlot` @ `0x00532d40` | **team-slot assignment / recruit**, `{u32 0x5a, u32 actorId, u32 team_slot}` `R`. Not CTF. It also sets `TeamSlots[n]+0xa0` and `+0x74`, calls `Actor` slot 26 `SetField0x18c`, and — for the role named `Hark` specifically — ORs `0x1000` into `nav_agent->traversal_flags` (`NavAgent+0x2c`), a character-specific nav capability grant |
| `0x5b`-`0x5d` | 12 | `0x00532e80` / `0x00532fe0` / `OnFlagCaptured` | CTF/objective state updates; `0x5d` is **flag captured** `{actorId, playerIdx}` `R` |
| `0x5f` | 60 | `MobileActor::AddWalkingSpeed` @ `0x00539ed0` | `{u32 0x5f, u32 actorId, s32 speed_delta, MotionSnapshot[0x30]}` `R`. **Never sent by the shipped binary** — the sender has no callers and no pointer to it exists anywhere in `.rdata`/`.data`, so it is not a vtable slot either |
| `0x60`/`0x61`/`0x63` | 8 | `AiThink_Bot` @ `0x00451220` | bot AI-state notifications. `AiThink_Bot` has **11** `BroadcastToPlayers` sites, not the 8 an earlier sweep saw — three more were recovered from the 8,400 bytes of the function that sat behind unresolved jump tables: 0x00453ed6 and 0x00453f46 (id **0x63**) and 0x00454055 (id **0x62**). At all three the payload is `{u32 id, u32 actor_id = [EBX+0xc]}` with `EDX = 8`, unreliable, plus a by-value `Vec3` stack argument — a shape **inferred from the call site**, not confirmed against `BroadcastToPlayers`' own body. Semantically: **`0x63` is emitted on entering `alert_state = 1`** (the 17-second reacquire state) and **`0x62` on leaving it**, which is what the ids' association with alert states used to rest on and is now measured. `ai_behaviour_notes.md` §11 / §2.1.1. `0x60`/`0x61`'s payloads are still not decoded |
| `0x62` | 8 | `SyncPositionAndBroadcast`; also `AiThink_Bot` @ 0x00454055 | position (short) — **and** the reacquire-window expiry above. Note that description is not consistent with `ai_behaviour_notes.md` §5.1, which reads the client arm at 0x005005bc as `Unit+0x24 = 0` plus a sound, i.e. an alert-state clear rather than a position; the handler decides it and was not re-read here |
| `0x65` | 8 | `ExecutorThreadProc` (case 0x26) | team notification |
| `0x66` | 8 | `FUN_004bdf90`, `MobileActor::ReleaseHoldMarkerOrder` @ `0x00538930` | hold-marker order released `{u32 0x66, u32 actorId}` |
| `0x67` | var | `QueueScriptExecution` | **run script file** `'g'` + filename `R` (see §8.11) |
| `0x68` | 16 | `AiThink_Minebot` @ `0x00456c50`, `AiThink_Swarm` @ `0x0045b620` | (AI state update) |
| `0x6b` | 12 | `Frag` | frag/score event |
| `0x6e` | 20 | `AiThink_Node` @ `0x0045a850` | (AI state update) |
| `0x6f` | 40 | `SyncPositionAndBroadcast`, `CommandTeleport` | **position+orientation state** `'o'` (see §8.1) |
| `0x70` | 40 | `CommandTeleportAndOrientate` | teleport + orientation `R` |
| `0x71` | 12 | `SyncPositionAndBroadcast` | position delta |
| `0x72` | 8 | `EquipObject` | unequip |
| `0x74`/`0x75` | 12/8 | `OnPickedUp` | animation/turret state |
| `0x78` | 12 | `MobileActor::GiveItemTo` @ `0x00537db0`, `ReceiveObject` | receive/transfer object |
| `0x7b`/`0x7c` | 12 | (inventory) | **consume an inventory item**; both reach `Unit_ConsumeInventoryItem` on the client, `0x7c` being the same effect **without the UI sound** |
| `0x7d` | 12 | `CommandRemoveItem`, `EquipObject`, `MobileActor::DropItem` @ `0x00538240` | remove inventory item `{actorId, itemId}` `R` |
| `0x7e`/`0x7f` | 12/16 | `SyncPositionAndBroadcast` | position variants |
| `0x80` | 12 | `ExecutorThreadProc` (case 0x15), `SyncPositionAndBroadcast` | item removed / position |
| `0x81` | 24 | `MobileActor::GiveItemTo` @ `0x00537db0` | object transfer variant `R` |
| `0x82` | 12 | `ExecuteSpecialAbility` | special ability |
| `0x83` | 12 | `OnMobileDamageReceived` | damage taken notification |
| `0x84` | 8 | `Associate` | associate objects |
| `0x85` | 8 | `PickupActor::SetPickupEnabled` @ `0x00546240` | pickup enabled/disabled `R` |
| `0x86` | 8 | `MobileActor::PopFrontOrder` @ `0x00538700` | order completed `{u32 0x86, u32 actorId}`, unreliable |
| `0x87` | 4 | `ExecutorThreadProc` | **turn / step advance** (lock-step; see §9) `R` |
| `0x88`/`0x89` | 12/8 | `SetGameSpeed` @ `0x00505380` | **game speed**, both `R`. `0x88` = `{f32 speed, i32 seq}` when `speed < 1.0f` (slow motion / Active Pause; also sets `ExecutorState_9e18 = 1` and increments the sequence counter `ExecutorState_9e1c`); `0x89` = `{f32 speed}` otherwise |
| `0x8a` | 12 | `InitPositionAndTiming` | initialise position + timing |
| `0x8b` | 16 | `PresidentActor::ApplyDamage` | death/gib effect `{actorId, seq, variant}` |
| `0x8c` | 8 | `OnPickedUp` | (turret state) |
| `0x8d` | 259 | `ExecutorThreadProc` (case 0x28), `EvaluateTriggers` | **chat / name broadcast** (255-byte string) |
| `0x8e` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x91` | 24 | `ReapDroppedPlayers` @ `0x005046e0` | the whole **player -> team table**, 5 dwords out of `0x006a7d18`, rebuilt and re-broadcast after a player drops `R` |
| `0x92` | 20 | `MobileActor::ThrowDecoy` @ `0x00541170` | decoy thrown `{u32 0x92, u32 actorId, Vec3 target}`, unreliable |
| `0x93`/`0x95` | 12 | `CommandSetActorArmor`, `ApplyTeamCarryOverState` @ `0x004da4a0` | armor/attribute set |
| `0x96` | 8 | `SyncPositionAndBroadcast` | position (short) |
| `0x97` | 8 | `Dissociate` | **dissociate objects** — the client applies it through `Unit` slot 55 `Dissociate` |
| `0x98` | 32 | `ApplyTeamCarryOverState` | team carry-over roster entry `R` |
| `0x99` | 12 | `ExecutorThreadProc` (case 0x31) | countdown start `{index, timer}` `R` |
| `0x9a` | 4 | `ExecutorThreadProc` (case 0x32) | countdown tick/stop `R` |
| `0x9b` | 8 | `ApplyDamage`, `OnFlagCaptured`, `SyncPositionAndBroadcast` | deathmatch frag credit `{killerId}` `R` |
| `0x9c` | 10 | `BroadcastMatchWinners` @ `0x00511150` | **match result**: 6 winner bytes `R`. The receiver (`0x004ffc36`) writes `TeamSlots[i]+0x78 = (msg[4+i] != 0)` for `i < NumTeamSlots`; the sender then calls `SetGameSpeed(0.0f)`, i.e. freezes the game |
| `0x9d`/`0x9e` | 20/16 | `EquipObject` | equip variants |
| `0x9f` | 8 | `AiThink_Mine` @ `0x004552a0` | (AI state update) |
| `0xa0` | 6 | `SendLocalizedMessageToOwner` @ `0x00508e70` | **localized message**, `{u32 0xa0, u16 string_id}`. Receiver `0x00502671`: `GetResourceString(string_id)` -> `ConsolePrint`. **Unicast** to the owning player in MP (`FUN_00504ea0` after `FUN_00512940`); only in loopback does it go through `BroadcastToPlayers` |
| `0xa1` | 5 | `SendSoundToOwner` @ `0x00508f60` | **UI sound**, `{u32 0xa1, u8 sound_id}`. Receiver `0x0050268c`: `PlayUiSound(sound_id)`. Unicast in MP, same as `0xa0` |
| `0xa2` | 35 | `ExecutorThreadProc` | **game rules + scoreboard sync** (see §8.9). Applied on the client by `ApplyMissionStatsUpdate` @ `0x005029d0`, and **not through the switch** — its byte-map entry is the default arm, so the client pump `FUN_004fdd60` calls it directly, and it applies only when `!IsExecutorRunning()` (i.e. on joiners) |
| `0xa3` | 12 | `MobileActor::DropItem` @ `0x00538240` | dropped item spawned `{actorId of the spawned pickup, quantity}`, unreliable; paired with the `0x7d` that removes it from the dropper |
| `0xa4` | 4 | `ExecutorThreadProc` (case 0x35) | (flag broadcast) `R` |
| `0xa5` | 8 | `DeleteTeam` | delete team |
| `0xa6` | 12 | `ApplyArmorDamage`, `ApplyShieldDamage` | armor/shield value update |
| `0xa7` | 16 | `ApplyShieldDamage` | shield damage |
| `0xa8` | 16 | `ApplyArmorDamage` | armor damage |
| `0xa9` | 73 | `CommandSetTrack` | set camera track: 4 parsed positions + carries-passengers flag `R` |
| `0xaa` | 56 | `CommandCameraTrack` | camera track path (parsed positions + float) |
| `0xab` | 12 | `CommandSetSpeed` | set track/animation speed |
| `0xac` | 12 | `CommandSetLoopTime` | set loop time |
| `0xaf` | 8 | `TrackObjectActor::SetMoveDirection` @ `0x00548760` | **door/platform direction change** `R`. Sent only on the reverse-in-place branch, which mirrors the animation start time about the tick clock |
| `0xb0`/`0xb1` | 8 | `TrackObjectActor::Pause` @ `0x005488f0` / `TrackObjectActor::Resume` @ `0x005489f0` | **pause / resume a moving platform**, both `R`. The console `PAUSE`/`UNPAUSE` commands; `Pause` stamps `TrackObjectActor+0x1a0` with the clock and `Resume` advances `+0x198` by the paused span |
| `0xb2` | 8 | `EvaluateTriggers` | trigger effect |
| `0xb4` | 8 | `CommandTrack` | camera track |
| `0xb5` | 8 | `CommandBoard` | board |
| `0xb6` | 24 | `EvaluateTriggers`, `AiBeginInvestigate` @ `0x0045e050` | trigger spawn/effect; from the AI it is the "go investigate" reveal `{0xb6, Vec3 pos, 20.0f, 0x00030001}` = kind 1, life 3 s, unreliable |
| `0xb7`/`0xb8` | 8 | `CommandDefogger`/`CommandFogger` | map defog/fog |
| `0xb9` | 12 | `Actor_SetHeapDropRole` @ `0x0052f2d0` | set the actor's heap-drop role `{u32 0xb9, u32 actorId, u32 roleId}` `R` |
| `0xba` | 12 | `CommandAnim`, `Actor_FixupAfterLoad` @ `0x005317b0`, `Frag` | play animation. The fixup pass emits `{0xba, actorId, 0x65}` under a **hardcoded one-level special case**: only when the actor's role name is `frend`, its team is 0 and the current level is `level04.gls` |
| `0xbb` | 8 | `CommandRemoveBB` | remove billboard |
| `0xbc` | 8 | `CommandOpenDoor` | **open door** `{doorId}` `R` (see §8.4) |
| `0xbd` | 8 | `CommandCloseDoor` | close door `R` |
| `0xbe` | 8 | `CommandSmoke` @ `0x00448640` | start the smoke effect on an actor `{actorId}`. Sent with zero coords, so the relevance filter always matches and a `SMOKE` reaches every player |
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
`0x38,0x3f,0x40,0x43,0x47,0x4a-0x4c,0x4e,0x5e,0x69,0x6c,0x6d,0x73,0x76,0x77,0x79,0x7a,0x8f,0x90,0x94,0xad,0xae,0xb3,0xc5-0xc7`
— these are additional actor/AI/effect updates in the same style (small fixed payloads, mostly
`{actorId, …}`), several produced by helpers whose id constant is set via non-standard codegen.
**Do not assume "small fixed payload" for anything on this list** — `0x67` sat here and turned out
to be a variable-length script-execution message (§8.11).

`0x3f`/`0x40` are **PROPOSED**, not measured, as the attack-*position* counterpart of the measured
`0x41`/`0x42` pair: the symmetry is that `Actor` slot 97 `AttackPosition` computes its broadcast id
the same way slot 96 does, and the client-side applier would be `Unit` slot 109
`Unit_SetAttackPosition` @ `0x004c3170`. What would settle it is finding the `ApplyUpdateMessage`
arm that reaches `[reg+0x1b4]`; it is not a `CALL dword ptr [reg+0x1b4]`, so look for a
`MOV EAX,[reg+0x1b4] ; CALL EAX` pair. Until then the two ids stay in the undecoded list above.

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
+0x0c u32     teamId           (consumed as a single byte)
+0x10 u32     ownerRef         (an owner UNIT ID PLUS ONE; 0 = none)
+0x14 f32[3]  position
+0x20 f32[4]  orientation quaternion
```

**The receiving side, and the reason no class tag is on the wire.** The handler is at
`0x004ff934`: it reads `roleId` at `msg+0x08`, resolves it with `GetRoleById` @ `0x004ae0d0`, and
hands the resulting `Role *` to `CreateUnit` @ `0x004fd450` — `int __fastcall(int team /*ECX*/,
Role * /*EDX*/, Vec3 *pos, Vec4 *quat, uint unit_id, uint owner_ref)`, `RET 0x10`, returning the new
`Unit+0xc`. `CreateUnit` then switches on **`role->ai`** (21-entry jump table at `0x004fd8d4`) and
picks one of the sixteen client `Unit` classes itself, so the client **re-derives the class from its
own roles hash** and the packet never names one. A peer with a different `Role` table therefore
builds a different class from the same bytes.

`ownerRef` being id + 1 is what makes 0 usable as "none": the constructors themselves take **-1**
for none, so the factory passes `owner_ref - 1` through.

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
+0x08 ...     serialised final state - a MotionSnapshot, filled by
              MobileActor::WriteMotionSnapshot @ 0x0053bb00 (see §8.12)
```

Related: `PresidentActor::ApplyDamage` also emits the death-effect `0x8b`
`{actorId, seq(num_actors++), variant}` (16 B, unreliable) and, in Deathmatch, the frag-credit
`0x9b` `{killerTeam/id}` (8 B, reliable).

### 8.6 Spawn projectile / weapon fire - update `0x46` ('F'), 64 bytes

`SpawnProjectileActor` @ `0x00503bd0`. **Unreliable.**

```
+0x00 u32     id = 0x46
+0x04 u32     attacker actorId
+0x08 i32     TARGET actorId        (-1 for a position shot)
+0x0c u32     projectile roleId
+0x10 u8      team                  (a byte, not the dword at +0x08)
+0x18 f32[3]  position
+0x24 f32[4]  direction, as a QUATERNION
              ... remaining params to 0x40
```

**`+0x08` is the target, not the team**, and the team is the byte at `+0x10`. The earlier reading
of this packet had the team at `+0x08` and described `+0x10` onwards as an undifferentiated blob
of "position + direction/velocity + damage". The corrected layout was recovered from the sender's
stack slots, which sum to exactly 0x40 — that total is the check that the field list is complete.
Note the direction is a **quaternion at `+0x24`**, not a direction vector; `combat_notes.md` has
the fire path that fills it.

**What the client builds from it is `ProjectileUnit`** — vtable 0x00664c88, 94 slots, size 0x180,
ctor 0x004c47e0 (`__thiscall`, 9 stack args, `RET 0x24`). The spawn is at `ApplyUpdateMessage`
@ `0x004fec38` and is **gated on `role->projectile != 0`**. This is the one client class reachable
from **neither** spawn factory: `CreateUnit` returns 0 for `role->character == 0 &&
role->projectile != 0` (`0x004fd826`/`82a`/`82c`) and `ClientSpawnActorForTeam` returns 0 for a
projectile role (`XOR EAX,EAX` @ `0x004fd397`). The only other builder is `LoadGame` @ `0x00506373`.
The ctor takes the owner unit id in its **7th stack argument** (`[EBP+0x20]`, -1 = none), looks it up
in the client units hash and addrefs it into `ProjectileUnit+0x170`.

**One more piece of client structure is built from an update rather than locally: the centipede
segment chain.** `ApplyUpdateMessage` @ `0x005006cc` resolves two unit ids through `GetUnitById`
@ `0x0044e070` and calls `CentibodyUnit::SetNextSegment` @ `0x004cbc30`, which writes the refcounted
`Unit *` at `CentibodyUnit+0x2e0`. That setter has no other caller, so nothing on the client links
the chain by itself. **The update id for that call site was not established** — it is the arm at
0x005006cc, and identifying which id reaches it is still open.

### 8.7 Movement / waypoint order - command `0x3b`/`0x3d` (server), 56 bytes

The **updates** `0x3b`/`0x3c` are also 56 bytes but are documented in §8.12, where their body is
measured from the writer rather than from this handler.

Command handler (executor cases `0x3b`/`0x3d`) reads:

```
+0x00 u32     id
+0x04 u32     actorId
+0x08 u32     flag                 (0x3b: clears +0xE0 flag & does door proximity; 0x3d: sets it)
+0x0c f32[3]  target position       -> SetPositionAndOrientation
+0x18 f32[4]  target orientation quat
+0x28 f32[3]  waypoint              -> NavAgent velocity (+0x1c/+0x20/+0x24)
+0x34 f32     delay / alarm
```

Worth noticing but **not yet concluded**: those offsets line up field-for-field with the
`MotionSnapshot` of §8.12 placed at `+0x08` — `+0x08` would be its `time`, `+0x28` its `velocity`
(which is exactly what this handler does with it), and `+0x34` its `nav_poly_id` rather than a
delay. The sender of this command (`FUN_004ad050`) has not been read, so the labels above are left
as the handler's own reading; if someone reads that sender, this is the first thing to check.

### 8.8 Chat / player name - command `0x28` & update `0x8d`, 259 bytes

`SendChatOrPlayerName` @ `0x004fcd00` (send) / executor case `0x28` (re-broadcast as `0x8d`).
**Reliable.**

```
+0x00 u32       id = 0x28 (to server) / 0x8d (to clients)
+0x04 char[255] null-terminated text (chat line or player name)
```

Every copy on this path is an unbounded inline `strcpy` that terminates only on a NUL byte — the
sender's own, the executor's copy to the lobby-name buffer `0x007b7150`, and the `0x8d`
re-broadcast's. §6.1 has the instruction addresses.

### 8.9 Game rules + scoreboard - update `0xa2`, 35 bytes

`ExecutorThreadProc` builds it from globals. The client applier is `ApplyMissionStatsUpdate`
@ `0x005029d0`, reached from the client pump `FUN_004fdd60` rather than from the update switch
(§7). It guards on `msg[0] == 0xa2`, copies the block out only `if (!IsExecutorRunning())` — i.e.
joiners apply it, the host does not — and then unconditionally sets `StatsScreenClientsReady = 1`.

The payload is **packed and unaligned**: three bytes followed by seven dwords at odd offsets.

```
+0x00 u32   id = 0xA2
+0x04 u8    DifficultyHealthToggle
+0x05 u8    IsFriendlyFireOn
+0x06 u8    AreFriendlyMinesOn
+0x07 u32   MissionShotsFired
+0x0b u32   MissionShotsHit
+0x0f u32   MissionShotsFiredTeam2
+0x13 u32   MissionShotsHitTeam2
+0x17 u32   MissionTimeSeconds
+0x1b u32   MissionResurrectPenaltyA
+0x1f u32   MissionResurrectPenaltyB
```

`0x1f + 4 = 0x23 = 35`, which is the check that the field list is complete.
`AreFriendlyMinesOn` @ `0x006abe1c` is read back by `AreFriendlyMinesEnabled` @ `0x00512a40`
(11 bytes: `CMP dword ptr [0x006abe1c],0 ; SETNZ AL ; RET`) — it is a **game-rule option arriving
over the wire**, which is worth knowing because `gadgets_notes.md` cites that function as a
line-of-sight test in the mine sweep. It performs no geometry and takes no argument.

### 8.10 Turn / step - update `0x87`, 4 bytes

`ExecutorThreadProc`. **Reliable**, broadcast to all. Payload is **just the id** (no body). Sent
once at executor start and each time all tracked objects report idle (lock-step advance; see §9).

### 8.11 Run script file - update `0x67` ('g'), variable length

`QueueScriptExecution` @ `0x00505080`. **Reliable**, broadcast to every player, and it reaches
all of them unconditionally — both of `BroadcastToPlayers`' drop mechanisms are bypassed:

- **Relevance filtering**: the caller passes `coords = {0,0,0}`, and the filter's first branch is
  `coords.x == FloatZero && .y == && .z ==`. `FloatZero` @ `0x007f5f40` lives in uninitialized `.data` and
  has exactly one writer in the binary (`StaticInit_FloatZero` @ `0x0043ab80`, a CRT static
  initializer doing `FILD 0` / `FSTP` / `MOVSS` at `0x0043ab9d`), i.e. it is the shared **0.0f**
  sentinel. A zero vector therefore always matches and `IsRelevantToPlayer` is never consulted.
- **Backlog throttling**: the guard is `if ((backlog < limit) || (guaranteed != 0))`. This message
  is sent with `guaranteed = 1`, so it short-circuits past the per-player backlog counters and the
  ~1-in-10 random-sample path entirely. Script broadcasts are never dropped.

```
+0x00 u32     id = 0x67          // 'g' in byte 0, bytes 1-3 explicitly zeroed
+0x04 char[]  script filename    // NUL-terminated; length from strlen_plus1 (INCLUDES the NUL)
```

Total size = `strlen(name) + 1 + 4`.

> **GkPlus always sends a JSON document in that field, never a bare name** (`src/ScriptQueue.cpp`,
> and the section in `CLAUDE.md`). A JSON string is the name — `"crtbaa.gcs"` — and anything else
> is a message delivered to a script level's `message_received`, so this message id is now the
> transport for script-to-script events as well as for file names. The framing is untouched: it is
> still one NUL-terminated string measured with `strlen_plus1`, so a payload merely gets longer.
>
> Interoperation goes both ways, and asymmetrically. A GkPlus recipient runs a **non**-JSON payload
> as a file name, which is exactly what a vanilla host sends — and since every local push goes
> through the hook, receiving one is now the *only* way an invalid payload can occur, which makes it
> a reliable "that peer is not running GkPlus" signal. In the other direction a vanilla recipient
> gets `"crtbaa.gcs"`, quotes included, tries to open a file by that name and fails; a message it
> would not have understood in any encoding.

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

### 8.12 `MotionSnapshot` - the 0x30-byte body shared by updates `0x39`/`0x3b`/`0x3c`/`0x5f`

Four of the large actor updates carry one common record, and identifying its writer settles all
four payloads at once. `MobileActor::WriteMotionSnapshot` @ `0x0053bb00`
(`__thiscall void(MobileActor *, MotionSnapshot *out)`, `RET 0x4`, 122 bytes, no callees) fills it,
and every field is pinned to an instruction:

```
+0x00 f32     time            <- Actor+0xd8       (0053bb06 MOV EAX,[ECX + 0xd8])
+0x04 f32[3]  position        <- Actor+0xa0..0xa8 (0053bb0e MOVQ, 0053bb1b MOV)
+0x10 f32[4]  orientation     <- Actor+0xac       (0053bb24 MOVUPS, 16 bytes, a quaternion)
+0x20 f32[3]  velocity        <- nav_agent+0x1c..0x24, nav_agent = Actor+0x200
+0x2c u32     nav_poly_id     <- nav_poly(+0x14)->+0x20, LOW 24 BITS ONLY (top byte preserved);
                                 0xffffff when nav_agent->nav_poly is NULL
```

`MotionSnapshot` is now a type in the Ghidra DB. `Actor+0xd8` is the **game time of this tick** in
seconds, confirmed from its writer `Actor::SyncPositionAndBroadcast` @ `0x0052f91a`; `Actor+0xdc` is
the same value at the last broadcast (`0x0052fa66`), i.e. broadcast dirty-tracking. All four
senders below end with `+0xdc = +0xd8`.

| update | size | layout |
|--------|------|--------|
| `0x39` | 0x50 = 80 | `{u32 0x39, u32 actorId, MotionSnapshot, Vec3 destination, Vec3 current_waypoint}` |
| `0x3b` | 0x38 = 56 | `{u32 0x3b, u32 actorId, MotionSnapshot}` |
| `0x3c` | 0x38 = 56 | `{u32 0x3c, u32 actorId, MotionSnapshot}` |
| `0x5f` | 0x3c = 60 | `{u32 0x5f, u32 actorId, s32 speed_delta, MotionSnapshot}` |

For `0x39` the `current_waypoint` comes from the head node of the actor's waypoint list
(`MobileActor+0x204`) when its count `+0x208` is non-zero, and otherwise from the goto target
`+0x1dc` or the actor's own position `+0xa0`.

**Reliability, measured from the third argument of `BroadcastToPlayers` at each site:** `0x39`,
`0x3b` and `0x3c` pass `0`, i.e. **unreliable**; `0x5f` passes `1`, i.e. **reliable**. Read it off
the call site rather than assuming it from the message's importance — for this family the two do
not agree, and a position update being unreliable while a dead code path is guaranteed is exactly
the shape that gets copied wrong.

**`0x5f` is never emitted by the shipped binary.** Its sender `MobileActor::AddWalkingSpeed`
@ `0x00539ed0` has no callers, and a byte scan of the whole `0x00600000+` range for the
little-endian pointer finds no reference in `.rdata` or `.data` either, so it is not a vtable slot.
The same scan found `PickupActor::SetPickupEnabled` @ `0x00668528` as its control, so the method is
known to work. Treat `0x5f` as a reachable-only-by-patching message.

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
| `0x005116a0` | func | `EnsureDirectPlayInterface` (lazy `CoCreateInstance`) |
| `0x005119f0` | func | `EnumerateServiceProviders` (`EnumConnections`, TCP/IP + IPX only) |
| `0x005119a0` | func | `TestServiceProviderConnection` (the per-provider filter) |
| `0x00511b50` | func | `InitializeDirectPlayConnection` |
| `0x00512080` | func | `BuildModemCompoundAddress` — **unreferenced / dead** |
| `0x00512160` | func | `EnumerateDPlaySessions` (session browser) |
| `0x00512b90` | `StdCall` | `DPlayEnumSessionsCallback` |
| `0x00512a50` | `StdCall` | `DPlayEnumConnectionsCallback` |
| `0x00512540` | func | `HostMultiplayerSession` (`Open` CREATE) |
| `0x00512370` | func | `JoinMultiplayerSession` (`Open` JOIN) |
| `0x005126b0` | func | `CreateLocalClientPlayer` |
| `0x00502db0` | func | `StartExecutorThread` (server player + group) |
| `0x00511700` | func | `ShutdownMultiplayer` (four list drains, then `Release`) |
| `0x005046e0` | func | `ReapDroppedPlayers` (updates `0x50` + `0x91`) |
| `0x00505380` | `FastCall<void, float*>` | `SetGameSpeed` (updates `0x88`/`0x89`) |
| `0x00502ab0` | func | `SendClientTimeSync` (command `0x36`, 2 s throttle) |
| `0x005029d0` | `FastCall<void, int*>` | `ApplyMissionStatsUpdate` (client applier for `0xa2`) |
| `0x00511250` | `FastCall<bool, int, Vec3f>` | `IsRelevantToPlayer` (spatial relevance filter) |
| `0x0053bb00` | `ThisCall<void, MotionSnapshot*>` | `MobileActor::WriteMotionSnapshot` (§8.12) |
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
| `0x0050bae0` | data | executor command byte index-map (indexed by `id - 4`) |
| `0x0050ba3c` | data | executor command pointer table (41 slots; slot `0x28` = `0x0050addc` is the default arm) |
| `0x007b6ddc` | u32 | `KeyModifierState` — bit `0x2` (CTRL) is the wire `close_range` selector |
| `0x00666234` | float | `CloseRangeSpeedFactor` = 0.65f |
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
  jump table (§7 tail), and the `0xc8` resync payload. The client-apply handlers live in an area
  Ghidra left as undefined bytes inside `ApplyUpdateMessage`; `disassemble` was applied to the ids
  inspected here, but a full sweep of every handler was not run.
- **Open, and named so the next pass can aim at it:**
  - The lobby ready-flag base — `DAT_007b7144[1..4]` here versus `LobbyPlayerReady[4]`
    @ `0x007b7148` from the lobby poll. See the note at the end of §6. Reading `FUN_004f10b0` (the
    lobby draw) settles it, and the DB is deliberately not typed over that range meanwhile.
  - What memory command `0x2a`'s write actually lands in. The formula is measured; the object is
    not. `0x007b70dc` is `MaxUnitsPerTeam[6]` and `0x007b70e4` is its element [2], so with
    `arg0 >= 1` the write lands at `0x007b70f8 + (arg0-1)*20 + arg1*4` — and `0x007b70f8 + 4*20`
    is exactly `0x007b7148`, hinting at an `int[4][5]` reached through a base biased by one row.
    Not proved.
  - Update ids `0x60`/`0x61` — sender identified (`AiThink_Bot`), payloads not decoded. `0x63` is
    settled at three of its sites: `{id, actor_id}`, emitted on entering the bot's reacquire state,
    paired with `0x62` on leaving it (see the row in §7 and `ai_behaviour_notes.md` §11). What is
    left there is the header/payload split — the `Vec3` those calls pass by value was read off the
    call shape, not out of `BroadcastToPlayers` — and whether `0x60`/`0x61` carry the same pair.
  - Update ids `0x3f`/`0x40` — the PROPOSED attack-position pair; see the end of §7.
  - Command `0x17` — whether it is *give item* (`inventory_notes.md` §12) or *board*
    (`orders_notes.md` §8, kind 6, `QueueBoardOrder` @ `0x0050a2e8`). The two files disagree and
    the sender is agnostic; the executor arms `0x0050a22f` / `0x0050a2e8` settle it. What **is**
    settled is the wire layout: the sender writes `{id, giver, arg_b, arg_a, f32 game_time, arg_c}`
    — note the fourth wire dword is the **game-time stamp**, not a payload field, and that the wire
    order is not the C argument order.
- **The §7 tail list is not safe to gloss.** `0x67` sat in it, described as one more small
  fixed-payload actor update; it is in fact a variable-length message that makes every client
  execute a local script file (§8.11). Its omission also produced a wrong claim in §9 (that
  scripts run host-only) which stood until the handler was actually read. Treat the remaining
  undecoded ids as unknown-shape, not as assumed-small - and decode the handler before asserting
  anything about what does or does not cross the wire.
