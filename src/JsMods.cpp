#include "Vfs.h"

#include "Core.h"
#include "FileHooks.h"
#include "JsBindings.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

JSClassID ModClassId;
JSClassID ModsClassId;

// --- the Mod wrapper -----------------------------------------------------------
//
// A wrapper points at a vfs::Mod record, and those are interned per canonical
// path and **never freed** (see Vfs.h), so this needs no finalizer and cannot
// dangle - the same arrangement the Level and Menu wrappers have. It also makes
// the wrapper an identity rather than a snapshot, which is what `enable()` reads:
// a mod's `enabled` and `order` change under it and a held wrapper reports the
// new values.

const vfs::Mod *ModOf(JSContext *ctx, JSValueConst self) {
  return static_cast<const vfs::Mod *>(JS_GetOpaque2(ctx, self, ModClassId));
}

JSValue NewModWrapper(JSContext *ctx, const vfs::Mod *mod) {
  if (!mod) {
    return JS_UNDEFINED;
  }
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(ModClassId));
  if (JS_IsException(obj)) {
    return obj;
  }
  // const_cast because JS_SetOpaque takes void *; nothing here writes through it.
  JS_SetOpaque(obj, const_cast<vfs::Mod *>(mod));
  return obj;
}

// Every string getter on the wrapper is this shape.
#define GK_MOD_STRING_GETTER(fn, member)                                       \
  JSValue fn(JSContext *ctx, JSValueConst self) {                               \
    const vfs::Mod *mod = ModOf(ctx, self);                                     \
    return mod ? JS_NewString(ctx, mod->member.c_str()) : JS_EXCEPTION;         \
  }

GK_MOD_STRING_GETTER(GetModName, name)
GK_MOD_STRING_GETTER(GetModEntry, entry)
GK_MOD_STRING_GETTER(GetModPath, path)
GK_MOD_STRING_GETTER(GetModAuthor, info.author)
GK_MOD_STRING_GETTER(GetModWebsite, info.website)
GK_MOD_STRING_GETTER(GetModLicense, info.license)
GK_MOD_STRING_GETTER(GetModVersion, info.version)
GK_MOD_STRING_GETTER(GetModReadme, readme)

#undef GK_MOD_STRING_GETTER

JSValue GetModArchive(JSContext *ctx, JSValueConst self) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? JS_NewBool(ctx, mod->archive) : JS_EXCEPTION;
}

JSValue GetModEnabled(JSContext *ctx, JSValueConst self) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? JS_NewBool(ctx, mod->enabled()) : JS_EXCEPTION;
}

// Position in the load order, where the **highest number wins** a conflict. -1
// for a mod that is loaded but not enabled.
JSValue GetModOrder(JSContext *ctx, JSValueConst self) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? JS_NewInt32(ctx, mod->order) : JS_EXCEPTION;
}

// What is wrong with this mod's metadata. Empty means the contract is met; a
// mod with problems still loads and still enables, so this is what a manager
// shows rather than what stops anything.
JSValue GetModProblems(JSContext *ctx, JSValueConst self) {
  const vfs::Mod *mod = ModOf(ctx, self);
  if (!mod) {
    return JS_EXCEPTION;
  }
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < mod->problems.size(); ++i) {
    if (JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i),
                             JS_NewString(ctx, mod->problems[i].c_str())) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue GetModHasIconSmall(JSContext *ctx, JSValueConst self) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? JS_NewBool(ctx, !mod->icon_small.empty()) : JS_EXCEPTION;
}

JSValue GetModHasIconBig(JSContext *ctx, JSValueConst self) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? JS_NewBool(ctx, !mod->icon_big.empty()) : JS_EXCEPTION;
}

