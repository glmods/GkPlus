#pragma once

namespace gk {
// A console command handler: __fastcall(len, argline). See RegisterConsoleCommand.
using ConsoleCommandCallback = void(__fastcall *)(int, char *);

// The console command registry. This is a **hash table**, not a list, and it
// lives at 0x007b6a70..0x007b6a7c - 0x007b6aa8 is `CommandsToExecute`, the
// unrelated per-frame command *queue*, which is what this used to point at.
//
// Buckets are `CommandListElem *[CommandTableNumBuckets]`, chained singly, and
// the hash is just the uppercased first character of the name masked by
// CommandTableMask (HashCommandName @ 0x004d4290). Registration prepends, so a
// bucket lists its commands newest-first; nothing is ever removed.
struct CommandData {
  const char *name;
  const char *help;
  ConsoleCommandCallback callback;
  // The minimum `CommandCondition` @ 0x006a642c this command needs before
  // ConsoleExecuteCommandLine will dispatch it (`condition <= CommandCondition`,
  // silently skipped otherwise). Registrations use 0, 1 and 3; the global is
  // initialised to 3 and only HandleConsoleKeyPress touches it - it sets it to
  // 3, which the default already is - so in a stock build every command passes.
  int condition;
};

struct CommandListElem {
  CommandData *command;
  CommandListElem *next;
};

// --- Native API over the console --------------------------------------------

// Whether `InitConsole` @ 0x004d5380 has run and `ShutdownConsole` @ 0x004d5620
// has not - i.e. whether the four font globals it builds are non-null. It is one
// byte, `ConsoleInitialized` @ 0x007b6c3a, set as InitConsole's last act and
// cleared as ShutdownConsole's, and the engine itself uses it as InitConsole's
// idempotence guard.
//
// This is not a curiosity: `WinMain` reaches InitConsole at 0x0046bb81, well
// *after* the engine has opened its first file, so anything running from
// FileHookSystem's first-open anchor (the profile's boot module, above all) is
// on the wrong side of it. Print() gates itself on this; a caller that wants to
// know rather than to print asks here.
bool ConsoleReady();

void Print(const char *what);                 // 0x004d4b50, a no-op until ready
void ExecuteCommandLine(const char *cmdline); // 0x004d59e0

// ConsoleCommandLine @ 0x007b6958 is `char[252]`, so a command line may hold 251
// characters plus the terminator.
//
// **`ExecuteCommand` @ 0x004d6090 copies into it with an unbounded byte loop**
// (0x004d6097..0x004d60a2 - no length check of any kind) and the next global is
// SmallFont @ 0x007b6a54, so a longer line writes a caller-controlled
// string through a font pointer. This is the same hazard `PumpQueuedConsoleCommand`
// has, and the reason ScriptQueue.cpp replaces that one.
//
// Nothing in the game can reach it - `fgets` caps a batch line at 249 and the
// console's own input is bounded by the buffer it types into - but a *script*
// can, so `ExecuteCommand` refuses anything longer and returns false rather than
// truncating: a half-command is not a safer thing to run than none.
inline constexpr int kConsoleCommandLineMax = 251;
bool ExecuteCommand(const char *cmd); // 0x004d6090

// 0x0043f250. Appends the file's lines to the command queue - it does not run
// them; PumpQueuedConsoleCommand pops one per frame. Lines are read with fgets
// at 0xfa bytes (249 chars), '//' starts a comment, and a leading '#' is a
// directive (ONLY IF SAFE / ONLY IF HINTS ON / CLEAR BATCH / EXECUTE
// IMMEDIATELY / NORMAL EXECUTION, matched case-insensitively).
//
// Returns whether the file opened. The game declares this `int`, but only AL is
// written (`MOV AL,1` / `XOR AL,AL`) and the upper 24 bits are leftover fclose
// garbage - so it is a bool return, and none of the game's own six callers
// reads it. The path is only ever handed to fopen, hence const.
bool ExecuteCommandFile(const char *fileName);
void RegisterConsoleCommand(const char *name, const char *help,
                            ConsoleCommandCallback callback,
                            int flags); // 0x004d5d50

// Console globals: CommandLine (char[252]) @ 0x007b6958, TextColor @ 0x007b6950,
// CursorColor @ 0x007c149c.
char *GetConsoleCommandLine();
unsigned *GetConsoleTextColor();
unsigned *GetConsoleCursorColor();

// The registry, walked bucket by bucket. `NumRegisteredCommands` @ 0x007b6a70 is
// the authoritative total (280 registrations in a stock build, 272 distinct
// names - a few are aliases, and one is the empty string).
//
// **Fifteen of those names come from glres<lang>.dll, not from the binary**:
// SetupConsoleCommands passes GetResourceString results for EXIT, QUIT, MENU,
// HELP, LINES, CONSOLE APPEAR, SAY, TIME, DATE, LIST COMMANDS, CLEAR HISTORY
// BUFFER, HISTORY BUFFER SIZE, HISTORY_BUFFER_LENGTH, QUEUE SIZE and QUEUE
// LENGTH. They are therefore **localized**, and a hard-coded `ExecuteCommand
// ("QUIT")` does nothing on a French or German install. Enumerating the registry
// is the only way to spell those portably.
int GetRegisteredCommandCount();      // 0x007b6a70
int GetCommandTableBucketCount();     // 0x007b6a74
CommandListElem **GetCommandTable();  // 0x007b6a7c

// The gate ConsoleExecuteCommandLine tests each CommandData::condition against.
int GetCommandCondition(); // 0x006a642c

// Calls `fn(command)` once per registered command, in bucket order.
void ForEachConsoleCommand(void *user, void (*fn)(void *, const CommandData *));
} // namespace gk
