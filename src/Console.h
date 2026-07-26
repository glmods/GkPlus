#pragma once

namespace gk {
// Node of the console command registry linked list (the CommandList @ 0x007b6aa8).
struct Command {
  void *dtor;
  Command *prev, *next;
  const char *command;
};

struct CommandList {
  Command *first;
  int num;
};

// A console command handler: __fastcall(len, argline). See RegisterConsoleCommand.
using ConsoleCommandCallback = void(__fastcall *)(int, char *);

// --- Native API over the console --------------------------------------------

void Print(const char *what);                 // 0x004d4b50
void ExecuteCommand(const char *cmd);         // 0x004d6090
void ExecuteCommandLine(const char *cmdline); // 0x004d59e0

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
// CursorColor @ 0x007c149c, and the command registry list @ 0x007b6aa8.
char *GetConsoleCommandLine();
unsigned *GetConsoleTextColor();
unsigned *GetConsoleCursorColor();
CommandList *GetConsoleCommandList();
} // namespace gk
