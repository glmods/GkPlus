#include "FileHooks.h"

#include "Core.h"
#include "ImageCodec.h"
#include "RenderMenu.h"
#include "Script.h"
#include "Vfs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gk {
namespace {

// --- the intercepted APIs ------------------------------------------------------

using CreateFileAFn = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI *)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using WriteFileFn = BOOL(WINAPI *)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CloseHandleFn = BOOL(WINAPI *)(HANDLE);
using SetFilePointerFn = DWORD(WINAPI *)(HANDLE, LONG, PLONG, DWORD);
using GetFileSizeFn = DWORD(WINAPI *)(HANDLE, LPDWORD);
using GetFileTimeFn = BOOL(WINAPI *)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
using GetFileAttributesAFn = DWORD(WINAPI *)(LPCSTR);
using SetEndOfFileFn = BOOL(WINAPI *)(HANDLE);

// The FILE * is never dereferenced - it belongs to gl.exe's own statically linked
// UCRT, which is a different runtime from this DLL's - so it is modelled as an
// opaque pointer rather than as a FILE that would tempt someone to read it.
using FopenFn = void *(__cdecl *)(const char *, const char *);
using FreopenFn = void *(__cdecl *)(const char *, const char *, void *);

CreateFileAFn OriginalCreateFileA = nullptr;
ReadFileFn OriginalReadFile = nullptr;
WriteFileFn OriginalWriteFile = nullptr;
CloseHandleFn OriginalCloseHandle = nullptr;
SetFilePointerFn OriginalSetFilePointer = nullptr;
GetFileSizeFn OriginalGetFileSize = nullptr;
GetFileTimeFn OriginalGetFileTime = nullptr;
GetFileAttributesAFn OriginalGetFileAttributesA = nullptr;
SetEndOfFileFn OriginalSetEndOfFile = nullptr;
FopenFn OriginalFopen = nullptr;
FreopenFn OriginalFreopen = nullptr;

// --- the virtual handle table --------------------------------------------------

struct VirtualFile {
  std::vector<char> data;
  uint64_t position = 0;
  FILETIME modified{};
};

std::mutex g_files_mutex;
std::unordered_map<HANDLE, VirtualFile> g_files;
// The fast path for every hook that takes a handle. A player with no mods, and a
// modded game between loads, both leave this at zero, and then ReadFile and
// CloseHandle cost one relaxed load on top of the real call. It is incremented
// only after the handle is in the map and before it is handed out, so a nonzero
// reading is the only state in which a lookup can succeed.
std::atomic<int> g_open_files{0};

// Diagnostics only, and the reason they exist is that a working mod is invisible:
// the replaced asset loads and the game looks exactly the same. See
// VirtualizedOpenCount / RecentVirtualizedOpens.
constexpr size_t kRecentMax = 64;
std::atomic<uint64_t> g_served{0};
std::mutex g_recent_mutex;
std::vector<std::string> g_recent; // oldest first

void NoteServed(const std::string &vpath) {
  g_served.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(g_recent_mutex);
  if (g_recent.size() >= kRecentMax) {
    g_recent.erase(g_recent.begin());
  }
  g_recent.push_back(vpath);
}

// --- read accounting -----------------------------------------------------------
//
// `GKPLUS_FILE_STATS=1`, off otherwise, because it takes a lock on a path that a
// level load runs 150,000 times. It exists because the shape of this layer's cost
// is not what `file_io_notes.md` §1 says: ".rif and sound are whole-file reads" is
// true and irrelevant, since something else issues six-figure counts of 64-byte
// ones and that is where a load's wall clock goes.
//
// The caller is `_ReturnAddress()`, which is the gl.exe call site itself - the IAT
// thunk is a `jmp`, so it adds no frame. Recorded as an RVA so it can be looked up
// in the Ghidra database or the profiler's symbol map without caring about ASLR.
constexpr size_t kReadSiteMax = 64;

struct ReadSite {
  uintptr_t rva = 0;
  uint64_t calls = 0;
  uint64_t bytes = 0;
};

