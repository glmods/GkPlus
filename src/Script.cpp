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
#include "Profile.h"
#include "Repl.h"
#include "Settings.h"
#include "Vfs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gk {
namespace {

StdCall<> SetupMenus;

JSRuntime *Runtime = nullptr;
JSContext *Context = nullptr;

// The profile's exports, resolved once at boot. Undefined when neither module
// provides them, or - for DrawGui - after it has thrown.
JSValue DrawGui = JS_UNDEFINED;
JSValue SetupMenusFn = JS_UNDEFINED;

// One enabled mod's script and whichever of the two callbacks it exports. A mod
// has its own slots rather than sharing the profile's for the reason Script.h
// gives: there is one profile, so last-loaded-wins is coherent there and is not
// a rule at all across mods.
//
// An entry is appended **before** the module is evaluated, so a script that
// throws is recorded as attempted and not retried - and it is keyed by the mod's
// canonical path, which is its identity for the life of the process.
struct ModHooks {
  std::string path;
  std::string module; // the `mod:` name, which is what a diagnostic should say
  JSValue draw_gui = JS_UNDEFINED;
  JSValue setup_menus = JS_UNDEFINED;
  bool menus_called = false;
};
std::vector<ModHooks> ModScripts;
// RunModScripts is re-entrant through mods.enable: a mod's script may enable
// more mods. The inner call returns at once and the outer loop, which re-reads
// the enabled set every time round, picks up whatever it added.
bool RunningModScripts = false;
// The two objects the script is handed rather than importing. Both are built
// directly, and neither is a "gk" export: an ImGui call is only valid inside the
// overlay's frame, and a menu item may only be added at boot, so each is scoped
// to the callback that runs at the right moment.
JSValue ImGuiNamespace = JS_UNDEFINED;
JSValue MenusNamespace = JS_UNDEFINED;

// The runtime exists (phase one has run). Not the same as Booted, which means
// the entry module has been through.
bool RuntimeUp = false;
bool Booted = false;

// --- paths -------------------------------------------------------------------

// Which file a settings key names, resolved against the profile directory. An
// explicit "" means "no module", which is how a profile that only mounts mods
// turns the entry script off - so the fallback is only used when the key is
// absent altogether, not when it is empty.
std::string ModulePathFromSettings(const char *key, const char *fallback) {
  const std::string configured = settings::GetString(key, fallback);
  return profile::Resolve(configured.c_str());
}

std::string BootModulePath() {
  return ModulePathFromSettings("core.boot", "boot.mjs");
}

std::string EntryModulePath() {
  return ModulePathFromSettings("core.script", "main.mjs");
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

// A module out of a mod's `metadata` directory rather than off disk, because for
// an archive mod there is no path to read: `mod:<mod path>/<path in metadata>`.
//
// The scheme is what makes a **relative import inside a mod work the same in a
// zip and in a directory**. QuickJS's default normalizer only ever splits on '/',
// so `./lib/util.mjs` imported from `mod:D:\mods\x.zip/mod.mjs` normalizes to
// `mod:D:\mods\x.zip/lib/util.mjs` - the mod's own path is one opaque segment
// (it is backslashed) and cannot be climbed out of: a `..` too many collapses
// the whole prefix away and leaves a name no mod claims.
constexpr const char *kModScheme = "mod:";

std::string ModModuleName(const vfs::Mod *mod, const std::string &inner) {
  return std::string(kModScheme) + mod->path + "/" + inner;
}

// Splits one of those back into the mod that owns it and the path inside its
// metadata directory. Null when `name` is not a `mod:` name at all, or when no
// loaded mod claims it. Longest prefix wins, so a mod living inside another
// mod's directory still resolves to itself.
const vfs::Mod *ModModuleOwner(const char *name, std::string *inner) {
  const std::string full = name;
  const size_t scheme = std::strlen(kModScheme);
  if (full.compare(0, scheme, kModScheme) != 0) {
    return nullptr;
  }
  const std::string rest = full.substr(scheme);
  const vfs::Mod *best = nullptr;
  size_t best_length = 0;
  for (const vfs::Mod *mod : vfs::Loaded()) {
    const size_t length = mod->path.size();
    if (rest.size() <= length + 1 || rest[length] != '/') {
      continue;
    }
    if (_strnicmp(rest.c_str(), mod->path.c_str(), length) != 0) {
      continue;
    }
    if (length > best_length) {
      best = mod;
      best_length = length;
    }
  }
  if (!best) {
    return nullptr;
  }
  *inner = rest.substr(best_length + 1);
  return best;
}

// `name` arrives already normalized (the default normalizer resolved it against
// the importing module), and bare specifiers never get here: module resolution
// checks the already-loaded modules - where JS_NewCModule registered "gk" and
// "ImGui" - before consulting a loader.
JSModuleDef *ModuleLoader(JSContext *ctx, const char *name, void *) {
  std::string source;
  std::string inner;
  const vfs::Mod *owner = ModModuleOwner(name, &inner);
  if (owner) {
    const std::string *cached = vfs::ModScript(owner, inner.c_str());
    if (!cached) {
      // Only a mod's *own* metadata scripts are readable this way, and they were
      // all read when the mod loaded - so this is a mod importing something it
      // does not ship, or something outside metadata/.
      JS_ThrowReferenceError(ctx, "'%s' does not ship metadata/%s",
                             owner->entry.c_str(), inner.c_str());
      return nullptr;
    }
    source = *cached;
  } else if (!ReadWholeFile(name, &source)) {
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
  if (owner) {
    // `import.meta.mod` is how a mod's script knows **which mod it is**, and it
    // is the only way: the module has no path to read its name out of, and a
    // callback argument would reach the entry module only, not the helpers it
    // imports. Every module of the mod gets the same object.
    JSValue meta = JS_GetImportMeta(ctx, module);
    if (!JS_IsException(meta)) {
      // Both values are checked because JS_EXCEPTION is not a value a property
      // may hold: defining it would store the exception tag itself, and the
      // pending exception would then surface somewhere unrelated.
      JSValue self = js::NewModValue(ctx, owner);
      if (JS_IsException(self)) {
        js::ReportException(ctx, name);
      } else {
        JS_DefinePropertyValueStr(ctx, meta, "mod", self, JS_PROP_C_W_E);
      }
      JS_FreeValue(ctx, meta);
    }
  }
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

// --- boot --------------------------------------------------------------------
//
// There are no host globals. QuickJS core provides no `console` (that lives in
// quickjs-libc, which this port does not install) and GkPlus does not add one:
// `log`/`info`/`warn`/`error`/`debug` are on the "gk" module's `console`
// alongside the game's own print() and colours, so a script has exactly one
// console object and reaches it the same way it reaches everything else.

// Links and evaluates the "gk" module at boot, purely as a check. Nothing here
// keeps its namespace - the host hands the script no "gk" object at all, and a
// script that wants one imports it.
//
// A C module is not linked until something imports it, and its export list is
// only validated at that point: a duplicated name makes the module fail to link
// with a SyntaxError. Doing the first import here reports that as `gkplus
// bootstrap` instead of against whichever script happened to write
// `import ... from "gk"` first.
bool LinkGkModule(JSContext *ctx) {
  static const char Source[] = "import \"gk\";\n";

  JSValue compiled = JS_Eval(ctx, Source, sizeof(Source) - 1, "<gkplus>",
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    js::ReportException(ctx, "gkplus bootstrap");
    return false;
  }
  JSValue result = Await(ctx, JS_EvalFunction(ctx, compiled)); // consumes it
  if (JS_IsException(result)) {
    js::ReportException(ctx, "gkplus bootstrap");
    return false;
  }
  JS_FreeValue(ctx, result);
  return true;
}

// The entry module's own path, kept so a relative specifier handed to
// LoadScriptModule resolves next to it rather than against the process's cwd -
// which the game moves around constantly while loading a level.
std::string EntryPath;

// Every path LoadModule has already evaluated. A profile is free to point
// `core.boot` and `core.script` at one file - the sensible shape for a small
// one - and evaluating it twice would run its top level twice: JS_Eval compiles
// a fresh module for the name each time rather than consulting the loader's
// cache, so nothing else would stop it.
std::vector<std::string> LoadedModules;

// Takes whichever of the two callbacks a module namespace exports into the given
// slots. **Only an export that is actually there replaces what is in a slot**:
// with the profile's two modules sharing one pair, taking the absent one would
// let `core.script` silently erase a `draw_gui` the boot module had provided.
// A mod's slots start empty, so for those it is only the type check that matters.
void CollectHooks(JSContext *ctx, JSValueConst ns, const char *what,
                  JSValue *draw_gui, JSValue *setup_menus) {
  const struct {
    const char *name;
    JSValue *slot;
  } Exports[] = {{"draw_gui", draw_gui}, {"setup_menus", setup_menus}};
  for (const auto &e : Exports) {
    JSValue value = JS_GetPropertyStr(ctx, ns, e.name);
    if (JS_IsUndefined(value)) {
      continue;
    }
    // An export of the wrong type is a script bug worth naming, not a silent
    // no-op: getting `export const draw_gui = ...` subtly wrong is easy.
    if (!JS_IsFunction(ctx, value)) {
      js::Log(
          (std::string("ignoring a non-function export in ") + what).c_str());
      JS_FreeValue(ctx, value);
      continue;
    }
    JS_FreeValue(ctx, *e.slot);
    *e.slot = value;
  }
}

// Loads a module and picks up whichever of the two callbacks it exports. False
// means the file could not be evaluated; the reason has already been logged.
// `what` names the settings key for the log line.
bool LoadModule(JSContext *ctx, const std::string &path, const char *what,
                bool quiet_if_missing) {
  for (const std::string &loaded : LoadedModules) {
    if (loaded == path) {
      return true;
    }
  }

  std::string source;
  if (!ReadWholeFile(path.c_str(), &source)) {
    if (!quiet_if_missing) {
      js::Log((std::string("no script loaded - ") + what + " names " + path +
               ", which does not exist")
                  .c_str());
    }
    return false;
  }
  LoadedModules.push_back(path);

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
  CollectHooks(ctx, ns, path.c_str(), &DrawGui, &SetupMenusFn);
  JS_FreeValue(ctx, ns);
  return true;
}

// Hands one script the `menus` collection, which it can reach no other way.
// Anything it throws is reported and swallowed, because this runs inside the
// game's SetupMenus and an exception must not escape into it.
void CallOneSetupMenus(JSContext *ctx, JSValueConst fn, const char *what) {
  if (JS_IsUndefined(fn) || JS_IsUndefined(MenusNamespace)) {
    return;
  }
  JSValueConst args[] = {MenusNamespace};
  JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, args);
  if (JS_IsException(result)) {
    js::ReportException(ctx, what);
  }
  JS_FreeValue(ctx, result);
}

// The profile's, then every mod script that has not had its turn. Indexed rather
// than iterated because a callback may enable another mod and append to the list
// while this is walking it; an append never moves the entries before it.
void CallSetupMenus(JSContext *ctx) {
  CallOneSetupMenus(ctx, SetupMenusFn, "setup_menus");
  for (size_t i = 0; i < ModScripts.size(); ++i) {
    if (ModScripts[i].menus_called) {
      continue;
    }
    ModScripts[i].menus_called = true;
    const JSValue fn = ModScripts[i].setup_menus;
    const std::string what = ModScripts[i].module;
    CallOneSetupMenus(ctx, fn, what.c_str());
  }
}

// --- a mod's own script ------------------------------------------------------

bool ModScriptRan(const std::string &path) {
  for (const ModHooks &hooks : ModScripts) {
    if (_stricmp(hooks.path.c_str(), path.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

// Evaluates one mod's entry module and records whatever it exports. Reports and
// returns on any failure - the mod stays enabled either way.
void RunOneModScript(JSContext *ctx, const vfs::Mod *mod) {
  const std::string name = ModModuleName(mod, mod->info.script);
  // Recorded before it runs, so a script that throws is not retried on the next
  // enable, and so a nested mods.enable cannot see it as still pending.
  ModScripts.push_back(ModHooks{mod->path, name});
  const size_t slot = ModScripts.size() - 1;

  // JS_LoadModule is QuickJS's own C-side `import()`: it normalizes, runs the
  // loader, links, evaluates and resolves with the namespace. The base name is
  // unused here - a `mod:` specifier is not relative, so the normalizer passes it
  // through - but a module already loaded returns its existing promise, which is
  // what makes this idempotent without a list of what has been evaluated.
  JSValue ns = Await(ctx, JS_LoadModule(ctx, mod->path.c_str(), name.c_str()));
  if (JS_IsException(ns)) {
    js::ReportException(ctx, name.c_str());
    return;
  }
  CollectHooks(ctx, ns, name.c_str(), &ModScripts[slot].draw_gui,
               &ModScripts[slot].setup_menus);
  JS_FreeValue(ctx, ns);
  js::Log(("ran " + name).c_str());

  // A mod enabled after the front end was built has already missed the
  // SetupMenus point, so its turn is now; one enabled before it waits for
  // CallSetupMenus. Custom items are applied lazily at draw time
  // (src/CustomMenu.h), so arriving late costs nothing.
  if (Booted && !ModScripts[slot].menus_called) {
    ModScripts[slot].menus_called = true;
    const JSValue fn = ModScripts[slot].setup_menus;
    CallOneSetupMenus(ctx, fn, name.c_str());
  }
}

// --- the seams ---------------------------------------------------------------

// The job queue first: the REPL settles promises off the same queue, so it should
// start from a drained one rather than pick up whatever the last frame left.
void OnFrame() {
  PumpJobs();
  PumpRepl();
  // After both, so a write either of them just made starts its settling delay
  // from this frame. This is where a change to `settings` reaches the disk
  // without a script asking - see src/Settings.h for why it is here rather than
  // only at DLL detach.
  settings::SaveSettled();
}

// One draw_gui. True when it survived; a slot that threw is cleared by the
// caller, because once per frame forever is not a diagnostic, it is a flood - and
// a script throwing mid-frame has usually left ImGui's stack unbalanced anyway.
bool CallOneDrawGui(JSValueConst fn, const char *what) {
  if (JS_IsUndefined(fn)) {
    return true;
  }
  JSValueConst args[] = {ImGuiNamespace};
  JSValue result = JS_Call(Context, fn, JS_UNDEFINED, 1, args);
  const bool ok = !JS_IsException(result);
  if (!ok) {
    js::ReportException(Context, what);
    js::Log((std::string("draw_gui disabled after an uncaught exception in ") +
             what)
                .c_str());
  }
  JS_FreeValue(Context, result);
  return ok;
}

void OnOverlayDraw() {
  if (!Context) {
    return;
  }
  if (!CallOneDrawGui(DrawGui, "draw_gui")) {
    JS_FreeValue(Context, DrawGui);
    DrawGui = JS_UNDEFINED;
  }
  // Each mod's panel is disabled on its own: one mod throwing is not a reason to
  // take the others' overlays down with it. Indexed for the same reason
  // CallSetupMenus is - a panel may enable a mod and append to this list.
  for (size_t i = 0; i < ModScripts.size(); ++i) {
    const JSValue fn = ModScripts[i].draw_gui;
    if (JS_IsUndefined(fn)) {
      continue;
    }
    const std::string what = ModScripts[i].module;
    if (!CallOneDrawGui(fn, what.c_str())) {
      JS_FreeValue(Context, ModScripts[i].draw_gui);
      ModScripts[i].draw_gui = JS_UNDEFINED;
    }
  }
}

// The runtime, the bindings and the two handed-over objects. Idempotent; false
// means the host is unusable and the reason has been logged.
bool StartRuntime() {
  if (RuntimeUp) {
    return Context != nullptr;
  }
  RuntimeUp = true;

  Runtime = JS_NewRuntime();
  if (!Runtime) {
    js::Log("could not create the JavaScript runtime");
    return false;
  }
  JS_SetModuleLoaderFunc(Runtime, nullptr, ModuleLoader, nullptr);

  Context = JS_NewContext(Runtime);
  if (!Context) {
    js::Log("could not create the JavaScript context");
    JS_FreeRuntime(Runtime);
    Runtime = nullptr;
    return false;
  }

  if (!js::RegisterGkModule(Context) || !LinkGkModule(Context)) {
    js::Log("could not register the script bindings; no script loaded");
    return false;
  }

  // The two handed-over objects. JS_EXCEPTION is not a value the destructor
  // could free, so neither slot keeps one.
  ImGuiNamespace = js_imgui_new_namespace(Context);
  MenusNamespace = js::NewMenusNamespace(Context);
  for (auto *slot : {&ImGuiNamespace, &MenusNamespace}) {
    if (JS_IsException(*slot)) {
      *slot = JS_UNDEFINED;
      js::ReportException(Context, "gkplus bootstrap");
    }
  }

  // Installed here rather than in the ScriptSystem constructor so that the
  // overlay and the frame loop only ever call into a context that exists.
  SetOverlayDrawCallback(OnOverlayDraw);
  SetFrameCallback(OnFrame);

  // Before any module on purpose: a REPL is most useful precisely when a script
  // is missing or throws, and both of the loads below return early on that.
  StartRepl(Runtime);
  return true;
}

} // namespace

void BootScriptProfile() {
  if (RuntimeUp) {
    return;
  }
  const std::string path = BootModulePath();
  if (path.empty()) {
    // `core.boot` set to "", or no profile directory at all. Either way there is
    // nothing to run this early, and the runtime is left for BootScriptHost.
    return;
  }
  // Existence is checked before the runtime is created rather than after, so a
  // profile with no boot module costs nothing and - more to the point - leaves
  // the host booting exactly where it always did. Creating a runtime inside the
  // engine's first file open is worth doing only for a profile that asked for
  // it. This is *our* GetFileAttributesA, not the slot patched into gl.exe's
  // import table, so it cannot re-enter the hook that called us.
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  if (!StartRuntime()) {
    return;
  }
  if (LoadModule(Context, path, "core.boot", /*quiet_if_missing=*/true)) {
    js::Log(("ran " + path).c_str());
  }
  PumpJobs();
}

void RunModScripts() {
  // No context means no host at all. Nothing can reach this in that state today
  // (enabling a mod is only possible from a script), so this is a guard rather
  // than a case.
  if (!Context || RunningModScripts) {
    return;
  }
  RunningModScripts = true;
  // The enabled set is re-read every time round, because a script may enable
  // more mods - including ahead of itself in the load order, which is why this
  // does not walk a snapshot.
  for (;;) {
    const vfs::Mod *next = nullptr;
    for (const vfs::Mod *mod : vfs::Enabled()) {
      if (mod->info.script.empty() || ModScriptRan(mod->path)) {
        continue;
      }
      next = mod;
      break;
    }
    if (!next) {
      break;
    }
    RunOneModScript(Context, next);
  }
  RunningModScripts = false;
  PumpJobs();
}

void BootScriptHost() {
  if (Booted) {
    return;
  }
  Booted = true;

  // Not conditional on the boot module having run: a profile may have neither, or
  // only one of the two.
  if (!StartRuntime()) {
    return;
  }

  std::string path = EntryModulePath();
  if (path.empty()) {
    // `core.script` set to "" - a profile that only mounts mods - or no profile
    // directory. Nothing more to load, but the menus callback may still be
    // waiting from a boot module that exported one.
    CallSetupMenus(Context);
    PumpJobs();
    return;
  }
  EntryPath = path;

  if (LoadModule(Context, path, "core.script", /*quiet_if_missing=*/false)) {
    js::Log(("running " + path).c_str());
  }
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

  // Before the context and the runtime below: the REPL's own context lives on
  // that runtime, and its teardown clears the shared menu/level registrations
  // that the host context's ReleaseCallbacks clears again a moment later.
  StopRepl();

  if (Context) {
    js::ReleaseCallbacks(Context);
    for (ModHooks &hooks : ModScripts) {
      JS_FreeValue(Context, hooks.draw_gui);
      JS_FreeValue(Context, hooks.setup_menus);
    }
    JS_FreeValue(Context, DrawGui);
    JS_FreeValue(Context, SetupMenusFn);
    JS_FreeValue(Context, ImGuiNamespace);
    JS_FreeValue(Context, MenusNamespace);
    DrawGui = SetupMenusFn = ImGuiNamespace = MenusNamespace = JS_UNDEFINED;
    JS_FreeContext(Context);
    Context = nullptr;
  }
  if (Runtime) {
    JS_FreeRuntime(Runtime);
    Runtime = nullptr;
  }
  // So a harness that constructs a second ScriptSystem gets a fresh host rather
  // than one that believes it has already evaluated every module it was given.
  LoadedModules.clear();
  ModScripts.clear();
  EntryPath.clear();
  RuntimeUp = Booted = RunningModScripts = false;

  DetourDetach(&SetupMenus, HookedSetupMenus);
}
} // namespace gk
