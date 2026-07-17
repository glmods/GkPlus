add_library(imgui-quickjs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/imgui-quickjs.cpp")
target_link_libraries(imgui-quickjs qjs imgui::imgui)
set_target_properties(imgui-quickjs
    PROPERTIES
    CXX_STANDARD 20)