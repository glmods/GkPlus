add_executable(riflights
    "${CMAKE_CURRENT_SOURCE_DIR}/riflights.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Rif.cpp")
# NO `target_include_directories` for src/: putting it on an include path makes <cstdlib>'s
# <math.h> resolve to src/Math.h on a case-insensitive filesystem, and the errors come out of the
# CRT headers with nothing pointing back here. CLAUDE.md names this as the trap a throwaway
# harness walks into; Rif.h is included by relative path instead, which is how src/ includes
# itself.
target_link_libraries(riflights PRIVATE huffman)
set_target_properties(riflights PROPERTIES CXX_STANDARD 20)
