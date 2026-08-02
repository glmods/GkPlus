find_path(RENDERDOC_PATH "renderdoc_app.h"
    HINTS "[HKLM/SOFTWARE/Microsoft/Windows/CurrentVersion;ProgramFilesDir]/RenderDoc"
    REGISTRY_VIEW 64)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(RenderDoc
    REQUIRED_VARS RENDERDOC_PATH)

if(RenderDoc_FOUND)
    if(NOT TARGET RenderDoc::Header)
        add_library(RenderDoc::Header INTERFACE IMPORTED)
        set_target_properties(RenderDoc::Header PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${RENDERDOC_PATH}")
    endif()
endif()