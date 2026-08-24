# Same ABI as the stock `x86-windows-static-md` triplet this project builds with on Windows
# (dynamic CRT, static libs) - just built from a non-Windows host via cmake/xwin-toolchain.cmake
# instead of a real Visual Studio install. vcpkg ships x86-windows-static-md itself, but that
# triplet leaves VCPKG_CMAKE_SYSTEM_NAME unset, which is fine when vcpkg's own host is Windows
# but would make it try to target the *host* OS here; and it has no chainload toolchain, which
# is the whole point of this file.
#
# This lives outside vcpkg/ because that directory is gitignored (bootstrapped fresh, not
# tracked) - pass it in with --overlay-triplets=cmake/vcpkg-triplets or VCPKG_OVERLAY_TRIPLETS.
#
# Requires XWIN_ROOT (env var or -DXWIN_ROOT=) pointing at an `xwin ... splat` output - see
# cmake/xwin-toolchain.cmake for how to produce one.

set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME "")
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../xwin-toolchain.cmake")

# vcpkg only forwards a listed env var into its own build environment rather than the host's
# whole one; harmless on a Linux host, where vcpkg does not sandbox the environment to begin
# with, but keeps this triplet correct if it is ever driven from a Windows host instead.
set(VCPKG_ENV_PASSTHROUGH XWIN_ROOT)
