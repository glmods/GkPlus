// The `repl` namespace: the backchannel out of a script and into whatever is
// connected to the REPL socket (src/Repl.h).
//
// Everything else in this layer answers a question the socket asked. This is the
// one direction that does not: a script pushes a line nobody requested, which is
// what makes it worth having. Polling `actors` from the outside can only sample -
// it sees the state of whichever frame the request happened to land in - so
// anything that *happens* (a trigger fired, a role spawned, a message arrived on
// the script queue) is invisible to a poller unless it leaves a trace. A
// notification is the trace.
//
// It is not a log. `console.log` goes to the game's console and to the debugger,
// carries text, and is what a human reads; this carries a structured value to a
// program, and costs a call and a branch when the channel is closed - which is
// every launch that did not set GKPLUS_REPL_PORT.

#include "Repl.h"

#include "JsBindings.h"

#include <iterator>

namespace gk::js {
namespace {

// notify(event, data?) -> the number of clients it reached.
//
// The count is the return value rather than nothing because it is the only
// answer to "did that go anywhere", and 0 is the ordinary case: the channel is
// usually closed, and even when it is open nobody may be attached yet.
JSValue NotifyJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "notify(event, data) expects an event name");
  }
  // Strings only, deliberately. `event` is what a client dispatches on, and
  // coercing a number or an object into one would produce a name nobody meant
  // to publish.
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "notify(event, data): event must be a string");
  }
  const char *event = JS_ToCString(ctx, argv[0]);
  if (!event) {
    return JS_EXCEPTION;
  }

  const int reached =
      NotifyRepl(ctx, event, argc > 1 ? argv[1] : JS_UNDEFINED);
  JS_FreeCString(ctx, event);
  if (reached < 0) {
    return JS_EXCEPTION; // NotifyRepl left the encoder's exception pending
  }
  return JS_NewInt32(ctx, reached);
}

JSValue GetClientsJs(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, ReplClientCount());
}

JSValue GetOpenJs(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, ReplOpen());
}

const JSCFunctionListEntry ReplProps[] = {
    JS_CFUNC_DEF("notify", 2, NotifyJs),
    JS_CGETSET_DEF("clients", GetClientsJs, nullptr),
    JS_CGETSET_DEF("open", GetOpenJs, nullptr),
};

} // namespace

JSValue NewReplNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ReplProps, static_cast<int>(std::size(ReplProps)));
}

} // namespace gk::js
