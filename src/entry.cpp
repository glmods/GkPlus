#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include "Debug.h"
#include "GUI.h"
#include "InputFix.h"
#include "Music.h"

#include <cassert>
#include <memory>

namespace gk {
// The behavioral hooks GkPlus installs. Each subsystem attaches its detours in
// its constructor and detaches them in its destructor, so constructing this
// aggregate inside a Detours transaction installs them all, and destroying it
// removes them. The struct-only / native-API subsystems (Actors, Roles, Map,
// Menu, Console, Tokens, Triggers, Misc, Camera, ...) resolve their offsets
// lazily per call and need no member here.
struct Subsystems {
  MusicSystem music;       // MusicTrack-ctor volume fix
  DebugSystem debug;       // DebugPrint* -> OutputDebugString
  GUISystem gui;           // ImGui/D3D overlay
  InputFixSystem inputfix; // suppress the vestigial DirectInput keyboard acquire
};

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
  if (DetourIsHelperProcess()) {
    return TRUE;
  }
  static std::unique_ptr<Subsystems> subsystems;

  if (reason == DLL_PROCESS_ATTACH) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    subsystems = std::make_unique<Subsystems>();
    assert(DetourTransactionCommit() == NO_ERROR);

  } else if (reason == DLL_PROCESS_DETACH) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    subsystems = nullptr;
    assert(DetourTransactionCommit() == NO_ERROR);
  }

  return TRUE;
}
} // namespace gk
