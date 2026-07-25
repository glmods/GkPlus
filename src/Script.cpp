#include "Script.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <imgui-quickjs.h>
#include <quickjs.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member
// pointers, and merely declaring them inside namespace gk hides the global
// templates that handle a plain function pointer.
#include "Core.h"
#include "GUI.h"
#include "Js.h"

#include <cstdio>
#include <string>
#include <vector>

namespace gk {
namespace {

StdCall<> SetupMenus;

JSRuntime *Runtime = nullptr;
JSContext *Context = nullptr;

// The entry module's exports, resolved once at boot. Undefined when the script
// does not provide them, or - for DrawGui - after it has thrown.
JSValue DrawGui = JS_UNDEFINED;
JSValue SetupMenusFn = JS_UNDEFINED;
// The two C modules' namespace objects, kept so the exports can be *handed* to
// the script rather than only importable. These are the same objects
// `import * as ImGui from "ImGui"` and `import * as gk from "gk"` produce.
JSValue ImGuiNamespace = JS_UNDEFINED;
JSValue GkNamespace = JS_UNDEFINED;

bool Booted = false;

// --- paths -------------------------------------------------------------------

// Forward slashes throughout, including the drive-letter form. QuickJS's default
// module normalizer resolves a relative specifier by scanning the importing
// module's name for '/' - with backslashes it finds none and `import "./x.mjs"`
// silently resolves to "x.mjs" in the process's current directory. Win32 takes
// forward slashes everywhere, so this costs nothing.
std::string ToForwardSlashes(std::string path) {
  for (char &c : path) {
    if (c == '\\') {
      c = '/';
    }
  }
  return path;
}

// This DLL's own directory. Derived from an address inside the module rather
// than from DllMain's HINSTANCE, so nothing has to be plumbed through.
std::string ModuleDirectory() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&ToForwardSlashes),
                          &self)) {
    return {};
  }
  char path[MAX_PATH]{};
  DWORD len = GetModuleFileNameA(self, path, sizeof(path));
  if (len == 0 || len >= sizeof(path)) {
    return {};
  }
  std::string dir = ToForwardSlashes(path);
  size_t slash = dir.find_last_of('/');
  return slash == std::string::npos ? std::string{} : dir.substr(0, slash);
}

std::string EntryModulePath() {
  char override[MAX_PATH]{};
  DWORD len = GetEnvironmentVariableA("GKPLUS_SCRIPT", override,
                                      sizeof(override));
  if (len > 0 && len < sizeof(override)) {
    return ToForwardSlashes(override);
  }
  std::string dir = ModuleDirectory();
  if (dir.empty()) {
    return {};
  }
  return dir + "/gkplus/main.mjs";
}

bool ReadWholeFile(const char *path, std::string *out) {
  std::FILE *file = std::fopen(path, "rb");
  if (!file) {
    return false;
  }
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, read);
  }
  bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

// --- module loading ----------------------------------------------------------

// `name` arrives already normalized (the default normalizer resolved it against
// the importing module), and bare specifiers never get here: module resolution
// checks the already-loaded modules - where JS_NewCModule registered "gk" and
// "ImGui" - before consulting a loader.
JSModuleDef *ModuleLoader(JSContext *ctx, const char *name, void *) {
  std::string source;
  if (!ReadWholeFile(name, &source)) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
    return nullptr;
  }
  JSValue compiled =
      JS_Eval(ctx, source.c_str(), source.size(), name,
              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    return nullptr;
  }
  auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
  JS_FreeValue(ctx, compiled);
  return module;
}

// Drains the job queue. Promise jobs are the only thing in it today - nothing
// here is asynchronous - but a script may still queue one with Promise.resolve.
void PumpJobs() {
  if (!Runtime) {
    return;
  }
  for (;;) {
    JSContext *ctx = nullptr;
    int rc = JS_ExecutePendingJob(Runtime, &ctx);
    if (rc == 0) {
      return;
    }
    if (rc < 0) {
      js::ReportException(ctx ? ctx : Context, "job");
    }
  }
}

// Settles a module-evaluation promise synchronously. Module evaluation returns a
// promise even without top-level await, so this is not optional; it is
// js_std_await minus the parts that need quickjs-libc's event loop.
JSValue Await(JSContext *ctx, JSValue value) {
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
        js::ReportException(job_ctx ? job_ctx : ctx, "job");
      } else if (rc == 0) {
        // Nothing left to run and still pending: the promise can never settle
        // (a top-level await on something no host API will ever resolve).
        JS_FreeValue(ctx, value);
        return JS_ThrowInternalError(
            ctx, "module evaluation is stuck on a pending promise");
      }
      break;
    }
    default: // not a promise at all
      return value;
    }
  }
}

