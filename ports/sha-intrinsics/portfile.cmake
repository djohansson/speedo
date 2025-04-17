vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/SHA-Intrinsics
    REF 1da6cf076c6b062f67db1c834b7488b9ef269587
    SHA512 5ed68cf080bcc0ec4096465ab3a2a20b8a0681a20df45d14e855739cb0c73c0ff7efb0cf02afaea08f68e05a050370738809bf48bf048a627b5b15436253edc9
    HEAD_REF master
    PATCHES
        0001-add-config.patch
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(PACKAGE_NAME sha-intrinsics CONFIG_PATH lib/cmake/SHA-Intrinsics)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")