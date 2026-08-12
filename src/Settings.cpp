#include "Settings.h"

#include "Core.h"
#include "Json.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gk::settings {
namespace {

std::string ToForwardSlashes(std::string text) {
  for (char &c : text) {
    if (c == '\\') {
      c = '/';
    }
  }
  return text;
}

// Beside main.mjs, and found the same way src/Script.cpp finds that: relative to
// this module rather than to the working directory, which the engine changes per
// asset category (the GLDir scheme in file_io_notes.md) and so cannot be trusted.
std::string ResolvePath() {
  char override[MAX_PATH]{};
  const DWORD len =
      ::GetEnvironmentVariableA("GKPLUS_SETTINGS", override, sizeof(override));
  if (len > 0 && len < sizeof(override)) {
    return ToForwardSlashes(override);
  }

  HMODULE self = nullptr;
  if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ResolvePath), &self)) {
    return {};
  }
  char path[MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameA(self, path, sizeof(path));
  if (n == 0 || n >= sizeof(path)) {
    return {};
  }
  std::string dir = ToForwardSlashes(path);
  const std::size_t slash = dir.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  return dir.substr(0, slash) + "/gkplus/settings.json";
}

bool ReadWholeFile(const char *path, std::string *out) {
  std::FILE *file = std::fopen(path, "rb");
  if (!file) {
    return false;
  }
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, read);
  }
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

// The document, plus the one-shot load. A magic static, so whichever thread
// touches settings first does the read; every operation on the Document itself is
// already serialised by the codec's lock.
struct Store {
  json::Document doc;
  bool loaded = false;
};

Store &TheStore() {
  static Store store;
  if (!store.loaded) {
    store.loaded = true;
    std::string text;
    if (!Path().empty() && ReadWholeFile(Path().c_str(), &text)) {
      // A file that is not a JSON object leaves an empty document rather than
      // being discarded silently at every later read. Saying so once is the
      // difference between "my settings do nothing" and a typo you can find.
      if (!store.doc.Parse(text.c_str())) {
        DebugWrite("gkplus settings: {} is not a JSON object; ignoring it\n",
                   Path());
      }
    }
  }
  return store;
}

// A leaf of the given kind, or nothing. Returns the JSON text so the caller can
// decode it; `kind` mismatches read as absent on purpose (see Settings.h).
bool Leaf(const char *path, json::Kind kind, std::string *json,
          std::string *decoded = nullptr) {
  *json = TheStore().doc.Get(path);
  if (json->empty()) {
    return false;
  }
  return json::Classify(json->c_str(), decoded) == kind;
}

} // namespace

const std::string &Path() {
  static const std::string path = ResolvePath();
  return path;
}

std::string GetJson(const char *path) { return TheStore().doc.Get(path); }

bool SetJson(const char *path, const char *json) {
  return TheStore().doc.Set(path, json);
}

bool Remove(const char *path) { return TheStore().doc.Remove(path); }

bool Has(const char *path) { return !TheStore().doc.Get(path).empty(); }

bool GetBool(const char *path, bool fallback) {
  std::string json;
  if (!Leaf(path, json::Kind::Bool, &json)) {
    return fallback;
  }
  return json == "true";
}

double GetNumber(const char *path, double fallback) {
  std::string json;
  if (!Leaf(path, json::Kind::Number, &json)) {
    return fallback;
  }
  // JSON's number grammar is a subset of strtod's, and Classify has already
  // proved this text is one, so the parse cannot fail or leave a tail.
  return std::strtod(json.c_str(), nullptr);
}

std::string GetString(const char *path, const char *fallback) {
  std::string json;
  std::string decoded;
  if (!Leaf(path, json::Kind::String, &json, &decoded)) {
    return fallback ? fallback : "";
  }
  return decoded;
}

bool SetBool(const char *path, bool value) {
  return SetJson(path, value ? "true" : "false");
}

bool SetNumber(const char *path, double value) {
  char text[32];
  // %.17g round-trips every double exactly, and drops to a short form for the
  // integers this file is mostly made of - 4, not 4.0000000000000000.
  std::snprintf(text, sizeof(text), "%.17g", value);
  return SetJson(path, text);
}

bool SetString(const char *path, const char *value) {
  return SetJson(path, json::Quote(value).c_str());
}

std::string Text() { return TheStore().doc.Stringify(true); }

bool Save() {
  if (Path().empty()) {
    return false;
  }
  const std::string text = TheStore().doc.Stringify(true);

  // `gkplus\` normally exists (main.mjs lives there) but nothing guarantees it,
  // and a mod storing settings is a reason for it to exist on its own.
  const std::size_t slash = Path().find_last_of('/');
  if (slash != std::string::npos) {
    ::CreateDirectoryA(Path().substr(0, slash).c_str(), nullptr);
  }

  const std::string temp = Path() + ".tmp";
  std::FILE *file = std::fopen(temp.c_str(), "wb");
  if (!file) {
    DebugWrite("gkplus settings: cannot write {}\n", temp);
    return false;
  }
  const bool written =
      std::fwrite(text.data(), 1, text.size(), file) == text.size();
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed) {
    ::DeleteFileA(temp.c_str());
    DebugWrite("gkplus settings: failed writing {}\n", temp);
    return false;
  }

  // MOVEFILE_REPLACE_EXISTING is what makes the swap atomic enough: a reader
  // sees the old file or the new one, never a truncated one carrying somebody
  // else's section.
  if (!::MoveFileExA(temp.c_str(), Path().c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileA(temp.c_str());
    DebugWrite("gkplus settings: cannot replace {}\n", Path());
    return false;
  }
  return true;
}

bool Reload() {
  Store &store = TheStore();
  std::string text;
  if (Path().empty() || !ReadWholeFile(Path().c_str(), &text)) {
    // No file is not an error: it is the state before the first Save.
    return store.doc.Parse("{}");
  }
  return store.doc.Parse(text.c_str());
}

} // namespace gk::settings
