if(VCPKG_TARGET_IS_LINUX)
    message("Warning: `glaze` requires Clang or GCC 10+ on Linux")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/glaze
    REF ce5ecf1
    SHA512 968c5c699038fd6275b68d602ed9793ac26b56fce6c578f628a82b6b6e04b3bbc9e818cbba52fac4aff2b9c334d212e2027a9a30566196d607831f771e36012d
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dglaze_DEVELOPER_MODE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
