# Both queues carry one JSON envelope

Gunlok's two script/command queues, and the envelope GkPlus puts on both. Read
`threading_model_notes.md` for which thread each hook runs on, and `save_system_notes.md`
for what this makes visible on disk.

### Both queues carry one JSON envelope (`src/ScriptQueue.h/cpp`)

Gunlok has exactly one channel for "something happened, react to it", and it moves **file names**:
a trigger fires, `QueueScriptExecution` @ 0x00505080 pushes a `.gcs` name onto `ScriptQueue`
@ 0x007ba35c *and* broadcasts it as update `0x67`, and every machine runs its own local copy of
that file. It has a second, unrelated queue for console commands: `CommandsToExecute` @ 0x007b6aa8,
holding one strdup'd line per node, popped one per frame. `ScriptQueueSystem` redefines **both**
payloads — and the fields the first comes from — as one JSON object:

```json
{"kind": "file" | "command" | "message", "body": <contents>}
```

- **file** — `body` names a `.gcs`, run exactly where and when the engine would have run it.
- **command** — `body` is a console command line. Every entry on `CommandsToExecute` is one of
  these; on the script queue it is a command replicated to every machine.
- **message** — `body` is anything, handed to `SetScriptMessageHandler`'s handler instead of to
  the file system. `CustomLevel` installs `DispatchCustomLevelMessage`, which is what makes a
  script level's `message_received` fire. The handler receives the **body**, so the envelope is
  invisible from JS.

A kind this build does not know is still a well-formed envelope: reported and dropped, never
opened as a file.

**The envelope is why the format is an object rather than a bare value.** It used to be "a JSON
string is a file name, anything else is a message", which mis-read a `.gcs` literally called `123`.
The test is now "is this an object with a string `kind` and a `body`?" (`gk::json::OpenEnvelope`),
and a colliding file name would have to contain `"` and `:` — both illegal in a Windows path. What
is not an envelope is a bare name, with no residual doubt. `gls.probe`-style content inspection is
gone, and so is the marker byte that preceded it.

**This is visible on disk and deliberately breaking**: `SaveGame` serialises a trigger's script name
verbatim *and* walks `CommandsToExecute` writing each pending line, so a save written by this build
carries envelopes in both and will not restore correctly in an unpatched Gunlok (see
`save_system_notes.md`). Reading an older save still works, through the residual path below.

**The wrapping happens where the value is written, not where it is queued.** Ten hooks - nine that
encode, plus one that repairs an engine defect the encoding makes reachable. Four
*writers* that wrap the bare name the engine hands them, so a script-name field holds an envelope
from the moment it is set, and five on the two queues.

| Hook | Address | Role |
|---|---|---|
| `RegisterTriggers` | 0x0043e240 | writer: `TriggerData::script_name`. Covers all 23 game-side registrations — 21 branches of `CommandAddTrigger`, plus `LoadLevel` and `Frag` |
| `PickupActor::Associate` | 0x005469f0 | writer: `associated_script`. The only implementation that stores — `Actor::Associate` @ 0x0054e640 is a `RET 0x8` stub |
| `ToRole` | 0x0047cc20 | writer: `Role::interface_beam_script`, which `AddInterfaceBeamVulnerability` later copies *by pointer* into `Vulnerability::script` |
| `AddInterfaceBeamVulnerability` | 0x00510fe0 | **not a writer** - gives each actor its own copy of that string. The original shares the Role's pointer into every spawned actor and sets `actor_scoped = 1`, so `~Actor` pool-frees it once per actor and the SCRIPT completion arm frees it again; nothing resets `Role+0x88`. No shipped role sets the field, so this is unreachable in retail Gunlok and reachable only through `make.role({interface_beam_script})` or a `gls`-authored role - i.e. through us. `game_defects_notes.md` §6 |
| `ToReplaceDestructibility` | 0x0047eaa0 | writer: `ReplaceDestructibility::script` |
| `CommandBatchAndBroadcast` | 0x00448400 | **replaced**: five calls reproduced, name wrapped |
| `MultiplayerRespawnRole` | 0x0050c8b0 | **replaced**: wraps its `.gcs` name, drops the 15-byte leak. `FUN_00511600` is `__thiscall` on `RespawnRoleList` @ 0x007b9d98 — the decompiler hides the ECX `this` |
| `CommandVulnerability` | 0x0044a600 | wrapped, then every `Vulnerability::script` swept and wrapped |
| `QueueScriptExecution` | 0x00505080 | guards the invariant for what the above do not cover |
| `RunQueuedScript` | 0x00505310 | host consumer: arms the payload window, runs the original |
| `ApplyUpdateMessage` | 0x004fde70 | joiner consumer: same, for the `case 0x67` arm inside it |
| `ExecuteCommandFile` | 0x0043f250 | consumes a script-queue payload; also **sweeps** whatever it just appended to `CommandsToExecute` into `command` envelopes |
| `PumpQueuedConsoleCommand` | 0x004d6120 | **replaced**: the console queue's consumer, see below |

