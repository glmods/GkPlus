find_path(LibSquish_INCLUDE_DIR squish.h)
FIND_LIBRARY(LibSquish_LIBRARY_RELEASE
    NAMES
    squish
    HINTS
    ${CMAKE_FIND_ROOT_PATH}
    PATHS
    ${CMAKE_FIND_ROOT_PATH}
    PATH_SUFFIXES
    "lib"
    "local/lib"
)

FIND_LIBRARY(LibSquish_LIBRARY_DEBUG
    NAMES
    squishd
    HINTS
    ${CMAKE_FIND_ROOT_PATH}
    PATHS
    ${CMAKE_FIND_ROOT_PATH}
    PATH_SUFFIXES
    "debug/lib"
    "lib"
    "local/lib"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibSquish
    REQUIRED_VARS LibSquish_INCLUDE_DIR LibSquish_LIBRARY_RELEASE LibSquish_LIBRARY_DEBUG)

if(LibSquish_FOUND)
    if(NOT TARGET LibSquish::Squish)
        add_library(LibSquish::Squish UNKNOWN IMPORTED)
        set_target_properties(LibSquish::Squish PROPERTIES
            IMPORTED_LOCATION "${LibSquish_LIBRARY_RELEASE}"
            IMPORTED_LOCATION_DEBUG "${LibSquish_LIBRARY_DEBUG}"
            INTERFACE_INCLUDE_DIRECTORIES "${LibSquish_INCLUDE_DIR}")
    endif()
endif()