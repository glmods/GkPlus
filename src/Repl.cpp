#include "Repl.h"

// winsock2.h has to precede windows.h: windows.h pulls in the winsock 1.1
// header, and the two redefine each other's types.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "GUI.h"
#include "Js.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gk {
namespace {

// --- limits ------------------------------------------------------------------
//
// Every one of these bounds something a peer controls. The channel is loopback
// and opt-in, so none of them is a security boundary; they are here so a wedged
// or confused client degrades into a dropped connection instead of an unbounded
// allocation or a stalled frame loop.

constexpr size_t kMaxConnections = 4;
constexpr size_t kMaxRequestBytes = 1u << 20;  // buffered, still no newline
constexpr size_t kMaxPendingBytes = 8u << 20;  // replies a client has not read
constexpr size_t kReadsPerFrame = 64;          // 256 KiB, then yield the frame
constexpr int kValueChars = 64 * 1024;         // formatted result, before eliding
constexpr uint64_t kEvalBudgetMs = 5000;

// --- the launcher rendezvous -------------------------------------------------

// The window class a launcher registers, and the message it is posted.
//
// The id comes from RegisterWindowMessage rather than being a WM_USER+n of our
// choosing: it is allocated system-wide from the string, so both processes
// compute the same number without agreeing on one, and it cannot collide with
// whatever the receiving window already defines in its own WM_USER range.
constexpr char kLauncherClass[] = "GkPlusLauncher";
constexpr char kLauncherMessage[] = "GkPlusReplPort";

// --- state -------------------------------------------------------------------

struct Connection {
  SOCKET socket = INVALID_SOCKET;
  std::string incoming;
  std::string outgoing;
};

bool WinsockReady = false;
SOCKET Listener = INVALID_SOCKET;
// The port actually bound, which is not necessarily the one asked for - see
// StartRepl. Only meaningful while Listener is open.
int ListenPort = 0;
std::vector<Connection> Connections;

JSRuntime *Runtime = nullptr;
JSContext *Context = nullptr;

// The display formatter (Formatter below), compiled once. Held in C rather than
// on globalThis so that a REPL user cannot break their own replies by
// reassigning it, and so it survives whatever they do to the global object.
JSValue Format = JS_UNDEFINED;

// Non-zero only while a snippet is running - see Interrupted.
uint64_t EvalDeadline = 0;
// Guards the whole of PumpRepl. A snippet runs arbitrary code, which could in
// principle reach PresentScene and re-enter the frame callback; without this the
// nested pump could accept a connection and reallocate Connections underneath the
// outer loop.
bool Pumping = false;

// Cuts a runaway snippet loose. Runtime-wide (QuickJS has one handler), but the
// deadline is only armed around a REPL eval, so nothing else pays for it beyond
// the periodic call. It cannot interrupt a *native* call that hangs - only
// JavaScript - so a game function that never returns still takes the process
// with it.
int Interrupted(JSRuntime *, void *) {
  return EvalDeadline != 0 && GetTickCount64() > EvalDeadline;
}

// --- the REPL context --------------------------------------------------------

// Seeds the globals. Enumerated off the module namespace rather than listed, so
// a namespace added to JsGk.cpp's table turns up here without anyone having to
// remember. `default` is skipped and re-attached as `gk`, matching what a script
// gets from `import gk, { actors } from "gk"`.
constexpr char Prelude[] = R"JS(
import * as ns from "gk";
for (const [name, value] of Object.entries(ns)) {
  if (name !== "default") globalThis[name] = value;
}
globalThis.gk = ns.default;
)JS";

