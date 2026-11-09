# speedo overlay: draco 1.5.9 (vcpkg's port is at 1.5.7; its patches rebased on 1.5.9 here, 2026-11-09), built with
# ninja, and without posix large file support on windows (0002-*). the FASTBuild generator can't build draco: its
# shared library is linked from object libraries only ("DLL() - Missing required property 'Libraries'"), and header
# only object libraries leave no output ("Failed due to missing file"). 1.5.9 has the <algorithm> include vcpkg's port
# downloads as a patch, and no link groups on apple platforms (our 0001-*, ld64.lld has none)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/draco
    REF "${VERSION}"
    SHA512 32209bdfa9ad350a8a037b23fba3da7957f72e2f3ece9b948eaa4e4da3489e308b7432c7509f89d1d2e5778a30cce3aba9cdbfa81059063c2a24d246f03a08c9
    HEAD_REF master
    PATCHES
        fix-compile-error-uwp.patch
        fix-uwperror.patch
        fix-pkgconfig.patch
        disable-symlinks.patch
        install-linkage.diff
        0002-no-posix-large-files-on-windows.patch # speedo: _FILE_OFFSET_BITS=64 picks ftello, which windows lacks
        0003-include-iterator.patch # speedo: std::back_inserter without <iterator>, which libc++ on linux doesn't include
)

if(VCPKG_TARGET_IS_EMSCRIPTEN)
    set(ENV{EMSCRIPTEN} "${EMSCRIPTEN_ROOT}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    GENERATOR Ninja
    OPTIONS
        -DPYTHON_EXECUTABLE=: # unused with DRACO_JS_GLUE off
        -DDRACO_JS_GLUE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH share/cmake/draco)
vcpkg_fixup_pkgconfig()

# Install tools and plugins
if(NOT VCPKG_TARGET_IS_EMSCRIPTEN)
    vcpkg_copy_tools(TOOL_NAMES draco_encoder draco_decoder AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
