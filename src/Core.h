#pragma once
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

/// \file
/// The base of every native-API call: where gl.exe is loaded, how an address in
/// it is turned into a callable function pointer, and where diagnostics go.
///
/// Every address in this codebase is written as the *offset* it has in the
/// address map, never as an absolute, and is combined with GetBaseAddress() at
/// the point of use. A function is then declared with one of the
/// calling-convention aliases below and resolved with GetObjectAtOffset().
///
/// The convention and the argument count are both load-bearing: a wrong one
/// drifts ESP and faults somewhere unrelated, long after the call. See
/// `ghidra_analysis_notes.md` for how each is established from the binary.
namespace gk {
/// A pointer to a `__cdecl` game function. `TRet` is the return type, `void`
/// when omitted; `TArgs` are the parameter types, in order.
template <typename TRet = void, typename... TArgs>
using CDecl = TRet(__cdecl *)(TArgs...);

/// A pointer to a variadic `__cdecl` game function; `TArgs` are the fixed
/// parameters that precede the `...`.
template <typename TRet = void, typename... TArgs>
using CDeclVarargs = TRet(__cdecl *)(TArgs..., ...);

/// A pointer to a `__stdcall` game function (callee cleans the stack).
template <typename TRet = void, typename... TArgs>
using StdCall = TRet(__stdcall *)(TArgs...);

/// A pointer to a `__fastcall` game function (first two integer arguments in
/// ECX and EDX).
template <typename TRet = void, typename... TArgs>
using FastCall = TRet(__fastcall *)(TArgs...);

/// A pointer to a `__thiscall` game member function (`this` in ECX). The
/// object pointer is the first `TArgs` entry.
template <typename TRet = void, typename... TArgs>
using ThisCall = TRet(__thiscall *)(TArgs...);

/// Where gl.exe is loaded: the running process's actual entry point minus the
/// image's recorded entry-point offset (0x005e50c8), so every address in
/// `address_map.md` can be used as written.
///
/// Computed on the first call and cached, which is what makes per-call offset
/// resolution cheap enough that nothing needs to hold a resolved pointer.
///
/// \return the load address of the host executable.
///
/// It derives from the *host* executable, so this, and therefore every
/// native-API call in `src/`, is meaningless outside Gunlok. A standalone
/// process faults rather than returning something wrong.
uintptr_t GetBaseAddress();

/// Points \p obj at `GetBaseAddress() + offset`.
///
/// \param obj  a pointer or function-pointer lvalue; its type states the
///             calling convention and signature the game function has.
/// \param offset the address as it appears in `address_map.md`.
///
/// Nothing validates the offset or the signature. A resolved pointer is only
/// as correct as the address map entry behind it.
template <typename T> void GetObjectAtOffset(T &obj, uintptr_t offset) {
  obj = reinterpret_cast<T>(GetBaseAddress() + offset);
}

/// Points a pointer-to-member-function at `GetBaseAddress() + offset`.
///
/// A member function pointer is not a plain address on MSVC, so it is written
/// through `memcpy` rather than cast. Used for the `__thiscall` targets that
/// `src/DetourUtils.h` hooks.
template <typename R, typename T, typename... Args>
void GetObjectAtOffset(R (T::*&func)(Args...), uintptr_t offset) {
  void *obj = reinterpret_cast<void *>(GetBaseAddress() + offset);

  std::memcpy(&func, &obj, sizeof(func));
}

/// Writes \p str through `OutputDebugString`, exactly as given: no prefix and
/// no trailing newline are added, so a caller that wants line breaks includes
/// them.
///
/// This is the whole of GkPlus's logging: there is no log file, so a debugger
/// or DebugView is the only sink. The `d3d8.log` in the game directory belongs
/// to the vendored d3d8to9 translation layer, not to this DLL.
void DebugWrite(const std::string &str);

/// DebugWrite() over a `std::format` string. Formatting happens on the caller's
/// thread before the single `OutputDebugString` call.
template <typename... Args>
void DebugWrite(std::format_string<Args...> fmt, Args &&...args) {
  auto str = std::format(fmt, std::forward<Args>(args)...);
  DebugWrite(str);
}
} // namespace gk