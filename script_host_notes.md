# The QuickJS script host and the REPL channel

How `main.mjs` is booted and how the loopback debug channel works. The `"gk"` module's own
surface is `js_bindings_notes.md`; the binding *conventions* that govern edits stay in
`CLAUDE.md`.

### Script host (`src/Script.h/cpp`)

One `JSRuntime`, one `JSContext`, one entry module. The entry module is `GKPLUS_SCRIPT` if that
environment variable is set, otherwise **`gkplus\main.mjs` next to `d3d8.dll`** (i.e. in Gunlok's
directory). A missing file logs the path it looked for and leaves the game unmodified. An
**empty** one does not: it loads, evaluates, exports nothing, and looks exactly like a working
host that happens to do nothing — check the file is non-empty before debugging the bindings.
`examples/main.mjs` is a working starting point.

```js
export function setup_menus(menus) { menus.Main.add_item("Hi", (item) => {}); }
export function draw_gui(ImGui)    { if (ImGui.Begin("x")) { … } ImGui.End(); }
```

Four facts pin the design, in decreasing order of how expensive they were to find:

- **The boot point is a detour on `SetupMenus` @ 0x004e95e0**, not `DllMain` and not the first
  frame. `WinMain` calls it exactly once, after `LoadResourceStringTable` (0x0046b355) and
  `InitConsole` (0x0046bb81) and before the frame loop (0x0046be47) — so at boot the localized
  strings and `gk::Print` both work, and the game's own menus are already populated. That last
  part is load-bearing: `OnMenuItemClicked` switches on the item *index*, so a script item added
  before the game's would shift every index in the dispatch table.
- **`ImGui` is not a module — it is only `draw_gui`'s argument.** Every call in it is valid only
  between `NewFrame` and `Render`, which is exactly the window `draw_gui` runs in, so
  `js_imgui_new_namespace` (imgui-quickjs) builds a plain object with `JS_SetPropertyFunctionList`
  and `BootScriptHost` hands it over. Making it importable would let a script call it from module
  scope or `setup_menus`, where it cannot work. The cost is that a duplicate name in
  `js_imgui_funcs` is now *silent* — see the export-list convention below, where the `grep`
  recipe is the only remaining guard.
- **`menus` is the same story** — it is `setup_menus`' argument and not a `"gk"` export, because
  the game's own items must already be in place when a script adds one. The host builds it with
  `js::NewMenusNamespace`.