**The console queue is read through the envelope, never written to it.** Its nodes keep the plain
lines the game put there, and the replaced consumer treats a bare line as `kind: "command"` — the
branch that had to exist for old savegames anyway. So both queues share one set of meanings and one
code path, while `CommandsToExecute` keeps byte-for-byte the representation vanilla Gunlok gives it.

- **An earlier design rewrote each node into a literal envelope, and it crashed the game.** After
  every `ExecuteCommandFile` the queue was swept, replacing each payload via the game's `strdup`
  and `pool_free`. It faulted as a wild call — EIP on the stack, so WER blamed "module: unknown" —
  in roughly two runs out of three, from inside the sweep's own `pool_free`. The lesson is not
  "that had a bug": a subsystem that rewrites another allocator's live objects on a hot path buys
  very little and risks everything, and all the rewrite bought was making the bytes on the queue
  *look* like the bytes on the wire.
- **Not writing to it costs nothing**, because the `#` directives, front-insertion, `CLEAR BATCH`,
  `NumCommandsToExecute` and `SaveGame`'s serialisation of pending lines all keep working by being
  left alone — and a save written by this build stays readable by vanilla Gunlok.
- **Consuming still had to be a replacement**, and that is a measurement, not a preference.
  `PumpQueuedConsoleCommand` copies a node's string into `ConsoleCommandLine` (`char[252]`
  @ 0x007b6958) with an **unbounded** byte loop at 0x004d61f0, and the next global is
  `ConsoleSmallFont` @ 0x007b6a54 with nothing in between. An envelope adds ~28 characters to a
  line `fgets` already allows 249 of — `level06.gcs` ships one 399 characters long — so letting
  the game pop an envelope would write JSON through a font pointer. The replacement decodes first
  and puts only the *body* in that buffer, truncating (with a note) if it still does not fit.
- **The replacement reproduces all three gates exactly** — the `WAIT` deadline, `REAL WAIT` with
  its TAB cancel, and `WAIT FOR` with the `REPTXT` re-print — plus the `LevelLoadReason == 3`
  bypass. Every comparison in the original is a signed test on the high dword then an unsigned one
  on the low (`JL`/`JG`/`JC`), which is what a signed 64-bit `<` compiles to, so they are written
  as ordinary comparisons. It differs from the original in one way, and it is a fix: it returns on
  an **empty** queue, where the original walks into the sentinel and reads a payload off a
  0xc-byte `List_Member_Base` that has no `data` — harmless only because both of its callers test
  `NumCommandsToExecute` first.
- A queue entry that is **not** an envelope is the normal case, not an exception: it is a plain
  line the game queued, run as the console command it always was. Old savegames land here by the
  same rule rather than through a special case.

Six things decide the shape of the script queue's half, in decreasing order of how much they
constrain it:

- **The script queue's consumers are not reimplemented — they are bracketed.** Both pop the payload themselves and
  hand it straight to `ExecuteCommandFile`, so each is wrapped in a *window* that marks provenance
  and the `ExecuteCommandFile` hook interprets whatever arrives inside one. Replacing either body
  instead would mean duplicating `MsgQueue_Pop`, the `SetCurrentDirectoryToGLDir(GL_Scripts)` dance
  and the `pool_free` of the popped buffer — the payload comes from `malloc` @ 0x005e3f72, which is
  the pool thunk, so `RunQueuedScript` frees it with the pool `free` @ 0x005e3f7b. That the window
  is unambiguous is *measured*: `RunQueuedScript` is 13 instructions with one
  `ExecuteCommandFile` call, and `ApplyUpdateMessage` contains exactly one (0x004ff971), reachable
  only from `case 0x67`, with none of its 164 direct callees reaching it transitively
  (`directplay_protocol_notes.md` §8.11). `ExecuteCommandFile` has six callers in all, and the
  other four — `LoadLevel`'s level `.gcs`, the console's own local `BATCH`, and two briefing-screen
  ones — are outside both windows. The window is also **one-shot**, so a payload that goes on to
  run a batch file with `#EXECUTE IMMEDIATELY` cannot have that file re-read as a payload.
- **Neither side guesses what it is holding.** A game-side write is always a bare name — a console
  argument or a GLS field — and GkPlus's own writes arrive inside an `EncodedPayloadScope`, a
  call-scoped "already a document, pass it through" that `triggers.create` and `actor.associate`
  wrap their engine calls in. That replaced an earlier **marker byte** in the stored value: once
  every writer encodes, there are no longer two representations to tell apart, so the marker had
  nothing left to do. Content inspection was the design before *that*, and it is wrong in a way no
  care fixes — a `.gcs` legitimately called `{a}.gcs` reads as an object.
