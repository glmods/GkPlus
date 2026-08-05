#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "WindowPlacement.h"

#include "Core.h"

#include <string>

namespace gk {
namespace {

using CreateWindowExAFn = HWND(WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                         int, int, HWND, HMENU, HINSTANCE,
                                         LPVOID);

// gl.exe's IAT slot for user32!CreateWindowExA (file_io_notes.md's slot map is
// the kernel32 half of the same table; the user32 range runs 0x0064d234-0x0064d2b8).
constexpr uintptr_t CreateWindowExASlot = 0x0064d294;

CreateWindowExAFn OriginalCreateWindowExA = nullptr;
void **Slot = nullptr;
void *SlotOriginal = nullptr;
bool Enabled = true;

// Which of WinMain's two creations this is. The windowed one is the captioned,
// non-topmost one; the fullscreen site is WS_EX_TOPMOST over a bare WS_POPUP and
// is deliberately left at the screen origin, where it belongs.
//
// The X/Y test is not redundant with those: it is what makes this a correction to
// a *hardcoded zero* rather than a policy applied to any position, so if some
// later build (or another mod) ever passes a real position, it survives.
bool IsStockWindowedMainWindow(DWORD ex_style, DWORD style, int x, int y) {
  return (style & WS_CAPTION) == WS_CAPTION && (ex_style & WS_EX_TOPMOST) == 0 &&
         x == 0 && y == 0;
}

// The work area of the monitor the game asked to appear on - i.e. the screen
// minus whatever edges the taskbar and any other appbars have claimed. On a
// left-docked taskbar rcWork.left is the taskbar's width; on a top-docked one
// rcWork.top is its height; on the usual bottom-docked setup both are zero and
// this is a no-op, which is why it needs no configuration.
//
// No clamping against the far edges: the position starts at the work area's own
// origin, so a window too large to fit would only be pushed back to exactly where
// it already is. Oversized stays oversized, as it was before.
bool WorkAreaOrigin(int x, int y, int *out_x, int *out_y) {
  const POINT requested = {x, y};
  HMONITOR monitor = ::MonitorFromPoint(requested, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (!monitor || !::GetMonitorInfoA(monitor, &info)) {
    return false;
  }
  *out_x = info.rcWork.left;
  *out_y = info.rcWork.top;
  return true;
}

HWND WINAPI HookedCreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                  LPCSTR window_name, DWORD style, int x, int y,
                                  int width, int height, HWND parent,
                                  HMENU menu, HINSTANCE instance,
                                  LPVOID param) {
  if (Enabled && IsStockWindowedMainWindow(ex_style, style, x, y)) {
    int placed_x = 0;
    int placed_y = 0;
    if (WorkAreaOrigin(x, y, &placed_x, &placed_y) &&
        (placed_x != x || placed_y != y)) {
      DebugWrite("gkplus: placing the game window at {},{} (work area) instead "
                 "of {},{}\n",
                 placed_x, placed_y, x, y);
      x = placed_x;
      y = placed_y;
    }
  }
  return OriginalCreateWindowExA(ex_style, class_name, window_name, style, x, y,
                                 width, height, parent, menu, instance, param);
}

// GKPLUS_WINDOW_PLACEMENT = work-area (default) | raw.
void ReadPlacementMode() {
  char value[32] = {};
  const DWORD len = ::GetEnvironmentVariableA("GKPLUS_WINDOW_PLACEMENT", value,
                                              sizeof(value));
  const std::string mode(value, len);
  if (mode.empty() || mode == "work-area") {
    return;
  }
  Enabled = false;
  if (mode != "raw") {
    DebugWrite("gkplus: unknown GKPLUS_WINDOW_PLACEMENT '" + mode +
               "'; leaving the window where the game puts it\n");
  }
}

// The same guarded write FileHookSystem uses, for the same reason: checking that
// the slot still holds the export it should stops a mistyped offset from
// overwriting an unrelated .rdata pointer, which would present as a crash far
// from here. A mismatch means either that or another patcher got there first;
// neither is worth guessing about, so the slot is left alone.
bool Patch() {
  void **slot = reinterpret_cast<void **>(GetBaseAddress() + CreateWindowExASlot);

  HMODULE user32 = ::GetModuleHandleA("user32.dll");
  void *expected =
      user32 ? reinterpret_cast<void *>(
                   ::GetProcAddress(user32, "CreateWindowExA"))
             : nullptr;
  if (!expected || *slot != expected) {
    DebugWrite("gkplus: import slot {:#x} does not hold user32!CreateWindowExA; "
               "leaving the window placement alone\n",
               CreateWindowExASlot);
    return false;
  }

  DWORD protect = 0;
  if (!::VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protect)) {
    DebugWrite("gkplus: cannot unprotect import slot {:#x} for "
               "CreateWindowExA\n",
               CreateWindowExASlot);
    return false;
  }
  SlotOriginal = *slot;
  OriginalCreateWindowExA = reinterpret_cast<CreateWindowExAFn>(SlotOriginal);
  *slot = reinterpret_cast<void *>(&HookedCreateWindowExA);
  ::VirtualProtect(slot, sizeof(void *), protect, &protect);
  Slot = slot;
  return true;
}

} // namespace

WindowPlacementSystem::WindowPlacementSystem() {
  ReadPlacementMode();
  if (!Enabled) {
    return;
  }
  Patch();
}

WindowPlacementSystem::~WindowPlacementSystem() {
  if (!Slot) {
    return;
  }
  DWORD protect = 0;
  if (::VirtualProtect(Slot, sizeof(void *), PAGE_READWRITE, &protect)) {
    *Slot = SlotOriginal;
    ::VirtualProtect(Slot, sizeof(void *), protect, &protect);
  }
  Slot = nullptr;
}

} // namespace gk
