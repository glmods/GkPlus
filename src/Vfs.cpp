#include "Vfs.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <physfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace gk::vfs {
namespace {

constexpr const char *kModsSubdir = "gkplus\\mods\\";
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
std::vector<Mod> g_mods;
std::string g_game_dir;
std::string g_mods_dir;
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

// Everything directly inside gkplus\mods, ascending by name, case-insensitively.
// Sorting here rather than trusting FindFirstFile is what makes the documented
// "a later name wins" rule hold: the directory order is whatever NTFS feels
// like, and on a FAT volume it is creation order.
std::vector<Mod> DiscoverMods(const std::string &mods_dir) {
  std::vector<Mod> found;
  WIN32_FIND_DATAA find{};
  HANDLE handle = FindFirstFileA((mods_dir + "*").c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE) {
    return found;
  }
  do {
    if (std::strcmp(find.cFileName, ".") == 0 ||
        std::strcmp(find.cFileName, "..") == 0) {
      continue;
    }
    Mod mod;
    mod.name = find.cFileName;
    mod.path = mods_dir + find.cFileName;
    mod.archive = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    found.push_back(std::move(mod));
  } while (FindNextFileA(handle, &find));
  FindClose(handle);

  std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b) {
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return found;
}

void RebuildIndex();

bool DoInitialize() {
  g_game_dir = ComputeGameDir();
  if (g_game_dir.empty()) {
    DebugWrite("gkplus vfs: cannot locate the game directory; mods disabled\n");
    return false;
  }
  g_mods_dir = g_game_dir + kModsSubdir;

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

  // Mounted last-to-first with appendToPath, so the alphabetically last mod ends
  // up earliest in the search path and therefore wins. g_mods is filled in the
  // same order, which makes index 0 the highest priority.
  //
  // Anything PhysicsFS does not recognize as an archive is reported and skipped
  // rather than filtered by extension, so a format a future PhysicsFS learns
  // works with no change here - and a stray readme is a log line, not an error.
  std::vector<Mod> discovered = DiscoverMods(g_mods_dir);
  for (auto it = discovered.rbegin(); it != discovered.rend(); ++it) {
    if (!PHYSFS_mount(it->path.c_str(), nullptr, 1)) {
      DebugWrite("gkplus vfs: skipping {} ({})\n", it->name, PhysfsError());
      continue;
    }
    DebugWrite("gkplus vfs: mounted {}\n", it->name);
    g_mods.push_back(std::move(*it));
  }

  RebuildIndex();
  if (g_mods.empty()) {
    DebugWrite("gkplus vfs: no mods in {}\n", g_mods_dir);
  } else {
    DebugWrite("gkplus vfs: {} mod(s) mounted, {} has priority, {} file(s)\n",
               g_mods.size(), g_mods.front().name, g_index.size());
  }
  g_has_mods.store(!g_mods.empty(), std::memory_order_release);
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
    g_mods.clear();
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
  g_mods.clear();
  g_materialized.clear();
  g_index.clear();
  g_has_mods.store(false, std::memory_order_release);
  g_ready.store(false, std::memory_order_release);
  // g_tried stays set: a Shutdown mid-process is teardown, not a reset, and
  // re-running DoInitialize from a detour-removal path would be worse than
  // doing nothing.
  g_tried.store(true, std::memory_order_release);
}

const std::vector<Mod> &Mods() {
  Ensure();
  return g_mods;
}

const std::string &GameDir() {
  Ensure();
  return g_game_dir;
}

const std::string &ModsDir() {
  Ensure();
  return g_mods_dir;
}

bool Mount(const char *path, std::string *error) {
  if (!path || !*path) {
    if (error) {
      *error = "no path given";
    }
    return false;
  }
  if (!Ensure()) {
    if (error) {
      *error = "the mod filesystem is not available";
    }
    return false;
  }
  std::lock_guard lock(g_mutex);
  // appendToPath 0: an explicit mount outranks everything auto-mounted, and
  // outranks any earlier explicit one.
  if (!PHYSFS_mount(path, nullptr, 0)) {
    if (error) {
      *error = PhysfsError();
    }
    return false;
  }

  Mod mod;
  mod.path = path;
  const char *slash = std::strrchr(path, '\\');
  const char *fwd = std::strrchr(path, '/');
  if (fwd && (!slash || fwd > slash)) {
    slash = fwd;
  }
  mod.name = slash ? slash + 1 : path;
  DWORD attrs = GetFileAttributesA(path);
  mod.archive =
      attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
  g_mods.insert(g_mods.begin(), std::move(mod));
  g_has_mods.store(true, std::memory_order_release);
  RebuildIndex();
  return true;
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
