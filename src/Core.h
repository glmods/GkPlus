#pragma once
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

namespace gk {
template <typename TRet = void, typename... TArgs>
using CDecl = TRet(__cdecl *)(TArgs...);

template <typename TRet = void, typename... TArgs>
using CDeclVarargs = TRet(__cdecl *)(TArgs..., ...);

template <typename TRet = void, typename... TArgs>
using StdCall = TRet(__stdcall *)(TArgs...);

template <typename TRet = void, typename... TArgs>
using FastCall = TRet(__fastcall *)(TArgs...);

template <typename TRet = void, typename... TArgs>
using ThisCall = TRet(__thiscall *)(TArgs...);

uintptr_t GetBaseAddress();

template <typename T> void GetObjectAtOffset(T &obj, uintptr_t offset) {
  obj = reinterpret_cast<T>(GetBaseAddress() + offset);
}

template <typename R, typename T, typename... Args>
void GetObjectAtOffset(R (T::*&func)(Args...), uintptr_t offset) {
  void *obj = reinterpret_cast<void *>(GetBaseAddress() + offset);

  std::memcpy(&func, &obj, sizeof(func));
}

void DebugWrite(const std::string &str);

template <typename... Args>
void DebugWrite(std::format_string<Args...> fmt, Args &&...args) {
  auto str = std::format(fmt, std::forward<Args>(args)...);
  DebugWrite(str);
}
} // namespace gk