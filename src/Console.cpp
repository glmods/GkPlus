#include "Console.h"

#include "Core.h"

namespace gk {
bool ConsoleReady() {
  // Re-read every call rather than cached: ShutdownConsole @ 0x004d5620 clears
  // this on the way out, after freeing the same four fonts, so a value latched at
  // boot would be wrong at teardown as well as before init.
  bool *flag;
  GetObjectAtOffset(flag, 0x007b6c3a);
  return *flag;
}

void Print(const char *what) {
  // The gate is ours, and it is not defensive programming - it is the difference
  // between a working early boot and an access violation. ConsolePrint reaches
  // Font_GetNormalizedLineHeight @ 0x005782b0 five times and Console_QueueTextLine
  // four, all of them with `SmallFont` @ 0x007b6a54 as `this`, and that global is
  // **null until InitConsole** @ 0x004d5380 runs - which WinMain does at
  // 0x0046bb81, long after the engine has opened its first file.
  //
  // Measured, not inferred: booting the profile's `core.boot` module from
  // FileHookSystem's first intercepted open crashed the game at gl.exe+0x1782b3
  // (`MOVD XMM0,[ECX+0xaf0]` on a null Font), and the dump's stack is
  //   HookedFopen -> EnsureFirstOpen -> BootScriptProfile -> StartRuntime
  //   -> StartRepl -> js::Log -> gk::Print -> ... -> Font_GetNormalizedLineHeight
  // The host's own boot logging is enough to reach it; a script's console.log is
  // not required. Here rather than in js::Log so every caller inherits it, which
  // is the same reasoning as ExecuteCommand's length check below.
  if (!ConsoleReady()) {
    return;
  }
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
