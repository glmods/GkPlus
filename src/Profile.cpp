#include "Profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gk::profile {
namespace {

// Forward slashes throughout, including the drive-letter form. Win32 takes them
// everywhere, and QuickJS's module normalizer resolves a relative specifier by
// scanning the importing module's name for '/' - with backslashes it finds none
// and `import "./x.mjs"` silently resolves against the process's current
// directory, which the engine moves per asset category.
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
  if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ToForwardSlashes),
                            &self)) {
    return {};
  }
  char path[MAX_PATH]{};
  const DWORD len = ::GetModuleFileNameA(self, path, sizeof(path));
  if (len == 0 || len >= sizeof(path)) {
    return {};
  }
  std::string dir = ToForwardSlashes(path);
  const std::size_t slash = dir.find_last_of('/');
  return slash == std::string::npos ? std::string{} : dir.substr(0, slash);
}

bool IsAbsolute(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  if (path[0] == '/') { // a rooted path, or a UNC share after normalization
    return true;
  }
  return path.size() >= 2 && path[1] == ':';
}

std::string ResolveDir() {
  char override[MAX_PATH]{};
  const DWORD len =
      ::GetEnvironmentVariableA("GKPLUS_PROFILE", override, sizeof(override));
  std::string dir;
  if (len > 0 && len < sizeof(override)) {
    dir = ToForwardSlashes(override);
    // **A relative GKPLUS_PROFILE is relative to the game directory**, not to
    // wherever the launching shell stood. Resolving it against the process's
    // current directory would be the obvious reading and is unusable: this is
    // first read from FileHookSystem's first intercepted open, and the engine
    // reaches that having already chdir'd into a GLDir (the crash stack that
    // found this arrived through the *scripts* loader), so "the current
    // directory" is whichever asset category happened to be loading. The game
    // directory is stable, knowable, and is what the default `gkplus` is
    // relative to anyway.
    if (!IsAbsolute(dir)) {
      const std::string module = ModuleDirectory();
      if (module.empty()) {
        return {};
      }
      dir = module + "/" + dir;
    }
  } else {
    const std::string module = ModuleDirectory();
    if (module.empty()) {
      return {};
    }
    dir = module + "/gkplus";
  }
  while (dir.size() > 1 && dir.back() == '/') {
    dir.pop_back();
  }
  return dir;
}

} // namespace

const std::string &Dir() {
  static const std::string dir = ResolveDir();
  return dir;
}

std::string Resolve(const char *relative) {
  if (!relative || !*relative) {
    return {};
  }
  std::string path = ToForwardSlashes(relative);
  if (IsAbsolute(path)) {
    return path;
  }
  if (Dir().empty()) {
    return {};
  }
  return Dir() + "/" + path;
}

} // namespace gk::profile