// The icons are **methods rather than accessors** because each returns a copy of
// the file's bytes: a getter reached from a per-frame panel would allocate the
// whole PNG every frame, invisibly. `has_icon_*` is the accessor to test with.
JSValue IconBytes(JSContext *ctx, const std::vector<char> &bytes) {
  if (bytes.empty()) {
    return JS_NULL;
  }
  return JS_NewArrayBufferCopy(
      ctx, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
}

JSValue ModIconSmall(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? IconBytes(ctx, mod->icon_small) : JS_EXCEPTION;
}

JSValue ModIconBig(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  const vfs::Mod *mod = ModOf(ctx, self);
  return mod ? IconBytes(ctx, mod->icon_big) : JS_EXCEPTION;
}

JSValue ModToString(JSContext *ctx, JSValueConst self, int, JSValueConst *) {
  const vfs::Mod *mod = ModOf(ctx, self);
  if (!mod) {
    return JS_EXCEPTION;
  }
  char buf[320];
  if (mod->info.version.empty()) {
    std::snprintf(buf, sizeof(buf), "[Mod '%s'%s]", mod->name.c_str(),
                  mod->enabled() ? "" : ", disabled");
  } else {
    std::snprintf(buf, sizeof(buf), "[Mod '%s' %s%s]", mod->name.c_str(),
                  mod->info.version.c_str(), mod->enabled() ? "" : ", disabled");
  }
  return JS_NewString(ctx, buf);
}

const JSCFunctionListEntry ModProto[] = {
    JS_CGETSET_DEF("name", GetModName, nullptr),
    JS_CGETSET_DEF("entry", GetModEntry, nullptr),
    JS_CGETSET_DEF("path", GetModPath, nullptr),
    JS_CGETSET_DEF("archive", GetModArchive, nullptr),
    JS_CGETSET_DEF("enabled", GetModEnabled, nullptr),
    JS_CGETSET_DEF("order", GetModOrder, nullptr),
    JS_CGETSET_DEF("author", GetModAuthor, nullptr),
    JS_CGETSET_DEF("website", GetModWebsite, nullptr),
    JS_CGETSET_DEF("license", GetModLicense, nullptr),
    JS_CGETSET_DEF("version", GetModVersion, nullptr),
    JS_CGETSET_DEF("readme", GetModReadme, nullptr),
    JS_CGETSET_DEF("problems", GetModProblems, nullptr),
    JS_CGETSET_DEF("has_icon_small", GetModHasIconSmall, nullptr),
    JS_CGETSET_DEF("has_icon_big", GetModHasIconBig, nullptr),
    JS_CFUNC_DEF("icon_small", 0, ModIconSmall),
    JS_CFUNC_DEF("icon_big", 0, ModIconBig),
    JS_CFUNC_DEF("toString", 0, ModToString),
};

const JSClassDef ModClass = {
    "Mod", nullptr, nullptr, nullptr, nullptr,
};

// --- the collection ------------------------------------------------------------
//
// The collection **is the enabled set in load order**, weakest first, so the last
// index wins a file conflict. That is the direction `enable(a, b)` reads in - the
// argument list and the collection are the same sequence, so there is one order to
// remember rather than two. The base install is not in it: it is never a mount,
// and a lookup miss is what makes the engine read the real file.

JSValue LookupModByIndex(JSContext *ctx, int index) {
  const std::vector<const vfs::Mod *> order = vfs::Enabled();
  if (index < 0 || static_cast<size_t>(index) >= order.size()) {
    return JS_UNDEFINED;
  }
  return NewModWrapper(ctx, order[index]);
}

// By display name (what info.json says) or by entry name (what it is called on
// disk), case-insensitively - a script should not have to know which of the two
// it has.
JSValue LookupModByName(JSContext *ctx, const char *name) {
  for (const vfs::Mod *mod : vfs::Enabled()) {
    if (_stricmp(mod->name.c_str(), name) == 0 ||
        _stricmp(mod->entry.c_str(), name) == 0) {
      return NewModWrapper(ctx, mod);
    }
  }
  return JS_UNDEFINED;
}

void CollectModKeys(std::vector<std::string> *out) {
  const size_t count = vfs::Enabled().size();
  out->reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out->emplace_back(std::to_string(i));
  }
}

unsigned CountMods() { return static_cast<unsigned>(vfs::Enabled().size()); }

JSValue NewModArray(JSContext *ctx, const std::vector<const vfs::Mod *> &mods) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < mods.size(); ++i) {
    if (JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i),
                             NewModWrapper(ctx, mods[i])) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// --- the namespace extras ------------------------------------------------------

