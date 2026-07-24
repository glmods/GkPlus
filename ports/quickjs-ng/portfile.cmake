vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO quickjs-ng/quickjs
    REF "v${VERSION}"
    SHA512 c963ebee04247e695601e13abddf552866d2ab54411563d46224c5974b1e84818e14c1e99a7fe776d08210d5acda5c2443f185cc9f0fdfc032820e436b7aa13b
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME qjs
    CONFIG_PATH lib/cmake/qjs
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/bin")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")