// Turns a result into one line of display text. JSON.stringify does the heavy
// lifting where it can and is allowed to fail - a circular structure, a getter
// that throws and an exotic collection that enumerates the world all end up as
// the Object.prototype.toString form instead of as an error.
constexpr char Formatter[] = R"JS(
(value, limit) => {
  const describe = (v) => {
    if (v === undefined) return "undefined";
    if (v === null) return "null";
    switch (typeof v) {
      case "string": return JSON.stringify(v);
      case "bigint": return v + "n";
      case "symbol": return v.toString();
      case "function": return "[Function: " + (v.name || "anonymous") + "]";
      case "object": break;
      default: return String(v);
    }
    // Two ways to fail and they are not the same: stringify returns undefined
    // for a value it has no representation for, and *throws* on a circular
    // structure or a getter that raises. Both fall back to the tag form.
    let json;
    try {
      json = JSON.stringify(v);
    } catch {
      json = undefined;
    }
    return json !== undefined ? json : Object.prototype.toString.call(v);
  };
  let out;
  try {
    out = describe(value);
  } catch (e) {
    try { out = "<could not format: " + e + ">"; } catch { out = "<could not format>"; }
  }
  if (typeof out !== "string") out = String(out);
  if (out.length > limit) {
    let cut = limit;
    const c = out.charCodeAt(cut - 1);
    if (c >= 0xd800 && c <= 0xdbff) cut -= 1;  // never split a surrogate pair
    // ASCII: no universal-character-name is formed inside a C++ raw string, so a
    // real ellipsis here would ride as raw bytes and depend on what the compiler
    // assumes the source charset is.
    out = out.slice(0, cut) + "... (" + (out.length - cut) + " more characters)";
  }
  return out;
}
)JS";

// Drops ctx's pending exception. Used where a failure has already been turned
// into a reply and letting the exception stand would poison the next eval.
void Discard(JSContext *ctx) {
  if (JS_HasException(ctx)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
  }
}

// Runs the job queue until `value` settles, and reports "still pending" rather
// than treating it as an error: awaiting something only a later frame can
// resolve is a legitimate thing to type at a REPL. Consumes `value`.
JSValue Settle(JSContext *ctx, JSValue value, bool *pending) {
  *pending = false;
  for (;;) {
    switch (JS_PromiseState(ctx, value)) {
    case JS_PROMISE_FULFILLED: {
      JSValue result = JS_PromiseResult(ctx, value);
      JS_FreeValue(ctx, value);
      return result;
    }
    case JS_PROMISE_REJECTED: {
      JSValue error = JS_Throw(ctx, JS_PromiseResult(ctx, value));
      JS_FreeValue(ctx, value);
      return error;
    }
    case JS_PROMISE_PENDING: {
      JSContext *job_ctx = nullptr;
      int rc = JS_ExecutePendingJob(Runtime, &job_ctx);
      if (rc < 0) {
        js::ReportException(job_ctx ? job_ctx : ctx, "repl job");
      } else if (rc == 0) {
        JS_FreeValue(ctx, value);
        *pending = true;
        return JS_UNDEFINED;
      }
      break;
    }
    default: // not a promise at all
      return value;
    }
  }
}

// Evaluates the prelude as a module (it needs `import`) and settles it.
bool RunPrelude(JSContext *ctx) {
  JSValue compiled = JS_Eval(ctx, Prelude, sizeof(Prelude) - 1, "<repl>",
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    js::ReportException(ctx, "repl");
    return false;
  }
  bool pending = false;
  JSValue result = Settle(ctx, JS_EvalFunction(ctx, compiled), &pending);
  if (JS_IsException(result)) {
    js::ReportException(ctx, "repl");
    return false;
  }
  JS_FreeValue(ctx, result);
  if (pending) {
    js::Log("repl: the prelude never settled; the channel is closed");
    return false;
  }
  return true;
}

bool BuildContext(JSRuntime *runtime) {
  Context = JS_NewContext(runtime);
  if (!Context) {
    js::Log("repl: could not create the context; the channel is closed");
    return false;
  }
  // Class ids are per-runtime and already handed out by the host's context, but
  // prototypes are per-context (JS_SetClassProto), so the bindings have to be
  // registered again here. RegisterClass is a no-op for the runtime half - see
  // its comment in JsCommon.cpp.
  if (!js::RegisterGkModule(Context)) {
    js::Log("repl: could not register the bindings; the channel is closed");
    return false;
  }
  if (!RunPrelude(Context)) {
    return false;
  }
  Format = JS_Eval(Context, Formatter, sizeof(Formatter) - 1, "<repl>",
                   JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(Format)) {
    Format = JS_UNDEFINED;
    js::ReportException(Context, "repl");
    return false;
  }
  return true;
}

