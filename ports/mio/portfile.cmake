# header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/mio
    REF c8bab5909f8d4afd5483fa7a3d8af6fa77ebdd41
    SHA512 a129577af74346b52a9dd12c5da9c400094b085fc1179c50d58fa98cebd41166877c68c9f2035a785244d7f691a79ed9913301dc1ce86cd3b1314c06bb424583
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