bool g_read_stats = false;
std::mutex g_read_mutex;
ReadSite g_read_sites[kReadSiteMax];
uint64_t g_read_buckets[24]; // by size, bucket n is [2^(n-1), 2^n)
uint64_t g_read_calls = 0;
uint64_t g_read_bytes = 0;

void NoteRead(void *caller, DWORD count) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const uintptr_t rva = reinterpret_cast<uintptr_t>(caller) - base;
  size_t bucket = 0;
  while (bucket + 1 < std::size(g_read_buckets) &&
         (1ull << bucket) <= static_cast<uint64_t>(count)) {
    ++bucket;
  }
  std::lock_guard lock(g_read_mutex);
  ++g_read_calls;
  g_read_bytes += count;
  ++g_read_buckets[bucket];
  for (ReadSite &site : g_read_sites) {
    if (site.rva == rva) {
      ++site.calls;
      site.bytes += count;
      return;
    }
    if (site.calls == 0) {
      site.rva = rva;
      site.calls = 1;
      site.bytes = count;
      return;
    }
  }
  // Past 64 distinct sites the table stops learning rather than evicting: the
  // question it answers is "which handful of call sites dominates", and an
  // eviction policy would make the counts of the ones that matter unstable.
}

// --- read-ahead ----------------------------------------------------------------
//
// The reason a warm level load takes a second. `LoadOrBuildSectionAdjacency`
// @ 0x0044fef0 reads the whole `<level>.map` adjacency cache **one 32-bit integer
// per ReadFile call** - measured at 39,364 four-byte reads for level02's 157,456
// byte cache, exactly its size, and ~150,000 for level10's 598 KB. At the ~4 us a
// read syscall costs from the OS cache that is 600 ms of a 1.1 s load, and it is
// syscall count rather than bytes: the same load moves only 9-23 MB.
//
// So this layer reads ahead. A handle it opened for reading gets a 64 KB buffer,
// and the position becomes **ours** rather than the OS handle's: the real handle
// is seeked only when the buffer misses. That is what makes it a win - keeping the
// OS position in step would cost a SetFilePointer per read and buy nothing.
//
// Owning the position is safe here only because this layer owns every API that
// could move it. gl.exe imports no `SetFilePointerEx`, no `ReadFileEx` and no
// overlapped I/O at any of its 31 `CreateFileA` sites (`file_io_notes.md` §1, §5),
// and a handle this layer did not open is never buffered - so there is no path by
// which the OS position can change behind us.
//
// `GKPLUS_FILE_BUFFER=raw` (or `0`) turns it off, which is the A/B.
constexpr size_t kReadAheadBytes = 64 * 1024;
// One buffer per concurrently open read handle, capped so a pathological open loop
// cannot grow this without bound. The engine opens files one or two at a time.
constexpr size_t kReadAheadHandles = 16;

struct ReadAhead {
  uint64_t position = 0; // the logical file position this layer owns
  uint64_t base = 0;     // file offset the buffer starts at
  uint32_t valid = 0;    // bytes of `data` that hold file content
  std::vector<uint8_t> data;
};

bool g_read_ahead = true;
std::mutex g_ahead_mutex;
std::unordered_map<HANDLE, ReadAhead> g_ahead;

// Only handles this layer opened itself, read-only and non-overlapped. Anything
// else keeps the stock path, including the virtual handles - those are already
// served out of memory and have their own position.
void TrackForReadAhead(HANDLE handle, DWORD access, DWORD flags) {
  if (!g_read_ahead || handle == INVALID_HANDLE_VALUE) {
    return;
  }
  if ((access & GENERIC_WRITE) != 0 || (access & GENERIC_READ) == 0) {
    return;
  }
  if ((flags & FILE_FLAG_OVERLAPPED) != 0) {
    return;
  }
  std::lock_guard lock(g_ahead_mutex);
  // Assignment rather than try_emplace, and that is not a style choice: Windows
  // recycles handle values, so an entry left behind by a close this layer did not
  // see would otherwise be *kept*, and the new file would be read from the old
  // file's position. Overwriting makes a stale entry harmless by construction.
  auto it = g_ahead.find(handle);
  if (it != g_ahead.end()) {
    it->second = ReadAhead{};
    return;
  }
  if (g_ahead.size() >= kReadAheadHandles) {
    return;
  }
  g_ahead.emplace(handle, ReadAhead{});
}

