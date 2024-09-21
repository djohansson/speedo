set(VCPKG_LIBRARY_LINKAGE dynamic)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO KhronosGroup/Vulkan-Loader
    REF "vulkan-sdk-${VERSION}"
    SHA512 8ec98e0da867f829e048e100a97d7b94a3c40f56f858e3eb81f11f6f58e20e59da6ca8785a9642958ff3b698c618b9968407028cc66dfa0ad296576bf9db45ca
    HEAD_REF main
    PATCHES
        0001-clang-windows.patch
)

vcpkg_find_acquire_program(PYTHON3)
get_filename_component(PYTHON3_DIR "${PYTHON3}" DIRECTORY)
vcpkg_add_to_path("${PYTHON3_DIR}")

if (VCPKG_TARGET_IS_WINDOWS)
    set(ASM_FLAGS -DUSE_MASM:BOOL=OFF)
else()
    set(ASM_FLAGS -DUSE_GAS:BOOL=OFF)
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DBUILD_TESTS:BOOL=OFF
    -DLOADER_CODEGEN:BOOL=ON
    ${ASM_FLAGS}
)
vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/VulkanLoader" PACKAGE_NAME VulkanLoader)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")

set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" @ONLY)
