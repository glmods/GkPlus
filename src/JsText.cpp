// The `text` namespace: the engine's own text layer, exposed to script.
//
// This wraps the queue, not a draw. `text.draw(...)` appends to the font's pending list
// and the game's per-frame overlay pass rasterizes it - so **a string drawn once is on
// screen for one frame and then gone**. Anything meant to persist has to be drawn from
// something that runs every frame; the ImGui overlay handed to `draw_gui` is the usual
// place, and is also the better tool when the goal is a panel rather than text sitting
// in the game's own font on the game's own render target.
//
// What this is for is the other case: text that has to look like the game's, because it
// goes through the game's fonts, colours, layout and batching.

#include "Font.h"

#include "JsBindings.h"

#include <iterator>
#include <string>

namespace gk::js {
namespace {

struct NamedFont {
  const char *name;
  FontId id;
};

// The names are ours, not the engine's - the game has no string for any of these. They
// are named for the role each font actually plays, established from the `.RIM` each is
// built from plus its consumers (see FontId); an earlier set of names had three of the
// four as "console" fonts, which only `small` is.
constexpr NamedFont Fonts[] = {
    {"small", FontId::Small},
    {"large", FontId::Large},
    {"hud", FontId::Hud},
    {"heading", FontId::Heading},
};

// Resolves a font name to the live object. Throws for an unknown name (a typo should
// not silently draw in the wrong font) and for a null font, which means the game has
// not finished starting up.
Font *FontFromName(JSContext *ctx, const char *name) {
  for (const NamedFont &candidate : Fonts) {
    if (std::string(name) == candidate.name) {
      Font *font = GetFont(candidate.id);
      if (!font) {
        JS_ThrowInternalError(ctx, "the font '%s' does not exist yet", name);
        return nullptr;
      }
      return font;
    }
  }
  JS_ThrowTypeError(ctx, "unknown font '%s'", name);
  return nullptr;
}

// Reads the optional `font` property, defaulting to the small font - the one the game
// itself uses for the version stamp, every console line and 32 of its 39 text draws.
Font *GetFontProp(JSContext *ctx, JSValueConst options) {
  JSValue prop = JS_GetPropertyStr(ctx, options, "font");
  if (JS_IsException(prop)) {
    return nullptr;
  }
  if (JS_IsUndefined(prop) || JS_IsNull(prop)) {
    JS_FreeValue(ctx, prop);
    Font *font = GetFont(FontId::Small);
    if (!font) {
      JS_ThrowInternalError(ctx, "the fonts do not exist yet");
    }
    return font;
  }
  const char *name = JS_ToCString(ctx, prop);
  JS_FreeValue(ctx, prop);
  if (!name) {
    return nullptr;
  }
  Font *font = FontFromName(ctx, name);
  JS_FreeCString(ctx, name);
  return font;
}

// `rect` is a nested object so the four members keep the engine's names rather than
// becoming left/top/right/bottom on the option object beside unrelated things. Absent
// members keep the default, the rule every option reader in this layer follows.
bool GetRectProp(JSContext *ctx, JSValueConst options, TextRect *out) {
  JSValue prop = JS_GetPropertyStr(ctx, options, "rect");
  if (JS_IsException(prop)) {
    return false;
  }
  if (JS_IsUndefined(prop) || JS_IsNull(prop)) {
    JS_FreeValue(ctx, prop);
    return true;
  }
  if (!JS_IsObject(prop)) {
    JS_FreeValue(ctx, prop);
    JS_ThrowTypeError(ctx, "rect must be an object with left, top, right and bottom");
    return false;
  }
  const bool ok = GetFloatProp(ctx, prop, "left", &out->left) &&
                  GetFloatProp(ctx, prop, "top", &out->top) &&
                  GetFloatProp(ctx, prop, "right", &out->right) &&
                  GetFloatProp(ctx, prop, "bottom", &out->bottom);
  JS_FreeValue(ctx, prop);
  return ok;
}

// A D3DCOLOR as a plain number. Read through int64 so 0xff00e500 survives - it does not
// fit a signed 32-bit int, and JS_ToInt32 would wrap it to a negative.
bool GetColorProp(JSContext *ctx, JSValueConst options, const char *name,
                  unsigned *out) {
  int64_t value = *out;
  if (!GetInt64Prop(ctx, options, name, &value)) {
    return false;
  }
  *out = static_cast<unsigned>(value);
  return true;
}

JSValue DrawJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "draw(options) expects an options object");
  }
  JSValueConst options = argv[0];

  Font *font = GetFontProp(ctx, options);
  if (!font) {
    return JS_EXCEPTION;
  }

  JSValue text_prop = JS_GetPropertyStr(ctx, options, "text");
  if (JS_IsException(text_prop)) {
    return JS_EXCEPTION;
  }
  const char *text = JS_ToCString(ctx, text_prop);
  JS_FreeValue(ctx, text_prop);
  if (!text) {
    return JS_EXCEPTION;
  }

  TextDraw draw;
  draw.text = text;
  // Deliberately full-screen rather than something more opinionated: the rect is the
  // layout box, so this default means "start at the top-left, wrap at the right edge,
  // clip at the bottom", which is what an omitted rect should do.
  draw.rect = TextRect{0.0f, 0.0f, 1.0f, 1.0f};

  int32_t flags = 0;
  int32_t max_chars = 0;
  int32_t skip_lines = 0;
  unsigned alt_color = 0;
  bool have_alt_color = false;

  bool ok = GetRectProp(ctx, options, &draw.rect) &&
            GetColorProp(ctx, options, "color", &draw.color) &&
            GetInt32Prop(ctx, options, "flags", &flags) &&
            GetInt32Prop(ctx, options, "max_chars", &max_chars) &&
            GetInt32Prop(ctx, options, "skip_lines", &skip_lines) &&
            GetFloatProp(ctx, options, "depth", &draw.depth);
  if (ok) {
    // Tracked separately from its value: the engine reads alt_color only when the flag
    // is set *and* the pointer is non-null, so "absent" and "black" have to differ.
    JSValue prop = JS_GetPropertyStr(ctx, options, "alt_color");
    if (JS_IsException(prop)) {
      ok = false;
    } else {
      have_alt_color = !JS_IsUndefined(prop) && !JS_IsNull(prop);
      JS_FreeValue(ctx, prop);
      if (have_alt_color) {
        ok = GetColorProp(ctx, options, "alt_color", &alt_color);
      }
    }
  }
  if (!ok) {
    JS_FreeCString(ctx, text);
    return JS_EXCEPTION;
  }

  draw.flags = static_cast<TextFlags>(flags);
  draw.max_chars = max_chars;
  draw.skip_lines = skip_lines;
  draw.alt_color = have_alt_color ? &alt_color : nullptr;

  const int lines = QueueText(font, draw);
  JS_FreeCString(ctx, text);
  return JS_NewInt32(ctx, lines);
}

