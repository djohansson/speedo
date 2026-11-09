set(VCPKG_LIBRARY_LINKAGE dynamic)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO LunarG/CrashDiagnosticLayer
    REF "vulkan-sdk-${VERSION}"
    SHA512 5bde995131106dc951a8e78b9d882e23647f08f04575c4f27bd396982f63076cfa9ad8b4df276075d08938f6bdcd34eb5ed8e0185b43d79c0de0b1a3b56f32d0
    HEAD_REF main
    PATCHES
        fix-clang-windows-def-exports.diff
        0002-libcxx-and-windows-h.diff
        0003-macos-gettid.diff
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
