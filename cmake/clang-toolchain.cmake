find_program(CMAKE_C_COMPILER "clang-cl")
find_program(CMAKE_CXX_COMPILER "clang-cl")
string(APPEND CMAKE_C_FLAGS_INIT " -m32 ")
string(APPEND CMAKE_CXX_FLAGS_INIT " -m32 ")