void DropReadAhead(HANDLE handle) {
  if (!g_read_ahead) {
    return;
  }
  std::lock_guard lock(g_ahead_mutex);
  g_ahead.erase(handle);
}

// Seeks the real handle to `offset` and fills the buffer from there. A short read
// is end of file and is recorded as such rather than retried - `valid` shrinking
// below the request is how the caller learns it.
bool RefillReadAhead(HANDLE handle, ReadAhead &state, uint64_t offset) {
  if (state.data.size() != kReadAheadBytes) {
    state.data.resize(kReadAheadBytes);
  }
  LONG high = static_cast<LONG>(offset >> 32);
  const DWORD low = OriginalSetFilePointer(
      handle, static_cast<LONG>(offset & 0xffffffffu), &high, FILE_BEGIN);
  if (low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
    return false;
  }
  DWORD got = 0;
  if (!OriginalReadFile(handle, state.data.data(),
                        static_cast<DWORD>(state.data.size()), &got, nullptr)) {
    return false;
  }
  state.base = offset;
  state.valid = got;
  return true;
}

// The whole point of the layer. `state.position` is authoritative; the OS handle's
// own pointer is scratch, moved only by a refill.
BOOL BufferedRead(HANDLE handle, ReadAhead &state, LPVOID buffer, DWORD count,
                  LPDWORD read) {
  if (read) {
    *read = 0;
  }
  if (count == 0) {
    return TRUE;
  }
  if (!buffer) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  // A request at or over the buffer size would evict more than it serves, so it
  // goes straight to the file - which is the whole-file `.rif` and sound path.
  if (count >= kReadAheadBytes) {
    LONG high = static_cast<LONG>(state.position >> 32);
    const DWORD low = OriginalSetFilePointer(
        handle, static_cast<LONG>(state.position & 0xffffffffu), &high,
        FILE_BEGIN);
    if (low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
      return FALSE;
    }
    DWORD got = 0;
    if (!OriginalReadFile(handle, buffer, count, &got, nullptr)) {
      return FALSE;
    }
    state.position += got;
    state.valid = 0; // the buffer no longer describes where we are
    if (read) {
      *read = got;
    }
    return TRUE;
  }

  uint8_t *out = static_cast<uint8_t *>(buffer);
  DWORD served = 0;
  while (served < count) {
    const bool covered = state.valid != 0 && state.position >= state.base &&
                         state.position < state.base + state.valid;
    if (!covered) {
      if (!RefillReadAhead(handle, state, state.position)) {
        return FALSE;
      }
      if (state.valid == 0) {
        break; // end of file
      }
      continue;
    }
    const uint64_t offset = state.position - state.base;
    const DWORD available = static_cast<DWORD>(state.valid - offset);
    // Explicit template argument: windows.h is included without NOMINMAX, so a
    // bare std::min is eaten by the `min` macro. Same trick as the virtual path.
    const DWORD take = std::min<DWORD>(available, count - served);
    std::memcpy(out + served, state.data.data() + offset, take);
    served += take;
    state.position += take;
  }
  if (read) {
    *read = served;
  }
  return TRUE;
}

FILETIME FileTimeFromUnix(int64_t seconds) {
  // PhysicsFS reports -1 for an archive that records no timestamp. "Now" is the
  // right answer there: it keeps a virtual file at least as new as any .opt/.map/
  // .cut cache sitting beside it on disk, so the engine's own freshness checks
  // rebuild rather than load a cache built from the unmodded original.
  if (seconds < 0) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return now;
  }
  ULARGE_INTEGER value{};
  value.QuadPart =
      (static_cast<uint64_t>(seconds) + 11644473600ull) * 10000000ull;
  FILETIME time{};
  time.dwLowDateTime = value.LowPart;
  time.dwHighDateTime = value.HighPart;
  return time;
}

