add_subdirectory(rifutil)
add_subdirectory(riflights)

if("rimutil" IN_LIST VCPKG_MANIFEST_FEATURES)
    add_subdirectory(rimutil)
endif()