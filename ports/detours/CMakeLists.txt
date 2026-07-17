cmake_minimum_required(VERSION 3.12)

project(detours CXX)

add_library(detours
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detours.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modules.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/disasm.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/image.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/creatwth.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/disolx86.cpp"
)

install(TARGETS detours)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detours.h"
    TYPE INCLUDE
)