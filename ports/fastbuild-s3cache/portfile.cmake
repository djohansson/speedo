# speedo's FASTBuild cache over S3 (see src/, and docs/notes/build.md, "FASTBuild's cache"): FBuild's cache plugin and the
# launcher CMake runs as FBuild, which hands the plugin to it. A host tool beside vcpkg-tool-fastbuild's FBuild, built
# from the sources in this port's directory, so that vcpkg's ABI hash follows them
if(VCPKG_CROSSCOMPILING)
	message(FATAL_ERROR "fastbuild-s3cache is a host tool")
endif()

vcpkg_cmake_configure(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/src")
vcpkg_cmake_install()

# Windows: the dlls the plugin loads (curl's) beside it, which the launcher puts on FBuild's path (FBuild's LoadLibrary
# searches its own directory and the path, not the plugin's); macOS and Linux find them by the plugin's rpath
if(VCPKG_TARGET_IS_WINDOWS)
	vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()

set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" "Copyright (c) 2018-2026 Daniel Johansson. All rights reserved. Part of speedo, under its license.\n")
