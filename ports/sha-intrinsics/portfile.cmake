vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/SHA-Intrinsics
    REF 9dd886303c2a4731345a93e24b7f3d6b6efed007
    SHA512 257e34df52652750efc275aee094d2166852caf830df640f88d11b07a7986449fafb2e45a8a1764eb123e747fd852f269a2bd1eeb89c818da11067459f0eee6e
    HEAD_REF master
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
