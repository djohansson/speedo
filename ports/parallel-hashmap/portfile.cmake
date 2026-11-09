#header-only library

# speedo overlay: 2.1.0, which includes <cstdlib> where libc++ needs it (vcpkg's port is at 2.0.0, which we patched)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO greg7mdp/parallel-hashmap
    REF "v${VERSION}"
    SHA512 373928a769bce519aa321b92a855838e663d4ccf123de0e9f66ac7ced28fda2aeb299085532f7d4867a630c06ed2a5c6ee32aadb692887d496e156c0c0b1f83d
    HEAD_REF master
)

# Use greg7mdp/parallel-hashmap's own build process, skipping examples and tests
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DPHMAP_BUILD_TESTS=OFF
        -DPHMAP_BUILD_EXAMPLES=OFF
)
vcpkg_cmake_install()

# Delete redundant directories
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/lib ${CURRENT_PACKAGES_DIR}/debug ${CURRENT_PACKAGES_DIR}/share/doc)

file(COPY ${SOURCE_PATH}/phmap.natvis DESTINATION ${CURRENT_PACKAGES_DIR}/include)

# Put the licence file where vcpkg expects it
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
