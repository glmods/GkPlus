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
//
// The channel also carries **unsolicited** lines the other way - the backchannel
// a script pushes notifications into (NotifyRepl, `repl.notify` in JS):
//
//   <- {"event": "spawned", "data": {"id": 12}}
//
// One rule tells the two apart, and it is the reason `event` is a distinct key
// rather than another `ok` shape: **a reply always has `ok`, a notification
// never does and always has `event`.** A client that only ever asked for
// replies can keep reading them by testing for `ok`, so adding this breaks
// nothing that was written before it existed.

// --- launching with a port nobody had to guess --------------------------------
//
// GKPLUS_REPL_PORT=0 (or "auto") binds the ephemeral port the OS picks instead
// of a named one, and GKPLUS_LAUNCHER_HWND says where to send the result.
//
// The two exist together because **a launcher cannot choose the port itself
// without a race**: every gap between "find a free port" and "the game binds
// it" is a window for something else to take it, and a launcher drawing a
// number out of the ephemeral range can also hit a block Hyper-V has reserved
// and get WSAEACCES with nothing listening there. Binding 0 has no gap - the OS
// picks under SO_EXCLUSIVEADDRUSE, which cannot hand one port to two binds - so
// the only thing left is telling the launcher what it got.
//
// The contract for the receiving side, which is all a launcher has to
// implement:
//
//   * register a window class named exactly "GkPlusLauncher" and create a
//     message-only window (HWND_MESSAGE) of it. The class name is checked
//     before anything is posted, because window handles are recycled and an
//     environment variable outlives whatever set it - a stale handle would
//     otherwise deliver the port to an unrelated window;
//   * put that HWND in GKPLUS_LAUNCHER_HWND, decimal or 0x-prefixed;
//   * RegisterWindowMessage("GkPlusReplPort") for the id - it is allocated
//     system-wide from the string, so both sides compute the same number - and
//     handle it with **the pid in wParam and the port in lParam**. There is no
//     buffer to marshal, copy or free;
//   * **pump messages while waiting.** This one is *posted*, so the game never
//     blocks on the launcher - but a posted message reaches a window procedure
//     only through DispatchMessage, so a launcher that never pumps simply never
//     learns the port. It will be sitting in the queue whenever it does;
//   * if the launcher runs at a higher integrity level than the game, call
//     ChangeWindowMessageFilterEx(hwnd, <that id>, MSGFLT_ALLOW, nullptr). UIPI
//     drops the message silently otherwise, which looks exactly like the game
//     never posting it.
//
// The `pid` is what distinguishes a live game from a leftover one, so a
// launcher should check it against the process it spawned rather than assume
// the only message it got is the one it wanted. It is also the launcher's job
// to give up after a while: a post is confirmed queued rather than delivered,
// so the game cannot tell it that nothing is coming.
//
// A window message cannot cross a session or a desktop, which rules this out
// for a session-0 shell driving a session-1 game (see the -it recipe in
// utils/rendertest/README.md). Nothing else here depends on it: the port is
// logged either way, and GKPLUS_REPL_PORT with a literal port still works
// exactly as it always did.

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

// --- the backchannel ---------------------------------------------------------

// Queues {"event": <event>, "data": <data>} to every connected client. `ctx` is
// the *caller's* context - the host's, the REPL's, whichever ran the script -
// and only has to be on the same runtime; it is what encodes `data`, so a value
// never crosses a context boundary. Pass JS_UNDEFINED for `data` to send an
// event with no payload, which omits the key rather than writing null.
//
// Returns how many clients it reached, or -1 with an exception pending on `ctx`
// when `data` could not be encoded (a circular structure, a getter that throws).
// Zero is the ordinary answer, not a failure: nobody is usually listening.
//
// Nothing is written to a socket here - the line joins the same per-connection
// buffer replies use and goes out on the next PumpRepl, so a notification can
// never block the frame on a client that has stopped reading. A client that has
// stopped reading for long enough is dropped, exactly as it is for replies.
//
// **When the channel is closed (GKPLUS_REPL_PORT unset - the normal case) this
// returns 0 without encoding anything**, so notifications left in shipped script
// cost a call and a branch. That the check is on the *channel* and not on the
// client count is deliberate: whether a payload encodes is then a property of
// how the game was launched, not of whether someone happened to be attached at
// that moment. A script that wants to skip building an expensive payload should
// test ReplClientCount (`repl.clients`) itself.
//
// Main thread only, like everything else here: it touches the connection list
// PumpRepl owns and no lock guards it.
int NotifyRepl(JSContext *ctx, const char *event, JSValueConst data);

// How many clients are connected. 0 when the channel is closed.
int ReplClientCount();

// Whether the listener is open at all - i.e. whether GKPLUS_REPL_PORT named a
// port and setup succeeded.
bool ReplOpen();

// The port actually bound, or 0 when the channel is closed. Worth asking rather
// than assuming even when a launcher named one: under GKPLUS_REPL_PORT=0 the
// number is the OS's, and this is read back from the socket either way.
int ReplPort();
} // namespace gk
