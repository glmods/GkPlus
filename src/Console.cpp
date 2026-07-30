#include "Console.h"

#include "Core.h"

namespace gk {
void Print(const char *what) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d4b50);
  fn(what);
}

bool ExecuteCommand(const char *cmd) {
  // The length check is ours, not the game's - see the note in Console.h. Done
  // here rather than in the binding so that every caller inherits it.
  if (cmd == nullptr) {
    return false;
  }
  size_t length = 0;
  while (cmd[length] != '\0') {
    if (++length > static_cast<size_t>(kConsoleCommandLineMax)) {
      return false;
    }
  }
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d6090);
  fn(cmd);
  return true;
}

void ExecuteCommandLine(const char *cmdline) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d59e0);
  fn(cmdline);
}

bool ExecuteCommandFile(const char *fileName) {
  FastCall<bool, const char *> fn;
  GetObjectAtOffset(fn, 0x0043f250);
  return fn(fileName);
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

int GetRegisteredCommandCount() {
  int *p;
  GetObjectAtOffset(p, 0x007b6a70);
  return *p;
}

int GetCommandTableBucketCount() {
  int *p;
  GetObjectAtOffset(p, 0x007b6a74);
  return *p;
}

CommandListElem **GetCommandTable() {
  // Three stars, and the dereference, are both load-bearing: 0x007b6a7c *holds* the
  // bucket array rather than being it. Every game reader loads the value
  // (`MOV EAX,[0x007b6a7c]` in RegisterConsoleCommand, CommandListCommands,
  // FreeCommands, ...), so returning the address of the global instead walked
  // `&CommandTableBuckets` as if it were the array: element 0 was the real array
  // pointer chased as a chain node, and element 1 onwards were the neighbouring
  // globals - ConsoleTextScrollTarget @ 0x007b6a80 is a float. It faulted on the
  // first `e->command` every time, which made `console.commands` a guaranteed
  // access violation.
  CommandListElem ***p;
  GetObjectAtOffset(p, 0x007b6a7c);
  return *p;
}

int GetCommandCondition() {
  int *p;
  GetObjectAtOffset(p, 0x006a642c);
  return *p;
}

void ForEachConsoleCommand(void *user,
                           void (*fn)(void *, const CommandData *)) {
  CommandListElem **buckets = GetCommandTable();
  const int count = GetCommandTableBucketCount();
  if (buckets == nullptr || count <= 0) {
    return;
  }
  // Walk every bucket rather than counting down from NumRegisteredCommands the
  // way CommandListCommands does: that loop walks off the end of the array when
  // the count and the chains disagree, which they would as soon as anything
  // registers a command without bumping the counter.
  for (int i = 0; i < count; ++i) {
    for (CommandListElem *e = buckets[i]; e != nullptr; e = e->next) {
      if (e->command != nullptr) {
        fn(user, e->command);
      }
    }
  }
}
} // namespace gk
