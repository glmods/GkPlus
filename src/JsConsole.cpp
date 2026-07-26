#include "Console.h"

#include "Js.h"
#include "JsBindings.h"

#include <iterator>
#include <string>

namespace gk::js {
namespace {

JSValue ConsolePrint(JSContext *ctx, JSValueConst, int argc,
                     JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "print(text) expects one argument");
  }
  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }
  Print(text);
  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// log/info/warn/error/debug, all the same thing. This is the host's own
// logging - it was a global `console` until there was no reason to have two
// console objects in scope. Unlike print() it takes any number of values of any
// type, joins them with spaces, and goes through js::Log, which also writes to
// the debugger and feeds the game console one line at a time (it keeps one list
// entry per line and does not wrap on '\n').
JSValue ConsoleWrite(JSContext *ctx, JSValueConst, int argc,
                     JSValueConst *argv) {
  std::string line;
  for (int i = 0; i < argc; ++i) {
    const char *text = JS_ToCString(ctx, argv[i]);
    if (!text) {
      return JS_EXCEPTION;
    }
    if (i) {
      line += ' ';
    }
    line += text;
    JS_FreeCString(ctx, text);
  }
  Log(line.c_str());
  return JS_UNDEFINED;
}

// Runs one console command, exactly as typing it would - the same entry point
// the console's own input takes, so a command's arguments are its own business.
JSValue Execute(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "execute(command) expects one argument");
  }
  const char *command = JS_ToCString(ctx, argv[0]);
  if (!command) {
    return JS_EXCEPTION;
  }
  ExecuteCommand(command);
  JS_FreeCString(ctx, command);
  return JS_UNDEFINED;
}

// Queues a file of console commands - what a level's .gcs is. It does not run
// them: each line is appended to the console's command queue, which pops one
// per frame, so the effects land over the following frames rather than here.
// The path is resolved by the game against its *current* directory, which moves
// during a level load. Returns whether the file opened; a missing one is false
// rather than an exception, which is how the game treats it.
JSValue ExecuteFile(JSContext *ctx, JSValueConst, int argc,
                    JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "execute_file(path) expects one argument");
  }
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  bool opened = ExecuteCommandFile(path);
  JS_FreeCString(ctx, path);
  return JS_NewBool(ctx, opened);
}

// Both colours are ARGB (0xAARRGGBB) and there are no setters in the native API:
// the getters hand back pointers into game memory and mutation means writing
// through them.
JSValue GetTextColor(JSContext *ctx, JSValueConst) {
  return JS_NewUint32(ctx, *GetConsoleTextColor());
}

JSValue SetTextColor(JSContext *ctx, JSValueConst, JSValueConst v) {
  uint32_t colour = 0;
  if (JS_ToUint32(ctx, &colour, v)) {
    return JS_EXCEPTION;
  }
  *GetConsoleTextColor() = colour;
  return JS_UNDEFINED;
}

JSValue GetCursorColor(JSContext *ctx, JSValueConst) {
  return JS_NewUint32(ctx, *GetConsoleCursorColor());
}

JSValue SetCursorColor(JSContext *ctx, JSValueConst, JSValueConst v) {
  uint32_t colour = 0;
  if (JS_ToUint32(ctx, &colour, v)) {
    return JS_EXCEPTION;
  }
  *GetConsoleCursorColor() = colour;
  return JS_UNDEFINED;
}

// The whole console surface, host logging included - there is no global
// `console`, so this object is the only one a script sees.
const JSCFunctionListEntry ConsoleProps[] = {
    JS_CFUNC_DEF2("print", 1, ConsolePrint,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("log", 1, ConsoleWrite,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("info", 1, ConsoleWrite,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("warn", 1, ConsoleWrite,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("error", 1, ConsoleWrite,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("debug", 1, ConsoleWrite,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("execute", 1, Execute,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF2("execute_file", 1, ExecuteFile,
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("text_color", GetTextColor, SetTextColor,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("cursor_color", GetCursorColor, SetCursorColor,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

} // namespace

JSValue NewConsoleNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ConsoleProps,
                      static_cast<int>(std::size(ConsoleProps)));
}

} // namespace gk::js