- **`ToRole`'s cache check is load-bearing.** It early-returns the `Role` it cached at
  `parsed+0x1b60` (its 13th instruction, 0x0047cc50 — 0x1b60 is `sizeof(ParsedThingBase)`), and
  nested conversions *do* call it again, since `ToFragData` builds `role` and `replace_role` through
  it. Reading that slot before calling the original is what stops the field being quoted once per
  call. `ToReplaceDestructibility` needs no such guard: it pool-allocs a fresh record every time.
- **A site that queues without storing has to be replaced, not hooked.** `CommandBatchAndBroadcast`
  (5 calls) and `MultiplayerRespawnRole` (3, plus an error path) are small enough to reproduce, so
  they are — encoding the name and dropping the allocations both originals leaked.
  `CommandVulnerability` is 1369 bytes of argument parsing that also fans one `Vulnerability*` out
  to every actor of a role, so it is wrapped and its result **swept** instead: any vulnerability
  left holding a non-document gets encoded. A sweep rather than a before/after diff because the
  diff would have to assume the pool never returns the address it just freed, which that command
  does free; "is it encoded yet?" holds regardless of how the value got there.
- **The engine's own events still queue their stock `.gcs`** — `CTFRespawn.gcs`, `RTPRespawn.gcs`,
  `CaptureFlag_team<N>.gcs` — now properly encoded. GkPlus sends no messages of its own; a script
  hears from the queue only what a script put there. `MultiplayerRespawnRole`'s replacement must keep
  appending to `RespawnRoleList` @ 0x007b9d98 exactly where the original did, because that is how the
  queued `.gcs` finds the actor to equip (see `threading_model_notes.md`).
- **`OnFlagCaptured` is left alone on purpose.** Its 760 bytes drive two `BroadcastToPlayers`
  payloads, five `TeamSlots` writes, a vtable getter/setter pair and a "Hark" special case;
  duplicating its team-to-script switch a few instructions earlier, to encode a name the queue hook
  encodes anyway, would only add a way to pick the wrong file. So it is the one *local* source that
  still reaches `ScriptQueuePayload` bare — alongside old savegames and peers without GkPlus, which
  this process cannot encode at all.
- **`ScriptQueuePayload` is that decision as a pure function**, and **its return value is always a
  valid JSON document** — the invariant everything else rests on. Being pure is what lets the
  harness assert it on every case, including the one documented ambiguity: a residual bare name that
  happens to *be* a document (a file literally called `123`) is passed through as one.
- **The payload is UTF-8; the engine is ANSI; `Encoding.h` is the seam.** Every `char *` the game
  holds is CP_ACP — a `.gls` is read as bytes, and `fopen` reads the codepage — so a name is
  transcoded on the way into a payload and back on the way to `ExecuteCommandFile`. Carrying the raw
  bytes instead *looks* like it works, because `gk::json`'s own decoder is byte-exact, and it was the
  first design; it breaks as soon as a name is embedded in a **message**, since QuickJS decodes that
  document and silently turns every invalid sequence into U+FFFD (the `UTF8_HAS_ERRORS` throw is
  commented out in `JS_NewStringLen`), handing the script a path that opens nothing. Transcoding also
  makes a *script*-authored non-ASCII path work for the first time — it used to reach `fopen` as
  UTF-8.
- **QuickJS is the codec throughout.** The script host's context writes a message
  (`ToScriptPayload`) and reads one (`OnMessage`); `gk::json` does the queue's own encoding on a
  **private runtime behind a lock**, because the producer and the four writer hooks are executor-side
  while the consumer is main-side, and one `JSRuntime` may only be used from one thread at a time.
  That private runtime is what a hand-written parser used to buy — the trade is a lock and
  `JS_UpdateStackTop` per operation, against a grammar nobody has to maintain.
- **The producer runs on the executor thread, the consumers on the main thread.** That is what makes
  interning atoms in the host's runtime a race rather than a nicety, and what makes calling into
  QuickJS from the handler safe. It holds by call graph: `RunQueuedScript` has one caller (the in-game
  tick) and `ApplyUpdateMessage` has one (`ClientReceivePump`), whose own three callers are the
  tick, the multiplayer lobby pump and `UpdateAndDrawMenuScreen` — all main-thread. That last one
  means an update can be applied **from the front end**, so a message can arrive with no level
  loaded; it is reported undelivered rather than held. The producer hook is therefore pure string
  work: no console printing, no game locks.
- **The message inherits the engine's delivery semantics exactly**, because it *is* the engine's
  delivery. One dispatch per machine: the host from its own queue, joiners from `0x67` (whose
  `!IsExecutorRunning()` gate is what stops the host running it twice). The host is throttled to
  **one payload per frame**; a joiner is not. `QueueScriptMessage` is the way in from native code
  and calls the *trampoline*, deliberately not the raw address — after `DetourAttach` that entry
  point is the wrap, which would turn a message into a file name.