// --- replies -----------------------------------------------------------------

// Serialises a reply object. QuickJS is the codec here for the same reason it is
// in src/Json.cpp: correct escaping of whatever a snippet returned, including the
// newlines that would otherwise break the line framing, without a grammar anyone
// has to maintain. Consumes `reply`.
std::string Encode(JSValue reply) {
  JSValue json = JS_JSONStringify(Context, reply, JS_UNDEFINED, JS_UNDEFINED);
  JS_FreeValue(Context, reply);
  if (JS_IsException(json)) {
    Discard(Context);
    return R"({"ok":false,"error":"the reply could not be encoded"})";
  }
  const char *text = JS_ToCString(Context, json);
  std::string out = text ? text : R"({"ok":false,"error":"the reply could not be encoded"})";
  JS_FreeCString(Context, text);
  JS_FreeValue(Context, json);
  return out;
}

// Reads a string property, or "" if it is missing or unreadable.
std::string PropertyString(JSValueConst object, const char *name) {
  JSValue value = JS_GetPropertyStr(Context, object, name);
  if (JS_IsException(value)) {
    Discard(Context);
    return {};
  }
  std::string out;
  if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
    const char *text = JS_ToCString(Context, value);
    if (text) {
      out = text;
      JS_FreeCString(Context, text);
    } else {
      Discard(Context); // toString threw
    }
  }
  JS_FreeValue(Context, value);
  return out;
}

// Turns the pending exception into `error` (and `stack`, when there is one) on
// `reply`. Clears it either way, so the next snippet starts clean.
void DescribeException(JSValue reply) {
  JSValue exception = JS_GetException(Context);

  std::string stack;
  if (JS_IsError(exception)) {
    stack = PropertyString(exception, "stack");
  }
  const char *text = JS_ToCString(Context, exception);
  std::string message = text ? text : "<an exception that could not be printed>";
  if (!text) {
    Discard(Context);
  }
  JS_FreeCString(Context, text);
  JS_FreeValue(Context, exception);

  JS_SetPropertyStr(Context, reply, "error",
                    JS_NewStringLen(Context, message.data(), message.size()));
  if (!stack.empty()) {
    JS_SetPropertyStr(Context, reply, "stack",
                      JS_NewStringLen(Context, stack.data(), stack.size()));
  }
}

// Runs the formatter over a result. Consumes `value`.
JSValue Display(JSValue value) {
  JSValueConst args[] = {value, JS_NewInt32(Context, kValueChars)};
  JSValue text = JS_Call(Context, Format, JS_UNDEFINED, 2, args);
  JS_FreeValue(Context, value);
  if (JS_IsException(text)) {
    Discard(Context);
    return JS_NewString(Context, "<could not format>");
  }
  return text;
}

// --- requests ----------------------------------------------------------------

