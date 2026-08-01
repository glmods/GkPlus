cmake_minimum_required(VERSION 3.24)

project(GkPlus
    VERSION 1.0
    LANGUAGES C CXX)

option(GKPLUS_COPY_DLL "Copy the generated DLL to Gunlok's directory using the `copy` target" ON)

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
find_package(volk CONFIG REQUIRED)
# Vulkan::Headers, NOT Vulkan::Vulkan: VMA only needs vulkan/vulkan.h, and linking the
# loader's import library would give d3d8.dll a load-time dependency on vulkan-1.dll -
# the thing volk and the vendored imgui backend exist to avoid.
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
# volk.c is compiled into GkPlus rather than taken from volk::volk, so that the platform
# defines below apply to volk's own translation unit as well as to ours. vcpkg's prebuilt
# volk library is built WITHOUT VK_USE_PLATFORM_WIN32_KHR, so its vkCreateWin32SurfaceKHR
# pointer does not exist - volk.h still declares it (the header sees our define), so the
# mismatch surfaces as an undefined symbol at link time rather than anywhere useful.
find_path(VOLK_SOURCE_DIR NAMES volk.c REQUIRED)


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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Debug.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Encoding.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/entry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FileHooks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GLS.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GUI.cpp"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsRender.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsRoles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsTokens.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsTriggers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/JsWorld.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Json.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MakeRole.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Map.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Math.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Memory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Menu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Misc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Music.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Render.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Repl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Roles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Script.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptQueue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Tokens.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Triggers.cpp"
    "${VOLK_SOURCE_DIR}/volk.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui_backends/imgui_impl_vulkan.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VertexFormat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkContext.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VkResources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Vfs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/World.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.def")
target_link_libraries(GkPlus PRIVATE detours imgui::imgui d3d8to9 d3d9 qjs imgui-quickjs ws2_32 volk::volk_headers GPUOpen::VulkanMemoryAllocator Vulkan::Headers
    $<IF:$<TARGET_EXISTS:PhysFS::PhysFS>,PhysFS::PhysFS,PhysFS::PhysFS-static>)
# Needed for VK_KHR_win32_surface and vkCreateWin32SurfaceKHR. On the target rather
# than per-TU so every Vulkan source agrees - a TU that forgot it would still compile
# and then miss the surface entry points at link time.
target_compile_definitions(GkPlus PRIVATE VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
set_target_properties(GkPlus
    PROPERTIES
    OUTPUT_NAME "d3d8"
    CXX_STANDARD 23)

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