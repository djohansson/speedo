# Don't file if the bin folder exists. We need exe and custom files.
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

string(REGEX REPLACE "^([0-9]+)[.]([0-9])\$" "\\1.0\\2" USD_VERSION "${VERSION}")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO djohansson/OpenUSD
    REF 236d693b9648fc965c6b1f8d3d204b3bfb1de06c
    SHA512 89f932f148548a7c12783e1421c254189ef3f6d10429fe806a36a4fa422511c95b51d205687a0b35376f72f395a8d63140b06838eac135ed27cfcf7722541b6b
    HEAD_REF tbb-fixes
    PATCHES
        fix_build-location.patch
)

if(NOT VCPKG_TARGET_IS_WINDOWS)
file(REMOVE ${SOURCE_PATH}/cmake/modules/FindTBB.cmake)
endif()

find_file(LOCAL_PYTHON3
    NAMES "python3${VCPKG_HOST_EXECUTABLE_SUFFIX}" "python${VCPKG_HOST_EXECUTABLE_SUFFIX}"
    PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/python3"
    NO_DEFAULT_PATH
    REQUIRED
)
set(USD_VENV_PATH "${CURRENT_INSTALLED_DIR}")
vcpkg_execute_required_process(
    COMMAND ${LOCAL_PYTHON3} -m venv ${USD_VENV_PATH}
    WORKING_DIRECTORY "${USD_VENV_PATH}"
    LOGNAME "usd-venv-setup-${TARGET_TRIPLET}")

vcpkg_cmake_get_vars(CMAKE_VARS_FILE)
include("${CMAKE_VARS_FILE}")

set(ENV{CC} "${VCPKG_DETECTED_CMAKE_C_COMPILER}")
set(ENV{CFLAGS} "${VCPKG_DETECTED_CMAKE_C_FLAGS}")

vcpkg_execute_required_process(
    COMMAND ${USD_VENV_PATH}/bin/pip install Jinja2 PySide6 PyOpenGL
    WORKING_DIRECTORY "${CURRENT_PORT_DIR}"
    LOGNAME "pip-install-packages-${TARGET_TRIPLET}")
vcpkg_execute_required_process(
    COMMAND ${USD_VENV_PATH}/bin/pip install ./PyOpenGL-accelerate-3.1.7-patch.tar.gz
    WORKING_DIRECTORY "${CURRENT_PORT_DIR}"
    LOGNAME "pip-install-packages-${TARGET_TRIPLET}")

unset(ENV{CC})
unset(ENV{CFLAGS})

set(Python3_FIND_VIRTUALENV FIRST)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DPXR_BUILD_ALEMBIC_PLUGIN:BOOL=ON
        -DPXR_BUILD_EMBREE_PLUGIN:BOOL=OFF
        -DPXR_BUILD_IMAGING:BOOL=ON
        -DPXR_BUILD_MONOLITHIC:BOOL=OFF
        -DPXR_BUILD_TESTS:BOOL=OFF
        -DPXR_BUILD_USD_IMAGING:BOOL=ON
        -DPXR_ENABLE_PYTHON_SUPPORT:BOOL=ON
        -DPXR_ENABLE_OPENVDB_SUPPORT:BOOL=ON
        -DPXR_BUILD_EXAMPLES:BOOL=OFF
        -DPXR_BUILD_TUTORIALS:BOOL=OFF
        -DPXR_BUILD_USD_TOOLS:BOOL=ON
        -DPXR_BUILD_USDVIEW:BOOL=ON
)

vcpkg_cmake_install()

# The CMake files installation is not standard in USD and will install pxrConfig.cmake in the prefix root and
# pxrTargets.cmake in "cmake" so we are moving pxrConfig.cmake in the same folder and patch the path to pxrTargets.cmake
vcpkg_replace_string(${CURRENT_PACKAGES_DIR}/pxrConfig.cmake "/cmake/pxrTargets.cmake" "/pxrTargets.cmake")

file(
    RENAME
        "${CURRENT_PACKAGES_DIR}/pxrConfig.cmake"
        "${CURRENT_PACKAGES_DIR}/cmake/pxrConfig.cmake")

vcpkg_cmake_config_fixup(CONFIG_PATH cmake PACKAGE_NAME pxr)

# Remove duplicates in debug folder
file(REMOVE ${CURRENT_PACKAGES_DIR}/debug/pxrConfig.cmake)
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)

# Handle copyright
vcpkg_install_copyright(FILE_LIST ${SOURCE_PATH}/LICENSE.txt)

if(VCPKG_TARGET_IS_WINDOWS)
    # Move all dlls to bin
    file(GLOB RELEASE_DLL ${CURRENT_PACKAGES_DIR}/lib/*.dll)
    file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/bin)
    file(GLOB DEBUG_DLL ${CURRENT_PACKAGES_DIR}/debug/lib/*.dll)
    file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/debug/bin)
    foreach(CURRENT_FROM ${RELEASE_DLL} ${DEBUG_DLL})
        string(REPLACE "/lib/" "/bin/" CURRENT_TO ${CURRENT_FROM})
        file(RENAME ${CURRENT_FROM} ${CURRENT_TO})
    endforeach()

    vcpkg_copy_pdbs()

    function(file_replace_regex filename match_string replace_string)
        file(READ ${filename} _contents)
        string(REGEX REPLACE "${match_string}" "${replace_string}" _contents "${_contents}")
        file(WRITE ${filename} "${_contents}")
    endfunction()

    # fix dll path for cmake
    file_replace_regex(${CURRENT_PACKAGES_DIR}/share/pxr/pxrTargets-debug.cmake "debug/lib/([a-zA-Z0-9_]+)\\.dll" "debug/bin/\\1.dll")
    file_replace_regex(${CURRENT_PACKAGES_DIR}/share/pxr/pxrTargets-release.cmake "lib/([a-zA-Z0-9_]+)\\.dll" "bin/\\1.dll")

    # fix plugInfo.json for runtime
    file(GLOB_RECURSE PLUGINFO_FILES ${CURRENT_PACKAGES_DIR}/lib/usd/*/resources/plugInfo.json)
    file(GLOB_RECURSE PLUGINFO_FILES_DEBUG ${CURRENT_PACKAGES_DIR}/debug/lib/usd/*/resources/plugInfo.json)
    foreach(PLUGINFO ${PLUGINFO_FILES} ${PLUGINFO_FILES_DEBUG})
        file_replace_regex(${PLUGINFO} [=["LibraryPath": "../../([a-zA-Z0-9_]+).dll"]=] [=["LibraryPath": "../../../bin/\1.dll"]=])
    endforeach()
endif()