// One rule decides what a line is: an object with a string `code` is a request,
// anything else is source. That keeps a bare `nc` session usable for one-liners
// (`actors.count` is not JSON at all, and `1` is JSON but not an object) while
// multi-line source travels as {"code": "..."} with its newlines escaped. The
// single ambiguity is a JS object literal that happens to have a string `code`
// property, which is read as a request.
std::string HandleRequest(const std::string &line) {
  std::string code;
  bool structured = false;
  JSValue id = JS_UNDEFINED;

  JSValue request = JS_ParseJSON(Context, line.c_str(), line.size(), "<repl>");
  if (JS_IsException(request)) {
    Discard(Context);
    request = JS_UNDEFINED;
  }
  if (JS_IsObject(request)) {
    JSValue field = JS_GetPropertyStr(Context, request, "code");
    if (JS_IsException(field)) {
      Discard(Context);
    } else if (JS_IsString(field)) {
      const char *text = JS_ToCString(Context, field);
      if (text) {
        code = text;
        structured = true;
        JS_FreeCString(Context, text);
        id = JS_GetPropertyStr(Context, request, "id");
        if (JS_IsException(id)) {
          Discard(Context);
          id = JS_UNDEFINED;
        }
      }
    }
    JS_FreeValue(Context, field);
  }
  JS_FreeValue(Context, request);
  if (!structured) {
    code = line;
  }

  JSValue reply = JS_NewObject(Context);
  if (JS_IsException(reply)) {
    Discard(Context);
    JS_FreeValue(Context, id);
    return R"({"ok":false,"error":"out of memory"})";
  }

  // Global scope, not module scope: it is what hands the expression's value
  // straight back and lets `var`/`function` persist between lines. The cost is
  // that `import` is unavailable - the prelude has already put every "gk"
  // namespace on globalThis, and dynamic import() still works for a script file.
  EvalDeadline = GetTickCount64() + kEvalBudgetMs;
  JSValue result =
      JS_Eval(Context, code.c_str(), code.size(), "<repl>", JS_EVAL_TYPE_GLOBAL);
  bool pending = false;
  if (!JS_IsException(result)) {
    result = Settle(Context, result, &pending);
  }
  EvalDeadline = 0;

  if (JS_IsException(result)) {
    JS_FreeValue(Context, result);
    JS_SetPropertyStr(Context, reply, "ok", JS_NewBool(Context, false));
    DescribeException(reply);
  } else {
    JS_SetPropertyStr(Context, reply, "ok", JS_NewBool(Context, true));
    if (pending) {
      JS_FreeValue(Context, result);
      JS_SetPropertyStr(Context, reply, "value",
                        JS_NewString(Context, "<pending>"));
      JS_SetPropertyStr(Context, reply, "pending", JS_NewBool(Context, true));
    } else {
      JS_SetPropertyStr(Context, reply, "value", Display(result));
    }
  }
  if (!JS_IsUndefined(id)) {
    JS_SetPropertyStr(Context, reply, "id", id);
  } else {
    JS_FreeValue(Context, id);
  }
  return Encode(reply);
}

// --- sockets -----------------------------------------------------------------

void Close(Connection &connection) {
  if (connection.socket != INVALID_SOCKET) {
    ::closesocket(connection.socket);
    connection.socket = INVALID_SOCKET;
  }
}

// Queues one line. Nothing is written here - Flush does that - so a client that
// has stopped reading can never block a frame.
//
// The terminator is CRLF rather than LF so a line-oriented terminal client sees
// each reply on its own line without a staircase. It costs a reader nothing: a
// consumer splitting on '\n' is left with a trailing '\r', which JSON counts as
// whitespace, so JSON.parse takes the line either way.
void Queue(Connection &connection, const std::string &line) {
  if (connection.outgoing.size() + line.size() + 2 > kMaxPendingBytes) {
    js::Log("repl: dropping a client that is not reading its replies");
    Close(connection);
    return;
  }
  connection.outgoing += line;
  connection.outgoing += "\r\n";
}

void Flush(Connection &connection) {
  while (!connection.outgoing.empty() && connection.socket != INVALID_SOCKET) {
    int chunk = static_cast<int>(
        (std::min)(connection.outgoing.size(), static_cast<size_t>(64 * 1024)));
    int sent = ::send(connection.socket, connection.outgoing.data(), chunk, 0);
    if (sent > 0) {
      connection.outgoing.erase(0, static_cast<size_t>(sent));
      continue;
    }
    if (sent < 0 && ::WSAGetLastError() == WSAEWOULDBLOCK) {
      return; // the rest goes out next frame
    }
    Close(connection);
    return;
  }
}

void Accept() {
  for (;;) {
    SOCKET client = ::accept(Listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      return; // WSAEWOULDBLOCK on the common path
    }
    if (Connections.size() >= kMaxConnections) {
      ::closesocket(client);
      continue;
    }
    u_long nonblocking = 1;
    ::ioctlsocket(client, FIONBIO, &nonblocking);
    BOOL nodelay = TRUE;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
    Connections.push_back(Connection{client, {}, {}});
  }
}

