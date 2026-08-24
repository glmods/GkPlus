# Cross-compiles the Windows x86 / MSVC-ABI target from a Linux (or any non-Windows) host,
# using clang-cl + lld-link against a real Microsoft CRT/Windows SDK sysroot fetched by `xwin`
# (https://github.com/Jake-Shadle/xwin). This is deliberately not a mingw-w64 build: clang-cl's
# `-fms-compatibility` mode reproduces the same struct/vtable layout and calling conventions as
# real MSVC, which is what this codebase's Actor/Map/vtable mirroring depends on (see CLAUDE.md,
# "Can we build this on Linux"). `cmake/clang-toolchain.cmake` is the sibling file for the
# Windows-hosted case, where a real Visual Studio install supplies the same headers/libs via
# vcvarsall instead of this file's explicit `-imsvc`/`-libpath:` flags.
#
# Fetch the sysroot first (x86 only - this project is 32-bit-only):
#   xwin --accept-license --arch x86 splat --output /path/to/xwin-out
# then point this toolchain at it, either via -DXWIN_ROOT=... or the XWIN_ROOT env var.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

if(NOT DEFINED XWIN_ROOT)
    if(DEFINED ENV{XWIN_ROOT})
        set(XWIN_ROOT "$ENV{XWIN_ROOT}")
    else()
        message(FATAL_ERROR
            "XWIN_ROOT is not set. Point it at the output of "
            "`xwin --accept-license --arch x86 splat --output <dir>` "
            "via -DXWIN_ROOT=<dir> or the XWIN_ROOT environment variable.")
    endif()
endif()

if(NOT EXISTS "${XWIN_ROOT}/crt/include" OR NOT EXISTS "${XWIN_ROOT}/sdk/include/um")
    message(FATAL_ERROR
        "XWIN_ROOT (${XWIN_ROOT}) doesn't look like an xwin splat output - expected "
        "${XWIN_ROOT}/crt/include and ${XWIN_ROOT}/sdk/include/um to exist.")
endif()

set(_xwin_crt_inc    "${XWIN_ROOT}/crt/include")
set(_xwin_sdk_um     "${XWIN_ROOT}/sdk/include/um")
set(_xwin_sdk_shared "${XWIN_ROOT}/sdk/include/shared")
set(_xwin_sdk_ucrt   "${XWIN_ROOT}/sdk/include/ucrt")

set(_xwin_crt_lib      "${XWIN_ROOT}/crt/lib/x86")
set(_xwin_sdk_lib_um   "${XWIN_ROOT}/sdk/lib/um/x86")
set(_xwin_sdk_lib_ucrt "${XWIN_ROOT}/sdk/lib/ucrt/x86")

