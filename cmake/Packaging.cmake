# An NSIS installer for the one file GkPlus actually ships: d3d8.dll, dropped next to gl.exe.
# There is no "GkPlus application" to install into its own Program Files folder, so the
# Directory page is made to default to wherever Steam says Gunlok is - cmake/nsis/GunlokDetect.nsh
# is what does the registry read and the libraryfolders.vdf lookup across every Steam library, not
# just the one Steam itself lives in.
#
# `cmake --build build --target copy` (see FindSteam.cmake) exists for iterating against a dev
# machine's own install and only ever looks at the main Steam library. This is the form meant to
# reach someone else's machine, so it has to find Gunlok wherever their library actually is.

set(CPACK_PACKAGE_NAME "GkPlus")
set(CPACK_PACKAGE_VENDOR "GkPlus")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "GkPlus - a modding framework for Gunlok (2000)")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "GkPlus")

# CPack's NSIS generator is picky about the license resource's extension; the checked-in LICENSE
# has none, so a renamed copy in the build tree is what gets pointed at rather than the file
# itself.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
    COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")

set(CPACK_NSIS_PACKAGE_NAME "GkPlus")
set(CPACK_NSIS_DISPLAY_NAME "GkPlus - Gunlok Modding Framework")
# The install directory *is* Gunlok's own folder, not something of GkPlus's own that would need
# adding to PATH.
set(CPACK_NSIS_MODIFY_PATH OFF)

# !define lines take effect wherever they land as long as it is before the matching
# `!insertmacro MUI_PAGE_DIRECTORY` in NSIS.template.in, which CPACK_NSIS_DEFINES always is - it is
# spliced in near the top of the generated script.
#
# Both quirks below were only found by actually running `cpack -G NSIS` and reading
# NSISOutput.log - configuring clean proves neither one.
#
# Deliberately unquoted: CPack re-serializes this value into CPackConfig.cmake through a plain
# `set(CPACK_NSIS_DEFINES "...")` that does not escape an embedded `"` - one turned the rest of
# this string into extra, semicolon-joined `set()` arguments and silently ate every quote in the
# generated .nsi. The include path is CMAKE_CURRENT_SOURCE_DIR, which never contains a space in
# this repo's own checkout instructions, so leaving it unquoted is the tradeoff rather than
# fighting the serializer.
#
# Deliberately backslashed rather than CMake's native forward slashes: makensis's own !include
# resolves a forward-slash absolute path (`C:/Users/.../GunlokDetect.nsh`) as "could not find",
# backslashes only.
#
# Deliberately doubled backslashes: that same unescaping serializer writes this value's raw bytes
# into CPackConfig.cmake and `cpack` parses that file as ordinary CMake source, so a single
# backslash survives only if it is written here as two - one and `cpack -G NSIS` fails the whole
# config with "Invalid character escape '\U'" (from "...\Users\...").
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nsis/GunlokDetect.nsh" GKPLUS_GUNLOK_DETECT_NSH)
string(REPLACE "\\" "\\\\" GKPLUS_GUNLOK_DETECT_NSH_ESCAPED "${GKPLUS_GUNLOK_DETECT_NSH}")
set(CPACK_NSIS_DEFINES
    "!include ${GKPLUS_GUNLOK_DETECT_NSH_ESCAPED}\n!define MUI_PAGE_CUSTOMFUNCTION_PRE GkPlus.DirectoryPre\n!define MUI_PAGE_CUSTOMFUNCTION_LEAVE GkPlus.DirectoryLeave\n")

# One DLL, no optional pieces - a components page would only be something else to click through.
set(CPACK_MONOLITHIC_INSTALL ON)

include(CPack)
