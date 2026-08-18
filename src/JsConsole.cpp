#include "Console.h"

#include "Encoding.h"
#include "Js.h"
#include "JsBindings.h"
#include "Misc.h"

#include <cstdint>
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
  bool ran = ExecuteCommand(command);
  size_t length = std::strlen(command);
  JS_FreeCString(ctx, command);
  if (!ran) {
    // The engine copies into a 252-byte buffer with no length check at all, so
    // a longer line writes through the font pointer that follows it. Refusing
    // is the fix; truncating would just run a different command.
    return JS_ThrowRangeError(
        ctx, "command is %d characters; the game's buffer holds %d",
        static_cast<int>(length), kConsoleCommandLineMax);
  }
  return JS_UNDEFINED;
}

// The console's own administration commands. Six of the eight are registered
// under names from glres<lang>.dll, which is exactly why they are here rather
// than left to console.execute.
#define GK_CONSOLE_COMMAND(fn, command)                                        \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunConsoleCommand(ctx, command, argc, argv);                        \
  }
#define GK_CONSOLE_LOCALIZED(fn, id)                                           \
  JSValue fn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {     \
    return RunLocalizedConsoleCommand(ctx, id, argc, argv);                    \
  }

GK_CONSOLE_COMMAND(Hide, "HIDE CONSOLE")
// LOG takes the rest of the line, so its note may contain spaces.
JSValue WriteLog(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return RunConsoleTextCommand(ctx, "LOG", argc, argv);
}
GK_CONSOLE_COMMAND(PrintVersion, "VERSION")
GK_CONSOLE_LOCALIZED(SetLines, 10004)
GK_CONSOLE_LOCALIZED(SetAppear, 10005)
GK_CONSOLE_LOCALIZED(ClearHistory, 10009)
GK_CONSOLE_LOCALIZED(HistorySize, 10010)
GK_CONSOLE_LOCALIZED(QueueSize, 10007)

#undef GK_CONSOLE_COMMAND
#undef GK_CONSOLE_LOCALIZED

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

// The registered command table, as an array of
// `{name, help, condition, available}`.
//
// It exists because fifteen command names are **localized**: SetupConsoleCommands
// registers EXIT, QUIT, MENU, HELP, LINES, CONSOLE APPEAR, SAY, TIME, DATE, LIST
// COMMANDS and the history/queue-size pairs under strings pulled from
// glres<lang>.dll, so `execute("QUIT")` is a no-op on a non-English install.
// Enumerating is the only portable way to find those - and the only way to
// discover what a mod or a future patch added.
//
// Encoding: the names and help are the engine's own `char *`, i.e. CP_ACP, so
// they go through Utf8FromGameText on the way out like every other game string.
struct CommandCollector {
  JSContext *ctx;
  JSValue array;
  uint32_t index;
  bool failed;
};

void CollectCommand(void *user, const CommandData *cmd) {
  auto *state = static_cast<CommandCollector *>(user);
  if (state->failed || cmd->name == nullptr) {
    return;
  }
  // The registry holds one deliberately empty name, whose handler is a RET stub
  // so that a blank console line does nothing. It is not a command.
  if (cmd->name[0] == '\0') {
    return;
  }
  JSValue entry = JS_NewObject(state->ctx);
  if (JS_IsException(entry)) {
    state->failed = true;
    return;
  }
  JS_SetPropertyStr(state->ctx, entry, "name",
                    JS_NewString(state->ctx, Utf8FromGameText(cmd->name).c_str()));
  JS_SetPropertyStr(
      state->ctx, entry, "help",
      cmd->help == nullptr
          ? JS_NULL
          : JS_NewString(state->ctx, Utf8FromGameText(cmd->help).c_str()));
  JS_SetPropertyStr(state->ctx, entry, "condition",
                    JS_NewInt32(state->ctx, cmd->condition));
  JS_SetPropertyStr(state->ctx, entry, "available",
                    JS_NewBool(state->ctx,
                               cmd->condition <= GetCommandCondition()));
  JS_SetPropertyUint32(state->ctx, state->array, state->index++, entry);
}

// `ECHO ON|OFF` - when off, the console's own printing is suppressed. It is also
// what `@` toggles for the span of one line.
JSValue GetEcho(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, GetConsoleEchoEnabled());
}

JSValue SetEcho(JSContext *ctx, JSValueConst, JSValueConst v) {
  SetConsoleEchoEnabled(JS_ToBool(ctx, v) != 0);
  return JS_UNDEFINED;
}

// Whether `InitConsole` has run. False only from the profile's boot module,
// which is anchored ahead of it - and there it is worth asking, because printing
// while it is false does nothing at all rather than queuing up for later.
JSValue GetReady(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, ConsoleReady());
}

JSValue GetCommands(JSContext *ctx, JSValueConst) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  CommandCollector state{ctx, array, 0, false};
  ForEachConsoleCommand(&state, CollectCommand);
  if (state.failed) {
    JS_FreeValue(ctx, array);
    return JS_EXCEPTION;
  }
  return array;
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
    JS_CGETSET_DEF2("echo", GetEcho, SetEcho,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF("ready", GetReady, nullptr),
    JS_CGETSET_DEF("commands", GetCommands, nullptr),
    JS_CFUNC_DEF("hide", 0, Hide),
    JS_CFUNC_DEF("write_log", 1, WriteLog),
    JS_CFUNC_DEF("print_version", 0, PrintVersion),
    JS_CFUNC_DEF("set_lines", 1, SetLines),
    JS_CFUNC_DEF("set_appear", 1, SetAppear),
    JS_CFUNC_DEF("clear_history", 0, ClearHistory),
    JS_CFUNC_DEF("history_size", 1, HistorySize),
    JS_CFUNC_DEF("queue_size", 1, QueueSize),
};

} // namespace

JSValue NewConsoleNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ConsoleProps,
                      static_cast<int>(std::size(ConsoleProps)));
}

} // namespace gk::js
