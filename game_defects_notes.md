# Game defects (Gunlok's own, not GkPlus's)

Bugs that live in `gl.exe` and reproduce without GkPlus. Recorded here so a later
session can decide whether the mod should paper over them, and so nobody spends a
second evening blaming our hooks for them.

---

## 1. `DrawText?` @ 0x005782e0 smashes its stack on any string over ~1024 chars

**Status:** open, unpatched. Reproduces reliably. Blocks the training-level debrief.

**Severity:** fatal — overwrites the return address, so it is an arbitrary stack
smash, not a graceful failure.

### Mechanism

`DrawText?` is `__thiscall` with the text as its second stack argument
(`Stack[0x8]`, i.e. `[EBP+0xc]`; the first, `[EBP+8]`, is a `float *`). Its
prologue is `SUB ESP,0x478` and it copies the caller's string into a **1028-byte
buffer at `EBP-0x404`** with **no bound**. It then inline-`strlen`s that buffer
into EBX (0x00578324..0x0057833b) and loops `[0, EBX)` replacing glyph-less
characters with `'?'` (`MOV byte ptr [EBP+EDI*1-0x404],0x3f` @ 0x0057838e).

1028 bytes below EBP means the copy runs into the saved EBP at `+0`, the return
address at `+4`, and the arguments at `+8` once the string passes 1028/1032/1036
characters.

The observed crash is the *next* instruction reloading the now-clobbered argument:

```
005783a1  MOV   EAX, dword ptr [EBP + 0x8]      ; arg 1, overwritten by the text
005783a4  MOVSS XMM0, dword ptr [EAX]           ; <-- AV, eax=0x7375202c
```

`0x7375202c` is ASCII `", us"` — a fragment of the text itself. Note the `'?'`
loop is *incidental*: it only stores for characters with no glyph, which is why
the frame holds readable text rather than `0x3f3f3f3f`.

### Reproduction

Complete the Training Level and enter the debrief. The training-completion text is
**1925 characters** (EBX = 0x785), 897 past the buffer:

> "Well done. You have now completed your training. Now here's some advice for the
> real thing.....Focus on your mission...You are a small group against a huge enemy
> force. …"

### Why it is not GkPlus

Established under cdb (`sxe av`) with the envelope build deployed:

- the buffer holds the game's own debrief prose, complete and unmangled — no JSON,
  no envelope, no truncation artifacts;
- `ConsoleCommandLine` @ 0x007b6958 is **empty** at the fault, so no queued console
  command was mid-dispatch — the fault is in the debrief screen's ordinary
  per-frame render of `BriefingTextList` @ 0x007b68bc;
- the smashed frame contains the text (`ret = 0x73656e69` `"ines"`,
  `arg = 0x7375202c` `", us"`), so the *copy* overflowed. Nothing GkPlus does
  bounds or supplies that string.

The arithmetic is build-independent: 1925 characters into 1028 bytes overflows in
vanilla Gunlok too.

### If we decide to fix it

Hook `DrawText?` and clamp the text before calling the original — the bound is
`0x404` minus the NUL, and the string is just `Stack[0x8]`. A wrapper that copies
at most 1027 chars into its own buffer and forwards that is ~15 lines and cannot
regress anything, since longer strings currently corrupt the stack rather than
rendering. Wrapping onto multiple lines would be nicer but needs the font metrics
the function already computes.

Deliberately **not** done yet: it is outside the change that found it, and the mod
has no other "fix the game's bugs" hooks to be consistent with.

### Ghidra

`DrawText?` @ 0x005782e0 has a plate comment recording all of the above. The name
still carries a `?` — the body was never fully read, only the buffer handling.

---

## 2. `PrintParseWarning` / `PrintParseError` discard their output (not a bug, a trap)

`PrintParseWarning` @ 0x00477050 and `PrintParseError` @ 0x00477000 `vsprintf` into
a 2048-byte **local** and throw it away, incrementing counters at 0x00739a3c /
0x00739a38. The output call was compiled out of the shipped build.

