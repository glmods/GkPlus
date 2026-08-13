#include "Vfs.h"

#include "FileHooks.h"
#include "JsBindings.h"

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

JSClassID ModsClassId;

// A mod has no identity to re-resolve against - it is a name, a path and a flag -
// so the collection yields a plain object rather than a wrapper, the same call
// `tokens` makes for its 8-byte pairs. Mutating it changes nothing.
JSValue NewModObject(JSContext *ctx, const vfs::Mod &mod, int index) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  if (JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, mod.name.c_str())) < 0 ||
      JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, mod.path.c_str())) < 0 ||
      JS_SetPropertyStr(ctx, obj, "archive", JS_NewBool(ctx, mod.archive)) < 0 ||
      JS_SetPropertyStr(ctx, obj, "priority", JS_NewInt32(ctx, index)) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

// Keyed by search-path position, so mods[0] is the mod that wins a conflict.
// That is also the order Mods() reports and the reverse of the "a later name
// overrides" rule the mount order implements - see Vfs.h.
JSValue LookupModByIndex(JSContext *ctx, int index) {
  const std::vector<vfs::Mod> &mods = vfs::Mods();
  if (index < 0 || static_cast<size_t>(index) >= mods.size()) {
    return JS_UNDEFINED;
  }
  return NewModObject(ctx, mods[index], index);
}

JSValue LookupModByName(JSContext *ctx, const char *name) {
  const std::vector<vfs::Mod> &mods = vfs::Mods();
  for (size_t i = 0; i < mods.size(); ++i) {
    if (_stricmp(mods[i].name.c_str(), name) == 0) {
      return NewModObject(ctx, mods[i], static_cast<int>(i));
    }
  }
  return JS_UNDEFINED;
}

void CollectModKeys(std::vector<std::string> *out) {
  const size_t count = vfs::Mods().size();
  out->reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out->emplace_back(std::to_string(i));
  }
}

unsigned CountMods() { return static_cast<unsigned>(vfs::Mods().size()); }

// --- the namespace extras ------------------------------------------------------

JSValue GetModsDir(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, vfs::ModsDir().c_str());
}

JSValue GetGameDir(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, vfs::GameDir().c_str());
}

// False means PhysicsFS could not start at all, so no mod can ever load. Having
// zero mods mounted is not the same thing and reports true.
JSValue GetAvailable(JSContext *ctx, JSValueConst) {
  vfs::Initialize();
  return JS_NewBool(ctx, vfs::IsInitialized());
}

// The one way to tell "mounted" from "actually being read": a replaced asset
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

JSValue ModsMount(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_EXCEPTION;
  }
  std::string error;
  bool ok = vfs::Mount(path, &error);
  JS_FreeCString(ctx, path);
  if (!ok) {
    return JS_ThrowInternalError(ctx, "cannot mount: %s", error.c_str());
  }
  return JS_UNDEFINED;
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
// paths. This is how a script finds what the mods actually brought - enumerating
// `rif` or `scripts` to build a level list, say.
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
    JS_CGETSET_DEF("dir", GetModsDir, nullptr),
    JS_CGETSET_DEF("game_dir", GetGameDir, nullptr),
    JS_CGETSET_DEF("available", GetAvailable, nullptr),
    JS_CGETSET_DEF("served", GetServed, nullptr),
    JS_CGETSET_DEF("recent", GetRecent, nullptr),
    JS_CFUNC_DEF("read_stats", 0, ModsReadStats),
    JS_CFUNC_DEF("reset_read_stats", 0, ModsResetReadStats),
    JS_CFUNC_DEF("mount", 1, ModsMount),
    JS_CFUNC_DEF("resolve", 1, ModsResolve),
    JS_CFUNC_DEF("exists", 1, ModsExists),
    JS_CFUNC_DEF("read", 1, ModsRead),
    JS_CFUNC_DEF("read_bytes", 1, ModsReadBytes),
    JS_CFUNC_DEF("files", 1, ModsFiles),
};

// No `assign`: a mount is an action with a result to report, so it is mount()
// rather than an assignment through the indexer.
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
  return NewCollection(ctx, &ModsClassId, &ModsOps);
}

} // namespace gk::js
