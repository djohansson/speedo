vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO eyalz800/zpp_bits
    REF "v${VERSION}"
    SHA512 4dd5d3954fe795729e195817591c71716c3f50ed9b94f5e3cadd8ef6738936ff2b961c24a81f4ee76227916e918dfddc3d4e67ee1c8fd056bfbe6e991bb653f1
    HEAD_REF master
)

file(INSTALL "${SOURCE_PATH}/zpp_bits.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
