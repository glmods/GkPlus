# Gunlok Threading Model - Reverse Engineering Notes

## Overview

Gunlok runs **two game threads plus an optional music thread**. Single-player is
architecturally a client/server game running in one process: the OS main thread acts as
the *client* (window, input, rendering, UI, console, script execution) and a dedicated
worker thread — whose id is stored in the global Ghidra has named `ExecutingThread` —
acts as the *server* (simulation authority: applies game-state commands, ticks actors,
evaluates triggers). The two sides talk through thread-safe message queues in
single-player and through DirectPlay in multiplayer, using the **same code paths**.
Additional threads exist inside BINKW32, DirectSound, DirectPlay, and the Steam client
libraries, but the game itself only ever creates the two below.

```
Main thread ("client")                     Executor thread ("server")
======================                     ==========================
WinMain PeekMessage loop                   ExecutorThreadProc @ 0x00509050
- input, window messages                   - WaitForMultipleObjects({kill, pause,
- rendering (D3D8 BeginScene etc.)           msg-available}, timeout=50ms)
- per-GameState tick functions             - pops game commands: loopback queue (SP)
- console UI, ExecuteCommandLine              or IDirectPlay4::Receive (MP)
- pops script queue -> ExecuteCommandFile  - big switch dispatch on message id
  (host only; joiners get scripts          - per-actor updates, EvaluateTriggers
   via update 0x67 in ClientReceivePump)   - sends updates:
- pops update queue  -> render-state sync      queue 0x007ba38c (SP)
- sends commands:                              or IDirectPlay4::SendEx (MP host)
    queue 0x007ba32c (SP)                  - queues trigger scripts -> 0x007ba35c
    or IDirectPlay4::SendEx (MP client)      AND broadcasts them as update 0x67
```

## Thread 1: Main thread

`WinMain` @ `0x0046aef0`. Classic `PeekMessageA` game loop:

- No message pending: if the app is active, set the FPU control word
  (`__controlfp(0x20100, 0x30300)` — every frame) and call the tick function for the
  current `GameState` (switch at the bottom of WinMain; state 2 = `FUN_0046cc80`,
  states 5/6/7 = in-game tick `FUN_0046e6c0`, 0x10/0x11 = `LoadLevel3`, ...).
  If inactive, `Sleep(0)`.
- Message pending: `GetMessageA`/`TranslateMessage`/`DispatchMessageA`. Time spent in
  message handling is measured and subtracted from the game clock (`DAT_00738f80`).

The in-game tick `FUN_0046e6c0` (GameState 5/6/7) does, per frame:
camera/UI updates, `BeginScene` + rendering, `RunQueuedScript` (pop one script filename
from queue `0x007ba35c` and run `ExecuteCommandFile` on it — **trigger scripts execute
on the main thread, one per frame**), and `ClientReceivePump` (drain the update queue /
DirectPlay receive — see Message flow below).

Both of those last two can run scripts, and which one does depends on the machine's role.
`RunQueuedScript` is the **host** path (via `ScriptQueue`, throttled to one per frame).
`ClientReceivePump` is the **joiner** path: update `0x67` carries a script filename and its
handler calls `ExecuteCommandFile` inline, unthrottled, bypassing `ScriptQueue` entirely.
Either way execution lands on the main thread.

The **call order within the tick is fixed and matters**: `RunQueuedScript` @ `0x0046e6d4`
precedes `ClientReceivePump` @ `0x0046e7ba`. A host therefore runs its one queued script
*before* applying that frame's updates, whereas a joiner runs scripts *interleaved with*
them. See `directplay_protocol_notes.md` §8.11 for the full host-vs-joiner scheduling table.

`ClientReceivePump` drains differently depending on session mode, and the familiar
"≤50 per frame" figure is **multiplayer-only**:

- `ClientDPlay == NULL` (single-player / loopback): `WakeExecutor()`, then pop `UpdateQueue`
  in a loop **until it is empty** — no cap.
- `ClientDPlay != NULL` (MP): read the queue depth through vtable `+0xC8`, clamp to `0x32`,
  then that many `DPlayReceive` calls.

Either way each message is applied by `ApplyUpdateMessage` and then `free`d by the pump — which is
the concrete demonstration that `MsgQueue_Pop` hands ownership of the payload to its caller.

DllMain of d3d8.dll (and therefore GkPlus init) also runs on this thread:
the game loads d3d8.dll from `InitD3DAndSetMode`, called from WinMain.

## Thread 2: Executor thread ("server")

The only game-created worker thread. Ghidra names its id global `ExecutingThread`.

| Item | Address | Notes |
|------|---------|-------|
| Start | `StartExecutorThread` @ 0x00502db0 | `__fastcall(IDirectPlay4*)`; param NULL in single-player |
| Stop | `StopExecutorThread` @ 0x00502ee0 | signals kill event, waits for ack event |
| Thread proc | `ExecutorThreadProc` @ 0x00509050 | priority `THREAD_PRIORITY_HIGHEST` (2) |
| Thread id | `ExecutingThread` @ 0x007b9d7c | out-param of CreateThread |
| Running flag | `ExecutorRunning` @ 0x007b9df0 | **this is the global GkPlus currently exposes as `gk.misc.foobar`** |
| Getter | `IsExecutorRunning` @ 0x00502da0 | checked by ~100 gameplay/command functions |