- **`LinkGkModule` imports `"gk"` at boot purely as a check**, and keeps nothing. A C module is not
  linked until something imports it, and its export list is only validated then — a duplicated name
  fails the link with a `SyntaxError`. Doing the first import at boot reports that as
  `gkplus bootstrap` rather than against whichever script imported `"gk"` first. (It used to also
  fetch the namespace for `menus`; if you ever need a C module's namespace from C, note that
  `JS_GetModuleNamespace` on an un-imported module does not return undefineds —
  `js_build_module_ns` falls through to the module's unset `func_obj` and crashes.)
- **Module evaluation returns a promise even without top-level await**, so `Await()` pumps
  `JS_ExecutePendingJob` until it settles, and reports a stuck pending promise instead of spinning.
- **The entry module's name uses forward slashes.** QuickJS's default normalizer resolves a
  relative specifier by scanning the *importing module's name* for `/`; with `C:\…\main.mjs` it
  finds none and `import "./x.mjs"` silently resolves to `x.mjs` in the process's cwd.

**The host loads exactly one module, and the bindings load none.** `levels.add` used to take a
module *path* and pull the file in itself, through a `LoadScriptModule` host service; it now takes
the description object, and a script reaches a level module with an ordinary
`import * as arena from "./levels/arena.mjs"` — the namespace is already the right shape. That
deleted the last binding-side module load, so `Script.h` exports only `BootScriptHost`. If a
binding ever does need to load a script file from C, the answer is
`Await(ctx, JS_LoadModule(ctx, EntryPath, specifier))` — QuickJS's own C-side `import()`, which
normalizes, runs the loader, links, evaluates and resolves with the namespace — with the **entry
module's path** as the base name, which is what makes a relative specifier resolve next to
`main.mjs` rather than against the cwd the game keeps moving. Do **not** open-code it by evaluating
a synthetic wrapper module that does the import; that is what the deleted version started as.

Two seams in `src/GUI.h` carry it: `SetOverlayDrawCallback` (inside the ImGui frame, F11 only)
runs `draw_gui`, and `SetFrameCallback` (once per `PresentScene`, overlay or not) drains the job
queue and then pumps the REPL channel. Both are installed by `BootScriptHost`, not by the
`ScriptSystem` ctor, so they can never call into a context that does not exist yet. Everything runs
on the main thread.

One runtime, but not necessarily one context: `StartRepl` adds a second one for the debug channel
(see "The REPL channel"). It is started *before* `LoadEntryModule`, because a REPL is most useful
exactly when `main.mjs` is missing or throws — both of which make `BootScriptHost` return early.

Nothing a script throws reaches game code: every seam ends at `gk::js::ReportException`, which
prints the message and stack to the console and the debugger. A `draw_gui` that throws is
**disabled for the session** — once per frame forever is a flood, not a diagnostic, and a script
that threw mid-frame has usually left ImGui's stack unbalanced.

**There are no host globals at all.** QuickJS core provides no `console` (that is quickjs-libc,
which this port does not install) and GkPlus deliberately does not add one — `log`/`info`/`warn`/
`error`/`debug` live on the `"gk"` module's `console` beside the game's own `print`, colours and
`execute`, so there is exactly one console object and a script reaches it the way it reaches
everything else. `js::Log` is still the C-side sink for all of it, including `ReportException`.

### The REPL channel (`src/Repl.h/cpp`)

A loopback socket that evaluates JavaScript in the running game. **Off unless
`GKPLUS_REPL_PORT` names a port**, and bound to `127.0.0.1` only — it executes
arbitrary code in the game process, and the game already takes attacker-authored
payloads off the network (update `0x67`), so a wildcard bind would be remote code
execution on whoever is playing. One line of NDJSON each way, UTF-8:

```
-> {"code": "actors.count", "id": 7}       `id` optional, echoed back
<- {"ok": true, "value": "37", "id": 7}
<- {"ok": false, "error": "TypeError: ...", "stack": "..."}
<- {"event": "spawned", "data": {"id": 12}}    unsolicited; see the backchannel
```

#### Using it

Set the port in the environment the game inherits, and launch `gl.exe` directly —
Steam will not pass the variable through:

```
GKPLUS_REPL_PORT=9222 "<Gunlok>/gl.exe"
```

The listener opens during `SetupMenus`, about a second in, and logs
`repl: listening on 127.0.0.1:9222` to the game console. Then anything that speaks
TCP will do:

```
printf 'actors.count\n' | nc 127.0.0.1 9222
```

#### Launching without choosing a port

`GKPLUS_REPL_PORT=0` (or `auto`) binds an **ephemeral** port instead, and
`GKPLUS_LAUNCHER_HWND` says where to send the one the OS picked. The pair exists
because **a launcher cannot pick the port itself without a race**: every gap
between "find a free port" and "the game binds it" is a window for something else
to take it. Binding 0 has no gap — the OS chooses under `SO_EXCLUSIVEADDRUSE`,
which cannot hand one port to two binds — so the only thing left is telling the
launcher what it got. It also sidesteps two things a fixed port hits: a number
drawn from the ephemeral range can land in a block Hyper-V has reserved and fail
with `WSAEACCES` while nothing is listening there, and a fixed port can stay
unbindable after a crash while the previous run's connections sit in `TIME_WAIT`.

`src/Repl.h` carries the receiving side's contract; the short version is a
message-only window of class `GkPlusLauncher`, its `HWND` in the environment, and
one **posted** `RegisterWindowMessage("GkPlusReplPort")` with the **pid in
`wParam` and the port in `lParam`**.

**A `WM_COPYDATA` was the obvious shape and is the wrong one.** It cannot be
posted at all: the window manager marshals its buffer into the receiver *during
the send*, so a posted one arrives as a pointer into the game's own address space
— meaningless to the launcher, and in the usual 32-bit-game/64-bit-launcher
pairing not even the same width. Sending it instead would put the game's main
thread, inside `SetupMenus`, at the mercy of the launcher's message loop; a
launcher sitting in a sleep would stall the game's startup until the send timed
out. A pid and a port fit in the two message parameters with room to spare, so
there is no buffer to marshal and nothing on the game's side ever waits.
`SendMessageTimeout` with `SMTO_ABORTIFHUNG` would have bounded that risk; not
having it is better than bounding it.

Three details are load-bearing, and two of them fail *silently*:

- **The class name is checked before anything is posted.** Window handles are
  recycled and an environment variable outlives whatever set it, so a stale
  `GKPLUS_LAUNCHER_HWND` would otherwise deliver the port to an unrelated window.
  Measured against a live foreign window (`ConsoleWindowClass`), which is refused
  by name.
- **The launcher has to pump messages.** A posted message reaches a window
  procedure only through `DispatchMessage`, so a launcher that never pumps never
  learns the port — it will simply be sitting in the queue. Unlike the send this
  replaced, being slow to pump costs the *game* nothing.
- **UIPI drops the message with no diagnostic** when the launcher runs at a
  higher integrity level than the game, which is what an elevated shell produces.
  The receiver needs `ChangeWindowMessageFilterEx(hwnd, <that id>, MSGFLT_ALLOW,
  nullptr)`; without it the failure looks exactly like the game never posting.

The message goes out only once the listener is *accepting*, so its arrival is the
readiness signal as well as the number — which retry-until-connect could not
give. The `pid` is what tells a live game from a leftover one, so a launcher
should check it against the process it spawned rather than trust the only message
it got. **Giving up is the launcher's job**: a post is confirmed queued rather
than delivered, so the game has no way to say that nothing is coming.
`repl.port` reports the same number from inside the game.

**A window message cannot cross a session or a desktop**, which rules this out
for a session-0 shell driving a session-1 game (see `utils/rendertest/README.md`
for the `schtasks /it` recipe). Nothing else depends on it: the port is logged
either way, and a literal `GKPLUS_REPL_PORT` still behaves exactly as it did.
`utils/rendertest/launch-gunlok.ps1` is a worked receiver.

**A line that is not a JSON object with a string `code` is treated as source**, so
one-liners work with no quoting ceremony. Multi-line source has to ride in the
object form, because a newline is the frame delimiter:

```
{"code": "for (const a of actors) if (!a.alive) console.print(a.id);\nactors.count", "id": 3}
```

Everything the `"gk"` module exports is already a global — all 26 namespaces,
enumerated at boot, plus the default export as `gk` — so there is nothing to
import and no host object to reach for:

```
actors.count                       // 158
[...actors].filter(a => a.alive).length
roles["gunlok"].id
game.simulation_running            // the authority test, false on a joining client
levels.start("level02.gls")        // into a level from the menu, no keystrokes
console.print("hello")             // goes to the game's own console
```

Evaluation is **global scope, not module scope**: the expression's value comes
straight back, and `var`/`function` persist between lines, so a session
accumulates state. The cost is that `import` is a syntax error there — which
costs little, since the prelude has already put everything importable in scope.

**Do not expect a relative dynamic `import()` to work from the REPL.** Not
measured, but it follows from the normalizer rule documented under "Script host":
QuickJS resolves a relative specifier by scanning the *importing* script's name
for `/`, and the REPL evaluates under the name `<repl>`, which has none — so
`import("./x.mjs")` should land against the process's current directory, which
the game moves around during a level load, rather than next to `main.mjs`. A
bare specifier needs no normalizer and should be fine. If you need this, measure
it first and replace this paragraph with what you find.

Worth knowing before leaning on it:

- **A snippet runs on the game's own thread, inside the frame callback.** It can
  see and mutate live game state, and while it runs the game does not advance. A
  `JS_SetInterruptHandler` deadline throws after 5 s so a runaway loop cannot wedge
  the process — but it **cannot interrupt a native call**, only JavaScript.
- **Start `level02` when the test does not care which level it is in.** `level01.gcs`
  opens with `PLAY CUTSCENE first contact`, so anything asserted right after the load
  is measuring a cutscene; `level02.gcs` plays none. See "Debugging the running game".
- **`ImGui` is not reachable**, by construction: the frame callback runs outside
  the ImGui frame, so those calls would be invalid. `draw_gui` is where they live.
- **It shares the runtime with `main.mjs`** but has its own context, so `actors`
  at the socket is the very collection the entry module sees, while globals seeded
  here never leak into the script's `globalThis`.
- Limits, all of them there so a wedged client degrades into a dropped connection
  rather than an unbounded allocation: 4 concurrent connections, a 1 MiB cap on a
  single un-terminated line, 8 MiB of unread replies, and results over 64 K
  characters are elided with a note.
- **Replies are formatted, not round-trippable.** `value` is display text —
  `JSON.stringify` where that works, `[object Object]` where it throws (a circular
  structure, a getter that raises). Do not parse it.

Verified against a running game in all four states — front-end menu and in-level,
window focused and unfocused — and offline by the harness described at the end of
this section.

Five things pin the design:

- **No thread, because the frame callback already exists.** `StartRepl` only opens
  the listener; `PumpRepl` accepts, reads, evaluates and writes from `OnFrame`,
  on the thread that owns the runtime. That is why none of `src/Json.cpp`'s
  locking or `JS_UpdateStackTop` applies here. A blocking `accept` on a worker
  would buy nothing — every snippet would still have to be marshalled back to the
  main thread to run — and would cost a second thread on the host `JSRuntime`,
  which is the hazard `Json.cpp` exists to work around.
- **`PresentScene` is not a sufficient heartbeat, because Gunlok stops running
  frames entirely while its window is inactive** — which is exactly the state a
  debug channel is used in, since you are typing in a terminal. All of this is
  measured on a running game, not inferred:
  - `OnActivateApp` @ 0x0046f400 clears `HasWindowFocus` @ 0x006a3744 on focus
    loss and calls `FUN_00574960`, which releases the D3D resources and clears
    `DAT_007c1230`;
  - `RenderSceneAndPresent` @ 0x00574c50 wraps its whole body — scene,
    `EndScene`, the `PresentScene` call — in `if (DAT_007c1230 != 0)`, so the
    frame hook never fires;
  - yet the process still spins a full core and `SendMessageTimeout(WM_NULL)`
    answers in 7 ms, so the main thread is in a loop that pumps messages.

  **How much else stops depends on where you are, and only the gate is
  constant.** At the front-end menus both per-thread clock accumulators
  (`DAT_007c07e8` main, `DAT_007c07b8` executor) read **+0 over four seconds** —
  the game is not merely unrendered, it is not running. In a loaded level the
  same two clocks keep advancing (~10,300 per 3 s each, measured unfocused), so
  the simulation continues and only rendering is gated. `PresentScene` is
  unreachable either way, which is the part that matters for the heartbeat; do
  not generalise the menu measurement into "the game pauses on focus loss".

  A message is therefore the only way in. `SetFrameWakeupEnabled` (GUI.h) puts a
  **WM_TIMER** on the game window, handled in the existing `HookedWndProc`, which
  runs `RunFrameCallback` on the main thread ~50×/s regardless of focus. Verified
  A/B: with `HasWindowFocus` 0 and the gate 0, the previous build timed out and
  this one answers 12/12 at 10–37 ms. **WM_TIMER rather than a thread posting
  `PostMessage`**, because `StopRepl` runs from `DllMain(DLL_PROCESS_DETACH)` and
  waiting on a worker thread under the loader lock deadlocks; `KillTimer` is a
  plain USER32 call and is safe there.

  Two traps cost most of an afternoon here, both worth keeping:
  **reachability is not execution** — the front-end frame `FUN_0046eae0` *does*
  call `RunGameFrame()`, so a caller-graph search says the front end reaches
  `PresentScene`, when the call inside is gated off; read the guard, don't trust
  the edge. And **the state that looks like "front end vs in-game" was window
  focus all along** — the same menu screen answers or doesn't depending only on
  whether the window is active, so sample the condition rather than the scenario.
- **Its own `JSContext` on the host's runtime.** Same runtime is the same object
  graph — `actors` at the socket is the collection `main.mjs` sees — while the
  separate context keeps the globals it seeds off the entry module's
  `globalThis`, so "there are no host globals" stays true for scripts. The cost is
  per-context prototypes: `RegisterGkModule` runs again on the REPL context
  (`RegisterClass` in `JsCommon.cpp` is already a no-op for the runtime half), and
  a wrapper is branded by whichever context minted it.
- **Global-scope eval, not module.** It hands the expression's value straight back
  and lets `var`/`function` persist between lines, which module scope does neither
  of. `import` is the thing given up, and the prelude covers it by enumerating
  `import * as ns from "gk"` onto `globalThis` — enumerated, not listed, so a
  namespace added to `JsGk.cpp`'s table appears without anyone remembering.
- **One rule decides what a line is**: an object with a string `code` is a
  request, anything else is source. That keeps `nc 127.0.0.1 <port>` usable for a
  one-liner (`actors.count` is not JSON at all; `1` is JSON but not an object)
  while multi-line source rides as `{"code": "..."}` with its newlines escaped.
  The one ambiguity is a JS object literal that happens to have a string `code`
  property.
- **`JS_SetInterruptHandler` on a deadline**, armed only around a REPL eval, so a
  runaway loop throws instead of freezing the game. It cannot interrupt a *native*
  call that hangs — only JavaScript.

`JSON.stringify` **throws** on a circular structure or a getter that raises rather
than returning `undefined`; the formatter needs both fallbacks, and a test
asserting only `ok: true` passes on the error path too.

#### The backchannel (`NotifyRepl`, `repl.notify`)

The protocol above is request/reply, and that is a real limit rather than a
stylistic one: **a client that can only ask can only sample.** It sees the state
of whichever frame its request happened to land in, so anything that *happens*
between two polls — a trigger fired, a role spawned, a message arrived on the
script queue, a level finished loading — is invisible unless something inside
the game says so at the moment it happens. `repl.notify(event, data)` is how it
says so:

```js
import { repl, triggers } from "gk";
triggers.add({ …, script: () => repl.notify("gate", { open: true }) });
```

```
<- {"event": "gate", "data": {"open": true}}
```

Five things pin it, and four of them are consequences of the channel it rides on
rather than fresh decisions:

- **`event` and no `ok`.** A reply always carries `ok`; a notification never does
  and always carries `event`. That one rule is the whole client-side change, and
  it is why a notification is not just another `ok` shape — a client written
  before this existed keeps working, because it can filter on `ok` and drop
  everything else.
- **Every connected client gets every notification.** No subscription, no
  filtering: this is a debug channel with a hard cap of 4 connections, and a
  subscribe verb would be protocol surface to maintain for a case nobody has.
  `notify` returns the number of clients it reached, which is the only useful
  answer to "did that go anywhere" and is usually `0`.
- **Nothing is written to a socket from `notify`.** The line joins the same
  per-connection buffer replies use and goes out on the next `PumpRepl`, which
  keeps the existing invariant that no game-side call can block on a client that
  has stopped reading. The 8 MiB backlog cap applies unchanged, so a script
  notifying every frame at a client that never reads drops that client rather
  than growing without bound.
- **The caller's context does the encoding.** `NotifyRepl` takes a `JSContext *`
  and stringifies `data` through it, so a value never crosses a context boundary
  — `main.mjs`'s objects are encoded by the host context and the REPL's by the
  REPL's, on the one runtime they share. A payload `JSON.stringify` refuses
  throws at the caller, matching what `ToScriptPayload` does for the script queue
  rather than silently sending a line with a field missing.
- **The closed-channel test is the *channel*, not the client count.** With
  `GKPLUS_REPL_PORT` unset — every ordinary launch — `notify` returns 0 before
  encoding anything, so notifications can be left in shipped script. It
  deliberately does **not** extend that shortcut to "open but nobody attached":
  whether a payload encodes (and therefore whether a bad one throws) would then
  depend on whether someone happened to be connected that second, which is the
  worst possible way for an error to be intermittent. A script that wants to skip
  building an expensive payload tests `repl.clients` itself.

This is one of the layers that *can* be exercised outside Gunlok (see
"Runtime-testing outside the game"): `Repl.cpp` reaches only `js::RegisterGkModule`,
`Log`, `ReportException` and `ReleaseCallbacks`, so a harness supplying those four
plus a one-namespace `"gk"` module drives the whole protocol over a real socket
with `PumpRepl` standing in for the frame hook. The backchannel was verified that
way first — two real sockets, both receiving the same line, ordering, the
newline-in-a-payload case, and every failure path — and then in the running game
at the front-end menu (`game.state` 8), where two clients see the identical line
and a disconnected one stops counting.
