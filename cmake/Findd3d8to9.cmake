find_path(d3d8to9_INCLUDE_DIR d3d8to9.hpp)
find_library(d3d8to9_LIBRARY d3d8to9)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(d3d8to9
    REQUIRED_VARS d3d8to9_INCLUDE_DIR d3d8to9_LIBRARY)

if(d3d8to9_FOUND)
    if(NOT TARGET d3d8to9)
        add_library(d3d8to9 UNKNOWN IMPORTED)
        set_target_properties(d3d8to9 PROPERTIES
            IMPORTED_LOCATION "${d3d8to9_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${d3d8to9_INCLUDE_DIR}")
    endif()
endif()