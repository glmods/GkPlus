add_library(imgui-quickjs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/imgui-quickjs.cpp")
target_link_libraries(imgui-quickjs qjs imgui::imgui)
# This directory only, so that <imgui-quickjs.h> resolves without putting src/
# on anyone's include path (where quickjs.h's <math.h> would find src/Math.h).
target_include_directories(imgui-quickjs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(imgui-quickjs
    PROPERTIES
    CXX_STANDARD 20)