// Reads what is available and runs every complete line. Returns with the
// connection closed if the peer went away or misbehaved.
void Read(Connection &connection) {
  char buffer[4096];
  for (size_t reads = 0; reads < kReadsPerFrame; ++reads) {
    int got = ::recv(connection.socket, buffer, sizeof(buffer), 0);
    if (got == 0) { // orderly close
      Close(connection);
      return;
    }
    if (got < 0) {
      if (::WSAGetLastError() != WSAEWOULDBLOCK) {
        Close(connection);
        return;
      }
      break; // nothing more this frame
    }
    if (connection.incoming.size() + static_cast<size_t>(got) >
        kMaxRequestBytes) {
      js::Log("repl: dropping a client that sent an oversized line");
      Close(connection);
      return;
    }
    connection.incoming.append(buffer, static_cast<size_t>(got));
  }

  size_t start = 0;
  for (;;) {
    size_t newline = connection.incoming.find('\n', start);
    if (newline == std::string::npos) {
      break;
    }
    std::string line = connection.incoming.substr(start, newline - start);
    start = newline + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back(); // a telnet-style client, or a file with CRLF endings
    }
    if (line.empty()) {
      continue;
    }
    Queue(connection, HandleRequest(line));
    if (connection.socket == INVALID_SOCKET) {
      return; // Queue hung up on it
    }
  }
  connection.incoming.erase(0, start);
}

bool OpenListener(int port) {
  Listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (Listener == INVALID_SOCKET) {
    js::Log("repl: could not create the listening socket");
    return false;
  }
  // Not SO_REUSEADDR: on Windows that lets a second process bind the same port
  // and silently steal connections, which for this channel would mean typing at
  // one game instance and reaching another.
  BOOL exclusive = TRUE;
  ::setsockopt(Listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK); // never INADDR_ANY
  address.sin_port = ::htons(static_cast<unsigned short>(port));
  if (::bind(Listener, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    js::Log("repl: could not bind the port; the channel is closed");
    return false;
  }
  if (::listen(Listener, static_cast<int>(kMaxConnections)) != 0) {
    js::Log("repl: could not listen; the channel is closed");
    return false;
  }

  // Read the port back rather than believing `port`: with 0 the OS chose it,
  // and that choice is the whole point of the mode. Doing it unconditionally
  // means one path produces the number everything downstream reports.
  sockaddr_in actual{};
  int actual_size = sizeof(actual);
  if (::getsockname(Listener, reinterpret_cast<sockaddr *>(&actual),
                    &actual_size) != 0) {
    js::Log("repl: could not read back the bound port; the channel is closed");
    return false;
  }
  ListenPort = ::ntohs(actual.sin_port);

  u_long nonblocking = 1;
  ::ioctlsocket(Listener, FIONBIO, &nonblocking);
  return true;
}

// Hands the bound port to the launcher named by GKPLUS_LAUNCHER_HWND: one
// posted message carrying the pid in wParam and the port in lParam.
//
// This exists because **a launcher cannot pick the port itself without a race**.
// Anything between "find a free port" and "the game binds it" is a window for
// something else to take it, and a launcher guessing inside the ephemeral range
// can also land on a block Hyper-V has reserved outright and get WSAEACCES with
// nothing listening there. So the game binds 0, the OS picks under
// SO_EXCLUSIVEADDRUSE - which cannot hand the same port to two binds - and the
// number travels back here. There is no gap to lose.
//
// The pid rides along rather than being left to the launcher to derive from the
// sender window, so it does not have to assume the game window is findable at
// this moment, and so one message answers both "which port" and "which gl.exe" -
// the second being what tells a live game from a leftover one.
void PublishToLauncher() {
  char configured[32]{};
  DWORD len = GetEnvironmentVariableA("GKPLUS_LAUNCHER_HWND", configured,
                                      sizeof(configured));
  if (len == 0 || len >= sizeof(configured)) {
    return; // no launcher listening, which is the normal case
  }
  char *end = nullptr;
  unsigned long long parsed = std::strtoull(configured, &end, 0);
  if (end == configured || *end != '\0' || parsed == 0) {
    js::Log("repl: GKPLUS_LAUNCHER_HWND is not a window handle; not published");
    return;
  }
  // A window handle is always 32-bit-safe, so a 64-bit launcher can hand its
  // HWND to this 32-bit process as a plain number without truncating it.
  HWND target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(parsed));

  // Handles are recycled and an environment variable outlives whatever set it,
  // so a stale one would deliver the port to an unrelated window. The class name
  // is what proves this is a launcher and not whoever inherited the number.
  char klass[64]{};
  if (!::IsWindow(target) ||
      ::GetClassNameA(target, klass, sizeof(klass)) == 0 ||
      std::strcmp(klass, kLauncherClass) != 0) {
    js::Log("repl: GKPLUS_LAUNCHER_HWND does not name a launcher window; "
            "not published");
    return;
  }

  const UINT message = ::RegisterWindowMessageA(kLauncherMessage);
  if (message == 0) {
    js::Log("repl: could not register the launcher message; not published");
    return;
  }

  // **Posted, not sent**, and the payload is small precisely so that it can be.
  // A WM_COPYDATA would have to be *sent*: the window manager marshals its
  // buffer into the receiver during the call, so a posted one arrives as a
  // pointer into this process's address space - meaningless to the launcher,
  // and in the usual 32-bit-game/64-bit-launcher pairing not even the same
  // width. Sending it would put the game's main thread, inside SetupMenus, at
  // the mercy of the launcher's message loop. A pid and a port fit in the two
  // parameters with room to spare, so nothing here waits on the launcher at
  // all.
  //
  // What that costs is knowing: a post is confirmed *queued*, not delivered, so
  // a launcher that has died still looks like success from here. The failure is
  // the launcher's to detect by timing out, which it must be able to do anyway -
  // the game may simply not have got this far yet.
  if (::PostMessageA(target, message,
                     static_cast<WPARAM>(::GetCurrentProcessId()),
                     static_cast<LPARAM>(ListenPort)) == 0) {
    // Also what a UIPI drop looks like: a launcher at a higher integrity level
    // has to allow this message id through with ChangeWindowMessageFilterEx, or
    // it never arrives.
    js::Log("repl: could not post the port to the launcher");
    return;
  }
  js::Log("repl: published the port to the launcher");
}

} // namespace