So GkPlus's `DebugSystem` hook is the only thing that ever made those messages
visible — and unhooking one routes nothing to the console, it just makes it silent
again. Worth knowing before anyone "restores" them: the GLS parser calls the
warning variant **once per unset field per section**, 13,000+ for one Training
Level load, and redirecting those to `OutputDebugString` makes the game unplayable
under any debugger (each call is a synchronous round-trip). That is why
`RedirectWarnings` is false in `src/Debug.cpp`.

---

## 3. `GetResourceString` @ 0x00579000 walks its table with no terminator

The localized string lookup is a five-instruction linear scan with **no end check**:

```
00579000  MOV EAX,dword ptr [ECX]        ; the table head
00579002  CMP dword ptr [EAX],EDX        ; is this the id?
00579004  JZ  0x0057900d
00579006  ADD EAX,0x14                   ; next 0x14-byte entry
00579009  CMP dword ptr [EAX],EDX        ; <-- faults here
0057900b  JNZ 0x00579006                 ; ... forever
0057900d  MOV ECX,dword ptr [EAX + 0x4]
00579010  TEST ECX,ECX
00579012  MOV EAX,0x7c14b4               ; a default string when the entry is null
00579017  CMOVNZ EAX,ECX
```

Ask for an id the table does not hold and the loop runs off the end of the resource
image and dereferences unmapped memory: **`0xc0000005` at fault offset `0x00179009`**
(RVA; the address is 0x00579009, and both the faulting module and the application are
`gl.exe`). Note the function *does* handle a **null** entry — that is what the
0x7c14b4 default is for — so the missing case it does not handle is a **absent** id,
not an empty one.

Observed twice while testing `.RIM` writing, on exit both times, in two separate
runs. It is not GkPlus's: one of the two runs had served no modded file at all, and
the reproduction is a stock code path with ~400 call sites (`SetupMenus`,
`RegisterAllKeyBindings`, `OnMenuItemClicked`, `LoadLevel`, most console commands).
It has not been narrowed to a specific caller or string id — the evidence is the
fault address plus the fact that it fired with and without mods present.

Practical consequence: **a Gunlok crash whose faulting offset is `0x00179009` is a
missing localized string, not whatever you were testing.** Worth checking first,
because it is reached from almost everywhere and it presents as a plain access
violation in `gl.exe` with no other clue. Likely more reachable on a non-English
install, where `glres<lang>.dll` may hold a different set of ids (see the localized
command-name hazard in `console_command_notes.md`).

---

## 4. The process faults on exit via the console `QUIT`

**Status:** open, undiagnosed. Reproduces every time. Harmless to the player — the
process is leaving anyway — but it means **process-exit cleanup cannot be relied on**.

Sending `QUIT` from the console (or `console.execute("QUIT")` from the REPL) exits
the process *and* leaves a WER minidump in `%LOCALAPPDATA%\CrashDumps`. Observed on
four consecutive runs; a `TerminateProcess` (Task Manager, `Stop-Process`) leaves no
dump, so it is the ordinary exit path that faults, not the kill.

**It is not GkPlus's mod loader**, and that is an A/B measurement rather than an
assumption: the build from before `FileHookSystem` existed produces a dump on the
same `QUIT` (`gl.exe.35284.dmp`). Whether it is *some other* part of GkPlus or
vanilla Gunlok has not been established — testing that needs a run with the mod
removed entirely, which nobody has done.

### The consequence that matters

Anything that tidies up in `DllMain(DLL_PROCESS_DETACH)` is best-effort only, and
that is compounded by `entry.cpp` destroying the `Subsystems` aggregate in reverse
declaration order — a fault in an earlier destructor takes out every later one.
`vfs::Shutdown()` removing its `%TEMP%\gkplus-vfs-<pid>` tree was written that way
and never ran; the working mechanism is the startup sweep in `Vfs.cpp`, which
removes any such directory whose pid is no longer alive. Prefer that shape — *clean
up other people's leftovers on the way in* — over trusting the way out.

### Where to start if someone picks this up

WER already wrote the dump; `cdb -z <dump> -c ".ecxr; k 40; q"` plus
`llvm-symbolizer` on the `d3d8+0x...` RVAs is the recipe below. The dumps from the
session that found this are gone (WER keeps a bounded ring), so reproduce first.

---