// A real, unsignalled event stands in for the file. Two reasons it is not a
// made-up number: handle values are then unique by construction, so a virtual
// handle can never collide with a real one; and an API this layer does not hook
// (D3DX reaches CreateFileMappingA) gets a valid handle of the wrong type and
// fails in an orderly way instead of running on an invented pointer.
HANDLE OpenVirtual(const std::string &vpath) {
  VirtualFile file;
  if (!vfs::Read(vpath.c_str(), &file.data)) {
    return INVALID_HANDLE_VALUE;
  }
  int64_t modtime = -1;
  vfs::Stat(vpath.c_str(), nullptr, &modtime);
  file.modified = FileTimeFromUnix(modtime);

  HANDLE handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!handle) {
    return INVALID_HANDLE_VALUE;
  }
  {
    std::lock_guard lock(g_files_mutex);
    g_files.emplace(handle, std::move(file));
  }
  g_open_files.fetch_add(1, std::memory_order_release);
  NoteServed(vpath);
  return handle;
}

bool AnyVirtual() { return g_open_files.load(std::memory_order_acquire) != 0; }

// --- the first-open anchor -------------------------------------------------------

// Everything that has to have happened before the engine reads its first byte,
// run from whichever of the four hooks below the engine reaches first. This is
// the one moment that is provably both late enough and early enough: the game
// only opens a file from WinMain onwards, so gl.exe's CRT heap exists - and a
// file is always opened before its bytes can be sniffed or its name looked up in
// the VFS, so nothing here can be too late for its consumer.
//
// It is one anchor rather than three detours because two subsystems must never
// detour one target (see Conventions in CLAUDE.md); each of the three is
// idempotent in its own right, and the flag is set before they run so a call
// back into a hook from inside one cannot recurse.
void EnsureFirstOpen() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  // The image-codec registry allocates from gl.exe's CRT heap. See src/ImageCodec.h
  // for why it is a registration here rather than a detour of its own.
  image::RegisterDdsCodec();
  // Inside WinMain puts this ahead of the device, so the stored renderer settings
  // are on the knobs before the renderer initialises rather than a frame or a
  // menu later. See src/RenderMenu.h.
  ApplyStoredRenderSettings();
  // The profile's boot module, which is what decides which mods are mounted -
  // and this is the last instant at which that decision can still be made, since
  // the caller is about to consult the VFS. See src/Script.h.
  BootScriptProfile();
}

// --- the Win32 hooks -----------------------------------------------------------

HANDLE WINAPI HookedCreateFileA(LPCSTR name, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                DWORD flags, HANDLE template_file) {
  // OPEN_EXISTING and nothing else. Every one of the 31 CreateFileA sites in the
  // binary uses either OPEN_EXISTING (21) or CREATE_ALWAYS (9) - there is no
  // OPEN_ALWAYS anywhere - so this covers every read and cannot intercept a
  // write. Writes (savegames, demos, the .opt/.map/.cut caches, GLkeys.cfg)
  // belong on the real filesystem.
  //
  // Access rights are deliberately *not* part of the test: IsFirstFileNewer
  // @ 0x004af430 opens with GENERIC_READ|GENERIC_WRITE and only reads
  // timestamps, and it has to see the mod's file or a stale on-disk cache wins.
  EnsureFirstOpen();

  if (name && disposition == OPEN_EXISTING) {
    try {
      if (auto vpath = vfs::Resolve(name)) {
        HANDLE handle = OpenVirtual(*vpath);
        if (handle != INVALID_HANDLE_VALUE) {
          return handle;
        }
      }
    } catch (...) {
      // Fall through to the real open: a failure here must degrade to vanilla
      // behaviour, never propagate a C++ exception into game code.
    }
  }
  HANDLE handle = OriginalCreateFileA(name, access, share, security, disposition,
                                      flags, template_file);
  TrackForReadAhead(handle, access, flags);
  return handle;
}