// Where gl.exe lives. Every VFS path is relative to it - but a relative path
// handed to load() is not: that resolves against the profile.
JSValue GetGameDir(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, vfs::GameDir().c_str());
}

// False means PhysicsFS could not start at all, so no mod can ever load. Having
// nothing enabled is not the same thing and reports true.
JSValue GetAvailable(JSContext *ctx, JSValueConst) {
  vfs::Initialize();
  return JS_NewBool(ctx, vfs::IsInitialized());
}

// Everything `load()` has been given, enabled or not, in load-call order. The
// collection itself is the *enabled* set, so this is the other half - the list a
// manager UI shows with a checkbox against each row.
JSValue GetLoaded(JSContext *ctx, JSValueConst) {
  return NewModArray(ctx, vfs::Loaded());
}

// The one way to tell "enabled" from "actually being read": a replaced asset
// usually looks identical from outside the game, so watch this across a level
// load.
JSValue GetServed(JSContext *ctx, JSValueConst) {
  return JS_NewInt64(ctx, static_cast<int64_t>(VirtualizedOpenCount()));
}

// The VFS paths behind the last few of those, oldest last so the newest reads
// first. Answers "is my file being picked up, and under what name" - which
// nothing else can, because the name is assembled from a GLDir and a string
// inside a .gls.
JSValue GetRecent(JSContext *ctx, JSValueConst) {
  std::vector<std::string> recent = RecentVirtualizedOpens();
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < recent.size(); ++i) {
    const std::string &path = recent[recent.size() - 1 - i];
    if (JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i),
                             JS_NewString(ctx, path.c_str())) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// What the engine's reads look like, behind `GKPLUS_FILE_STATS=1`. A level load
// is bound by the number of reads and not by their size, and this is what says so
// - `sites` names the gl.exe call site by RVA, sorted by call count.
JSValue ModsReadStats(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const ReadStats stats = ReadAccounting();
  JSValue out = JS_NewObject(ctx);
  if (JS_IsException(out)) {
    return out;
  }
  JS_SetPropertyStr(ctx, out, "enabled", JS_NewBool(ctx, stats.enabled));
  JS_SetPropertyStr(ctx, out, "calls",
                    JS_NewInt64(ctx, static_cast<int64_t>(stats.calls)));
  JS_SetPropertyStr(ctx, out, "bytes",
                    JS_NewInt64(ctx, static_cast<int64_t>(stats.bytes)));

  JSValue buckets = JS_NewArray(ctx);
  for (size_t i = 0; i < stats.buckets.size(); ++i) {
    JSValue row = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, row, "at_least",
                      JS_NewInt64(ctx, stats.buckets[i].at_least));
    JS_SetPropertyStr(
        ctx, row, "calls",
        JS_NewInt64(ctx, static_cast<int64_t>(stats.buckets[i].calls)));
    JS_SetPropertyUint32(ctx, buckets, static_cast<uint32_t>(i), row);
  }
  JS_SetPropertyStr(ctx, out, "buckets", buckets);

  JSValue sites = JS_NewArray(ctx);
  for (size_t i = 0; i < stats.sites.size(); ++i) {
    JSValue row = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, row, "rva",
                      JS_NewInt64(ctx, static_cast<int64_t>(stats.sites[i].rva)));
    JS_SetPropertyStr(
        ctx, row, "calls",
        JS_NewInt64(ctx, static_cast<int64_t>(stats.sites[i].calls)));
    JS_SetPropertyStr(
        ctx, row, "bytes",
        JS_NewInt64(ctx, static_cast<int64_t>(stats.sites[i].bytes)));
    JS_SetPropertyUint32(ctx, sites, static_cast<uint32_t>(i), row);
  }
  JS_SetPropertyStr(ctx, out, "sites", sites);
  return out;
}

JSValue ModsResetReadStats(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  ResetReadAccounting();
  return JS_UNDEFINED;
}

