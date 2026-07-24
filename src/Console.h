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
int ExecuteCommandFile(unsigned char *file);  // 0x0043f250
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