BOOL WINAPI HookedReadFile(HANDLE handle, LPVOID buffer, DWORD count,
                           LPDWORD read, LPOVERLAPPED overlapped) {
  if (g_read_stats) {
    NoteRead(_ReturnAddress(), count);
  }
  if (AnyVirtual()) {
    std::lock_guard lock(g_files_mutex);
    auto it = g_files.find(handle);
    if (it != g_files.end()) {
      if (overlapped) {
        // No call site in the game uses FILE_FLAG_OVERLAPPED (file_io_notes.md
        // §1), so this cannot fire - it is here so that if one ever appears it
        // fails loudly instead of silently ignoring the OVERLAPPED offset.
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
      }
      VirtualFile &file = it->second;
      const uint64_t size = file.data.size();
      const uint64_t remaining = file.position < size ? size - file.position : 0;
      const DWORD wanted = static_cast<DWORD>(std::min<uint64_t>(count, remaining));
      if (wanted != 0) {
        if (!buffer) {
          SetLastError(ERROR_INVALID_PARAMETER);
          return FALSE;
        }
        std::memcpy(buffer, file.data.data() + file.position, wanted);
        file.position += wanted;
      }
      if (read) {
        *read = wanted;
      }
      // A short read at end of file is success with zero bytes, exactly as the
      // real ReadFile reports it - which is what the engine's callers expect.
      return TRUE;
    }
  }
  if (g_read_ahead && !overlapped) {
    std::lock_guard lock(g_ahead_mutex);
    auto it = g_ahead.find(handle);
    if (it != g_ahead.end()) {
      return BufferedRead(handle, it->second, buffer, count, read);
    }
  }
  return OriginalReadFile(handle, buffer, count, read, overlapped);
}

BOOL WINAPI HookedWriteFile(HANDLE handle, LPCVOID buffer, DWORD count,
                            LPDWORD written, LPOVERLAPPED overlapped) {
  if (AnyVirtual()) {
    std::lock_guard lock(g_files_mutex);
    if (g_files.find(handle) != g_files.end()) {
      SetLastError(ERROR_ACCESS_DENIED);
      return FALSE;
    }
  }
  return OriginalWriteFile(handle, buffer, count, written, overlapped);
}

BOOL WINAPI HookedSetEndOfFile(HANDLE handle) {
  if (AnyVirtual()) {
    std::lock_guard lock(g_files_mutex);
    if (g_files.find(handle) != g_files.end()) {
      SetLastError(ERROR_ACCESS_DENIED);
      return FALSE;
    }
  }
  return OriginalSetEndOfFile(handle);
}

DWORD WINAPI HookedSetFilePointer(HANDLE handle, LONG distance,
                                  PLONG distance_high, DWORD method) {
  if (AnyVirtual()) {
    std::lock_guard lock(g_files_mutex);
    auto it = g_files.find(handle);
    if (it != g_files.end()) {
      VirtualFile &file = it->second;
      int64_t base = 0;
      switch (method) {
      case FILE_BEGIN:
        base = 0;
        break;
      case FILE_CURRENT:
        base = static_cast<int64_t>(file.position);
        break;
      case FILE_END:
        base = static_cast<int64_t>(file.data.size());
        break;
      default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
      }
      const int64_t delta =
          distance_high
              ? static_cast<int64_t>(
                    (static_cast<uint64_t>(static_cast<uint32_t>(*distance_high))
                     << 32) |
                    static_cast<uint32_t>(distance))
              : static_cast<int64_t>(distance);
      const int64_t position = base + delta;
      if (position < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return INVALID_SET_FILE_POINTER;
      }
      // Seeking past the end is legal, exactly as for a real file; a read there
      // then returns zero bytes.
      file.position = static_cast<uint64_t>(position);
      if (distance_high) {
        *distance_high = static_cast<LONG>(position >> 32);
      }
      // INVALID_SET_FILE_POINTER is a legal low dword, so callers tell it from a
      // failure by GetLastError - which makes clearing the error mandatory, not
      // tidiness.
      SetLastError(NO_ERROR);
      return static_cast<DWORD>(position & 0xffffffffu);
    }
  }
  if (g_read_ahead) {
    std::lock_guard lock(g_ahead_mutex);
    auto it = g_ahead.find(handle);
    if (it != g_ahead.end()) {
      ReadAhead &state = it->second;
      int64_t base = 0;
      switch (method) {
      case FILE_BEGIN:
        base = 0;
        break;
      case FILE_CURRENT:
        // Ours, not the OS handle's - the real pointer trails wherever the last
        // refill left it, which is the whole reason this hook has to be here.
        base = static_cast<int64_t>(state.position);
        break;
      case FILE_END: {
        DWORD high = 0;
        const DWORD low = OriginalGetFileSize(handle, &high);
        if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
          return INVALID_SET_FILE_POINTER;
        }
        base = static_cast<int64_t>((static_cast<uint64_t>(high) << 32) | low);
        break;
      }
      default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
      }
      const int64_t delta =
          distance_high
              ? static_cast<int64_t>(
                    (static_cast<uint64_t>(static_cast<uint32_t>(*distance_high))
                     << 32) |
                    static_cast<uint32_t>(distance))
              : static_cast<int64_t>(distance);
      const int64_t position = base + delta;
      if (position < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return INVALID_SET_FILE_POINTER;
      }
      // The buffer is kept, not dropped: a seek backwards inside it is exactly
      // the pattern a chunk reader produces, and re-reading would undo the win.
      state.position = static_cast<uint64_t>(position);
      if (distance_high) {
        *distance_high = static_cast<LONG>(position >> 32);
      }
      SetLastError(NO_ERROR);
      return static_cast<DWORD>(position & 0xffffffffu);
    }
  }
  return OriginalSetFilePointer(handle, distance, distance_high, method);
}

