#include "Settings.h"

#include "Core.h"
#include "Json.h"
#include "Profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gk::settings {
namespace {

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
  // Whether anything has been written since the last Save or Reload, and when.
  // These exist because the script-facing tree writes straight through
  // (src/JsSettings.cpp), so there is no longer a save call to mark the end of a
  // change: `dirty` is what keeps SaveIfDirty from rewriting the file on a launch
  // that changed nothing, and the two stamps are what let SaveSettled tell "the
  // script has finished changing things" from "it is still changing them".
  bool dirty = false;
  unsigned long long dirty_since_ms = 0; // the write that made it dirty
  unsigned long long last_write_ms = 0;  // the most recent one
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
  static const std::string path = profile::Resolve("settings.json");
  return path;
}

std::string GetJson(const char *path) { return TheStore().doc.Get(path); }

namespace {
void MarkDirty(Store &store) {
  const unsigned long long now = ::GetTickCount64();
  if (!store.dirty) {
    store.dirty = true;
    store.dirty_since_ms = now;
  }
  store.last_write_ms = now;
}
} // namespace

bool SetJson(const char *path, const char *json) {
  Store &store = TheStore();
  if (!store.doc.Set(path, json)) {
    return false;
  }
  MarkDirty(store);
  return true;
}

bool Remove(const char *path) {
  Store &store = TheStore();
  if (!store.doc.Remove(path)) {
    return false;
  }
  MarkDirty(store);
  return true;
}

json::Kind KindAt(const char *path) { return TheStore().doc.KindAt(path); }

std::vector<std::string> Keys(const char *path) {
  return TheStore().doc.Keys(path);
}

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
  // Cleared before the write rather than after it: a failure is reported and
  // logged, and retrying it at every later save point would turn one diagnostic
  // into one per call for a directory that is not going to become writable.
  TheStore().dirty = false;

  // The profile directory normally exists (the scripts live there) but nothing
  // guarantees it - a GKPLUS_PROFILE naming a fresh directory is exactly how a
  // new profile starts - and a mod storing settings is a reason of its own.
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

bool SaveIfDirty() { return !TheStore().dirty || Save(); }

void SaveSettled() {
  const Store &store = TheStore();
  if (!store.dirty) {
    return;
  }
  const unsigned long long now = ::GetTickCount64();
  const bool settled = now - store.last_write_ms >= 1000;
  const bool overdue = now - store.dirty_since_ms >= 15000;
  if (settled || overdue) {
    Save();
  }
}

bool Reload() {
  Store &store = TheStore();
  store.dirty = false;
  std::string text;
  if (Path().empty() || !ReadWholeFile(Path().c_str(), &text)) {
    // No file is not an error: it is the state before the first Save.
    return store.doc.Parse("{}");
  }
  return store.doc.Parse(text.c_str());
}

} // namespace gk::settings
