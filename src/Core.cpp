#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>

#include "Core.h"
namespace gk {
namespace {
constexpr uintptr_t EntryPointOffset = 0x005e50c8;

static uintptr_t ComputeBaseAddress() {
  HANDLE process = GetCurrentProcess();
  HMODULE module = GetModuleHandle(nullptr);
  MODULEINFO info;
  GetModuleInformation(process, module, &info, sizeof(info));

  return (uintptr_t)info.EntryPoint - EntryPointOffset;
}
} // namespace

uintptr_t GetBaseAddress() {
  static uintptr_t base_address = ComputeBaseAddress();
  return base_address;
}

void DebugWrite(const std::string &str) { OutputDebugString(str.c_str()); }
} // namespace gk