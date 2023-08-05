if(VCPKG_TARGET_IS_LINUX)
    message("Warning: `glaze` requires Clang or GCC 10+ on Linux")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/glaze
    REF e061c7e4737e1fe408663e218599acce0e78aea0
    SHA512 e4d813124b7f84a6b668886f74c945dd7550ad1d25a5726fa90d11b1f569895511499b6431463b3dab06288e8b82d06bfee971fc4bd323bc8ad7c043b2fd7f25
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
