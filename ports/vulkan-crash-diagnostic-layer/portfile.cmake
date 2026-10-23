set(VCPKG_LIBRARY_LINKAGE dynamic)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO LunarG/CrashDiagnosticLayer
    REF "vulkan-sdk-${VERSION}"
    SHA512 551784d3613d4e6f5d8b6bee66efc97e7f1a81e61121fd9cbb71ed9ed6154766837f415cd7130406e0c88c36f8616f0968c739be53dfbb044f3d907763bf734e
    HEAD_REF main
    PATCHES
        fix-clang-windows-def-exports.diff
        0002-libcxx-and-windows-h.diff
)

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DBUILD_TESTS:BOOL=OFF
    -DUPDATE_DEPS:BOOL=OFF
    -DBUILD_WERROR:BOOL=OFF
    # spirv.hpp, from <dir>/include/spirv/unified1
    "-DSPIRV_HEADERS_INSTALL_DIR=${CURRENT_INSTALLED_DIR}"
)
vcpkg_cmake_install()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(layer_path "<vcpkg_installed>/bin")
if(NOT VCPKG_TARGET_IS_WINDOWS)
    set(layer_path "<vcpkg_installed>/share/vulkan/explicit_layer.d")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" @ONLY)
