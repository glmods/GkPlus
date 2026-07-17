find_path(STEAM_PATH "steam.exe"
    HINTS "[HKLM/SOFTWARE/Microsoft/Windows/CurrentVersion;ProgramFilesDir]/Steam"
    REGISTRY_VIEW 32)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Steam
    REQUIRED_VARS STEAM_PATH)