// --- globals -----------------------------------------------------------------

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
  js::Log(line.c_str());
  return JS_UNDEFINED;
}

const JSCFunctionListEntry ConsoleGlobal[] = {
    JS_CFUNC_DEF("log", 1, ConsoleWrite),
    JS_CFUNC_DEF("info", 1, ConsoleWrite),
    JS_CFUNC_DEF("warn", 1, ConsoleWrite),
    JS_CFUNC_DEF("error", 1, ConsoleWrite),
    JS_CFUNC_DEF("debug", 1, ConsoleWrite),
};

// A global `console`, which QuickJS core does not provide (that lives in
// quickjs-libc, which this port does not install). It writes to the game console
// and the debugger, same as an uncaught exception does. Note this is *not* the
// `console` exported by "gk" - that one is the game's console object, and
// importing it shadows this inside that module.
bool InstallGlobals(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);
  if (JS_IsException(console)) {
    JS_FreeValue(ctx, global);
    return false;
  }
  if (JS_SetPropertyFunctionList(ctx, console, ConsoleGlobal,
                                 static_cast<int>(std::size(ConsoleGlobal))) <
      0) {
    JS_FreeValue(ctx, console);
    JS_FreeValue(ctx, global);
    return false;
  }
  bool ok = JS_SetPropertyStr(ctx, global, "console", console) >= 0;
  JS_FreeValue(ctx, global);
  return ok;
}

// --- boot --------------------------------------------------------------------

// Forces both C modules to link, and hands back their namespace objects. A C
// module's exports are only materialised when something imports it, so reaching
// for JS_GetModuleNamespace on an un-imported module would produce a namespace
// full of undefined - this is the cheapest correct way to get one, and it fails
// loudly at boot instead of inside the user's first import.
bool EvalBootstrap(JSContext *ctx) {
  static const char Source[] = "import * as ImGui from \"ImGui\";\n"
                               "import * as gk from \"gk\";\n"
                               "export { ImGui, gk };\n";

  JSValue compiled = JS_Eval(ctx, Source, sizeof(Source) - 1, "<gkplus>",
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    js::ReportException(ctx, "gkplus bootstrap");
    return false;
  }
  auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
  JSValue result = Await(ctx, JS_EvalFunction(ctx, compiled)); // consumes it
  if (JS_IsException(result)) {
    js::ReportException(ctx, "gkplus bootstrap");
    return false;
  }
  JS_FreeValue(ctx, result);

  JSValue ns = JS_GetModuleNamespace(ctx, module);
  if (JS_IsException(ns)) {
    js::ReportException(ctx, "gkplus bootstrap");
    return false;
  }
  ImGuiNamespace = JS_GetPropertyStr(ctx, ns, "ImGui");
  GkNamespace = JS_GetPropertyStr(ctx, ns, "gk");
  JS_FreeValue(ctx, ns);
  if (JS_IsException(ImGuiNamespace) || JS_IsException(GkNamespace)) {
    js::ReportException(ctx, "gkplus bootstrap");
    return false;
  }
  return true;
}

// Loads the entry module and picks up its exports. False means nothing is
// callable afterwards; the reason has already been logged.
bool LoadEntryModule(JSContext *ctx, const std::string &path) {
  std::string source;
  if (!ReadWholeFile(path.c_str(), &source)) {
    js::Log(("no script loaded - " + path + " does not exist").c_str());
    return false;
  }

  JSValue compiled =
      JS_Eval(ctx, source.c_str(), source.size(), path.c_str(),
              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    js::ReportException(ctx, path.c_str());
    return false;
  }
  auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
  JSValue result = Await(ctx, JS_EvalFunction(ctx, compiled)); // consumes it
  if (JS_IsException(result)) {
    js::ReportException(ctx, path.c_str());
    return false;
  }
  JS_FreeValue(ctx, result);

  JSValue ns = JS_GetModuleNamespace(ctx, module);
  if (JS_IsException(ns)) {
    js::ReportException(ctx, path.c_str());
    return false;
  }
  DrawGui = JS_GetPropertyStr(ctx, ns, "draw_gui");
  SetupMenusFn = JS_GetPropertyStr(ctx, ns, "setup_menus");
  JS_FreeValue(ctx, ns);

  // An export of the wrong type is a script bug worth naming, not a silent
  // no-op: getting `export const draw_gui = ...` subtly wrong is easy.
  for (auto *slot : {&DrawGui, &SetupMenusFn}) {
    if (!JS_IsUndefined(*slot) && !JS_IsFunction(ctx, *slot)) {
      js::Log(("ignoring a non-function export in " + path).c_str());
      JS_FreeValue(ctx, *slot);
      *slot = JS_UNDEFINED;
    }
  }
  return true;
}

