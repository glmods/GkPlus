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

# See Finddetours.cmake/Findd3d8to9.cmake: only release is REQUIRED. A debug variant may not
# exist at all - vcpkg's x86-windows-static-md-xwin triplet builds release-only, since `xwin`
# has no debug CRT static libs to link a debug config against.
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
    REQUIRED_VARS LibSquish_INCLUDE_DIR LibSquish_LIBRARY_RELEASE)

if(LibSquish_FOUND)
    if(NOT TARGET LibSquish::Squish)
        add_library(LibSquish::Squish UNKNOWN IMPORTED)
        set_target_properties(LibSquish::Squish PROPERTIES
            IMPORTED_LOCATION "${LibSquish_LIBRARY_RELEASE}"
            INTERFACE_INCLUDE_DIRECTORIES "${LibSquish_INCLUDE_DIR}")
        if(LibSquish_LIBRARY_DEBUG)
            set_property(TARGET LibSquish::Squish PROPERTY
                IMPORTED_LOCATION_DEBUG "${LibSquish_LIBRARY_DEBUG}")
        endif()
    endif()
endif()