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
