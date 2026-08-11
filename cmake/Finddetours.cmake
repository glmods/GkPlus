find_path(detours_INCLUDE_DIR detours.h)

# vcpkg ships a release and a debug build of each library, and they differ in
# `_ITERATOR_DEBUG_LEVEL`. A single `find_library` picks whichever of the two prefixes it reaches
# first and then pins it for *every* configuration - which resolved to the debug one here, so a
# RelWithDebInfo link failed with `/failifmismatch: mismatch detected for '_ITERATOR_DEBUG_LEVEL'`
# against our own objects. Both variants are located, and the imported target maps each
# configuration onto the right one.
find_library(detours_LIBRARY_RELEASE NAMES detours
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(detours_LIBRARY_DEBUG NAMES detours
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
# Not building under vcpkg: fall back to a plain search. A `find_library` whose cache variable is
# already set is a no-op, so this only runs when the targeted lookup above found nothing.
find_library(detours_LIBRARY_RELEASE NAMES detours)

if(detours_LIBRARY_RELEASE)
    set(detours_LIBRARY "${detours_LIBRARY_RELEASE}")
else()
    set(detours_LIBRARY "${detours_LIBRARY_DEBUG}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(detours
    REQUIRED_VARS detours_INCLUDE_DIR detours_LIBRARY)

if(detours_FOUND)
    if(NOT TARGET detours)
        add_library(detours UNKNOWN IMPORTED)
        set_target_properties(detours PROPERTIES
            IMPORTED_LOCATION "${detours_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${detours_INCLUDE_DIR}")
        if(detours_LIBRARY_RELEASE)
            set_property(TARGET detours APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_property(TARGET detours PROPERTY
                IMPORTED_LOCATION_RELEASE "${detours_LIBRARY_RELEASE}")
        endif()
        if(detours_LIBRARY_DEBUG)
            set_property(TARGET detours APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_property(TARGET detours PROPERTY
                IMPORTED_LOCATION_DEBUG "${detours_LIBRARY_DEBUG}")
        endif()
        # vcpkg builds no RelWithDebInfo or MinSizeRel variant; both are release ABI.
        set_property(TARGET detours PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
        set_property(TARGET detours PROPERTY MAP_IMPORTED_CONFIG_MINSIZEREL Release)
    endif()
endif()