// Hands the script the `menus` collection - the same object `import { menus }
// from "gk"` yields. Anything it throws is reported and swallowed, because this
// runs inside the game's SetupMenus and an exception must not escape into it.
void CallSetupMenus(JSContext *ctx) {
  if (JS_IsUndefined(SetupMenusFn)) {
    return;
  }
  JSValue menus = JS_GetPropertyStr(ctx, GkNamespace, "menus");
  if (JS_IsException(menus)) {
    js::ReportException(ctx, "setup_menus");
    return;
  }
  JSValueConst args[] = {menus};
  JSValue result = JS_Call(ctx, SetupMenusFn, JS_UNDEFINED, 1, args);
  JS_FreeValue(ctx, menus);
  if (JS_IsException(result)) {
    js::ReportException(ctx, "setup_menus");
  }
  JS_FreeValue(ctx, result);
}

// --- the seams ---------------------------------------------------------------

void OnFrame() { PumpJobs(); }

void OnOverlayDraw() {
  if (!Context || JS_IsUndefined(DrawGui)) {
    return;
  }
  JSValueConst args[] = {ImGuiNamespace};
  JSValue result = JS_Call(Context, DrawGui, JS_UNDEFINED, 1, args);
  if (JS_IsException(result)) {
    js::ReportException(Context, "draw_gui");
    // Once per frame forever is not a diagnostic, it is a flood - and a script
    // throwing mid-frame has usually left ImGui's stack unbalanced anyway.
    JS_FreeValue(Context, DrawGui);
    DrawGui = JS_UNDEFINED;
    js::Log("draw_gui disabled after an uncaught exception");
  }
  JS_FreeValue(Context, result);
}

} // namespace

void BootScriptHost() {
  if (Booted) {
    return;
  }
  Booted = true;

  std::string path = EntryModulePath();
  if (path.empty()) {
    js::Log("could not work out where this DLL lives; no script loaded");
    return;
  }

  Runtime = JS_NewRuntime();
  if (!Runtime) {
    js::Log("could not create the JavaScript runtime");
    return;
  }
  JS_SetModuleLoaderFunc(Runtime, nullptr, ModuleLoader, nullptr);

  Context = JS_NewContext(Runtime);
  if (!Context) {
    js::Log("could not create the JavaScript context");
    JS_FreeRuntime(Runtime);
    Runtime = nullptr;
    return;
  }

  if (!js::RegisterGkModule(Context) || !js_init_module_imgui(Context) ||
      !InstallGlobals(Context) || !EvalBootstrap(Context)) {
    js::Log("could not register the script bindings; no script loaded");
    return;
  }

  // Installed here rather than in the ScriptSystem constructor so that the
  // overlay and the frame loop only ever call into a context that exists.
  SetOverlayDrawCallback(OnOverlayDraw);
  SetFrameCallback(OnFrame);

  if (!LoadEntryModule(Context, path)) {
    return;
  }
  js::Log(("running " + path).c_str());
  CallSetupMenus(Context);
  PumpJobs();
}

namespace {
void __stdcall HookedSetupMenus() {
  SetupMenus();
  BootScriptHost();
}
} // namespace

ScriptSystem::ScriptSystem() {
  GetObjectAtOffset(SetupMenus, 0x004e95e0);
  DetourAttach(&SetupMenus, HookedSetupMenus);
}

ScriptSystem::~ScriptSystem() {
  SetOverlayDrawCallback(nullptr);
  SetFrameCallback(nullptr);

  if (Context) {
    js::ReleaseCallbacks(Context);
    JS_FreeValue(Context, DrawGui);
    JS_FreeValue(Context, SetupMenusFn);
    JS_FreeValue(Context, ImGuiNamespace);
    JS_FreeValue(Context, GkNamespace);
    DrawGui = SetupMenusFn = ImGuiNamespace = GkNamespace = JS_UNDEFINED;
    JS_FreeContext(Context);
    Context = nullptr;
  }
  if (Runtime) {
    JS_FreeRuntime(Runtime);
    Runtime = nullptr;
  }

  DetourDetach(&SetupMenus, HookedSetupMenus);
}
} // namespace gk
