vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO quickjs-ng/quickjs
    REF "v${VERSION}"
    SHA512 c9e27746287571603db0ab5b5af5d8d1e7784bd0d70dbc549a2d5ea34378b512d695a19a561e8763218919b6cbbd494664660835aabfc96d0379905aa2dceedf
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME qjs
    CONFIG_PATH lib/cmake/quickjs
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/bin")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")