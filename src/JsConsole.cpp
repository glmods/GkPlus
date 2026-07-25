#include "Console.h"

#include "JsBindings.h"

#include <iterator>

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

// Print plus the two colours, and deliberately nothing else: binding
// ExecuteCommand/ExecuteCommandFile would hand scripts the whole console command
// surface, including the ability to run arbitrary command files.
const JSCFunctionListEntry ConsoleProps[] = {
    JS_CFUNC_DEF2("print", 1, ConsolePrint,
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
