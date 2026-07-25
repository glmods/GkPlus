#include "Js.h"

#include "JsBindings.h"

#include <iterator>

namespace gk::js {
namespace {

struct NamespaceEntry {
  const char *name;
  JSValue (*make)(JSContext *ctx);
};

// `actors` before `roles` only for readability - a C module's exports are only
// materialised on import, and nothing here runs at registration time.
//
// One name worth knowing about: `import { console } from "gk"` shadows any
// global `console` inside that module. Nothing collides today (QuickJS core has
// no console object - that lives in quickjs-libc, which this port does not
// install), but a host that adds one would.
const NamespaceEntry Namespaces[] = {
    {"camera", NewCameraNamespace},   {"console", NewConsoleNamespace},
    {"actors", NewActorsNamespace},   {"roles", NewRolesNamespace},
    {"tokens", NewTokensNamespace},   {"triggers", NewTriggersNamespace},
};

int InitModule(JSContext *ctx, JSModuleDef *m) {
  JSValue aggregate = JS_NewObject(ctx); // the default export
  if (JS_IsException(aggregate)) {
    return -1;
  }

  for (const NamespaceEntry &entry : Namespaces) {
    JSValue ns = entry.make(ctx);
    if (JS_IsException(ns)) {
      JS_FreeValue(ctx, aggregate);
      return -1;
    }
    // The named export and the default export's property are the same object,
    // so `import gk from "gk"` and `import { camera } from "gk"` agree.
    if (JS_SetPropertyStr(ctx, aggregate, entry.name, JS_DupValue(ctx, ns)) < 0) {
      JS_FreeValue(ctx, ns);
      JS_FreeValue(ctx, aggregate);
      return -1;
    }
    // Consumes `ns` whether it succeeds or not.
    if (JS_SetModuleExport(ctx, m, entry.name, ns) < 0) {
      JS_FreeValue(ctx, aggregate);
      return -1;
    }
  }

  return JS_SetModuleExport(ctx, m, "default", aggregate);
}

} // namespace

bool RegisterGkModule(JSContext *ctx) {
  // A bare specifier needs no module loader and no normalizer:
  // js_default_module_normalize_name passes through anything not starting with
  // '.', and module resolution checks the already-loaded modules - where
  // JS_NewCModule registers this one - before consulting any loader.
  JSModuleDef *m = JS_NewCModule(ctx, "gk", InitModule);
  if (!m) {
    return false;
  }
  for (const NamespaceEntry &entry : Namespaces) {
    if (JS_AddModuleExport(ctx, m, entry.name) < 0) {
      return false;
    }
  }
  return JS_AddModuleExport(ctx, m, "default") >= 0;
}

} // namespace gk::js