DWORD WINAPI HookedGetFileSize(HANDLE handle, LPDWORD size_high) {
  if (AnyVirtual()) {
    std::lock_guard lock(g_files_mutex);
    auto it = g_files.find(handle);
    if (it != g_files.end()) {
      const uint64_t size = it->second.data.size();
      if (size_high) {
        *size_high = static_cast<DWORD>(size >> 32);
      }
      SetLastError(NO_ERROR); // same INVALID_FILE_SIZE ambiguity as above
      return static_cast<DWORD>(size & 0xffffffffu);
    }
  }
  return OriginalGetFileSize(handle, size_high);
}

BOOL WINAPI HookedGetFileTime(HANDLE handle, LPFILETIME creation,
                              LPFILETIME last_access, LPFILETIME last_write) {
  if (AnyVirtual()) {
    std::lock_guard lock(g_files_mutex);
    auto it = g_files.find(handle);
    if (it != g_files.end()) {
      if (creation) {
        *creation = it->second.modified;
      }
      if (last_access) {
        *last_access = it->second.modified;
      }
      if (last_write) {
        *last_write = it->second.modified;
      }
      return TRUE;
    }
  }
  return OriginalGetFileTime(handle, creation, last_access, last_write);
}

BOOL WINAPI HookedCloseHandle(HANDLE handle) {
  if (AnyVirtual()) {
    bool was_virtual = false;
    {
      std::lock_guard lock(g_files_mutex);
      was_virtual = g_files.erase(handle) != 0;
    }
    if (was_virtual) {
      g_open_files.fetch_sub(1, std::memory_order_release);
    }
  }
  DropReadAhead(handle);
  // The handle is a real event whether or not it was ours, so the real
  // CloseHandle is what releases it. Everything else in the process - threads,
  // events, the executor's own handles - lands here untouched.
  return OriginalCloseHandle(handle);
}

DWORD WINAPI HookedGetFileAttributesA(LPCSTR name) {
  // Also an anchor: this consults the VFS, so on the (unlikely) launch where the
  // engine asks about a file before it opens one, the boot module still gets to
  // mount first.
  EnsureFirstOpen();
  if (name) {
    try {
      if (vfs::Resolve(name)) {
        // READONLY does real work rather than being cosmetic: the rif
        // recompressor at 0x005b03b0 rewrites its input file in place unless
        // this bit is set, and its input can be a file a mod supplied - which
        // would mean writing mod content into the base install.
        return FILE_ATTRIBUTE_READONLY;
      }
    } catch (...) {
    }
  }
  return OriginalGetFileAttributesA(name);
}

// --- the CRT hooks -------------------------------------------------------------

// Only a read. "r" and "rb" qualify; anything with a '+', and every "w"/"a", is
// a write and belongs on the real filesystem.
bool IsReadOnlyMode(const char *mode) {
  if (!mode || mode[0] != 'r') {
    return false;
  }
  for (const char *c = mode; *c; ++c) {
    if (*c == '+') {
      return false;
    }
  }
  return true;
}

