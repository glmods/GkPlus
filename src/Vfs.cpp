#include "Vfs.h"

#include "Core.h"
#include "Json.h"
#include "Profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <physfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace gk::vfs {
namespace {

// Materialized files live here; the suffix is our pid so two running copies of
// the game cannot fight over the same tree.
constexpr const char *kTempPrefix = "gkplus-vfs-";
// Directory nesting the index will walk. Mods are shallow; the cap is only so
// that a hostile or malformed archive cannot recurse the stack away.
constexpr int kMaxIndexDepth = 32;

// Recursive because Materialize() calls Read(), and both take it.
std::recursive_mutex g_mutex;

// The three atomics are the fast path. Resolve() runs on every file the engine
// opens, and for the overwhelmingly common case - a player with no mods - it has
// to cost two relaxed loads and nothing else.
std::atomic<bool> g_tried{false};    // Initialize() has run, successfully or not
std::atomic<bool> g_ready{false};    // ... and PhysicsFS is up
std::atomic<bool> g_has_mods{false}; // ... with at least one mod mounted

bool g_initializing = false;

// Every Load()ed mod, in load-call order, and the map that interns them by
// canonical path. **Records are never freed and never move** - the JS layer wraps
// a `Mod *` with no finalizer and treats it as an identity, and Enable() holds
// raw pointers into this - which is why it is a vector of unique_ptr rather than
// a vector of Mod.
std::vector<std::unique_ptr<Mod>> g_loaded;
std::unordered_map<std::string, Mod *> g_by_key; // Lower(path) -> record
// The enabled set in **load order**, so the last entry wins a conflict.
std::vector<Mod *> g_enabled;

std::string g_game_dir;
// Lower(g_game_dir) with no trailing separator: what Load() refuses, because the
// install is what every lookup miss already falls through to.
std::string g_game_dir_key;
// The profile directory, backslashed and with a trailing backslash - what a
// relative path given to Load() is resolved against. Read once here rather than
// per call so AbsolutePath is a string join under the lock.
std::string g_profile_dir;
std::string g_temp_dir;
// Lowercased vpath -> absolute path of the materialized copy.
std::unordered_map<std::string, std::string> g_materialized;

// Lowercased vpath -> the spelling PhysicsFS actually knows the file by.
//
// This exists because **PhysicsFS is case-sensitive inside an archive**: its zip
// archiver builds its directory tree with `__PHYSFS_DirTreeInit(..., case_sensitive
// = 1, ...)`, so PHYSFS_stat("rif/levels/level01.rif") misses an entry stored as
// "RIF/Levels/level01.RIF". A mount of a plain *directory* goes to the Windows
// filesystem and is case-insensitive, so without this the two kinds of mod would
// not even behave the same way.
//
// The casing a mod author would have to match is undiscoverable anyway: the
// directory part comes from `gldirs.gls` (`rif`, lowercase) while the file part
// comes from whatever a .gls or a string literal in the exe says
// (`bitmaps\water.rim`, `User Interface/Main Menu.RIF`). So the index is built
// once per mount and every lookup goes through it.
//
// It also makes the merged view deduplicated, which raw PHYSFS_enumerate is not -
// it reports a name once per search-path element that has it.
std::unordered_map<std::string, std::string> g_index;

const char *PhysfsError() {
  const char *msg = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
  return msg ? msg : "unknown error";
}

// Every path this file hands to Win32 or keeps in a Mod is backslashed; only the
// profile arrives the other way round.
std::string ToBackslashes(std::string s) {
  std::replace(s.begin(), s.end(), '/', '\\');
  return s;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// The directory containing gl.exe, with a trailing backslash. Run through
// GetFullPathNameA so it is normalized the same way Resolve() normalizes what
// the engine hands us - the two are compared as strings, so both have to have
// been through the same normalizer.
std::string ComputeGameDir() {
  char module[MAX_PATH * 2];
  DWORD n = GetModuleFileNameA(nullptr, module, sizeof(module));
  if (n == 0 || n >= sizeof(module)) {
    return {};
  }
  char *slash = std::strrchr(module, '\\');
  if (!slash) {
    return {};
  }
  slash[1] = '\0';

  char full[MAX_PATH * 2];
  DWORD m = GetFullPathNameA(module, sizeof(full), full, nullptr);
  std::string dir = (m == 0 || m >= sizeof(full)) ? std::string(module)
                                                  : std::string(full, m);
  if (dir.empty() || dir.back() != '\\') {
    dir.push_back('\\');
  }
  return dir;
}

std::string TempRoot() {
  char buf[MAX_PATH * 2];
  DWORD n = GetTempPathA(sizeof(buf), buf);
  if (n == 0 || n >= sizeof(buf)) {
    return {};
  }
  std::string dir(buf, n);
  if (dir.empty() || dir.back() != '\\') {
    dir.push_back('\\');
  }
  return dir;
}

// `dir` ends with a backslash.
void RemoveTree(const std::string &dir) {
  WIN32_FIND_DATAA find{};
  HANDLE handle = FindFirstFileA((dir + "*").c_str(), &find);
  if (handle != INVALID_HANDLE_VALUE) {
    do {
      if (std::strcmp(find.cFileName, ".") == 0 ||
          std::strcmp(find.cFileName, "..") == 0) {
        continue;
      }
      std::string child = dir + find.cFileName;
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        RemoveTree(child + "\\");
      } else {
        SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(child.c_str());
      }
    } while (FindNextFileA(handle, &find));
    FindClose(handle);
  }
  RemoveDirectoryA(dir.c_str());
}

bool ProcessIsAlive(DWORD pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) {
    // ERROR_ACCESS_DENIED means it exists but belongs to someone else; anything
    // else (ERROR_INVALID_PARAMETER) means there is no such process.
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return alive;
}

// Removes the materialization directory of any session that is no longer
// running. Shutdown() does its own on the way out, but this is what the design
// actually relies on: DLL_PROCESS_DETACH is not reached after a crash, and even
// on a clean exit it runs a chain of other subsystem destructors first. Keying on
// "is that pid still alive" rather than on the directory's age means a leftover
// goes at the next launch instead of a day later, and it can never take a
// directory belonging to a second copy of the game running right now.
void SweepStaleTempDirs(const std::string &root, DWORD our_pid) {
  WIN32_FIND_DATAA find{};
  HANDLE handle = FindFirstFileA((root + kTempPrefix + "*").c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  const size_t prefix_length = std::strlen(kTempPrefix);
  do {
    if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      continue;
    }
    const char *suffix = find.cFileName + prefix_length;
    if (*suffix == '\0') {
      continue;
    }
    DWORD pid = 0;
    bool numeric = true;
    for (const char *c = suffix; *c; ++c) {
      if (*c < '0' || *c > '9') {
        numeric = false;
        break;
      }
      pid = pid * 10 + static_cast<DWORD>(*c - '0');
    }
    if (!numeric || pid == our_pid || ProcessIsAlive(pid)) {
      continue;
    }
    std::string dir = root + find.cFileName + "\\";
    DebugWrite("gkplus vfs: removing leftover {}\n", dir);
    RemoveTree(dir);
  } while (FindNextFileA(handle, &find));
  FindClose(handle);
}

// `dir` ends with a backslash.
bool CreateDirectoryTree(const std::string &dir) {
  for (size_t i = 0; i < dir.size(); ++i) {
    if (dir[i] != '\\') {
      continue;
    }
    std::string level = dir.substr(0, i);
    if (level.empty() || (level.size() == 2 && level[1] == ':')) {
      continue;
    }
    if (!CreateDirectoryA(level.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
  }
  return true;
}

// **There is deliberately no directory walk here.** A mod is named by a script or
// by something a script read out of config; nothing enumerates anything looking
// for candidates, and there is no mods directory to enumerate, so a mod nobody
// names does not load. See Vfs.h for what that rules out.

void RebuildIndex();

bool DoInitialize() {
  g_game_dir = ComputeGameDir();
  if (g_game_dir.empty()) {
    DebugWrite("gkplus vfs: cannot locate the game directory; mods disabled\n");
    return false;
  }
  // **There is no mods directory.** This layer knows nothing about where a mod
  // lives: a mod is whatever path something names, anywhere on disk. A blessed
  // directory would be back to "it was enabled because of where it sat", one
  // indirection further away.
  //
  // A *relative* path is resolved against the profile, which is what makes a
  // profile portable: the same settings.json listing `mods/hi-res.zip` follows
  // GKPLUS_PROFILE to another directory instead of naming a location that only
  // exists on the machine it was written on. profile::Dir() is itself pinned to
  // the install when GKPLUS_PROFILE is relative, so this is stable however the
  // launch was configured - unlike the process's current directory, which at this
  // point is whichever GLDir the engine chdir'd into to open the file that got us
  // here.
  g_profile_dir = ToBackslashes(profile::Dir());
  if (!g_profile_dir.empty() && g_profile_dir.back() != '\\') {
    g_profile_dir.push_back('\\');
  }

  char module[MAX_PATH * 2];
  if (GetModuleFileNameA(nullptr, module, sizeof(module)) == 0) {
    return false;
  }
  if (!PHYSFS_init(module)) {
    DebugWrite("gkplus vfs: PHYSFS_init failed: {}\n", PhysfsError());
    return false;
  }

  std::string temp_root = TempRoot();
  if (!temp_root.empty()) {
    const DWORD pid = GetCurrentProcessId();
    g_temp_dir = temp_root + kTempPrefix + std::to_string(pid) + "\\";
    SweepStaleTempDirs(temp_root, pid);
  }

  // The bottom of every load order, and the only Mod that is not a mount. See
  // Vfs.h: a lookup miss *is* "read the base install", so describing it as a mod
  // costs nothing and means a manager listing the load order does not have to
  // special-case the bottom of it.
  // Normalized exactly as AbsolutePath returns a path - no trailing separator -
  // so Load() can compare against it directly. Mounting the install as a mod
  // would add an index walk over every shipped asset and change nothing about
  // what the engine reads, so Load refuses it; and it is a real risk rather than
  // a hypothetical, since `mods.game_dir` is right there in the same namespace.
  g_game_dir_key = g_game_dir;
  while (g_game_dir_key.size() > 3 && g_game_dir_key.back() == '\\') {
    g_game_dir_key.pop_back();
  }
  g_game_dir_key = Lower(g_game_dir_key);

  // **Nothing is enabled here.** Which mods a launch gets is the profile's boot
  // script's decision, not this file's - see the header. The search path starts
  // empty and stays that way until something calls Enable(), which for a profile
  // with no boot module means the game runs unmodified.
  RebuildIndex();
  DebugWrite("gkplus vfs: ready, nothing enabled yet\n");
  g_has_mods.store(false, std::memory_order_release);
  return true;
}

// Every entry point funnels through here. Returns false when there is nothing to
// look in, which is the normal state for a player with no mods installed.
bool Ensure() {
  if (g_tried.load(std::memory_order_acquire)) {
    return g_ready.load(std::memory_order_acquire);
  }
  std::lock_guard lock(g_mutex);
  if (g_tried.load(std::memory_order_relaxed)) {
    return g_ready.load(std::memory_order_relaxed);
  }
  // PhysicsFS reaches the filesystem through *this* DLL's imports, never through
  // gl.exe's patched IAT, so it cannot re-enter the hooks that called us. The
  // guard is here because that is a property of the current implementation
  // rather than a guarantee of the API.
  if (g_initializing) {
    return false;
  }
  g_initializing = true;
  bool ok = false;
  try {
    ok = DoInitialize();
  } catch (...) {
    ok = false;
  }
  g_initializing = false;
  if (!ok) {
    PHYSFS_deinit();
    g_enabled.clear();
    // g_by_key first: its values point into g_loaded.
    g_by_key.clear();
    g_loaded.clear();
    g_has_mods.store(false, std::memory_order_release);
  }
  g_ready.store(ok, std::memory_order_release);
  g_tried.store(true, std::memory_order_release);
  return ok;
}

struct IndexWalk {
  std::unordered_set<std::string> seen_dirs;
  int depth = 0;
};

PHYSFS_EnumerateCallbackResult IndexOne(void *data, const char *origdir,
                                        const char *fname);

// Rebuilt from scratch after every mount, because a new mount can introduce a
// spelling of a path that is already indexed under a different one.
void RebuildIndex() {
  g_index.clear();
  IndexWalk walk;
  PHYSFS_enumerate("", IndexOne, &walk);
}

PHYSFS_EnumerateCallbackResult IndexOne(void *data, const char *origdir,
                                        const char *fname) {
  auto *walk = static_cast<IndexWalk *>(data);
  std::string path;
  if (origdir && *origdir && std::strcmp(origdir, "/") != 0) {
    path = origdir;
    if (path.back() != '/') {
      path.push_back('/');
    }
  }
  path += fname;
  std::string key = Lower(path);

  PHYSFS_Stat stat{};
  if (!PHYSFS_stat(path.c_str(), &stat)) {
    return PHYSFS_ENUM_OK;
  }
  if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
    // One walk per directory however many mounts contain it, or the merged view
    // multiplies: two mods with a `scripts` directory would otherwise have every
    // file under it reported twice.
    if (walk->depth < kMaxIndexDepth && walk->seen_dirs.insert(key).second) {
      ++walk->depth;
      PHYSFS_enumerate(path.c_str(), IndexOne, walk);
      --walk->depth;
    }
  } else if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
    // First spelling seen wins. Which mod *serves* it is not decided here - that
    // is the PhysicsFS search path, and it re-resolves at open time.
    g_index.emplace(std::move(key), std::move(path));
  }
  return PHYSFS_ENUM_OK;
}

// The spelling PhysicsFS knows `vpath` by, or null. Caller holds the lock.
const std::string *Canonical(const char *vpath) {
  std::string key = Lower(vpath);
  std::replace(key.begin(), key.end(), '\\', '/');
  auto it = g_index.find(key);
  return it == g_index.end() ? nullptr : &it->second;
}

// --- metadata ------------------------------------------------------------------
//
// `<mod>/metadata/` is where a mod says who it is, and it is the one directory in
// a mod that is *not* game content: the engine has no `metadata` category, so
// nothing an engine open asks for can ever land in it. info.json and README.md
// are expected of every mod and their absence is a Mod::problem; the two icons
// are optional.

constexpr const char *kMetadataDir = "metadata";
constexpr const char *kInfoFile = "info.json";
constexpr const char *kReadmeFile = "README.md";
constexpr const char *kIconSmallFile = "icon_small.png";
constexpr const char *kIconBigFile = "icon_big.png";

// The mount point Load() reads a mod's metadata through, and it has to be a
// mount point rather than the ordinary search path for two reasons. The mod is
// being inspected *before* anything decides to enable it, so it must not be
// visible to Resolve() while that happens - and every mod has a
// metadata/info.json, so reading one through the merged view would find whichever
// mod is on top rather than the one being asked about. Nothing rebuilds g_index
// for this mount, so it is invisible to every other entry point here.
constexpr const char *kInspectMount = "gkplus-inspect";

// A file this big is a mistake or an attack rather than a mod's identity, and
// these bytes are cached in the record for the life of the process.
constexpr PHYSFS_sint64 kMaxMetadataBytes = 4 * 1024 * 1024;

// The eight bytes every PNG starts with. An icon that does not is reported rather
// than handed to whatever ends up decoding it.
constexpr unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

struct ChildSearch {
  const char *want;
  std::string found;
};

PHYSFS_EnumerateCallbackResult MatchChild(void *data, const char *,
                                          const char *fname) {
  auto *search = static_cast<ChildSearch *>(data);
  if (_stricmp(fname, search->want) == 0) {
    search->found = fname;
    return PHYSFS_ENUM_STOP;
  }
  return PHYSFS_ENUM_OK;
}

// The child of `dir` named `want` case-insensitively, or "". Enumerating rather
// than stat'ing the name we want is the same problem g_index solves and has the
// same cause: PhysicsFS is case-sensitive inside an archive, so a mod shipping
// `Metadata/Info.json` would otherwise have no metadata at all. A `metadata`
// directory holds four entries, so the walk is free.
std::string FindChild(const std::string &dir, const char *want) {
  ChildSearch search{want, {}};
  PHYSFS_enumerate(dir.c_str(), MatchChild, &search);
  return search.found;
}

// Reads one metadata file by its exact PhysicsFS path. `why` is only filled for a
// file that is there and unreadable - a missing one is not an error here, because
// three of the four are allowed to be absent.
bool ReadMetadataFile(const std::string &path, std::vector<char> *out,
                      std::string *why) {
  PHYSFS_File *file = PHYSFS_openRead(path.c_str());
  if (!file) {
    *why = PhysfsError();
    return false;
  }
  const PHYSFS_sint64 length = PHYSFS_fileLength(file);
  if (length < 0) {
    *why = "the archive does not report its size";
    PHYSFS_close(file);
    return false;
  }
  if (length > kMaxMetadataBytes) {
    *why = "it is larger than 4 MB";
    PHYSFS_close(file);
    return false;
  }
  out->resize(static_cast<size_t>(length));
  PHYSFS_sint64 got = 0;
  if (length > 0) {
    got = PHYSFS_readBytes(file, out->data(),
                           static_cast<PHYSFS_uint64>(length));
  }
  PHYSFS_close(file);
  if (got != length) {
    *why = PhysfsError();
    out->clear();
    return false;
  }
  return true;
}

// One field of info.json. Every field is a string and every field is optional, so
// an absent one is silent and anything of another type leaves the field empty and
// says so.
//
// **A number is reported rather than tolerated**, which was measured rather than
// chosen: an unquoted `"version": 1.3` read back through Document::Get as
// "1.2999999999999998", because the text is whatever the JSON codec's number
// formatter produces and quickjs-ng 0.15.1's is not shortest-round-trip here
// (V8 prints the same double as "1.3"). Silently reporting a version nobody wrote
// is worse than an empty one beside a problem saying why.
void ReadInfoField(const json::Document &doc, const char *key, std::string *out,
                   std::vector<std::string> *problems) {
  const json::Kind kind = doc.KindAt(key);
  if (kind == json::Kind::Invalid) {
    return;
  }
  if (kind != json::Kind::String) {
    problems->push_back(std::string("metadata/") + kInfoFile + ": `" + key +
                        "` must be a string");
    return;
  }
  json::Classify(doc.Get(key).c_str(), out);
}

void ReadIcon(const std::string &dir, const char *want, std::vector<char> *out,
              std::vector<std::string> *problems) {
  const std::string name = FindChild(dir, want);
  if (name.empty()) {
    return; // both icons are optional
  }
  std::string why;
  if (!ReadMetadataFile(dir + "/" + name, out, &why)) {
    problems->push_back(std::string("metadata/") + want + ": " + why);
    return;
  }
  if (out->size() < sizeof(kPngSignature) ||
      std::memcmp(out->data(), kPngSignature, sizeof(kPngSignature)) != 0) {
    problems->push_back(std::string("metadata/") + want + " is not a PNG");
    out->clear();
  }
}

// Fills `mod`'s info, readme, icons and problems from its `metadata` directory.
//
// False only when PhysicsFS cannot open the mod **at all**, which is what makes a
// stray readme.txt sitting in the mods directory a skipped candidate rather than
// a nameless mod. Metadata that is absent or malformed is a Mod::problem and a
// successful load - see ModInfo in Vfs.h for why that is not strictness misplaced.
bool ReadMetadata(Mod *mod, std::string *error) {
  if (!PHYSFS_mount(mod->path.c_str(), kInspectMount, 0)) {
    if (error) {
      *error = PhysfsError();
    }
    return false;
  }

  const std::string root = kInspectMount;
  const std::string metadata_name = FindChild(root, kMetadataDir);
  std::string dir;
  if (!metadata_name.empty()) {
    dir = root + "/" + metadata_name;
    PHYSFS_Stat stat{};
    if (!PHYSFS_stat(dir.c_str(), &stat) ||
        stat.filetype != PHYSFS_FILETYPE_DIRECTORY) {
      mod->problems.push_back("metadata is not a directory");
      dir.clear();
    }
  }

  if (dir.empty()) {
    mod->problems.push_back(std::string("metadata/") + kInfoFile + " is missing");
    mod->problems.push_back(std::string("metadata/") + kReadmeFile +
                            " is missing");
  } else {
    const std::string info_name = FindChild(dir, kInfoFile);
    if (info_name.empty()) {
      mod->problems.push_back(std::string("metadata/") + kInfoFile +
                              " is missing");
    } else {
      std::vector<char> data;
      std::string why;
      if (!ReadMetadataFile(dir + "/" + info_name, &data, &why)) {
        mod->problems.push_back(std::string("metadata/") + kInfoFile + ": " +
                                why);
      } else {
        // Parse refuses anything that is not one complete JSON *object*, which
        // is exactly the test wanted here: an array or a bare number is a
        // document no field name can address.
        const std::string text(data.begin(), data.end());
        json::Document doc;
        if (!doc.Parse(text.c_str())) {
          mod->problems.push_back(std::string("metadata/") + kInfoFile +
                                  " is not a JSON object");
        } else {
          ReadInfoField(doc, "name", &mod->info.name, &mod->problems);
          ReadInfoField(doc, "author", &mod->info.author, &mod->problems);
          ReadInfoField(doc, "website", &mod->info.website, &mod->problems);
          ReadInfoField(doc, "license", &mod->info.license, &mod->problems);
          ReadInfoField(doc, "version", &mod->info.version, &mod->problems);
        }
      }
    }

    const std::string readme_name = FindChild(dir, kReadmeFile);
    if (readme_name.empty()) {
      mod->problems.push_back(std::string("metadata/") + kReadmeFile +
                              " is missing");
    } else {
      std::vector<char> data;
      std::string why;
      if (!ReadMetadataFile(dir + "/" + readme_name, &data, &why)) {
        mod->problems.push_back(std::string("metadata/") + kReadmeFile + ": " +
                                why);
      } else {
        // CRLF and lone CR both become LF. The field is Markdown *text* whose
        // only consumer is something that displays it, and a mod authored on
        // Windows or zipped from a Windows tree carries CRLF - so normalising
        // once here is the alternative to every consumer doing it, and a stray
        // \r renders as a box in ImGui.
        mod->readme.clear();
        mod->readme.reserve(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
          if (data[i] == '\r') {
            if (i + 1 < data.size() && data[i + 1] == '\n') {
              continue; // the \n right after it is the newline
            }
            mod->readme.push_back('\n');
            continue;
          }
          mod->readme.push_back(data[i]);
        }
      }
    }

    ReadIcon(dir, kIconSmallFile, &mod->icon_small, &mod->problems);
    ReadIcon(dir, kIconBigFile, &mod->icon_big, &mod->problems);
  }

  // Every file above is closed, so this cannot fail for the one reason
  // PHYSFS_unmount ever refuses. Reported rather than ignored because a leaked
  // inspection mount would hold the archive open for the whole session.
  if (!PHYSFS_unmount(mod->path.c_str())) {
    DebugWrite("gkplus vfs: cannot release the inspection mount of {} ({})\n",
               mod->path, PhysfsError());
  }

  mod->name = mod->info.name.empty() ? mod->entry : mod->info.name;
  return true;
}

// `path` made absolute and backslashed, with no trailing separator - the form
// every record keeps and every PHYSFS_mount/PHYSFS_unmount pair is given, since
// PhysicsFS matches a mount by strcmp on that exact string. It is also what makes
// a record an identity: two spellings of one archive intern to one entry, where
// two mounts of differing spellings would have mounted it twice.
//
// A **relative** path is resolved against the **profile** (g_profile_dir), never
// against the process's current directory. Two reasons, and neither is a
// convenience: the current directory when this layer is first reached is
// whichever GLDir the engine chdir'd into to open the file that triggered it, so
// a CWD-relative path would land somewhere the caller could not predict; and the
// profile is what makes a mod list portable, since a settings.json naming
// `mods/hi-res.zip` follows GKPLUS_PROFILE instead of hard-coding a location that
// only exists on the machine it was written on. An absolute path is untouched,
// which is what a mod living anywhere else needs.
//
// With no profile directory to resolve against a relative path is **refused**
// rather than resolved against the CWD - the wrong answer there is a mod loaded
// from an asset directory, which would be baffling.
std::string AbsolutePath(const char *path) {
  std::string given = path;
  const bool relative = !(given.size() >= 2 && given[1] == ':') &&
                        given[0] != '\\' && given[0] != '/';
  if (relative) {
    if (g_profile_dir.empty()) {
      return {};
    }
    given = g_profile_dir + given; // g_profile_dir carries its trailing backslash
  }

  char full[MAX_PATH * 2];
  const DWORD n = GetFullPathNameA(given.c_str(), sizeof(full), full, nullptr);
  if (n == 0 || n >= sizeof(full)) {
    return {};
  }
  std::string out(full, n);
  while (out.size() > 3 && (out.back() == '\\' || out.back() == '/')) {
    out.pop_back();
  }
  return ToBackslashes(std::move(out));
}

std::string LeafName(const std::string &path) {
  const size_t slash = path.find_last_of('\\');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void AppendError(std::string *error, const std::string &line) {
  if (!error) {
    return;
  }
  if (!error->empty()) {
    *error += "; ";
  }
  *error += line;
}

} // namespace

bool Initialize() { return Ensure(); }

bool IsInitialized() { return g_ready.load(std::memory_order_acquire); }

void Shutdown() {
  std::lock_guard lock(g_mutex);
  if (g_ready.load(std::memory_order_relaxed)) {
    PHYSFS_deinit();
  }
  if (!g_temp_dir.empty()) {
    RemoveTree(g_temp_dir);
  }
  // The mounts went with PHYSFS_deinit, so the enabled set is emptied and every
  // record's order reset - but **g_loaded is deliberately left alone**. A Mod *
  // is documented as good for the life of the process and the JS layer wraps one
  // with no finalizer, so freeing the records here would trade a leak that ends
  // with the process for a dangling pointer.
  for (Mod *mod : g_enabled) {
    mod->order = -1;
  }
  g_enabled.clear();
  g_materialized.clear();
  g_index.clear();
  g_has_mods.store(false, std::memory_order_release);
  g_ready.store(false, std::memory_order_release);
  // g_tried stays set: a Shutdown mid-process is teardown, not a reset, and
  // re-running DoInitialize from a detour-removal path would be worse than
  // doing nothing.
  g_tried.store(true, std::memory_order_release);
}

const std::string &GameDir() {
  Ensure();
  return g_game_dir;
}

const Mod *Load(const char *path, std::string *error) {
  if (!path || !*path) {
    if (error) {
      *error = "no path given";
    }
    return nullptr;
  }
  if (!Ensure()) {
    if (error) {
      *error = "the mod filesystem is not available";
    }
    return nullptr;
  }
  std::lock_guard lock(g_mutex);

  const std::string absolute = AbsolutePath(path);
  if (absolute.empty()) {
    if (error) {
      *error = "the path cannot be resolved";
    }
    return nullptr;
  }
  // Interned by canonical path, so loading the same mod twice hands back the
  // same record and re-reads nothing. That is also what keeps the inspection
  // mount from ever colliding with a real one: a path in the search path is by
  // construction already in here, so ReadMetadata below only ever runs for a
  // path PhysicsFS has never been given.
  std::string key = Lower(absolute);
  auto existing = g_by_key.find(key);
  if (existing != g_by_key.end()) {
    return existing->second;
  }
  // The install is not a mod: it is what every lookup miss already falls through
  // to, so mounting it would cost an index walk over every shipped asset and
  // change nothing about what the engine reads.
  if (key == g_game_dir_key) {
    if (error) {
      *error = "that is the game directory, which is not a mod";
    }
    return nullptr;
  }

  const DWORD attributes = GetFileAttributesA(absolute.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (error) {
      *error = "no such file or directory";
    }
    return nullptr;
  }

  auto mod = std::make_unique<Mod>();
  mod->path = absolute;
  mod->entry = LeafName(absolute);
  mod->archive = (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  if (!ReadMetadata(mod.get(), error)) {
    return nullptr;
  }

  Mod *record = mod.get();
  g_loaded.push_back(std::move(mod));
  g_by_key.emplace(std::move(key), record);
  DebugWrite("gkplus vfs: loaded {} ({}{})\n", record->entry, record->name,
             record->problems.empty() ? "" : ", incomplete metadata");
  return record;
}

std::vector<const Mod *> Loaded() {
  Ensure();
  std::lock_guard lock(g_mutex);
  std::vector<const Mod *> out;
  out.reserve(g_loaded.size());
  for (const auto &mod : g_loaded) {
    out.push_back(mod.get());
  }
  return out;
}

int Enable(const std::vector<std::string> &paths, std::string *error) {
  if (!Ensure()) {
    if (error) {
      *error = "the mod filesystem is not available";
    }
    return -1;
  }

  // Everything is loaded *before* anything is unmounted, so a path that turns
  // out not to be a mod cannot leave the previous set torn down. Load takes the
  // same recursive lock this function does.
  std::vector<Mod *> wanted;
  for (const std::string &path : paths) {
    std::string why;
    const Mod *mod = Load(path.c_str(), &why);
    if (!mod) {
      DebugWrite("gkplus vfs: skipping {} ({})\n", path, why);
      AppendError(error, path + ": " + why);
      continue;
    }
    Mod *record = const_cast<Mod *>(mod);
    // A path listed twice keeps its **last** position, because that is the one
    // that decides what wins.
    wanted.erase(std::remove(wanted.begin(), wanted.end(), record),
                 wanted.end());
    wanted.push_back(record);
  }

  std::lock_guard lock(g_mutex);
  for (Mod *mod : g_enabled) {
    if (!PHYSFS_unmount(mod->path.c_str())) {
      DebugWrite("gkplus vfs: cannot unmount {} ({})\n", mod->entry,
                 PhysfsError());
    }
    mod->order = -1;
  }
  g_enabled.clear();

  // appendToPath 0: every mount outranks the one before it, so walking the list
  // forward makes the **last** entry win a conflict - which is the load order
  // Vfs.h documents, and the direction a mod manager reads in.
  for (Mod *mod : wanted) {
    if (!PHYSFS_mount(mod->path.c_str(), nullptr, 0)) {
      const std::string why = PhysfsError();
      DebugWrite("gkplus vfs: cannot mount {} ({})\n", mod->entry, why);
      AppendError(error, mod->entry + ": " + why);
      continue;
    }
    g_enabled.push_back(mod);
  }
  for (size_t i = 0; i < g_enabled.size(); ++i) {
    g_enabled[i]->order = static_cast<int>(i);
  }

  g_has_mods.store(!g_enabled.empty(), std::memory_order_release);
  RebuildIndex();
  DebugWrite("gkplus vfs: {} mod(s) enabled\n", g_enabled.size());
  return static_cast<int>(g_enabled.size());
}

std::vector<const Mod *> Enabled() {
  Ensure();
  std::lock_guard lock(g_mutex);
  std::vector<const Mod *> out;
  out.reserve(g_enabled.size());
  for (Mod *mod : g_enabled) {
    out.push_back(mod);
  }
  return out;
}

std::optional<std::string> Resolve(const char *engine_path) {
  if (!engine_path || !*engine_path) {
    return std::nullopt;
  }
  // Two atomic loads for a player with no mods, and then out.
  if (!Ensure() || !g_has_mods.load(std::memory_order_acquire)) {
    return std::nullopt;
  }

  // GetFullPathNameA does the CWD join and the ./.. collapse in one call, which
  // is exactly the translation needed: the CWD is whichever GLDir the caller
  // just chdir'd to. It is not one of the APIs this DLL patches, so there is no
  // recursion here.
  char full[MAX_PATH * 2];
  DWORD n = GetFullPathNameA(engine_path, sizeof(full), full, nullptr);
  if (n == 0 || n >= sizeof(full)) {
    return std::nullopt;
  }

  std::lock_guard lock(g_mutex);
  if (n <= g_game_dir.size() ||
      _strnicmp(full, g_game_dir.c_str(), static_cast<int>(g_game_dir.size())) !=
          0) {
    // Outside the game tree - a savegame under the user's profile, or a GLDir
    // pointed somewhere absolute. Never virtualized.
    return std::nullopt;
  }

  std::string vpath(full + g_game_dir.size(), n - g_game_dir.size());
  if (vpath.empty()) {
    return std::nullopt;
  }
  // The index resolves the casing; see g_index for why that cannot be left to
  // PhysicsFS.
  const std::string *canonical = Canonical(vpath.c_str());
  if (!canonical) {
    return std::nullopt;
  }
  return *canonical;
}

bool Exists(const char *vpath) {
  if (!vpath || !*vpath || !Ensure()) {
    return false;
  }
  std::lock_guard lock(g_mutex);
  return Canonical(vpath) != nullptr;
}

bool Read(const char *vpath, std::vector<char> *out) {
  if (!vpath || !*vpath || !out || !Ensure()) {
    return false;
  }
  std::lock_guard lock(g_mutex);
  const std::string *canonical = Canonical(vpath);
  if (!canonical) {
    return false;
  }
  PHYSFS_File *file = PHYSFS_openRead(canonical->c_str());
  if (!file) {
    return false;
  }
  PHYSFS_sint64 length = PHYSFS_fileLength(file);
  if (length < 0) {
    PHYSFS_close(file);
    return false;
  }
  out->resize(static_cast<size_t>(length));
  PHYSFS_sint64 got = 0;
  if (length > 0) {
    got = PHYSFS_readBytes(file, out->data(), static_cast<PHYSFS_uint64>(length));
  }
  PHYSFS_close(file);
  if (got != length) {
    out->clear();
    return false;
  }
  return true;
}

bool Stat(const char *vpath, uint64_t *size, int64_t *modtime) {
  if (!vpath || !*vpath || !Ensure()) {
    return false;
  }
  std::lock_guard lock(g_mutex);
  const std::string *canonical = Canonical(vpath);
  if (!canonical) {
    return false;
  }
  PHYSFS_Stat stat{};
  if (!PHYSFS_stat(canonical->c_str(), &stat) ||
      stat.filetype != PHYSFS_FILETYPE_REGULAR) {
    return false;
  }
  if (size) {
    *size = stat.filesize < 0 ? 0 : static_cast<uint64_t>(stat.filesize);
  }
  if (modtime) {
    *modtime = stat.modtime;
  }
  return true;
}

std::vector<std::string> Files(const char *dir) {
  std::vector<std::string> out;
  if (!Ensure()) {
    return out;
  }
  std::lock_guard lock(g_mutex);
  std::string prefix = dir ? Lower(dir) : std::string();
  std::replace(prefix.begin(), prefix.end(), '\\', '/');
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.pop_back();
  }
  if (!prefix.empty()) {
    prefix.push_back('/');
  }
  for (const auto &entry : g_index) {
    if (prefix.empty() || entry.first.compare(0, prefix.size(), prefix) == 0) {
      out.push_back(entry.second);
    }
  }
  // g_index is unordered, and a caller comparing two runs deserves better.
  std::sort(out.begin(), out.end());
  return out;
}

bool Materialize(const char *vpath, std::string *out) {
  if (!vpath || !*vpath || !out || !Ensure()) {
    return false;
  }
  std::lock_guard lock(g_mutex);
  if (g_temp_dir.empty()) {
    return false;
  }

  const std::string *canonical = Canonical(vpath);
  if (!canonical) {
    return false;
  }
  std::string key = Lower(*canonical);
  auto cached = g_materialized.find(key);
  if (cached != g_materialized.end()) {
    if (GetFileAttributesA(cached->second.c_str()) != INVALID_FILE_ATTRIBUTES) {
      *out = cached->second;
      return true;
    }
    g_materialized.erase(cached);
  }

  std::vector<char> data;
  if (!Read(canonical->c_str(), &data)) {
    return false;
  }

  std::string real = g_temp_dir + *canonical;
  std::replace(real.begin(), real.end(), '/', '\\');
  size_t slash = real.find_last_of('\\');
  if (slash == std::string::npos ||
      !CreateDirectoryTree(real.substr(0, slash + 1))) {
    return false;
  }

  // This DLL's own CreateFileA, not the game's - the IAT patch is on gl.exe.
  HANDLE handle = CreateFileA(real.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool ok = true;
  if (!data.empty()) {
    DWORD written = 0;
    ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written,
                   nullptr) != 0 &&
         written == data.size();
  }
  CloseHandle(handle);
  if (!ok) {
    DeleteFileA(real.c_str());
    return false;
  }

  g_materialized.emplace(std::move(key), real);
  *out = std::move(real);
  return true;
}

} // namespace gk::vfs
