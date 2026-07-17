find_package(SPNG CONFIG REQUIRED)
find_package(LibSquish MODULE REQUIRED)

add_executable(rimutil "${CMAKE_CURRENT_SOURCE_DIR}/rimutil.cpp")
target_link_libraries(rimutil PRIVATE $<IF:$<TARGET_EXISTS:spng::spng>,spng::spng,spng::spng_static> LibSquish::Squish)
set_target_properties(rimutil PROPERTIES CXX_STANDARD 23)