void *__cdecl HookedFopen(const char *name, const char *mode) {
  EnsureFirstOpen();
  if (name && IsReadOnlyMode(mode)) {
    try {
      if (auto vpath = vfs::Resolve(name)) {
        std::string real;
        if (vfs::Materialize(vpath->c_str(), &real)) {
          NoteServed(*vpath);
          return OriginalFopen(real.c_str(), mode);
        }
      }
    } catch (...) {
    }
  }
  return OriginalFopen(name, mode);
}

void *__cdecl HookedFreopen(const char *name, const char *mode, void *stream) {
  EnsureFirstOpen();
  if (name && IsReadOnlyMode(mode)) {
    try {
      if (auto vpath = vfs::Resolve(name)) {
        std::string real;
        if (vfs::Materialize(vpath->c_str(), &real)) {
          NoteServed(*vpath);
          return OriginalFreopen(real.c_str(), mode, stream);
        }
      }
    } catch (...) {
    }
  }
  return OriginalFreopen(name, mode, stream);
}

// --- import table patching -----------------------------------------------------

struct ImportPatch {
  void **slot;
  void *original;
};
std::vector<ImportPatch> g_patches;

// `name` is the export the slot must currently hold. Checking it is what stops a
// mistyped offset from overwriting an unrelated .rdata pointer - a failure mode
// that would present as a random crash far from here. A mismatch means either
// that, or that something else in the process patched the same slot first;
// neither is worth guessing about, so the slot is left alone and the game runs
// unmodified.
template <typename Fn>
bool Patch(uintptr_t offset, const char *name, Fn replacement, Fn *original) {
  void **slot = reinterpret_cast<void **>(GetBaseAddress() + offset);
  *original = reinterpret_cast<Fn>(*slot);

  HMODULE kernel = GetModuleHandleA("kernel32.dll");
  void *expected =
      kernel ? reinterpret_cast<void *>(GetProcAddress(kernel, name)) : nullptr;
  if (!expected || *slot != expected) {
    DebugWrite("gkplus vfs: import slot {:#x} does not hold kernel32!{}; "
               "leaving it alone\n",
               offset, name);
    return false;
  }

  DWORD protect = 0;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protect)) {
    DebugWrite("gkplus vfs: cannot unprotect import slot {:#x} for {}\n", offset,
               name);
    return false;
  }
  void *previous = *slot;
  *slot = reinterpret_cast<void *>(replacement);
  VirtualProtect(slot, sizeof(void *), protect, &protect);
  g_patches.push_back({slot, previous});
  return true;
}

void Unpatch() {
  for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) {
    DWORD protect = 0;
    if (VirtualProtect(it->slot, sizeof(void *), PAGE_READWRITE, &protect)) {
      *it->slot = it->original;
      VirtualProtect(it->slot, sizeof(void *), protect, &protect);
    }
  }
  g_patches.clear();
}

} // namespace

uint64_t VirtualizedOpenCount() {
  return g_served.load(std::memory_order_relaxed);
}

std::vector<std::string> RecentVirtualizedOpens() {
  std::lock_guard lock(g_recent_mutex);
  return g_recent;
}

ReadStats ReadAccounting() {
  ReadStats stats;
  if (!g_read_stats) {
    return stats;
  }
  std::lock_guard lock(g_read_mutex);
  stats.enabled = true;
  stats.calls = g_read_calls;
  stats.bytes = g_read_bytes;
  for (size_t i = 0; i < std::size(g_read_buckets); ++i) {
    if (g_read_buckets[i] != 0) {
      stats.buckets.push_back({i == 0 ? 0u : 1u << (i - 1), g_read_buckets[i]});
    }
  }
  for (const ReadSite &site : g_read_sites) {
    if (site.calls == 0) {
      break;
    }
    stats.sites.push_back({site.rva, site.calls, site.bytes});
  }
  std::sort(stats.sites.begin(), stats.sites.end(),
            [](const ReadStats::Site &a, const ReadStats::Site &b) {
              return a.calls > b.calls;
            });
  return stats;
}

