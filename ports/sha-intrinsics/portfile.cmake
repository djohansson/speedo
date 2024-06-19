vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/SHA-Intrinsics
    REF 22798a18203f18e17eb0f3a1cba2cbbdabe5d618
    SHA512 82f7f1de0d1dc5d70a00b19210d9f8c5dcb77888ad504663ecf6fbed6285b8b9fa29a40d6d87ee93c9a4ebb0970a7a66b10b5ff46ba40c03152ffd4e6fdc3f4f
    HEAD_REF master
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
