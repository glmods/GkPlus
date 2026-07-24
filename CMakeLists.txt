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

find_package(Lua 5.4 REQUIRED)

find_package(detours MODULE REQUIRED)

find_package(d3d8to9 MODULE REQUIRED)

find_package(qjs CONFIG REQUIRED)

add_subdirectory(imgui-quickjs)

add_library(GkPlus MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Actors.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Console.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Debug.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/entry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GLS.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiBindings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GUI.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/InputFix.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/LuaEngine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Map.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Math.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Memory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Menu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Misc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Music.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Roles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Tokens.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Triggers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Vulnerability.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.def")
target_link_libraries(GkPlus PRIVATE detours imgui::imgui d3d8to9 d3d9 ${LUA_LIBRARIES} qjs)
target_include_directories(GkPlus PRIVATE ${LUA_INCLUDE_DIR})
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