`StartExecutorThread(dplay)` creates five auto-reset events, optionally binds a DirectPlay
session (CreatePlayer with the msg-available event, CreateGroup), then CreateThread.
Started on every level start (`FUN_004e2560` @ 0x004e2560, reached from briefing-end /
load-save paths) — **the executor thread runs in single-player too**. Stopped on level
end / FMV / credits / next-level (`FUN_004e2710` @ 0x004e2710).

Thread proc structure (loop):

1. `WaitForMultipleObjects(3, {kill, pause-request, msg-available}, FALSE, 50ms)`
2. index 0 (kill): drain queues, Release DirectPlay, close events, clear running
   flag, `SetEvent(ack)`, return.
3. index 1 (pause-request): `SetEvent(paused-ack)`, `WaitForSingleObject(resume,
   INFINITE)` — see Pause handshake.
4. index 2 / timeout: receive messages — `DPlayReceive` (`IDirectPlay4::Receive`
   wrapper, handles `DPERR_BUFFERTOOSMALL`/`DPERR_NOMESSAGES`) when a DirectPlay
   session exists (`ServerDPlay != 0`), else pop `CommandQueue` @ 0x007ba32c —
   and dispatch each on a big message-id switch (spawn, death, damage, doors, chat,
   full-state sync, ...). Then per-actor updates and `EvaluateTriggers` @ 0x0050ccc0.
   **`EvaluateTriggers` is called from this thread only.**
5. "Turn" logic: when all tracked objects report idle (vcall +0x18), the thread
   computes a deadline and broadcasts message `0x87` (step/turn advance; also sent
   once at thread start).

Sets its own FPU control word at startup (`__control87(0x20100, 0x30300)`), matching
what WinMain sets per-frame — float determinism across both threads.

### Events (all created by StartExecutorThread unless noted)

| Address | Role |
|---------|------|
| 0x007b9df4 | msg-available (DirectPlay player event in MP; `SetEvent` by main thread in SP via `FUN_00505280` to wake the executor early) |
| 0x007b9df8 | kill request |
| 0x007b9dfc | kill ack (created on demand by StopExecutorThread) |
| 0x007b9e00 | pause request (main -> executor) |
| 0x007b9e04 | resume (main -> executor) |
| 0x007b9e08 | paused ack (executor -> main) |

## Thread 3: Music streaming thread

Bink is used for music playback (music tracks are Bink audio files, routed to
DirectSound via `BinkSetSoundSystem(BinkOpenDirectSound, ...)`).

- `PlayMusicTrack` @ 0x00587b60: BinkOpen the track, CreateThread(`PlayMusicThread` @
  0x00587a90) at priority `THREAD_PRIORITY_ABOVE_NORMAL` (1). Music object fields:
  +0x04 Bink handle, +0x08 thread id, +0x0C thread handle, +0x10 stop-event handle
  (doubles as the stop flag), +0x1C loop flag.
- `PlayMusicThread`: loop `BinkWait`/`BinkDoFrame`/`BinkNextFrame` with `Sleep(20)`
  between frames until the stop field becomes nonzero, then `SetEvent` on it and exit.
- Stop (`StopMusicTrack` @ 0x00587bf0, thunk 0x00587b50): create event into +0x10, wait
  for the thread to signal it, `BinkClose`.

### Music volume, and the startup bug

The track object is 0x20 bytes (the C++ mirror is `MusicTrack` in `src/Music.cpp`);
`MusicTrack_Ctor` @ 0x00587b10 zeroes it, stores the sound device at +0x14, and sets
**+0x18 = 0x8000 hardcoded** — the volume, at Bink full scale.
`MusicTrack_SetVolume` @ 0x00587ca0 writes +0x18 and calls `BinkSetVolume` when a handle is
open; `PlayMusicTrack` re-applies +0x18 on open, so setting it before playback also works.

Two scales, two globals — both *precomputed*, which is the whole problem:

| Global | Offset | Formula | Range | Consumer |
|--------|--------|---------|-------|----------|
| `CDMusicVolumeScaled` | 0x006a79dc | `CDMusicVolume * 0x1c70` | 0..65520 | CD/aux path |
| `BattleMusicVolumeScaled` | 0x006a7c18 | `BattleMusicVolume * 0xe38` | 0..32760 | Bink track |

Their `.data` initializers are 36400 and 25480 — i.e. baked from the *default* settings
(`CDMusicVolume` 5, `BattleMusicVolume` 7 @ 0x006abe04/08).

`ApplyMusicVolumeSettings` @ 0x004e7810 is the only function that re-derives both from the
settings, and it is called **only** from `LoadLevel` and `CommandCDAuto`. `ReadGLKeys` @
0x004f6f10 (WinMain) loads the settings into the globals but never recomputes the scaled pair
and never touches `TheMusicTrack` @ 0x007f5bdc. Contrast `SoundEffectsVolume`, which
`InitSoundEffectsSystem` @ 0x004e7070 reads *lazily* at device init — that one honours the
config file.

