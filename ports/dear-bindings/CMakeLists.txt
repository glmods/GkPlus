cmake_minimum_required(VERSION 3.12)

project(dear-bindings CXX)

find_package(imgui CONFIG REQUIRED)

add_library(dcimgui "${CMAKE_CURRENT_SOURCE_DIR}/dcimgui.cpp")
target_link_libraries(dcimgui PUBLIC imgui::imgui)

install(TARGETS dcimgui)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/dcimgui.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dcimgui.json"
    TYPE INCLUDE
)