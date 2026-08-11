find_path(d3d8to9_INCLUDE_DIR d3d8to9.hpp)

# See Finddetours.cmake: one `find_library` pins one path for every configuration, and vcpkg's
# debug and release builds disagree about `_ITERATOR_DEBUG_LEVEL`. d3d8to9 is the library that
# actually emitted the mismatch, being the only one of these that is C++ and includes the STL.
find_library(d3d8to9_LIBRARY_RELEASE NAMES d3d8to9
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(d3d8to9_LIBRARY_DEBUG NAMES d3d8to9
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(d3d8to9_LIBRARY_RELEASE NAMES d3d8to9)

if(d3d8to9_LIBRARY_RELEASE)
    set(d3d8to9_LIBRARY "${d3d8to9_LIBRARY_RELEASE}")
else()
    set(d3d8to9_LIBRARY "${d3d8to9_LIBRARY_DEBUG}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(d3d8to9
    REQUIRED_VARS d3d8to9_INCLUDE_DIR d3d8to9_LIBRARY)

if(d3d8to9_FOUND)
    if(NOT TARGET d3d8to9)
        add_library(d3d8to9 UNKNOWN IMPORTED)
        set_target_properties(d3d8to9 PROPERTIES
            IMPORTED_LOCATION "${d3d8to9_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${d3d8to9_INCLUDE_DIR}")
        if(d3d8to9_LIBRARY_RELEASE)
            set_property(TARGET d3d8to9 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_property(TARGET d3d8to9 PROPERTY
                IMPORTED_LOCATION_RELEASE "${d3d8to9_LIBRARY_RELEASE}")
        endif()
        if(d3d8to9_LIBRARY_DEBUG)
            set_property(TARGET d3d8to9 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_property(TARGET d3d8to9 PROPERTY
                IMPORTED_LOCATION_DEBUG "${d3d8to9_LIBRARY_DEBUG}")
        endif()
        # vcpkg builds no RelWithDebInfo or MinSizeRel variant; both are release ABI.
        set_property(TARGET d3d8to9 PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
        set_property(TARGET d3d8to9 PROPERTY MAP_IMPORTED_CONFIG_MINSIZEREL Release)
    endif()
endif()