# Standard CMake cross-compiling hygiene, and not optional here: without it, a dependency's
# own `find_library(M_LIB m)` (libspng's own CMakeLists.txt does exactly this, the ordinary
# portable-CMake way to ask "do I need -lm on this platform") searches the *host's* library
# paths by default even though CMAKE_SYSTEM_NAME says the target is Windows - and finds the
# host's real libm.so, not a Windows library at all, then hands lld-link a request for a
# nonexistent "m.lib". PROGRAM stays NEVER: host tools (python3, cmake helper scripts) live on
# the host, not under XWIN_ROOT, and asking to find *those* only under the sysroot would break
# every port that shells out to one during its build.
#
# APPEND, not set: vcpkg's own toolchain script has already populated CMAKE_FIND_ROOT_PATH
# with vcpkg_installed/<triplet> by the time it chainloads this file, which is how
# find_package(Vulkan) and friends find that tree's own exported *Config.cmake files -
# overwriting it broke exactly that, measured, not assumed (the `vulkan` meta-port's own
# portfile calls find_package(Vulkan) as its whole body, so it is a clean, minimal repro of
# this specific mistake).
list(APPEND CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(CMAKE_C_COMPILER NAMES clang-cl clang-cl-22 clang-cl-21 clang-cl-20 REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang-cl clang-cl-22 clang-cl-21 clang-cl-20 REQUIRED)
find_program(CMAKE_LINKER NAMES lld-link REQUIRED)
find_program(CMAKE_AR NAMES llvm-lib llvm-lib-22 llvm-lib-21 llvm-lib-20 REQUIRED)
# Only needed if a dependency embeds a .rc resource; not required to link d3d8.dll itself.
find_program(CMAKE_RC_COMPILER NAMES llvm-rc llvm-rc-22 llvm-rc-21 llvm-rc-20)

set(CMAKE_C_COMPILER_TARGET i386-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i386-pc-windows-msvc)

# `xwin` (as of 0.10.0, checked with --include-debug-runtime) does not fetch the debug CRT
# static import libs (msvcrtd.lib/ucrtd.lib/vcruntimed.lib) - only the debug runtime DLLs
# themselves. CMake's own compiler sanity check (CMakeTestCCompiler.cmake) always tries a
# `/MDd` compile regardless of the project's requested build type, so without this it fails
# before a single project file is ever touched, even for a Release-only build. Forcing the
# sanity check itself to Release sidesteps that; it says nothing about whether an actual
# Debug-config *project* build would link here, which - until a source for those three debug
# libs is found - it currently cannot.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# CMake's own "Check for working compiler" probe (CMakeTestCCompiler.cmake) links a trivial
# /MD-default EXE using nothing but the bare CMAKE_<LANG>_LINKER_FLAGS below, which (see the
# comment on _xwin_linker_flags) no longer carries vcruntime/ucrt at all - so, exactly like
# any other /MD target that doesn't ask for them itself, it would fail the same
# __acrt_initialize-is-undefined way this file's own history is full of. It is not a target
# this project controls, so it cannot be given its own target_link_options; skipping the
# probe is the only option, and real project targets never go through it anyway - they link
# the normal way, through Ninja.
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

# clang-cl finds its own resource-dir headers without help; only the CRT/SDK need pointing at
# explicitly, since there is no vcvarsall on this host to populate INCLUDE/LIB.
set(_xwin_includes
    "-imsvc${_xwin_crt_inc}"
    "-imsvc${_xwin_sdk_um}"
    "-imsvc${_xwin_sdk_shared}"
    "-imsvc${_xwin_sdk_ucrt}"
)
list(JOIN _xwin_includes " " _xwin_includes_str)

# Real cl.exe has enabled SSE2 codegen by default on x86 since VS2012, so code across the
# vcpkg dependency set (libspng's PNG defilter, confirmed) uses <emmintrin.h> intrinsics with
# no feature-detection guard of its own, assuming that default. clang-cl's own default for a
# plain i686 target does not include it, and unlike the missing vcruntime/ucrt defaultlibs,
# this cannot be pushed onto the one target that needs it - it would have to be pushed onto
# every dependency that turns out to need it, discovered one at a time. -msse2 once, here,
# matches what these dependencies already assume they can rely on.
set(CMAKE_C_FLAGS_INIT
    "-m32 -msse2 -fms-compatibility -fms-extensions ${_xwin_includes_str}")
set(CMAKE_CXX_FLAGS_INIT
    "-m32 -msse2 -fms-compatibility -fms-extensions -fdelayed-template-parsing ${_xwin_includes_str}")

# CMake's clang-cl+llvm-rc .rc handling preprocesses through the C compiler in a separate
# invocation (`cmake -E cmake_llvm_rc`) that only inherits CMAKE_RC_FLAGS, not
# CMAKE_C_FLAGS/CMAKE_CXX_FLAGS - so without this, that preprocess step gets none of the
# -imsvc paths above and any .rc that includes an SDK header (winresrc.h, in d3d8to9's case)
# fails before llvm-rc ever runs.
set(CMAKE_RC_FLAGS_INIT "${_xwin_includes_str}")

# The -libpath:s stand in for the LIB environment variable vcvarsall would set, and are safe
# to hand to every target unconditionally: they are search paths, not a request to actually
# link anything, and both the dynamic and static CRT/SDK libs live under them.
#
# vcruntime.lib/ucrt.lib are deliberately *not* added here (nor is delayimp.lib, which GkPlus
# needs for its Vulkan /DELAYLOAD and asks for itself the ordinary way, via
# target_link_libraries - confirmed that alone is enough, with no toolchain-level default,
# by linking a real delay-loaded MODULE and checking its Delay Import Directory came out
# non-empty). A /MD (dynamic CRT) target does need vcruntime/ucrt (unlike msvcrt/oldnames/
# uuid/msvcprt, no compiled object's own .drectve section requests them, confirmed linking an
# EXE/MODULE/static-lib through this toolchain), but CMAKE_<TYPE>_LINKER_FLAGS(_INIT) does not
# evaluate generator expressions -
# confirmed the hard way, twice: not just in CMake's own internal compiler-check probe, but
# in two genuine Ninja-built targets, one /MD and one explicitly /MT, both fed the literal
# unevaluated `$<...>` text. So there is no way to add them here *only* for a /MD target.
# Adding them unconditionally instead breaks the other direction: vcpkg's vulkan-loader port
# forces static CRT linkage on itself (matching the Vulkan SDK's own shipped loader), already
# gets libvcruntime/libucrt correctly from its own .drectve, and adding the dynamic pair on
# top turned every one of their shared symbols into an lld-link "duplicate symbol" error -
# also measured, not guessed, building vulkan-loader through vcpkg.
#
# A /MD target therefore has to ask for vcruntime/ucrt itself, the same way any MSVC project
# would ask for a library its headers don't already pragma-comment in - GkPlus's own
# CMakeLists.txt does this on the `GkPlus` target alongside its other target_link_options.
# Once they're present, the CRT-startup weak-external symbols (__acrt_initialize and friends)
# resolve to their real "___scrt_stub_for_*" fallbacks on their own - an earlier version of
# this file added explicit -alternatename: entries for those, which turned out both
# unnecessary and actively wrong (lld-link rejects them as conflicting with a default already
# embedded in vcruntime.lib/ucrt.lib's own object members).
set(_xwin_linker_flags
    "-libpath:${_xwin_crt_lib}"
    "-libpath:${_xwin_sdk_lib_um}"
    "-libpath:${_xwin_sdk_lib_ucrt}"
)
list(JOIN _xwin_linker_flags " " _xwin_linker_flags_str)

set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_linker_flags_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_linker_flags_str}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_linker_flags_str}")