// Reads a mod's metadata and interns it. Nothing is put in front of the engine -
// that is enable() - so this is the call a script or a manager makes to find out
// what a candidate *is* before deciding anything.
JSValue ModsLoad(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  std::string error;
  const vfs::Mod *mod = vfs::Load(path, &error);
  if (!mod) {
    JSValue thrown =
        JS_ThrowInternalError(ctx, "cannot load '%s': %s", path, error.c_str());
    JS_FreeCString(ctx, path);
    return thrown;
  }
  JS_FreeCString(ctx, path);
  return NewModWrapper(ctx, mod);
}

// There is no `discover()` and no `dir`: nothing here enumerates anything, and
// this layer does not know where a mod lives. A mod is named - by a script, or by
// something a script read out of config - and a mod nobody names does not load.
// A path may be absolute, or relative to the profile directory - so a config is
// free to keep `mods/hi-res.zip`, which follows GKPLUS_PROFILE, or
// `D:/gunlok-mods/hi-res.zip`, which does not. Neither is more blessed than the
// other. See src/Vfs.h for what all of this rules out.

// One argument of enable(): a Mod, a path string, or an array of either. Depth is
// bounded so a self-referencing array cannot recurse the stack away.
bool CollectEnablePaths(JSContext *ctx, JSValueConst v,
                        std::vector<std::string> *out, int depth) {
  if (depth > 8) {
    JS_ThrowTypeError(ctx, "mods.enable: the argument nests too deeply");
    return false;
  }
  if (const vfs::Mod *mod =
          static_cast<const vfs::Mod *>(JS_GetOpaque(v, ModClassId))) {
    out->push_back(mod->path);
    return true;
  }
  if (JS_IsArray(v)) {
    JSValue length_value = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(length_value)) {
      return false;
    }
    uint32_t length = 0;
    const int failed = JS_ToUint32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    if (failed) {
      return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
      JSValue item = JS_GetPropertyUint32(ctx, v, i);
      if (JS_IsException(item)) {
        return false;
      }
      const bool ok = CollectEnablePaths(ctx, item, out, depth + 1);
      JS_FreeValue(ctx, item);
      if (!ok) {
        return false;
      }
    }
    return true;
  }
  if (JS_IsString(v)) {
    const char *path = JS_ToCString(ctx, v);
    if (!path) {
      return false;
    }
    out->emplace_back(path);
    JS_FreeCString(ctx, path);
    return true;
  }
  JS_ThrowTypeError(ctx, "mods.enable: expected a Mod, a path, or an array of "
                         "either");
  return false;
}

// Declares the enabled set, in load order, so the **last** argument wins a file
// conflict. It *replaces* rather than adds: enabling a shorter list is how a mod
// is switched off and enabling the same list in a different order is how one is
// reordered, so the load order is stated in one place instead of accumulated by a
// sequence of calls whose order is then the only record of it.
//
// A path never loaded is loaded here; the base install among the arguments is
// ignored, since it is always the bottom.
JSValue ModsEnable(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  std::vector<std::string> paths;
  for (int i = 0; i < argc; ++i) {
    if (!CollectEnablePaths(ctx, argv[i], &paths, 0)) {
      return JS_EXCEPTION;
    }
  }
  std::string error;
  const int enabled = vfs::Enable(paths, &error);
  if (enabled < 0) {
    return JS_ThrowInternalError(ctx, "cannot enable mods: %s", error.c_str());
  }
  // A partial failure is not an exception: the set that *did* enable is live, and
  // throwing here would leave a boot script with no way to report the rest.
  if (!error.empty()) {
    DebugWrite("gkplus: mods.enable skipped {}\n", error);
  }
  return JS_NewInt32(ctx, enabled);
}

// What the engine would get if it opened `path` right now, as a VFS path, or
// null when no mod provides it. The argument is resolved exactly the way a game
// open is - against the process's current directory - so from a script, where
// the current directory is whatever the last load left behind, prefer a path
// relative to the game root.
JSValue ModsResolve(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  std::optional<std::string> vpath = vfs::Resolve(path);
  JS_FreeCString(ctx, path);
  return vpath ? JS_NewString(ctx, vpath->c_str()) : JS_NULL;
}

