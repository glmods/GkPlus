#include "GLS.h"

#include "Js.h"
#include "JsBindings.h"
#include "Roles.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// The `gls` namespace: what only the GLS *parser* can answer.
//
// Building game objects used to live here too - `gls.role({...}).register()` made
// a 0x1b60-byte ParsedThing and pushed it through the game's ToXxx converters.
// That is now `make`, over the native constructors in src/MakeRole.cpp, and this
// file keeps only the three things a reimplementation cannot provide:
//
//   schema     - the field table each section constructor declares about itself
//   probe      - what integer a GLS enum keyword stands for
//   try_parse  - does this .gls text parse
//
// All three run the real parser, so all three carry its hazard: a syntax error
// poisons LoadGLS for the rest of the process, and none of them may be called
// while a level is loading.

namespace gk::js {
namespace {

// Field type names as the schema reports them. Index is gls::FieldType.
const char *const TypeNames[] = {"none",   "boolean", "integer",
                                 "float",  "object",  "string"};

// The JS spelling of a GLS keyword: spaces become underscores.
std::string JsFieldName(const char *keyword) {
  std::string name = keyword ? keyword : "";
  for (char &c : name) {
    if (c == ' ') {
      c = '_';
    }
  }
  return name;
}

JSValue NewFieldList(JSContext *ctx, gls::SectionType section) {
  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) {
    return arr;
  }
  uint32_t n = 0;
  for (const gls::FieldInfo &field : gls::SectionFields(section)) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
    auto type_index = static_cast<size_t>(field.type);
    JS_SetPropertyStr(ctx, entry, "name",
                      JS_NewString(ctx, JsFieldName(field.name).c_str()));
    JS_SetPropertyStr(ctx, entry, "keyword",
                      JS_NewString(ctx, field.name ? field.name : ""));
    JS_SetPropertyStr(ctx, entry, "type",
                      JS_NewString(ctx, type_index < std::size(TypeNames)
                                            ? TypeNames[type_index]
                                            : "none"));
    JS_SetPropertyStr(ctx, entry, "required", JS_NewBool(ctx, !field.optional));
    JS_SetPropertyStr(ctx, entry, "none_ok", JS_NewBool(ctx, field.none_ok));
    if (field.type == gls::FieldType::Float) {
      JS_SetPropertyStr(ctx, entry, "min", JS_NewFloat64(ctx, field.min_float));
      JS_SetPropertyStr(ctx, entry, "max", JS_NewFloat64(ctx, field.max_float));
    } else if (field.type == gls::FieldType::Integer ||
               field.type == gls::FieldType::Boolean) {
      JS_SetPropertyStr(ctx, entry, "min", JS_NewInt32(ctx, field.min_integer));
      JS_SetPropertyStr(ctx, entry, "max", JS_NewInt32(ctx, field.max_integer));
    }
    if (JS_SetPropertyUint32(ctx, arr, n++, entry) < 0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }
  return arr;
}

bool ResolveSection(JSContext *ctx, JSValueConst v, gls::SectionType *out) {
  const char *name = JS_ToCString(ctx, v);
  if (!name) {
    return false;
  }
  *out = gls::SectionTypeFromName(name);
  bool ok = *out != gls::SectionType::Unknown;
  if (!ok) {
    JS_ThrowReferenceError(ctx, "no such section type '%s'", name);
  }
  JS_FreeCString(ctx, name);
  return ok;
}

// schema(section) -> [{name, keyword, type, required, none_ok, min, max}].
//
// Every section constructor writes its own field table into the object it builds -
// types, keywords, which fields are required, and the bounds CheckValue enforces -
// so this is read off the game rather than transcribed, and cannot drift from it.
JSValue GlsSchema(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  gls::SectionType section = gls::SectionType::Unknown;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "schema(section)");
  }
  if (!ResolveSection(ctx, argv[0], &section)) {
    return JS_EXCEPTION;
  }
  return NewFieldList(ctx, section);
}