bool StartRepl(JSRuntime *runtime) {
  char configured[16]{};
  DWORD len = GetEnvironmentVariableA("GKPLUS_REPL_PORT", configured,
                                      sizeof(configured));
  if (len == 0 || len >= sizeof(configured)) {
    return false; // not configured, which is the normal case
  }
  // "0" and "auto" both mean "let the OS choose", which is the mode a launcher
  // should use: see PublishToLauncher for why picking a number is a race. The
  // parse is strtol rather than atoi so that garbage still reports as garbage -
  // atoi answers 0 for "banana", which would now read as a request for the
  // ephemeral mode instead of as the mistake it is.
  char *end = nullptr;
  long parsed = std::strtol(configured, &end, 10);
  const bool automatic = std::strcmp(configured, "auto") == 0;
  if (!automatic &&
      (end == configured || *end != '\0' || parsed < 0 || parsed > 65535)) {
    js::Log("repl: GKPLUS_REPL_PORT is not a port; the channel is closed");
    return false;
  }
  int port = automatic ? 0 : static_cast<int>(parsed);

  // Not from DllMain: WSAStartup loads DLLs, so calling it under the loader lock
  // can deadlock. BootScriptHost runs from the game's SetupMenus, well past that.
  // The call is refcounted per process, so it is safe whether or not DirectPlay
  // has already made it.
  WSADATA winsock{};
  if (::WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    js::Log("repl: winsock is unavailable; the channel is closed");
    return false;
  }
  WinsockReady = true;

  Runtime = runtime;
  // The context first: a failed one must not leave a port listening that answers
  // nothing.
  if (!BuildContext(runtime) || !OpenListener(port)) {
    StopRepl();
    return false;
  }
  JS_SetInterruptHandler(Runtime, Interrupted, nullptr);
  // Without this the channel answers only while the game window has focus,
  // because the frame loop stops dead when it does not - see
  // SetFrameWakeupEnabled in GUI.h. Enabled with the channel rather than always,
  // so a build with no REPL pays nothing.
  SetFrameWakeupEnabled(true);

  js::Log(("repl: listening on 127.0.0.1:" + std::to_string(ListenPort))
              .c_str());
  // Last, so that everything a client can reach is already in place: the
  // launcher's next move is to connect.
  PublishToLauncher();
  return true;
}

