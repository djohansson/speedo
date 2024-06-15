vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/SHA-Intrinsics
    REF 9e7d7fde019c7fd15eec20be4fdea15c73ea0e1b
    SHA512 74fa425823826e6692375a074653d7532192349cbeea93e528c4cd3a8b56dcc66b044bf58f177f480141474557f7515f2ee62fa5582dd4f66643bac52c3fec9b
    HEAD_REF master
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