JSValue LineHeightJs(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  Font *font = nullptr;
  if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
    font = GetFont(FontId::Small);
    if (!font) {
      return JS_ThrowInternalError(ctx, "the fonts do not exist yet");
    }
  } else {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
      return JS_EXCEPTION;
    }
    font = FontFromName(ctx, name);
    JS_FreeCString(ctx, name);
    if (!font) {
      return JS_EXCEPTION;
    }
  }
  return JS_NewFloat64(ctx, LineHeight(font));
}

JSValue GetFontsJs(JSContext *ctx, JSValueConst) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  uint32_t index = 0;
  for (const NamedFont &font : Fonts) {
    JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, font.name));
  }
  return array;
}

JSValue GetMaxLengthJs(JSContext *ctx, JSValueConst) {
  return JS_NewInt32(ctx, kMaxTextLength);
}

// The flag bits, so a script can spell `flags` without hardcoding numbers. A plain
// nested object rather than accessors - these are constants.
const JSCFunctionListEntry FlagProps[] = {
    JS_PROP_INT32_DEF("measure_only", 0x001, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("align_center", 0x002, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("console_cursor", 0x004, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("no_layout", 0x008, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("last_char_alt_color", 0x010, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("outline", 0x020, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("anchor_bottom", 0x040, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("clip_to_bottom", 0x080, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("align_right", 0x100, JS_PROP_ENUMERABLE),
};

const JSCFunctionListEntry TextProps[] = {
    JS_CFUNC_DEF("draw", 1, DrawJs),
    JS_CFUNC_DEF("line_height", 1, LineHeightJs),
    JS_CGETSET_DEF("fonts", GetFontsJs, nullptr),
    JS_CGETSET_DEF("max_length", GetMaxLengthJs, nullptr),
    JS_OBJECT_DEF("flags", FlagProps, static_cast<int>(std::size(FlagProps)),
                  JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

} // namespace

JSValue NewTextNamespace(JSContext *ctx) {
  return NewNamespace(ctx, TextProps, static_cast<int>(std::size(TextProps)));
}

} // namespace gk::js
