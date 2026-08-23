cmake_minimum_required(VERSION 3.24)

project(GkPlus
    VERSION 1.0
    LANGUAGES C CXX)

option(GKPLUS_COPY_DLL "Copy the generated DLL to Gunlok's directory using the `copy` target" ON)
option(GKPLUS_RENDERDOC_INTEGRATION "Enabled the RenderDoc integration" ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Only 32bits builds are supported")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Only Windows builds are supported")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(imgui CONFIG REQUIRED)

find_package(detours MODULE REQUIRED)

find_package(d3d8to9 MODULE REQUIRED)

find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# Vulkan::Loader is vcpkg's 32-bit import library for vulkan-1.dll - the one the Vulkan SDK
# does not ship (it has no Lib32). Linking it is what lets both this codebase and vcpkg's
# prebuilt imgui Vulkan backend call vk* directly instead of through loaded pointers.
#
# It is DELAY-loaded, see target_link_options below: the import library is a build-time
# convenience, not a promise that the machine has Vulkan.
find_package(VulkanLoader CONFIG REQUIRED)

find_package(qjs CONFIG REQUIRED)

find_package(PhysFS CONFIG REQUIRED)

add_subdirectory(imgui-quickjs)

add_library(GkPlus MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Actors.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Console.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CustomLevel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CustomMenu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/D3D8Capture.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/D3D8CaptureReport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Dds.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Debug.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Encoding.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/entry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FileHooks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Font.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GLS.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GUI.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/HudFix.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageCodec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ActorArg.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSettings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/InputFix.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsActors.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsCamera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsCommands.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsCommon.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsConsole.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsGame.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsGk.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsGls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsLevels.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsMake.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsMenus.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsMods.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsProf.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsRender.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsRepl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsRoles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsSettings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsText.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsTokens.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsTriggers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsWorld.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Json.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/LoadScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MakeRole.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Map.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MapLights.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Math.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Memory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Menu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Misc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Music.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Render.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/RenderMenu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Repl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Rif.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Roles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Script.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptQueue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Settings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Tokens.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Triggers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VertexFormat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkContext.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkDraw.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkLighting.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkResources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Vfs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WindowPlacement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/World.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.def")
target_link_libraries(GkPlus PRIVATE huffman detours imgui::imgui d3d8to9 d3d9 qjs imgui-quickjs ws2_32 GPUOpen::VulkanMemoryAllocator Vulkan::Headers Vulkan::Loader delayimp
    $<IF:$<TARGET_EXISTS:PhysFS::PhysFS>,PhysFS::PhysFS,PhysFS::PhysFS-static>)

# GkPlus *is* `d3d8.dll`, so a load-time dependency on vulkan-1.dll would stop Gunlok launching
# at all on a machine with no Vulkan - and the Vulkan renderer is opt-in
# (`GKPLUS_RENDERER=vulkan`), so that would be a hard requirement for a path most runs never
# take. Delay-loading keeps the import table clean of it until the first vk* call, which is
# inside vulkan::Initialize() and only after its LoadLibrary probe has found the loader
# (src/VkContext.cpp). `delayimp` above is the helper the stubs jump through.
target_link_options(GkPlus PRIVATE "LINKER:/DELAYLOAD:vulkan-1.dll")

if(GKPLUS_RENDERDOC_INTEGRATION)
    find_package(RenderDoc REQUIRED)
    target_link_libraries(GkPlus PRIVATE RenderDoc::Header)

    target_sources(GkPlus PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/VkCapture.cpp")
else()
    target_sources(GkPlus PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/VkCapture_dummy.cpp")
endif()

# Needed for VK_KHR_win32_surface and vkCreateWin32SurfaceKHR. On the target rather
# than per-TU so every Vulkan source agrees - a TU that forgot it would still compile
# and then miss the surface entry points at link time.
target_compile_definitions(GkPlus PRIVATE VK_USE_PLATFORM_WIN32_KHR)
set_target_properties(GkPlus
    PROPERTIES
    OUTPUT_NAME "d3d8"
    CXX_STANDARD 23)

# The only file the NSIS installer (cmake/Packaging.cmake) packages - straight into the install
# root, since that root is made to be Gunlok's own directory rather than one of GkPlus's own.
install(TARGETS GkPlus LIBRARY DESTINATION . RUNTIME DESTINATION .)

# The two generated headers are **build products in the binary tree**, not checked-in sources:
# `${CMAKE_CURRENT_BINARY_DIR}/generated`, on GkPlus's include path and nowhere else.
#
# They used to live in `src/` and be committed, so that d3d8.dll built on a machine with no
# shader toolchain - the same reasoning that makes the renderer delay-load `vulkan-1.dll`. That
# is the property this trades away: **slangc and Python are now hard requirements**, and the two
# branches below say so with FATAL_ERROR rather than letting the compiler report a missing
# header. What it buys is that a generated file can no longer be committed stale, reviewed as if
# it were source, or conflict on a merge - the failure mode that produced the byte-identical
# screenshots in vulkan_renderer_notes.md 4.46 was exactly a stale one of these being believed.
#
# The staleness machinery below is kept even so, because it guards a second thing the move does
# not fix: within one build tree, Ninja still has to be told the header is an output (see the
# comment on the custom command) or it compiles against the previous SPIR-V.
#
# `--deps` keeps `ENTRY_POINTS` in gen-shaders.py the single source of truth for which files that
# depends on - nothing here names a shader, so the two lists cannot drift.
set(GKPLUS_SHADER_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/src/gen-shaders.py")

# Only generated headers go in here. It is on the include path, so anything else dropped in it
# would be reachable as <name> from every Vulkan TU - and the reason `src/` itself is kept OFF
# the include path (quickjs.h's `#include <math.h>` finding `src/Math.h` on a case-insensitive
# filesystem) applies just as much to this directory.
set(GKPLUS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GKPLUS_GENERATED_DIR}")
target_include_directories(GkPlus PRIVATE "${GKPLUS_GENERATED_DIR}")

find_package(Python3 COMPONENTS Interpreter QUIET)

# Mirrors find_slangc() in the generator - PATH first, then the newest installed SDK - so CMake
# and the script cannot pick different compilers. REVERSE because file(GLOB) sorts ascending and
# find_program takes the first hint that matches, where the script takes sorted()[-1].
file(GLOB GKPLUS_VULKAN_SDK_BIN "C:/VulkanSDK/*/Bin")
list(REVERSE GKPLUS_VULKAN_SDK_BIN)
find_program(GKPLUS_SLANGC NAMES slangc HINTS ${GKPLUS_VULKAN_SDK_BIN})

if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR
        "Python 3 not found, and both generated shader headers are produced by Python scripts "
        "into the build tree rather than being checked in. Install Python 3.")
elseif(GKPLUS_SLANGC)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${GKPLUS_SHADER_GENERATOR}" --deps
        OUTPUT_VARIABLE GKPLUS_SHADER_SOURCES
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" GKPLUS_SHADER_SOURCES "${GKPLUS_SHADER_SOURCES}")
    message(STATUS "Slang shaders: ${GKPLUS_SLANGC}")

    # BOTH the header and a stamp are outputs, and each does a different job. Getting this wrong
    # is silent in exactly the way this whole block exists to prevent, so:
    #
    #   - the **header** must be an output or Ninja treats it as a plain source, decides every
    #     TU including it is clean *before* the generator runs, and links a DLL built against the
    #     previous SPIR-V. Measured: with the stamp alone, editing a shader recompiled nothing
    #     and produced a byte-identical d3d8.dll - §4.46's failure mode with extra steps.
    #   - the **stamp** is always touched, so the edge is clean next build even when the header
    #     was not rewritten; without it the edge re-runs slangc on every single build.
    #
    # The generator rewrites the header only when its contents change, and CMake sets
    # `restat = 1` on custom-command edges - so an unchanged shader leaves the header's mtime
    # alone and Ninja keeps every dependent TU out of the rebuild.
    add_custom_command(
        OUTPUT "${GKPLUS_GENERATED_DIR}/Shaders.gen.inc.h"
               "${CMAKE_CURRENT_BINARY_DIR}/shaders.stamp"
        COMMAND "${Python3_EXECUTABLE}" "${GKPLUS_SHADER_GENERATOR}"
                --output "${GKPLUS_GENERATED_DIR}/Shaders.gen.inc.h"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/shaders.stamp"
        DEPENDS ${GKPLUS_SHADER_SOURCES} "${GKPLUS_SHADER_GENERATOR}"
        COMMENT "Compiling Slang shaders"
        VERBATIM)
    add_custom_target(gkplus_shaders DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/shaders.stamp")
    add_dependencies(GkPlus gkplus_shaders)
else()
    # No fallback left to take: with the SPIR-V generated into the build tree there is no
    # checked-in header to compile against, so this cannot degrade to a staleness check the way
    # it did when `src/Shaders.gen.inc.h` was committed.
    message(FATAL_ERROR
        "slangc not found, and the SPIR-V header is generated into the build tree rather than "
        "checked in. Install the Vulkan SDK or put slangc on PATH.")
endif()

# The shader ABI: one `offsetof` per field of every struct declared on both sides of the boundary,
# derived from the SHADER's declaration (vulkan_renderer_notes.md §4.67). Its own generator and its
# own edge, deliberately separate from the one above:
#
#   - it needs **no slangc**, only Python, so it runs on a machine that cannot compile a shader at
#     all - which is where a drifting struct would otherwise be least likely to be noticed;
#   - the SPIR-V is a build *product* and these asserts are a build *check*, and a check that only
#     runs where the product can be rebuilt is not much of a check.
#
# Same shape otherwise: the header is an output so Ninja does not decide VkDraw.cpp is clean before
# the generator has run, the stamp keeps the edge from re-running every build, and the generator
# rewrites the header only on a real change.
set(GKPLUS_ABI_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/src/gen-shader-abi.py")
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${GKPLUS_ABI_GENERATOR}" --deps
        OUTPUT_VARIABLE GKPLUS_ABI_SOURCES
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" GKPLUS_ABI_SOURCES "${GKPLUS_ABI_SOURCES}")
    add_custom_command(
        OUTPUT "${GKPLUS_GENERATED_DIR}/ShaderAbi.gen.inc.h"
               "${CMAKE_CURRENT_BINARY_DIR}/shader-abi.stamp"
        COMMAND "${Python3_EXECUTABLE}" "${GKPLUS_ABI_GENERATOR}"
                --output "${GKPLUS_GENERATED_DIR}/ShaderAbi.gen.inc.h"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/shader-abi.stamp"
        DEPENDS ${GKPLUS_ABI_SOURCES} "${GKPLUS_ABI_GENERATOR}"
        COMMENT "Deriving the shader ABI asserts"
        VERBATIM)
    add_custom_target(gkplus_shader_abi
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/shader-abi.stamp")
    add_dependencies(GkPlus gkplus_shader_abi)
endif()

add_library(huffman
    "${CMAKE_CURRENT_SOURCE_DIR}/huffman/huffman_compress.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/huffman/huffman_decompress.cpp")
target_include_directories(huffman PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/huffman")

add_subdirectory(utils)

if(GKPLUS_COPY_DLL)
    find_package(Steam REQUIRED)

    add_custom_target(copy
        ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:GkPlus>" "${STEAM_PATH}/steamapps/common/Gunlok/"
    )
endif()

# Must come after every install() above, and last in this file - CPack's own documented rule.
include(cmake/Packaging.cmake)