#include "LoadScreen.h"

#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include <cstdlib>

namespace gk {
namespace loadscreen {
namespace {

// __fastcall(bool freshStart) - see the header. Returns void; the flag is the only
// argument and it arrives in CL, so there is nothing on the stack to clean up.
using LoadLevelFn = void(__fastcall *)(char);
LoadLevelFn OriginalLoadLevel = nullptr;

// LoadLevel runs on the main thread only (file_io_notes.md section 1: no level
// loading on the executor), so a plain int is the right counter. A depth rather
// than a bool because nothing forbids the game reaching it re-entrantly, and a
// bool would clear the flag on the inner return.
int Depth = 0;

// The interval between presents that are allowed through, in QPC ticks. Zero
// disables the throttle entirely.
long long IntervalTicks = 0;
long long LastPresent = 0;
long long Frequency = 0;

// Diagnostics for one load, reported when it ends. "How many presents did the bar
// actually need" is the question this whole file is an answer to.
int Shown = 0;
int Dropped = 0;

long long Now() {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

void __fastcall HookedLoadLevel(char fresh_start) {
  ++Depth;
  Shown = 0;
  Dropped = 0;
  // Not reset to Now(): the first present of a load should go through so the
  // screen appears immediately, and the previous frame's timestamp is already old
  // enough to guarantee that.
  OriginalLoadLevel(fresh_start);
  --Depth;
  if (Depth == 0 && (Shown + Dropped) != 0) {
    DebugWrite("GkPlus: load presented {} frames, dropped {}\n", Shown, Dropped);
  }
}

} // namespace

bool Loading() { return Depth > 0; }

bool SuppressPresent() {
  if (Depth <= 0 || IntervalTicks == 0) {
    return false;
  }
  return (Now() - LastPresent) < IntervalTicks;
}

void NotePresented() {
  LastPresent = Now();
  if (Depth > 0) {
    ++Shown;
  }
}

void NoteDropped() { ++Dropped; }

LoadScreenSystem::LoadScreenSystem() {
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  Frequency = frequency.QuadPart;

  // 32 ms is ~30 Hz: two vertical blanks at 60 Hz, so under FIFO the throttle and
  // the refresh do not beat against each other into an uneven bar.
  long long milliseconds = 32;
  char setting[16]{};
  if (GetEnvironmentVariableA("GKPLUS_LOAD_PRESENT_MS", setting,
                              sizeof(setting)) != 0) {
    if (setting[0] == 'r' || setting[0] == 'R') {
      milliseconds = 0;
    } else {
      milliseconds = std::atoll(setting);
      if (milliseconds < 0) {
        milliseconds = 0;
      }
    }
  }
  IntervalTicks = milliseconds * Frequency / 1000;

  GetObjectAtOffset(OriginalLoadLevel, 0x004e0980);
  // ::DetourAttach, not gk::DetourAttach - the wrappers in DetourUtils.h are for
  // member function pointers and would not match this anyway.
  if (::DetourAttach(&reinterpret_cast<PVOID &>(OriginalLoadLevel),
                     reinterpret_cast<PVOID>(HookedLoadLevel)) != NO_ERROR) {
    // Losing the throttle is acceptable; a half-installed detour is not. Null it
    // so SuppressPresent can never fire against a load it cannot see the end of.
    OriginalLoadLevel = nullptr;
    IntervalTicks = 0;
  }
}

LoadScreenSystem::~LoadScreenSystem() {
  if (OriginalLoadLevel == nullptr) {
    return;
  }
  ::DetourDetach(&reinterpret_cast<PVOID &>(OriginalLoadLevel),
                 reinterpret_cast<PVOID>(HookedLoadLevel));
}

} // namespace loadscreen
} // namespace gk