// probe(section, field, names) -> { name: value | null }. The only way to learn
// what integer a GLS enum keyword stands for: the keywords are compiled into the
// lexer's flex DFA, so they are not strings in the binary and not in any shipped
// header. This hands the parser a one-field section per keyword and reads the
// stored integer back.
//
// null means "rejected, or never tested" - it STOPS at the first refusal, because
// a syntax error poisons the parser for the rest of the process.
JSValue GlsProbe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(ctx, "probe(section, field, names)");
  }
  gls::SectionType section = gls::SectionType::Unknown;
  if (!ResolveSection(ctx, argv[0], &section)) {
    return JS_EXCEPTION;
  }

  const char *field_name = JS_ToCString(ctx, argv[1]);
  if (!field_name) {
    return JS_EXCEPTION;
  }
  const gls::FieldInfo *field = gls::FindField(section, field_name);
  if (!field) {
    JSValue err = JS_ThrowReferenceError(ctx, "a %s has no field '%s'",
                                         gls::SectionTypeName(section),
                                         field_name);
    JS_FreeCString(ctx, field_name);
    return err;
  }
  JS_FreeCString(ctx, field_name);

  std::vector<std::string> keywords;
  uint32_t length = 0;
  JSValue len = JS_GetPropertyStr(ctx, argv[2], "length");
  int rc = JS_IsException(len) ? -1 : JS_ToUint32(ctx, &length, len);
  JS_FreeValue(ctx, len);
  if (rc < 0) {
    return JS_EXCEPTION;
  }
  for (uint32_t i = 0; i < length; ++i) {
    JSValue entry = JS_GetPropertyUint32(ctx, argv[2], i);
    if (JS_IsException(entry)) {
      return JS_EXCEPTION;
    }
    const char *text = JS_ToCString(ctx, entry);
    JS_FreeValue(ctx, entry);
    if (!text) {
      return JS_EXCEPTION;
    }
    keywords.emplace_back(text);
    JS_FreeCString(ctx, text);
  }

  std::vector<std::optional<int32_t>> values;
  if (!gls::ProbeKeywords(section, field->id, keywords, &values)) {
    return JS_ThrowInternalError(
        ctx, "the probe script could not be written or parsed");
  }

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  for (size_t i = 0; i < keywords.size() && i < values.size(); ++i) {
    JS_SetPropertyStr(ctx, obj, keywords[i].c_str(),
                      values[i] ? JS_NewInt32(ctx, *values[i]) : JS_NULL);
  }
  return obj;
}

// try_parse(source) -> object count, or -1 when nothing reached the list. The
// bisection tool for "why does this section not parse". Note -1 covers both
// "syntax error" and "every section demoted to abstract for want of a required
// field", which the parser does not distinguish to callers.
JSValue GlsTryParse(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "try_parse(source)");
  }
  const char *source = JS_ToCString(ctx, argv[0]);
  if (!source) {
    return JS_EXCEPTION;
  }
  int count = gls::TryParse(source);
  JS_FreeCString(ctx, source);
  return JS_NewInt32(ctx, count);
}

// sections -> the fifteen GLS section keywords, as schema() takes them.
JSValue GetSections(JSContext *ctx, JSValueConst) {
  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) {
    return arr;
  }
  uint32_t n = 0;
  for (const gls::SectionType *type = gls::AllSectionTypes();
       *type != gls::SectionType::Unknown; ++type) {
    if (JS_SetPropertyUint32(
            ctx, arr, n++,
            JS_NewString(ctx, JsFieldName(gls::SectionTypeName(*type)).c_str())) <
        0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }
  return arr;
}

const JSCFunctionListEntry GlsProps[] = {
    JS_CGETSET_DEF("sections", GetSections, nullptr),
    JS_CFUNC_DEF("schema", 1, GlsSchema),
    JS_CFUNC_DEF("probe", 3, GlsProbe),
    JS_CFUNC_DEF("try_parse", 1, GlsTryParse),
};

} // namespace

JSValue NewGlsNamespace(JSContext *ctx) {
  return NewNamespace(ctx, GlsProps, static_cast<int>(std::size(GlsProps)));
}

} // namespace gk::js
