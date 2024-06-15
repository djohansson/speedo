vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/SHA-Intrinsics
    REF 3d4edf8f1eee4bafa129e383793cf127710821f8
    SHA512 08e2d48ebcbf52ee259c610fecbe688d1edee8adaa5f790853936cabde71a7c947879e211d0342ee3fd935b999fa26fdd67a5bc7563cb81fea50aa58a2a079f8
    HEAD_REF master
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
