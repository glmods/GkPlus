vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO dearimgui/dear_bindings
    REF "v${VERSION}"
    SHA512 33c94e963e9889c4916a56ff8f3c7cf78072ec5ae88d4fe1136a92cf48c643dc706af3089984004c338bb480e19be21cb58af22bcf70fe08c96ea0e615710271
    HEAD_REF main
)

x_vcpkg_get_python_packages(
    PYTHON_VERSION 3
    REQUIREMENTS_FILE "${SOURCE_PATH}/requirements.txt"
    OUT_PYTHON_VAR PYTHON3
)

execute_process(
    COMMAND "${PYTHON3}" "${SOURCE_PATH}/dear_bindings.py" -o "${SOURCE_PATH}/dcimgui" "${CURRENT_INSTALLED_DIR}/include/imgui.h"
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()