Consequence (a real shipped bug): `EnterMainMenuScreen` @ 0x004e7e50 constructs the track and
immediately plays `sound\music\track1.bik` on a `TheMusicTrack == NULL` guard, so the front-end
music runs at the constructor's 0x8000 — above even the slider's own maximum of 32760 —
regardless of the saved level. Audio menu item 1 (`OnMenuItemClicked`, menu 25) calls
`MusicTrack_SetVolume(TheMusicTrack, BattleMusicVolume * 0xe38)` on the live track, so the
slider works immediately and `WriteGLKeys` persists it; it is simply never re-applied on the
next launch. `LoadLevel` repairs it, so in-game music is correct — the window is startup up to
the first level load.

The same defect covers three more call sites that also construct-and-play without setting a
volume: `FUN_0046f020` and `FUN_0046d0f0` (`2a.bik`) and `FUN_004db3c0` (`victory.bik`). The
three call sites that *do* set their own volume are cinematics (`FUN_004dd4b0`,
`CinematicsVolume * 0xe38`), the Audio-menu slider, and battle music (`FUN_004e7a40`).

GkPlus fixes all four by detouring `MusicTrack_Ctor` in `MusicModule` (`src/Music.cpp`) and
seeding `volume` from `BattleMusicVolume` instead of the hardcoded 0x8000; the three
volume-setting callers overwrite it immediately and are unaffected. Hooking `ReadGLKeys` does
**not** work — it returns before `WinMain`'s copy-out at 0x0046b667, so the volume globals still
hold their `.data` defaults, which is the same ordering trap the game itself falls into with
`ApplyShadowQuality`.

## Message flow (the core of the model)

Two DirectPlay interface pointers exist because one process can be both client and
server (SP always, MP when hosting):

