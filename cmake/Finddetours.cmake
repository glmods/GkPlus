find_path(detours_INCLUDE_DIR detours.h)
find_library(detours_LIBRARY detours)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(detours
    REQUIRED_VARS detours_INCLUDE_DIR detours_LIBRARY)

if(detours_FOUND)
    if(NOT TARGET detours)
        add_library(detours UNKNOWN IMPORTED)
        set_target_properties(detours PROPERTIES
            IMPORTED_LOCATION "${detours_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${detours_INCLUDE_DIR}")
    endif()
endif()