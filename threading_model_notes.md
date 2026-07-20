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

DllMain of d3d8.dll (and therefore GkPlus init + `main.lua`) also runs on this thread:
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

Two allocators are in play per entry: the **payload** comes from the CRT `malloc`
(internally locked), the **node** from the game pool allocator (whose lock is compiled
out — see below). Only the queue's own RW lock serialises the node traffic.

**Payload ownership: the caller of `MsgQueue_Pop` owns the returned buffer and must
`free()` it.** `RunQueuedScript` is the model: `ExecuteCommandFile(name); free(name);`.
Despite the name, `last_popped` is *not* a deferred-free slot on these instances —
scanning every accessor of all three globals shows nothing ever writes it non-zero, so
the `free(last_popped)` calls in Push/PopFront/Flush are dead here. It and `cache_flag`
are generic-container fields this usage never exercises.

`MsgQueueList_PopFront` recovers the just-unlinked node through the *new* front's stale
`prev` pointer before fixing it up — correct, but fragile to reordering.

| Queue | Direction | Producer | Consumer |
|-------|-----------|----------|----------|
| 0x007ba32c | commands: main -> executor | `SendToServer` @ 0x004fdbc0 (or SendEx to server in MP) | thread proc |
| 0x007ba38c | updates: executor -> main | `BroadcastToPlayers` @ 0x00504bf0 (or SendEx to all players in MP) | `ClientReceivePump` @ 0x004fdc70, called from the in-game tick; applies each via `ApplyUpdateMessage` |
| 0x007ba35c | trigger script filenames: executor -> main | `QueueScriptExecution` @ 0x00505080 (7 callers, all host-side — see below) | `RunQueuedScript` @ 0x00505310: one `ExecuteCommandFile` per frame on the main thread |

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
| `CommandBatchAndBroadcast` | 0x00448400 | console command that runs a script file | body is inside `if (LevelLoadReason != 3) if (IsExecutorRunning())` — **guarded, proven** |
| `Frag` | 0x0052e220 | kill/score credit | Actor vtable slot; broadcasts — *inferred* |
| `SyncPositionAndBroadcast` | 0x0053d8d0 | position sync | Actor vtable slot; broadcasts — *inferred* |
| `OnFlagCaptured` | 0x00533120 | CTF capture; queues `CaptureFlag_team<N>.gcs` | called from `SyncPositionAndBroadcast` @ 0x00533720; broadcasts — *inferred* |
| `OnPickedUp` | 0x00546440 | item pickup; queues the item's `associated_script` | `PickupActor` vtable slot; broadcasts — *inferred* |

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
- the token system (`Tokens` @ 0x007b6af8) — `CreateToken`, `Get/SetTokenValue`,
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

Which thread runs what GkPlus touches:

| GkPlus code | Thread | Safe w.r.t. main-thread Lua? |
|-------------|--------|------------------------------|
| DllMain init, `main.lua` | main (d3d8.dll loaded from WinMain) | yes |
| `HookedExecuteCommandLine` / `HookedExecuteCommandFile` (Lua trigger callbacks) | main only (console input, LoadLevel, per-frame script-queue pop, **and update `0x67` via `ClientReceivePump` on MP joiners**) | yes |
| `HookedSetupConsoleCommands`, `HookedSetupMenus`, `HookedOnMenuItemClicked` | main | yes |
| ImGui / D3D hooks | main | yes |
| `HookedDebugPrint` | both (error paths exist on the executor thread) | yes — no Lua use |
| `HookedPrint` (`gk.console.set_onprint` -> `lua_call`) | mostly main; executor-side prints via gameplay code are possible | **risk** |
| `TriggerData::HookedRemoveTrigger` (`luaL_unref`) | **executor only** — `RemoveTrigger`'s sole caller is `EvaluateTriggers` | **no — cross-thread Lua call** |

Guidelines:

- A Lua callback that must touch world state from the main thread while the executor
  is live should be bracketed with the pause handshake: call `SuspendExecutor`
  (0x00505290) before and `ResumeExecutor` (0x005052d0) after (cheap, recursion-safe,
  no-op if called on the executor thread).
- `gk.misc.foobar` (0x007b9df0) is actually the executor-thread-running flag —
  consider renaming it (e.g. `simulation_running`).
- Reading actors/roles/tokens from Lua (main thread) races with the executor mutating
  them; for consistent multi-field reads, use the pause handshake.
- Anything that makes the executor thread call into the Lua VM (like the current
  `HookedRemoveTrigger`) can race with main-thread Lua. Options: defer the unref to
  the main thread (queue it), or take a GkPlus-side mutex around all Lua entry points.
- `gk.triggers` callbacks are safe *because* the game never runs trigger scripts on the
  executor thread — the host routes them through `ScriptQueue` to its main thread, and a
  joiner runs them from `ClientReceivePump`, also on the main thread. Do not "optimize"
  by running them at `EvaluateTriggers` time.
- **In multiplayer a Lua trigger callback fires on every machine, not just the host.**
  Update `0x67` carries only the script *filename*, so each client re-runs the script —
  and therefore any GkPlus Lua ref encoded in it — against its own `Scripts\` directory.
  A callback that mutates Lua-side state will run once per participant, and a client
  whose script files differ from the host's will take a different path entirely. Anything
  that must happen exactly once should be gated on `IsExecutorRunning()`, matching what
  the game's own `Command*` handlers do.

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