void ResetReadAccounting() {
  std::lock_guard lock(g_read_mutex);
  g_read_calls = 0;
  g_read_bytes = 0;
  std::fill(std::begin(g_read_buckets), std::end(g_read_buckets), 0ull);
  for (ReadSite &site : g_read_sites) {
    site = ReadSite{};
  }
}

FileHookSystem::FileHookSystem() {
  char stats[8]{};
  g_read_stats = GetEnvironmentVariableA("GKPLUS_FILE_STATS", stats,
                                         sizeof(stats)) != 0 &&
                 stats[0] != '0';
  char buffered[8]{};
  if (GetEnvironmentVariableA("GKPLUS_FILE_BUFFER", buffered,
                              sizeof(buffered)) != 0) {
    g_read_ahead = buffered[0] != '0' && buffered[0] != 'r' && buffered[0] != 'R';
  }

  // Slot addresses from file_io_notes.md §5. GetBaseAddress() is the relocation
  // delta, so these are the same "offsets" every other subsystem uses.
  //
  // The five marked essential are the ones a virtual handle cannot live without.
  // If any of them does not take, the whole Win32 half is rolled back: a handle
  // handed out by a patched CreateFileA whose ReadFile was *not* patched would
  // fail every read, i.e. break asset loading outright. Losing mod support is
  // the acceptable failure here; a half-hooked game is not.
  bool essential = true;
  essential &= Patch(0x0064d068, "CreateFileA", HookedCreateFileA,
                     &OriginalCreateFileA);
  essential &= Patch(0x0064d06c, "ReadFile", HookedReadFile, &OriginalReadFile);
  essential &= Patch(0x0064d074, "CloseHandle", HookedCloseHandle,
                     &OriginalCloseHandle);
  essential &= Patch(0x0064d0b8, "GetFileSize", HookedGetFileSize,
                     &OriginalGetFileSize);
  essential &= Patch(0x0064d228, "SetFilePointer", HookedSetFilePointer,
                     &OriginalSetFilePointer);
  // The rest only refine behaviour: without them a virtual file reports the
  // real file's timestamp and attributes, and a write to one would reach the
  // original instead of being refused.
  Patch(0x0064d070, "WriteFile", HookedWriteFile, &OriginalWriteFile);
  Patch(0x0064d0bc, "GetFileTime", HookedGetFileTime, &OriginalGetFileTime);
  Patch(0x0064d220, "GetFileAttributesA", HookedGetFileAttributesA,
        &OriginalGetFileAttributesA);
  Patch(0x0064d224, "SetEndOfFile", HookedSetEndOfFile, &OriginalSetEndOfFile);

  if (!essential) {
    DebugWrite("gkplus vfs: incomplete import patch; reverting, mods disabled\n");
    Unpatch();
  }

  // gl.exe's own static-CRT fopen/freopen, not this DLL's. Both open with the
  // 5-byte MOV EDI,EDI / PUSH EBP / MOV EBP,ESP hot-patch prologue, so Detours
  // takes them cleanly.
  GetObjectAtOffset(OriginalFopen, 0x005f067e);
  GetObjectAtOffset(OriginalFreopen, 0x0060e0dc);
  ::DetourAttach(reinterpret_cast<void **>(&OriginalFopen),
                 reinterpret_cast<void *>(HookedFopen));
  ::DetourAttach(reinterpret_cast<void **>(&OriginalFreopen),
                 reinterpret_cast<void *>(HookedFreopen));
}

FileHookSystem::~FileHookSystem() {
  ::DetourDetach(reinterpret_cast<void **>(&OriginalFopen),
                 reinterpret_cast<void *>(HookedFopen));
  ::DetourDetach(reinterpret_cast<void **>(&OriginalFreopen),
                 reinterpret_cast<void *>(HookedFreopen));
  Unpatch();

  // Nothing can reach a virtual handle now, so drop them and let the VFS remove
  // its materialization directory.
  {
    std::lock_guard lock(g_files_mutex);
    for (const auto &entry : g_files) {
      OriginalCloseHandle(entry.first);
    }
    g_files.clear();
  }
  g_open_files.store(0, std::memory_order_release);
  vfs::Shutdown();
}

} // namespace gk
