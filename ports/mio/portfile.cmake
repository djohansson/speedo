# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/mio
    REF c87a68d7da62377c764c23cd6ee5de76617b6784
    SHA512 6cc0eefed1f75f13b80dea8fe0f2641bd53ee64210b046b9b561936e29bdd14a2b176b29bfc66343a4480313839d9a3d5c94ea42990b5316c4f956796d247688
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dmio.tests=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH share/cmake/mio)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")

# Handle copyright
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
