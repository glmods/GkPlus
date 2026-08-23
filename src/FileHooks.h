#pragma once

// What makes the engine read a mod's files instead of its own. The lookup half
// is src/Vfs.h; this is the interception.
//
// --- Why the import table -------------------------------------------------------
//
// Every file call in gl.exe goes through its own IAT - `CALL dword ptr [slot]`,
// or `MOV reg,[slot]` then `CALL reg`, and both read the slot at run time. So one
// pointer write per slot catches every call site, including the register-cached
// ones, and it catches only gl.exe: GkPlus's own runtime and D3D calls resolve
// through this DLL's imports and are untouched. Detouring kernel32 would hit
// everything in the process instead. Slot addresses and the whole inventory are
// in file_io_notes.md §5.
//
// Nine slots carry the Win32 half (CreateFileA, ReadFile, SetFilePointer,
// GetFileSize, GetFileTime, GetFileAttributesA, CloseHandle, and WriteFile plus
// SetEndOfFile to refuse a write to a virtual file). That is enough because of
// three measurements: no call site anywhere uses FILE_FLAG_OVERLAPPED, no file
// I/O happens on the executor thread, and the two formats that matter - .rif via
// File_Chunk @ 0x005afeb0 and sound samples via SoundSample_LoadFile
// @ 0x005d3740 - read the whole file in a single ReadFile and parse it in memory.
//
// --- Two shapes of interception -------------------------------------------------
//
// A virtualized CreateFileA hands back a **real kernel handle** - an unsignalled
// event - with the file's bytes held beside it. Using a genuine handle rather
// than a made-up value is what makes an unhooked API fail cleanly: anything that
// reaches, say, CreateFileMappingA (D3DX does) gets a valid handle of the wrong
// type and an orderly error, instead of dereferencing a number we invented.
//
// The game's statically linked UCRT cannot be served that way - it would mean
// virtualizing CreateFileW and then GetFileType, SetFilePointerEx and the rest of
// lowio - so `fopen` and `freopen` are detoured directly (both are hot-patch
// prologues in gl.exe's private CRT copy, so this cannot affect GkPlus's own
// runtime) and a hit is written out to a temp file that the real fopen then
// opens. That costs a small write for a .gls, .gsh or .gcs, and in exchange the
// FILE * is genuine, so nothing downstream can misbehave. See
// vfs::Materialize.
//
// --- What is not covered --------------------------------------------------------
//
// Bink. `BinkOpen` takes a file name and opens it inside BINKW32.DLL, which does
// not use gl.exe's IAT, so music and FMV still come off disk. So do the
// glres<lang>.dll resource strings, which LoadLibrary needs a real file for.

#include <cstdint>
#include <string>
#include <vector>

namespace gk {

// How many opens this layer has answered from a mod rather than from disk, since
// the process started. Exposed as `mods.served` because it is the only way to
// tell "the mod is mounted" from "the mod is actually being read": a replaced
// asset usually looks identical from outside the game.
uint64_t VirtualizedOpenCount();

// The VFS paths behind the last few of those, oldest first. `mods.recent`, and
// the answer to "is the engine picking my file up, and under what name" - which
// is otherwise unanswerable, because the name the engine asks for is assembled
// from a GLDir and a string in a .gls.
std::vector<std::string> RecentVirtualizedOpens();

// What the engine's reads actually look like, behind `GKPLUS_FILE_STATS=1`. Off
// otherwise: it locks on a path a level load runs six figures of times.
//
// It exists because the cost of a level load is not where the notes implied. The
// ".rif and sound are whole-file reads" of §1 is true, and a load still issues
// ~150,000 `ReadFile` calls averaging 64-150 bytes for 9-18 MB - so the load is
// bound by syscall count, not by bytes. `sites` is what names the caller: an RVA
// into gl.exe, resolvable against the Ghidra database or the profiler's symbol map.
struct ReadStats {
  /// One calling site in gl.exe and what it read. `rva` is an offset into the
  /// module, resolvable against the Ghidra database or the profiler's symbol
  /// map, rather than a runtime address.
  struct Site {
    uintptr_t rva;
    uint64_t calls;
    uint64_t bytes;
  };
  /// One bucket of the read-size histogram: how many calls asked for at least
  /// `at_least` bytes and fewer than the next bucket's.
  struct Bucket {
    uint32_t at_least;
    uint64_t calls;
  };
  bool enabled = false;
  uint64_t calls = 0;
  uint64_t bytes = 0;
  std::vector<Bucket> buckets;
  std::vector<Site> sites; // most calls first
};

/// A snapshot of the read counters. With `GKPLUS_FILE_STATS` unset the result
/// has `enabled == false` and every count zero rather than being unavailable.
ReadStats ReadAccounting();
/// Zeroes the read counters, so a measurement can be scoped to one level load
/// rather than to the session.
void ResetReadAccounting();

// RAII, like every other *System: construct inside a Detours transaction from
// entry.cpp. The IAT writes are plain memory stores and need no transaction of
// their own; the two CRT detours do.
class FileHookSystem {
public:
  FileHookSystem();
  ~FileHookSystem();
  FileHookSystem(const FileHookSystem &) = delete;
  FileHookSystem &operator=(const FileHookSystem &) = delete;
};

} // namespace gk
