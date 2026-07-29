#pragma once

#include <quickjs.h>

namespace gk {
// A loopback JavaScript REPL, driven from the frame callback.
//
// Off unless GKPLUS_REPL_PORT names a port - the channel executes arbitrary code
// inside the game process, so it is opt-in rather than something everyone who
// installs the mod ends up listening on. It binds 127.0.0.1 only, for the same
// reason: the game already takes attacker-authored payloads off the network
// (update 0x67), and a wildcard bind here would be remote code execution.
//
// No thread. StartRepl only creates the listener; PumpRepl does the accepting,
// reading, evaluating and writing, once per frame on the main thread, which is
// the thread that owns the runtime. That is the whole reason this needs none of
// the locking src/Json.cpp does. A blocking accept on a worker would buy
// nothing: every snippet would still have to be marshalled back here to run.
//
// "Once per frame" is two seams, not one, and assuming it was one made the
// channel look completely dead: Gunlok stops running frames at all while its
// window is inactive - which is the state you use a REPL in - so PresentScene
// never fires there. StartRepl therefore also arms a WM_TIMER on the game
// window (SetFrameWakeupEnabled in GUI.h), which is the only heartbeat that
// survives losing focus. The full measurement is in that header.
//
// The REPL gets its own JSContext on the host's runtime. Same runtime means the
// same object graph - `actors` at the socket is the collection main.mjs sees, and
// a Role built here is the one the game holds - while the separate context keeps
// the globals it seeds for typing convenience out of the entry module, so the
// host's "there are no host globals" rule stays true for scripts.
//
// One line of NDJSON per message, UTF-8, in both directions:
//
//   -> {"code": "actors.count", "id": 7}    `id` is optional and echoed back
//   <- {"ok": true, "value": "37", "id": 7}
//   <- {"ok": false, "error": "TypeError: ...", "stack": "..."}
//
// A line that is not an object with a string `code` is treated as source, so
// `nc 127.0.0.1 <port>` works for a one-liner. Multi-line source has to travel
// as {"code": "..."}, which is what the JSON escaping is for.

// Creates the REPL context and opens the listener. False means the channel is
// closed - either GKPLUS_REPL_PORT is unset (silent, the normal case) or setup
// failed (logged). `runtime` must be the host's, and this must not run under the
// loader lock: WSAStartup loads DLLs.
bool StartRepl(JSRuntime *runtime);

// Accepts, reads, evaluates and writes. Call once per frame from the main
// thread; a no-op if the channel is closed. Re-entrant calls do nothing, so a
// snippet that somehow presents a frame cannot pump the REPL from inside itself.
void PumpRepl();

// Closes every connection and frees the REPL context. Must run before the host's
// runtime is freed, since the context lives on it.
void StopRepl();
} // namespace gk
