vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO KhronosGroup/KTX-Software
    REF "v${VERSION}"
    SHA512 19514da2d5b021d7fd1e24251dfd27d0e032018bdb84c7f76328de0ad431aeff12a77e7b3c857a1933a0b258a83ffd4b77cd053672702cc6f7132afcd1fa253e
    HEAD_REF master
    PATCHES
        0001-Use-vcpkg-zstd.patch
        0003-mkversion.patch
        0004-quirks.patch
        0005-no-vendored-libs.patch
        0006-fix-ios-install.patch
        ktxread-libtool.diff
        0007-version-without-bash.patch # version.h from cmake: no msys2 bash, which IT policy may block
)
file(GLOB third_party "${SOURCE_PATH}/external/*" "${SOURCE_PATH}/external/basisu/zstd" "${SOURCE_PATH}/other_include/*")
list(FILTER third_party EXCLUDE REGEX "/(astc-encoder|basisu|dfdutils|etcdec|imageio|glm|lodepng|SDL_gesture)\$")
file(REMOVE_RECURSE ${third_party})

vcpkg_list(SET OPTIONS)
if(VCPKG_TARGET_IS_APPLE AND VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    vcpkg_list(APPEND OPTIONS "-DASTCENC_ISA_SSE41=ON") # use x86_64, not x64_64h
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tools   KTX_FEATURE_TOOLS
        vulkan  KTX_FEATURE_VK_UPLOAD
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DKTX_GIT_VERSION_FULL=v${VERSION}-vcpkg
        -DKTX_FEATURE_TESTS=OFF
        -DKTX_FEATURE_LOADTEST_APPS=OFF
        ${FEATURE_OPTIONS}
        ${OPTIONS}
    OPTIONS_DEBUG
        -DKTX_FEATURE_TOOLS=OFF
    DISABLE_PARALLEL_CONFIGURE
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/ktx)

if(tools IN_LIST FEATURES)
    vcpkg_copy_tools(
        TOOL_NAMES
            ktx
            toktx
            ktxsc
            ktxinfo
            ktx2ktx2
            ktx2check
        AUTO_CLEAN
    )
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(GLOB LICENSE_FILES "${SOURCE_PATH}/LICENSES/*")
file(COPY ${LICENSE_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/LICENSES")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