## Debugging Gunlok: what actually works

- **cdb is at** `C:\Program Files\WindowsApps\Microsoft.WinDbg_*\x86\cdb.exe`. It
  cannot be executed in place (WindowsApps denies execute) but **can be read and
  copied out** — copy `cdb.exe`, `dbgeng.dll`, `dbghelp.dll`, `dbgcore.dll`,
  `dbgmodel.dll`, `symsrv.dll`, `srcsrv.dll` plus the `winext\` and `triage\`
  folders to a writable directory and run it from there.
- **`cppvsdbg` (VS Code) uses `vsdbg.exe`** from the cpptools extension. It speaks
  DAP on stdio and the handshake works, but it enforces a licence check restricting
  it to VS Code / Visual Studio as the host and aborts the session for anything
  else. Usable by pressing F5; not scriptable.
- **`WinDbgX.exe`'s command-line launch does not work** in this environment (it
  fails on `notepad.exe` too, so it is not Gunlok-specific).
- **`bp d3d8+0x...` silently resolves wrong**: cdb parses `d3d8` as the hex literal
  `0xD3D8`, because all four characters are hex digits. Use `module!symbol` form,
  or breakpoint the *caller* in `gl.exe` — `Gl` parses fine as a module name.
- **cdb will not load our clang-built PDB** (`bm d3d8!*Foo*` reports "No matching
  code symbols found") even with `-y` pointing at `build/Debug` and `.reload /f`.
  `llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address <rva>` resolves
  our frames perfectly, so symbolize offsets that way instead.
- **Do not breakpoint `Gl+0x6e498`** (`RunGameFrame`'s pump call) during play. It
  is gated on `NumCommandsToExecute != 0`, which *stays* non-zero for the whole
  duration of a pending `WAIT`, so it fires every frame and makes the game
  unplayable.
- **cdb echoes its entire `-c` command list on startup.** Any `.echo MARKER` you
  add appears twice: once in that echo and once as real output. Anchor greps with
  `^MARKER$` or you will parse the echo and conclude a fault occurred on every
  clean run.
- For crash-detection soaks, prefer running **without** a debugger and reading
  WER's Application Error log plus the exit code; a clean exit writes
  `scripts\GLkeys.cfg`. Undebugged the game loads a level and exits in ~22s; under
  cdb with the warning redirect on it could not finish the parse in 40s.
- **WER already writes full dumps, and `cdb -z` on one beats every live attach.**
  Crashes land in `%LOCALAPPDATA%\CrashDumps\gl.exe.<pid>.dmp` (~6 MB) with no
  configuration needed. `cdb -z <dump> -c ".ecxr; k 40; q"` gives a complete stack;
  **attaching to the live process does not** - `-pv` (non-invasive) reports "Could
  not fetch any stack frames" for the faulting thread, and an invasive `-p` attach
  breaks in on its own thread, reports "Unable to get initial context information"
  for `k`, and killed the target on `qd` twice out of two. Symbolize our frames from
  the RVAs the stack prints (`d3d8+0x3c25d`) with
  `llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address 0x3c25d`. That took
  a `make.role` fault from "the game stopped responding" to
  `MakeRoleJs -> MakeRole (MakeRole.cpp:447) -> HierarchyResolveNamedPointPos ->
  ___ascii_stricmp` in one command.
- **"Not responding" is not evidence of a spin, and neither is high CPU.** Gunlok
  burns a full core at the front end normally, so a climbing `Process.CPU` says
  nothing. The fault above presented as a hang - `Responding: False`, CPU rising,
  main thread parked in `ZwWaitForMultipleObjects` - and was an access violation
  all along, with that wait being WER's own error reporting. Read the Application
  Error log before concluding anything about a wedged process.
- **The REPL is a debugger you already have.** Most of this session's findings came
  from `GKPLUS_REPL_PORT=9222` plus a 30-line TCP client, not from cdb: it reads
  live game state (`roles.count`, `[...roles].map(r => r.name)`), and a binding that
  crashes the game names itself, because the snippet that stopped answering is the
  one that did it. Note the failure mode, though - **a crash looks like a socket
  timeout**, so check the process afterwards rather than trusting the timeout.
