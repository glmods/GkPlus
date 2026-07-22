#pragma once

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

namespace gk {
template <typename R, typename T, typename... Args>
LONG DetourAttach(R (T::**func)(Args...), R (T::*detour)(Args...)) {
  void **ppPointer;
  void *pDetour;
  std::memcpy(&ppPointer, &func, sizeof(ppPointer));
  std::memcpy(&pDetour, &detour, sizeof(pDetour));

  return ::DetourAttach(ppPointer, pDetour);
}

template <typename R, typename T, typename... Args>
LONG DetourDetach(R (T::**func)(Args...), R (T::*detour)(Args...)) {
  void **ppPointer;
  void *pDetour;
  std::memcpy(&ppPointer, &func, sizeof(ppPointer));
  std::memcpy(&pDetour, &detour, sizeof(pDetour));

  return ::DetourDetach(ppPointer, pDetour);
}
} // namespace gk