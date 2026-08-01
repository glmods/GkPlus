#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include "CustomLevel.h"
#include "CustomMenu.h"
#include "D3D8Capture.h"
#include "Debug.h"
#include "FileHooks.h"
#include "GLS.h"
#include "GUI.h"
#include "InputFix.h"
#include "Music.h"
#include "Script.h"
#include "ScriptQueue.h"

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
  // First, and deliberately: it patches gl.exe's file imports, and everything
  // the game subsequently loads - including the assets read during WinMain,
  // before any other subsystem's hook can fire - has to go through it for a mod
  // to be able to replace them.
  FileHookSystem files; // mod archives layered over the game's data tree
  // Before GUISystem: it wraps the IDirect3DDevice8 the game creates, and GUISystem's
  // GetDX9Device reads the game's global expecting whatever CreateDevice handed back.
  d3d8::D3D8CaptureSystem d3d8;
  MusicSystem music;       // MusicTrack-ctor volume fix
  DebugSystem debug;       // DebugPrint* -> OutputDebugString
  GUISystem gui;           // ImGui/D3D overlay
  InputFixSystem inputfix; // suppress the vestigial DirectInput keyboard acquire
  CustomMenuSystem menus;   // front-end menu items owned by GkPlus
  ScriptQueueSystem queue;  // the script queue carries JSON, not bare .gcs names
  gls::GlsSystem gls;       // lets the GLS parser take a source text, not a file
  CustomLevelSystem levels; // levels built from script instead of .gls + .gcs
  ScriptSystem script;      // QuickJS host; runs gkplus/main.mjs
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