void PumpRepl() {
  if (Listener == INVALID_SOCKET || Pumping) {
    return;
  }
  Pumping = true;

  // Accepting first is what keeps the loop below safe: it is the only phase that
  // grows Connections, so nothing reallocates under the iteration.
  Accept();
  for (Connection &connection : Connections) {
    if (connection.socket != INVALID_SOCKET) {
      Read(connection);
    }
    if (connection.socket != INVALID_SOCKET) {
      Flush(connection);
    }
  }
  std::erase_if(Connections, [](const Connection &connection) {
    return connection.socket == INVALID_SOCKET;
  });

  Pumping = false;
}

int NotifyRepl(JSContext *ctx, const char *event, JSValueConst data) {
  // Before anything is built: with no listener there is nobody to encode for,
  // and this is the state every ordinary launch is in. See the header for why
  // the test is the channel rather than the client count.
  if (Listener == INVALID_SOCKET || !event) {
    return 0;
  }

  JSValue line = JS_NewObject(ctx);
  if (JS_IsException(line)) {
    return -1;
  }
  JS_SetPropertyStr(ctx, line, "event", JS_NewString(ctx, event));
  if (!JS_IsUndefined(data)) {
    JS_SetPropertyStr(ctx, line, "data", JS_DupValue(ctx, data));
  }

  // Encoded in the caller's context, which is the only one `data` is safe to be
  // read through, and which is also where a thrown getter has to surface. Same
  // codec as everywhere else in this file, for the same reason: it escapes the
  // newlines that would otherwise break the framing.
  JSValue json = JS_JSONStringify(ctx, line, JS_UNDEFINED, JS_UNDEFINED);
  JS_FreeValue(ctx, line);
  if (JS_IsException(json)) {
    return -1; // the exception is the caller's to propagate
  }
  const char *text = JS_ToCString(ctx, json);
  JS_FreeValue(ctx, json);
  if (!text) {
    return -1;
  }
  std::string encoded = text;
  JS_FreeCString(ctx, text);

  int reached = 0;
  for (Connection &connection : Connections) {
    if (connection.socket == INVALID_SOCKET) {
      continue; // dropped earlier this frame; PumpRepl erases it
    }
    Queue(connection, encoded);
    if (connection.socket != INVALID_SOCKET) {
      ++reached; // Queue hangs up on a client too far behind
    }
  }
  return reached;
}

int ReplClientCount() {
  int connected = 0;
  for (const Connection &connection : Connections) {
    if (connection.socket != INVALID_SOCKET) {
      ++connected;
    }
  }
  return connected;
}

bool ReplOpen() { return Listener != INVALID_SOCKET; }

int ReplPort() { return Listener != INVALID_SOCKET ? ListenPort : 0; }

void StopRepl() {
  // KillTimer is a plain USER32 call, so this is safe from
  // DllMain(DLL_PROCESS_DETACH) - which is precisely why the wake-up is a
  // WM_TIMER and not a thread posting messages.
  SetFrameWakeupEnabled(false);

  for (Connection &connection : Connections) {
    Close(connection);
  }
  Connections.clear();
  if (Listener != INVALID_SOCKET) {
    ::closesocket(Listener);
    Listener = INVALID_SOCKET;
  }
  ListenPort = 0;
  if (Context) {
    if (Runtime) {
      JS_SetInterruptHandler(Runtime, nullptr, nullptr);
    }
    // Anything the REPL registered - a menu item, a level - is held with this
    // context, and both Release* helpers filter on it. Runs before the host's
    // own teardown, which is where the shared registrations get cleared.
    js::ReleaseCallbacks(Context);
    JS_FreeValue(Context, Format);
    Format = JS_UNDEFINED;
    JS_FreeContext(Context);
    Context = nullptr;
  }
  Runtime = nullptr;
  if (WinsockReady) {
    ::WSACleanup();
    WinsockReady = false;
  }
}

} // namespace gk
