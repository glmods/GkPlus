#include "Console.h"

#include "Core.h"

namespace gk {
void Print(const char *what) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d4b50);
  fn(what);
}

void ExecuteCommand(const char *cmd) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d6090);
  fn(cmd);
}

void ExecuteCommandLine(const char *cmdline) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d59e0);
  fn(cmdline);
}

int ExecuteCommandFile(unsigned char *file) {
  FastCall<int, unsigned char *> fn;
  GetObjectAtOffset(fn, 0x0043f250);
  return fn(file);
}

void RegisterConsoleCommand(const char *name, const char *help,
                            ConsoleCommandCallback callback, int flags) {
  FastCall<void, const char *, const char *, ConsoleCommandCallback, int> fn;
  GetObjectAtOffset(fn, 0x004d5d50);
  fn(name, help, callback, flags);
}

char *GetConsoleCommandLine() {
  char *p;
  GetObjectAtOffset(p, 0x007b6958);
  return p;
}

unsigned *GetConsoleTextColor() {
  unsigned *p;
  GetObjectAtOffset(p, 0x007b6950);
  return p;
}

unsigned *GetConsoleCursorColor() {
  unsigned *p;
  GetObjectAtOffset(p, 0x007c149c);
  return p;
}

CommandList *GetConsoleCommandList() {
  CommandList *p;
  GetObjectAtOffset(p, 0x007b6aa8);
  return p;
}
} // namespace gk
