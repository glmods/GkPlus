add_executable(rifutil
    "${CMAKE_CURRENT_SOURCE_DIR}/rifutil.cpp")
target_link_libraries(rifutil PRIVATE huffman)