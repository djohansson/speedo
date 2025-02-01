vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO eyalz800/zpp_bits
    REF "v${VERSION}"
    SHA512 f95292b490e0de14c1919504897333ad7ea0088ec6bee825dad9cdb73a25f5d2dd034dd2b2bc48bfb23ef93913977b0416cd8b8dec5760059195657419b240e9
    HEAD_REF master
)

file(INSTALL "${SOURCE_PATH}/zpp_bits.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