| Global | Side | Used by |
|--------|------|---------|
| `ServerDPlay` @ 0x007b9dec | executor/server session | thread proc Receive, `BroadcastToPlayers` SendEx |
| `ClientDPlay` @ 0x007b9d60 | main/client session (+ `ClientPlayerId` @ 0x007b9d64 = the local player's DirectPlay id; the server is player id 1) | `ClientReceivePump` receive pump, `SendToServer` SendEx |

When a pointer is NULL, the corresponding traffic goes through in-process loopback
queues instead (see below). `ClientRoutingActive` @ 0x007b9d68
(`IsClientRoutingActive` getter): when set, command handlers only *enqueue* their message and
let the executor apply it; when clear they also apply the effect directly.
`MultiplayerActive` @ 0x007b9e74 is the `IDirectPlay4*` of the multiplayer session
(NULL outside MP; `IsMultiplayerActive` just checks non-NULL).

### Host vs joiner: only the host runs the executor thread

`StartExecutorThread` has exactly two callers: the single-player level-start path
(0x004e2560) and `HostMultiplayerSession` @ 0x00512540, which does
`IDirectPlay4::Open(DPOPEN_CREATE)` and then starts the executor with the session
interface. The join path (`JoinMultiplayerSession` @ 0x00512370,
`Open(DPOPEN_JOIN)`) never starts it. So a **joining client runs only the main
thread**: `CreateLocalClientPlayer` @ 0x005126b0 creates its normal DirectPlay player
and calls `InitClientRouting`; commands go to the host over the network
(`SendToServer`, idTo = `DPID_SERVERPLAYER` = 1 — the executor's player is created
with `DPPLAYER_SERVERPLAYER`) and world updates come back through
`ClientReceivePump`. The host's main thread is itself just another client of its own
executor, attached to the same session. Consequences: triggers only evaluate on the
host, and on a joining client `IsExecutorRunning()` is false, so the `Command*`
handlers no-op.

**Trigger scripts, however, run on every machine — not just the host.**
`QueueScriptExecution` does *two* things: it pushes the filename onto the local
`ScriptQueue` **and** it broadcasts update `0x67` ('g') carrying that filename to all
players. Each client then runs `ExecuteCommandFile` on its **own local copy** of the
file, from the `case 0x67` arm of the update applier `ApplyUpdateMessage`, guarded by
`if (!IsExecutorRunning())` so the host does not run it twice. That path is
synchronous inside `ClientReceivePump` and does **not** use `ScriptQueue`: on a joiner
the queue exists (it is a static global) and is drained every frame by
`RunQueuedScript`, but nothing ever pushes to it, because all seven
`QueueScriptExecution` callers are host-side. See `directplay_protocol_notes.md` §8.11.

So two clients with differing `Scripts\` contents *will* diverge. The `IsExecutorRunning()`
gate in the `Command*` handlers bounds this, but **only partially — 97 of 249 handlers are
gated; 152 execute fully on a joining client** (measured transitively, converging at depth 2;
see `directplay_protocol_notes.md` §8.11 for the breakdown).

The gate tracks *actor/world authority*: `CommandSpawn`, `CommandGive`, `CommandOpenDoor`,
`CommandTeleport`, `CommandSetActorArmor` and `CommandAddTrigger` are gated, so authoritative
actor state still arrives only through the host's update stream. But `CommandSet` / `CommandInc`
(**the token table — the script language's variables**), `CommandCompleteObjective`,
`CommandDoor` and `CommandExplode` are not, so a divergent client script mutates local tokens
freely, and `CommandIf` reads tokens, so that client's script control flow diverges as well.

Effects stay client-local: on a joiner `ServerDPlay == NULL`, so an ungated handler calling
`BroadcastToPlayers` pushes onto the joiner's own loopback `UpdateQueue` and self-applies rather
than reaching the host.

### The three loopback queues

All three are instances of the same 0x30-byte struct. The three globals sit exactly
0x30 apart (0x007ba32c / 0x007ba35c / 0x007ba38c), which pins the size. Total queued
bytes counter @ 0x007ba328.

```c
struct MsgQueue {              // 0x30 — types applied in the Ghidra DB
    RWLock       lock;         // +0x00  ALWAYS taken exclusively; never shared
    MsgQueueList list;         // +0x20
};

struct MsgQueueList {          // 0x10 — same header shape as the menu system's LevelList
    MsgQueueNode *sentinel;    // +0x00  ->next = front, ->prev = back
    int           count;       // +0x04
    void         *last_popped; // +0x08  see ownership note below
    byte          cache_flag;  // +0x0c  only ever cleared, never set, on these instances
};

struct MsgQueueNode {          // 0x10 — game pool alloc; freed via vtbl[0](1)
    void         *vptr;        // +0x00  -> 1-slot vtable @ 0x00669fd4 (scalar deleting dtor)
    MsgQueueNode *prev;        // +0x04
    MsgQueueNode *next;        // +0x08
    void         *payload;     // +0x0c  CRT malloc'd copy of the pushed bytes
};
```

Circular doubly-linked with a sentinel; **push at tail, pop at head** (FIFO).

| Op | Address | Notes |
|----|---------|-------|
| `MsgQueue_Push` | 0x0056d9a0 | `__thiscall(MsgQueue*, void* payload, uint size)`; copies payload |
| `MsgQueue_Pop` | 0x0056da40 | returns `void*` payload, or NULL if empty |
| `MsgQueue_Flush` | 0x0056da80 | frees all payloads + nodes; resets the byte counter |
| `MsgQueueList_PopFront` | 0x0056dc00 | takes `queue+0x20`, **not** the queue base; does no locking of its own |

**Both allocations per entry come out of the game pool.** The **payload** is `malloc`
@ 0x005e3f72 (`MsgQueue_Push` @ 0x0056d9b3) and the **node** is a direct `pool_alloc`
@ 0x00571470 (@ 0x0056d9d0) — and the game's `malloc` is a bare `JMP pool_alloc`, so the
two are the same heap, not the CRT's and the pool's. (An earlier revision of this file
said the payload was CRT `malloc`, "internally locked"; it is not, and the pool's own lock
is compiled out — see below.) Only the queue's own RW lock serialises either.

**Payload ownership: the caller of `MsgQueue_Pop` owns the returned buffer and must
`free()` it** — the pool `free` @ 0x005e3f7b, which is what `RunQueuedScript` calls
(@ 0x00505335). `RunQueuedScript` is the model: `ExecuteCommandFile(name); free(name);`.
Despite the name, `last_popped` is *not* a deferred-free slot on these instances —
scanning every accessor of all three globals shows nothing ever writes it non-zero, so
the `free(last_popped)` calls in Push/PopFront/Flush are dead here. It and `cache_flag`
are generic-container fields this usage never exercises.

`MsgQueueList_PopFront` recovers the just-unlinked node through the *new* front's stale
`prev` pointer before fixing it up — correct, but fragile to reordering.

**`ScriptQueue` has exactly one producer and one consumer, and this was checked exhaustively**
rather than assumed: of the ten references to 0x007ba35c, one is `MsgQueue_Push`
(`QueueScriptExecution`), one is `MsgQueue_Pop` (`RunQueuedScript`), **four are `MsgQueue_Flush`**
(`CommandNextLevel`, `StopExecutorThread`, `LoadGame`, `ExecutorThreadProc` — all discards), two are
the CRT static ctor/dtor pair around its `RWLock`, and one is exception unwind data. Nothing pushes
to it except `QueueScriptExecution`, which is what lets GkPlus define the payload format at that one
hook.

| Queue | Direction | Producer | Consumer |
|-------|-----------|----------|----------|
| 0x007ba32c | commands: main -> executor | `SendToServer` @ 0x004fdbc0 (or SendEx to server in MP) | thread proc |
| 0x007ba38c | updates: executor -> main | `BroadcastToPlayers` @ 0x00504bf0 (or SendEx to all players in MP) | `ClientReceivePump` @ 0x004fdc70, called from the in-game tick; applies each via `ApplyUpdateMessage` |
| 0x007ba35c | trigger script filenames: executor -> main | `QueueScriptExecution` @ 0x00505080 (7 callers, all host-side — see below) | `RunQueuedScript` @ 0x00505310: one `ExecuteCommandFile` per frame on the main thread |

> **Under GkPlus the script-queue payload is always a JSON document, never a bare filename - and so
> is the field it came from.** `ScriptQueueSystem` (`src/ScriptQueue.cpp`) hooks the four *writers*
> of a script-name field (`RegisterTriggers`, `PickupActor::Associate`, `ToRole`,
> `ToReplaceDestructibility`) so a bare name is stored as the JSON string `"crtbaa.gcs"`, and the two
> consumers unwrap it: a JSON string still means "run this `.gcs`", anything else is a message
> delivered to a script level's `message_received` instead. `QueueScriptExecution` is still hooked to
> quote what the writers do not cover - the three sites that queue a literal without storing it,
> `CommandVulnerability`'s field, an old savegame, and a peer without GkPlus. Nothing about the
> queue's own mechanics changes; that hook is on the **executor thread** and the consumers on the
> **main thread**, which is exactly the split this file describes - and the reason the payload codec
> is hand-written instead of QuickJS's.
>
> **A save written this way does not load in an unpatched Gunlok** - `SaveGame` serialises the
> trigger field verbatim. See `save_system_notes.md`.

`BroadcastToPlayers(msg, size, guaranteed, coords)` in MP: per-player send with position
relevance filtering (`FUN_00511250`), per-player backlog counters (`0x007b9d84[i]`),
and probabilistic throttling — when a player's queue is backed up, unreliable messages
are dropped except a ~1-in-10 random sample (using the per-thread RNG). Reliable
messages go `DPSEND_GUARANTEED | DPSEND_ASYNC`, unreliable get a 3000ms timeout.

### Who queues scripts (all seven callers of `QueueScriptExecution`)

Every caller is server-side, by one of two mechanisms — an explicit `IsExecutorRunning()`
guard, or being simulation-authority code that broadcasts. This is why a joining client's
`ScriptQueue` stays empty even though the queue itself exists there.

> **This is a statement about the queue, not about script execution.** It does *not* mean a
> joining client never runs scripts. It does: a joiner reads `.gcs` files from its own disk and
> executes them, just never by way of `ScriptQueue`. The filename arrives as update `0x67` and
> `ApplyUpdateMessage` calls `SetCurrentDirectoryToGLDir(GL_Scripts)` + `ExecuteCommandFile` on
> it directly. What is *partially* limited on a joiner is the script's effects, not whether it
> runs: `IsExecutorRunning()` is consulted **per command, not per script**, and only 97 of the 249
> `Command*` handlers consult it at all. Authoritative actor mutation is gated; tokens, objectives
> and various effects are not. See the breakdown above.

| Caller | Address | What it does | Why it is host-side |
|--------|---------|--------------|---------------------|
| `EvaluateTriggers` | 0x0050ccc0 | fires trigger scripts | sole caller is `ExecutorThreadProc` — **executor-only, proven** |
| `MultiplayerRespawnRole` | 0x0050c8b0 | respawns a role for a team, then queues `CTFRespawn.gcs` / `RTPRespawn.gcs` | sole caller is `EvaluateTriggers` (14 sites) — **executor-only by call graph, proven** |

> **`RespawnRoleList` @ 0x007b9d98 is how the respawn hands the actor to that script.**
> `MultiplayerRespawnRole` appends the `SpawnRole` result to it (`FUN_00511600`, `__thiscall` on the
> list header), and the queued `.gcs` then equips the actor at the head with `GIVE ROLE ID` /
> `GIVE AND EQUIP ROLE ID` — "gives to last respawned actor if it matches role" — and pops it with
> `NEXT RESPAWN ID` @ 0x0044a530. It is a FIFO rather than a single slot precisely because
> `RunQueuedScript` drains one script per frame while several respawns can already be pending, so
> the ids have to stay paired with their scripts. Anything replacing that function must keep
> appending, or the stock script equips the wrong actor.
| `CommandBatchAndBroadcast` | 0x00448400 | console command that runs a script file | body is inside `if (LevelLoadReason != 3) if (IsExecutorRunning())` — **guarded, proven** |
| `Frag` | 0x0052e220 | kill/score credit | Actor vtable slot; broadcasts — *inferred* |
| `SyncPositionAndBroadcast` | 0x0053d8d0 | position sync | Actor vtable slot; broadcasts — *inferred* |
| `OnFlagCaptured` | 0x00533120 | CTF capture; queues `CaptureFlag_team<N>.gcs` | called from `SyncPositionAndBroadcast` @ 0x00533720; broadcasts — *inferred* |
| `OnPickedUp` | 0x00546440 | item pickup; queues the item's `associated_script` | `PickupActor` vtable slot; broadcasts — *inferred* |

#### Where each caller's string comes from

Measured, not inferred: the argument register at each call site, traced back. This is the complete
set of ways a string can reach the queue, and `QueueScriptExecution` has **exactly seven
references, all `UNCONDITIONAL_CALL`** — no data references, so it is in no vtable or function
table and cannot be reached indirectly.

| Caller | Argument | Source of the string |
|--------|----------|----------------------|
| `EvaluateTriggers` | `TriggerData+0x54` | `script_name`, `strdup`'d by `RegisterTriggers` — from a `.gls`/`.gcs` `add trigger`, or `gk::RegisterTriggers` |
| `OnPickedUp` | `PickupActor+0x134` | `associated_script`, set by `Actor` vtable slot 66 (`Associate`) — the console's `ASSOCIATE`, or `actor.associate()` |
| `Frag` | `Destructibility+0x08`, tag 4 | `ReplaceDestructibility::script` — GLS field 0x00 of the "replace destructibility" section (whose keyword is `name`; it is a `.gcs` path, see `role_subobjects_notes.md`) |
| `SyncPositionAndBroadcast` @ 0x0053d8d0 | `[[this+0xc]+0x10]` | `Vulnerability::script` — including the one `AddInterfaceBeamVulnerability` @ 0x00510fe0 synthesises from `Role::interface_beam_script`. **Freed immediately after queueing**, so it fires once |
| `MultiplayerRespawnRole` | stack buffer | the literal `RTPRespawn.gcs` / `TPRespawn.gcs` @ 0x006679d4 |
| `OnFlagCaptured` | stack buffer | the literal `CaptureFlag_team5.gcs` @ 0x00669574 with the team digit patched |
| `CommandBatchAndBroadcast` | `g_ConsoleWordBuf` @ 0x006af5f8 | the console line, via `CopyRemainingArgs` — the `BATCHANDBROADCAST` command ("a bit like BATCH but it tells clients to batch it as well"). The open-ended one: any console input reaches it, including `console.execute()` from a script |

So four of the seven read a **field** and two are compile-time literals; the seventh is the console.
That split is what decides where GkPlus encodes: the four fields have a *writer* to hook
(`RegisterTriggers`, `PickupActor::Associate`, `ToRole`, `ToReplaceDestructibility` — see
`src/ScriptQueue.h`), while the other three build a string and queue it with nothing in between, so
there is nothing to convert and the queue hook handles them.

> **`MultiplayerRespawnRole`'s `IsExecutorRunning()` guard is not what makes it host-side.**
> The guard covers only the `SpawnRole` call; `QueueScriptExecution` runs unconditionally below
> it. What proves it executor-only is the call graph — its sole caller is `EvaluateTriggers`.
>
> Note also that `SyncPositionAndBroadcast` names **11 distinct functions**; the one that queues
> scripts is `0x0053d8d0`, *not* the `0x00533720` that calls `OnFlagCaptured`. Resolve by address.

Caveat on the last four: they are **vtable-dispatched**, so their callers cannot be enumerated
statically. `OnPickedUp` is `PickupActorVtbl` slot 84 (`0x0066852c`, offset 0x150); see
`actor_vtable_notes.md`. Their host-side status is inferred from the fact that they perform authoritative broadcasts, not proven. `OnPickedUp` in
particular enqueues with no `IsExecutorRunning()` guard at the call site, so if a client could
ever reach that virtual method it would enqueue and run its own local file. Resolving this needs
dynamic tracing.

The three script names these paths can queue are all hardcoded, so the set of files a host can
make clients execute through *these* producers is fixed and small: `CTFRespawn.gcs`,
`RTPRespawn.gcs`, `CaptureFlag_team1..5.gcs`. Only `EvaluateTriggers` (from a trigger's
`script_name` field) and `CommandBatchAndBroadcast` (from console input) can queue an arbitrary
filename; `OnPickedUp` queues whatever `associated_script` the item carries, which comes from the
level's `.gls`.

Two allocation bugs turned up while reading these, both harmless in practice but worth knowing:
`MultiplayerRespawnRole` never frees its 15-byte script-name buffer, and `OnFlagCaptured`
allocates the default `CaptureFlag_team2.gcs` buffer *before* its switch and orphans it whenever
a case re-allocates (teams 1/3/4/5), then leaks the survivor too. `QueueScriptExecution` copies
the string, so nothing dangles — it just leaks.

> **Trap when doing reachability analysis on this binary.** A naive caller-closure walks
> `StartExecutorThread` -> `ExecutorThreadProc` as though it were a call edge, when it is the
> `CreateThread` *entry-point* reference. Leave it in and every executor-only function falsely
> appears reachable from the main-thread tick. Cut that edge and treat `ExecutorThreadProc` as
> a thread root.

### Console commands

The ~100 `Command*` console handlers are the boundary layer. Canonical pattern
(e.g. `CommandOpenDoor` @ 0x00445f60):

```c
if (IsExecutorRunning()) {            // executor running (i.e., in a level)?
    BroadcastMsg(id, args);          // BroadcastToPlayers
    if (!IsClientRoutingActive())             // no loopback/client routing?
        ApplyDirectly();             // e.g. OpenDoor(id)
}
```

Command scripts (.gcf files, trigger scripts) always execute on the **main thread** —
this part holds on every machine. There are four entry points, not three:

1. console input (`ExecuteCommandLine`);
2. `LoadLevel`;
3. the per-frame `ScriptQueue` pop (`RunQueuedScript`) — **host only**;
4. the `case 0x67` arm of the update applier `ApplyUpdateMessage`, reached from
   `ClientReceivePump` — **joining clients only** (the host is excluded by
   `if (!IsExecutorRunning())`).

(4) is also main-thread, so "all script execution is main-thread" is still true — but
"all script execution is host-side" is **not**. See the host/joiner section above.

## Synchronization primitives

### 1. Pause handshake (main pauses the executor)

`SuspendExecutor` @ 0x00505290 / `ResumeExecutor` @ 0x005052d0 (previously
tentatively named `EnterCriticalSection?`/`ExitCriticalSection?`). Not a critical
section: a recursion-counted event handshake. On the main thread, the first Suspend
sets the pause-request event and blocks until the executor acks; the last Resume sets the resume
event. On the executor thread itself both are no-ops (`GetCurrentThreadId() ==
ExecutingThread` check). ~71 `Command*` handlers and `AddTriggerToGlobalList` bracket
world mutations with this pair. Recursion counter @ 0x007b9e0c (not atomic — safe only
because a single thread uses it).

### 2. Reader/writer spin lock class (0x20 bytes)

Ctor `RWLock_Ctor` @ 0x005796b0, dtor 0x005796f0, lock `RWLock_Lock` @ 0x00579700,
unlock `RWLock_Unlock` @ 0x005797c0, trylock 0x00579760 (unused). Layout: +0x00
exclusive flag (byte), +0x04 CRITICAL_SECTION, +0x1C reader count. Writers spin with
`Sleep(5)` until readers drain; readers bump the count. Instances:

- embedded in the three loopback queues (all three take it exclusively);
- the token system (`Tokens` @ 0x007b6af8) — `SetOrCreateToken`, `Get/SetTokenValue`,
  `FindTokenWithValue`, `ListTokens`, `FreeTokens` all lock it (tokens are read and
  written from both threads);
- door open/close (`OpenDoor`/`CloseDoor` @ 0x0043fbd0/0x0043fc50);
- position sync (`SyncPositionAndBroadcast`, `InitPositionAndTiming` variants);
- a renderer shared-VB manager object (ctor `FUN_00489990`).

### 3. Plain critical sections

- Shared vertex buffer manager ("AwBBSharedVB", billboard geometry): objects created
  by `SharedVB_Ctor` @ 0x00594f50 (CS at +0x08), registered in a global list @
  0x00803c70, load-balanced by picking the least-full VB (`SharedVB_AddEntry`). Guarded
  because both threads generate billboard/particle geometry entries.
- Pool allocator (below).
- CRT-internal locks (`__acrt_lock`, `__Init_thread_*` thread-safe statics, file
  locks) — compiler boilerplate.

### 4. Pool allocator with a *disabled* lock

The game's small-block allocator (`malloc` @ 0x00571470, free path `FUN_005715b0`;
page-masked chunk headers, per-size free lists @ 0x007ba668) takes
`EnterCriticalSection(0x007c0670)` **only if the byte flag @ 0x007c066c is set — and
nothing in the binary ever sets it** (it lives in uninitialized .data, no write refs).
So the pool allocator is not thread-safe at runtime. The CRT `malloc` @ 0x005e3f72
(used for message payloads) *is* internally locked. Queue list nodes come from the
unlocked pool allocator, serialized only by the queue's RW lock; other cross-thread
pool traffic relies on the coarse pause-handshake discipline. This looks fragile by
design (likely a shipped-with-lock-off decision), and is a plausible source of the
game's known multiplayer instability.

## Per-thread state (hand-rolled TLS)

The binary predates convenient TLS; instead ~180 functions do
`GetCurrentThreadId() == ExecutingThread ? use_executor_copy : use_main_copy`.
This inlined pattern is why so many gameplay functions reference
`GetCurrentThreadId` — it is *not* locking, just per-thread variable selection.

### Game clocks (two 0x30-byte structs)

| Instance | Address | Owner |
|----------|---------|-------|
| executor clock | 0x007c07a0 | thread id == ExecutingThread |
| main clock | 0x007c07d0 | any other thread |

Layout (offsets relative to instance base): +0x0C ticks-per-frame-ish int (0x007c07ac /
0x007c07dc), +0x10 int, +0x14 float time scale (0x007c07b4 / 0x007c07e4), +0x18 64-bit
accumulated counter, +0x20 last raw sample.

Time source: `rdtsc >> 20` (`ReadRawClock` @ 0x0044e8a0). `AccumThreadClock64` @ 0x0044df20
accumulates 22-bit-masked deltas into the calling thread's 64-bit counter — per-thread
so the last-sample/accumulator pair is never shared between threads. Readers:
`FUN_00571b60` (32-bit scaled), `FUN_00571bb0` (64-bit scaled, takes a conversion
struct like game time @ 0x006aaaa0 or trigger time @ 0x006aaad0), `FUN_00571b10`
(float seconds). WinMain initializes the main clock (`FUN_005718b0(&0x007c07d0, 0)`)
and copies the whole struct to the executor clock if the thread is running; `LoadLevel`
re-syncs both on level load.

### Random number generator (two states)

Lagged-Fibonacci-style RNG with a 0x7C-byte state table per thread @ 0x006a8140
(+ `i*0x7c`), and per-thread pointer arrays @ 0x006a8238 / 0x006a8240 / 0x006a8248
indexed by `i = (GetCurrentThreadId() == ExecutingThread)`. Keeps simulation
randomness (executor) independent from client-side randomness (main).

## Process-level

- Single-instance guard: named mutex `"GunLok"` (`CheckSingleInstance` @ 0x0046a6d0, called
  from WinMain; `ERROR_ALREADY_EXISTS` -> "GunLok is already running" message box).
- WINMM imports are only `timeGetTime`/`timeBeginPeriod`/`timeEndPeriod`/mci — no
  `timeSetEvent`, so no multimedia-timer callback threads.
- No `TlsAlloc` outside the CRT; no thread pools; no Interlocked use outside the CRT.

## Implications for GkPlus

Which thread runs the GkPlus hooks:

| GkPlus hook | Thread | Notes |
|-------------|--------|-------|
| DllMain init (`Subsystems` ctor) | main (d3d8.dll loaded from WinMain) | installs all detours |
| ImGui / D3D overlay (`GUISystem`) | main | — |
| `MusicSystem` volume-fix ctor hook | main (MusicTrack construction sites) | — |
| `HookedDebugPrint` (`DebugSystem`) | both (error paths exist on the executor thread) | safe — only `OutputDebugString` |

Native code reaching into game state has to respect the two-thread split:

- Reading actors/roles/tokens/the map from the main thread races with the executor
  mutating them. For a consistent multi-field read, or for a mutation while the executor
  is live, bracket it with the pause handshake: `SuspendExecutor` (0x00505290) before and
  `ResumeExecutor` (0x005052d0) after (cheap, recursion-safe, no-op on the executor thread).
- `IsExecutorRunning()` (0x00502da0; the `Foobar`/executor-running flag @ 0x007b9df0 —
  consider renaming it `simulation_running`) is the gate for once-only actions: on a listen
  host the executor and the client both run, so anything that must happen exactly once should
  be gated on it, matching what the game's own `Command*` handlers do.
- The game never runs trigger scripts on the executor thread — the host routes them through
  `ScriptQueue` to its main thread, and a joiner runs them from `ClientReceivePump`, also on
  the main thread. In multiplayer the script runs on *every* machine, not just the host:
  update `0x67` carries only the script *filename*, so each client re-runs it against its own
  `Scripts\` directory. A native action driven off a trigger therefore runs once per
  participant, and a client whose script files differ from the host's takes a different path.
  GkPlus's message channel inherits all of this unchanged, and one property of it for free: a
  message *does* cross the wire, so a trigger carrying data is consistent across machines in the
  way a trigger carrying a filename is not.

## Key addresses summary

All names below (and the matching plate/EOL comments) have been applied to the Ghidra
project database.

| Offset | Type | Name |
|--------|------|------|
| 0x00509050 | thread proc | ExecutorThreadProc |
| 0x00502db0 | FastCall<int, IDirectPlay4*> | StartExecutorThread |
| 0x00502ee0 | StdCall<void> | StopExecutorThread |
| 0x00502da0 | getter | IsExecutorRunning (executor running) |
| 0x00505290 / 0x005052d0 | StdCall<void> | SuspendExecutor / ResumeExecutor |
| 0x00505280 | StdCall<void> | WakeExecutor (SetEvent msg-available) |
| 0x007b9d7c | DWORD | ExecutingThread (executor thread id) |
| 0x007b9df0 | bool | executor running flag ("Foobar") |
| 0x007b9df4..0x007b9e08 | HANDLE x5 | events (see table above) |
| 0x007b9dec / 0x007b9d60 | IDirectPlay4* | server-side / client-side session |
| 0x007b9d68 | bool | client routing active |
| 0x007b9e74 | int | multiplayer session active |
| 0x007ba32c / 0x007ba35c / 0x007ba38c | MsgQueue (0x30) | CommandQueue / ScriptQueue / UpdateQueue |
| 0x007ba328 | int | TotalQueuedBytes |
| 0x0056d9a0 / 0x0056da40 / 0x0056da80 | ThisCall | MsgQueue_Push / Pop / Flush |
| 0x0056dc00 | ThisCall<void, MsgQueueList*> | MsgQueueList_PopFront (takes queue+0x20) |
| 0x0056dba0 | ThisCall | MsgQueueNode_ScalarDeletingDtor (vtable @ 0x00669fd4) |
| 0x00504bf0 | FastCall | BroadcastToPlayers (server send) |
| 0x004fdbc0 | FastCall | SendToServer (client send) |
| 0x004fdc70 | StdCall | ClientReceivePump (per frame) |
| 0x004fde70 | | update applier (big id switch; `case 0x67` runs scripts on joiners) |
| 0x00505080 | FastCall<void, char*> | QueueScriptExecution (queues locally **and** broadcasts 0x67) |
| 0x00505310 | StdCall<void> | RunQueuedScript (per frame, host only) |
| 0x00579700 / 0x005797c0 | ThisCall<void, bool> | RWLock Lock / Unlock |
| 0x007c07a0 / 0x007c07d0 | 0x30-byte struct | executor / main game clock |
| 0x006a8140 | 2 x 0x7c | per-thread RNG state |
| 0x007c066c / 0x007c0670 | byte / CS | pool-allocator lock enable (never set) / CS |
| 0x00587b60 / 0x00587a90 / 0x00587bf0 | | PlayMusicTrack / PlayMusicThread / StopMusicTrack |
| 0x0046a6d0 | FastCall<int, HANDLE*> | CheckSingleInstance ("GunLok") |
