#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

#include "Core.h"
#include "CustomLevel.h"
#include "CustomMenu.h"
#include "D3D8Capture.h"
#include "Debug.h"
#include "FileHooks.h"
#include "Font.h"
#include "GLS.h"
#include "GUI.h"
#include "InputFix.h"
#include "MapLights.h"
#include "Music.h"
#include "Script.h"
#include "ScriptQueue.h"
#include "WindowPlacement.h"

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
  // Also patches an import gl.exe uses during WinMain, but unlike FileHookSystem
  // it needs no particular position here: everything in this aggregate is built
  // from DllMain, which runs before WinMain does.
  WindowPlacementSystem window; // keep the game window clear of the taskbar
  // After D3D8CaptureSystem: the stamp it draws names the renderer, and which one
  // that is only becomes known when the capture layer resolves Direct3DCreate8.
  // Reading it is deferred to the first draw, so this is ordering for clarity
  // rather than correctness - but it is the order the dependency actually runs in.
  VersionTextSystem version; // "GkPlus - <renderer>" instead of "v1.3 DX8"
  CustomMenuSystem menus;   // front-end menu items owned by GkPlus
  ScriptQueueSystem queue;  // the script queue carries JSON, not bare .gcs names
  gls::GlsSystem gls;       // lets the GLS parser take a source text, not a file
  CustomLevelSystem levels; // levels built from script instead of .gls + .gcs
  // Hooks LoadOrGetRifFile, which is where the level's rif path and unit scale can be caught -
  // both are gone by the time the level is playable, since LoadLevel frees the rif object right
  // after ConvertParsedObjects. Position does not matter: it only records, and its first read is
  // long after every hook here is installed. See src/MapLights.h.
  MapLightSystem map_lights; // the .rif's own lights, which the engine loads and never reads
  // Detours SetupMenus purely to reach a point where the game's allocator exists:
  // RegisterImageCodec builds its trie with pool_alloc, which bottoms out in gl.exe's
  // static CRT heap, and that is not initialised until _mainCRTStartup - long after
  // DllMain. Every texture loaded before this point is a hardcoded .RIM literal, so
  // nothing is missed.
  // The DDS codec has no member here and installs no detour of its own: it registers
  // from FileHookSystem's first intercepted open, which is the only anchor that is both
  // past gl.exe's CRT init and ahead of every image dispatch. See src/ImageCodec.h.
  ScriptSystem script;      // QuickJS host; runs gkplus/main.mjs
};

// Commits the open Detours transaction, and says so when it fails.
//
// This is a function rather than the `assert(DetourTransactionCommit() == NO_ERROR)` it replaces
// because that call is the whole point of the statement, and `assert` **discards its argument**
// under NDEBUG - which every optimized configuration defines. The transaction was therefore begun
// and never committed in RelWithDebInfo and Release: every subsystem's `DetourAttach` queued
// normally, `Subsystems` constructed without complaint, and then not one hook was installed. The
// symptom is not a crash but stock Gunlok with our DLL sitting loaded in the process - no version
// stamp, no REPL listener, no file hooks, no D3D8 capture - which reads as "the build is broken"
// rather than as a one-line defect. It is why only a Debug DLL was ever deployed.
//
// Nothing here may throw or fault: this runs under the loader lock, and on detach a failure to
// unpatch leaves gl.exe running with detours into a DLL that is about to be unmapped.
void Commit(const char *phase) {
  const LONG error = DetourTransactionCommit();
  if (error != NO_ERROR) {
    DebugWrite("gkplus: Detours {} transaction failed to commit: {}\n", phase, error);
  }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
  if (DetourIsHelperProcess()) {
    return TRUE;
  }
  static std::unique_ptr<Subsystems> subsystems;

  if (reason == DLL_PROCESS_ATTACH) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    subsystems = std::make_unique<Subsystems>();
    Commit("attach");

  } else if (reason == DLL_PROCESS_DETACH) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    subsystems = nullptr;
    Commit("detach");
  }

  return TRUE;
}
} // namespace gk