JSValue ModsExists(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *vpath = JS_ToCString(ctx, argv[0]);
  if (!vpath) {
    return JS_EXCEPTION;
  }
  bool exists = vfs::Exists(vpath);
  JS_FreeCString(ctx, vpath);
  return JS_NewBool(ctx, exists);
}

// Decoded as UTF-8. The engine reads a .gls or .gcs as ANSI bytes (Encoding.h is
// that seam), so a mod's text file with non-ASCII in it is worth reading through
// read_bytes instead of guessing here.
JSValue ModsRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *vpath = JS_ToCString(ctx, argv[0]);
  if (!vpath) {
    return JS_EXCEPTION;
  }
  std::vector<char> data;
  bool ok = vfs::Read(vpath, &data);
  JS_FreeCString(ctx, vpath);
  if (!ok) {
    return JS_NULL;
  }
  return JS_NewStringLen(ctx, data.data(), data.size());
}

JSValue ModsReadBytes(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *vpath = JS_ToCString(ctx, argv[0]);
  if (!vpath) {
    return JS_EXCEPTION;
  }
  std::vector<char> data;
  bool ok = vfs::Read(vpath, &data);
  JS_FreeCString(ctx, vpath);
  if (!ok) {
    return JS_NULL;
  }
  return JS_NewArrayBufferCopy(
      ctx, reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

// Every regular file at or below `dir` (the whole VFS with no argument), as VFS
// paths. This is how a script finds what the enabled mods actually brought -
// enumerating `rif` or `scripts` to build a level list, say. The base install is
// not in here: it is never mounted, so nothing enumerates it.
JSValue ModsFiles(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  std::string dir;
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    const char *arg = JS_ToCString(ctx, argv[0]);
    if (!arg) {
      return JS_EXCEPTION;
    }
    dir = arg;
    JS_FreeCString(ctx, arg);
  }

  std::vector<std::string> files = vfs::Files(dir.c_str());
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < files.size(); ++i) {
    if (JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i),
                             JS_NewString(ctx, files[i].c_str())) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

const JSCFunctionListEntry ModsProps[] = {
    JS_CGETSET_DEF("game_dir", GetGameDir, nullptr),
    JS_CGETSET_DEF("available", GetAvailable, nullptr),
    JS_CGETSET_DEF("loaded", GetLoaded, nullptr),
    JS_CGETSET_DEF("served", GetServed, nullptr),
    JS_CGETSET_DEF("recent", GetRecent, nullptr),
    JS_CFUNC_DEF("read_stats", 0, ModsReadStats),
    JS_CFUNC_DEF("reset_read_stats", 0, ModsResetReadStats),
    JS_CFUNC_DEF("load", 1, ModsLoad),
    JS_CFUNC_DEF("enable", 1, ModsEnable),
    JS_CFUNC_DEF("resolve", 1, ModsResolve),
    JS_CFUNC_DEF("exists", 1, ModsExists),
    JS_CFUNC_DEF("read", 1, ModsRead),
    JS_CFUNC_DEF("read_bytes", 1, ModsReadBytes),
    JS_CFUNC_DEF("files", 1, ModsFiles),
};

// No `assign`: enabling is an ordered declaration of the whole set, which an
// assignment through the indexer cannot express. The collection is the **load
// order**, weakest first and the last index winning a conflict, and `loaded` is
// the separate list of what has merely been named, because nothing enables on its
// own and nothing is discovered on its own either (src/Vfs.h).
const CollectionOps ModsOps = {
    .class_name = "Mods",
    .lookup_id = LookupModByIndex,
    .lookup_name = LookupModByName,
    .collect_keys = CollectModKeys,
    .count = CountMods,
    .assign = nullptr,
    .props = ModsProps,
    .props_len = static_cast<int>(std::size(ModsProps)),
};

} // namespace

JSValue NewModsNamespace(JSContext *ctx) {
  if (!EnsureClass(ctx, &ModClassId, &ModClass, ModProto,
                   static_cast<int>(std::size(ModProto)))) {
    return JS_EXCEPTION;
  }
  return NewCollection(ctx, &ModsClassId, &ModsOps);
}

} // namespace gk::js
