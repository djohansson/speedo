vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO shader-slang/glslang
    REF 4c20dc0cbb3b20d2afb9ee8595a39508f1066623
    SHA512 db2e4e3d51e0d5effa0ddde9e145aac74a87487ae9cb9b44125755b2c3a290ba295004ba8573bd9b5aa0a541f788e3ed3db8fbb81b41222d9503fe62a19a67cb
    HEAD_REF master
    PATCHES
        cmake.patch # Remove on next version (Upstream PR #3406 and #3420).
        standalone_header_install_fix.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        opt ENABLE_OPT
        opt ALLOW_EXTERNAL_SPIRV_TOOLS
        tools ENABLE_GLSLANG_BINARIES
        rtti ENABLE_RTTI
)

if (ENABLE_GLSLANG_BINARIES)
    vcpkg_find_acquire_program(PYTHON3)
    get_filename_component(PYTHON_PATH ${PYTHON3} DIRECTORY)
    vcpkg_add_to_path("${PYTHON_PATH}")
endif ()

if (VCPKG_TARGET_IS_WINDOWS)
    set(PLATFORM_OPTIONS "-DOVERRIDE_MSVCCRT=OFF")
endif ()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_EXTERNAL=OFF
        -DENABLE_CTEST=OFF
        -DSKIP_GLSLANG_INSTALL=OFF
        ${FEATURE_OPTIONS}
        ${PLATFORM_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/glslang DO_NOT_DELETE_PARENT_CONFIG_PATH)
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${PORT}/glslang-config.cmake"
    [[${PACKAGE_PREFIX_DIR}/lib/cmake/glslang/glslang-targets.cmake]]
    [[${CMAKE_CURRENT_LIST_DIR}/glslang-targets.cmake]]
)
file(REMOVE_RECURSE CONFIG_PATH "${CURRENT_PACKAGES_DIR}/lib/cmake" "${CURRENT_PACKAGES_DIR}/debug/lib/cmake")

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/glslang/Public/ShaderLang.h" "ifdef GLSLANG_IS_SHARED_LIBRARY" "if 1")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/glslang/Include/glslang_c_interface.h" "ifdef GLSLANG_IS_SHARED_LIBRARY" "if 1")
endif()

vcpkg_copy_pdbs()

if (ENABLE_GLSLANG_BINARIES)
    vcpkg_copy_tools(TOOL_NAMES glslang glslangValidator spirv-remap AUTO_CLEAN)
endif ()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
