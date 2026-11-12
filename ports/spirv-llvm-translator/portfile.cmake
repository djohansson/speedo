# speedo overlay, not in vcpkg's registry: for the mesa port (mesa_clc). Static, as LLVM is: a shared library with LLVM
# linked in beside mesa's static LLVM would register LLVM's options twice. Its version follows LLVM's major and minor,
# which mesa checks (its latest tag, v23.1.3, serves LLVM 23.1.5: it tags no later patch releases), and it is built
# against the SPIRV-Headers commit it pins (spirv-headers-tag.conf), not vcpkg's.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO KhronosGroup/SPIRV-LLVM-Translator
	REF "v${VERSION}"
	SHA512 a64784a79495680895d138f083444e1f2b686caa65ab4a71a1eb8160173090835d76a1e95c7f0245f7fa435ab81aebcfbbf12ecb44afb7b0366b21f9c2260a06
	HEAD_REF main
	PATCHES
		# its pinned SPIRV-Headers before LLVM's include directory, the install tree's, which holds vcpkg's older
		# spirv-headers (SPIRV-Tools' dependency)
		0001-spirv-headers-before-llvm-includes.patch
)

file(READ "${SOURCE_PATH}/spirv-headers-tag.conf" SPIRV_HEADERS_TAG)
string(STRIP "${SPIRV_HEADERS_TAG}" SPIRV_HEADERS_TAG)
if(NOT SPIRV_HEADERS_TAG STREQUAL "cb42dec3830d3ac67fa449ecdc0c0f73d5e74498")
	message(FATAL_ERROR "SPIRV-LLVM-Translator pins SPIRV-Headers ${SPIRV_HEADERS_TAG}: update the headers' REF and SHA512")
endif()
vcpkg_from_github(
	OUT_SOURCE_PATH SPIRV_HEADERS_SOURCE_PATH
	REPO KhronosGroup/SPIRV-Headers
	REF "${SPIRV_HEADERS_TAG}"
	SHA512 66fb59a2e1ce535c9a9cd9e5364e423b2055d5d3e589bd95c6100c2390214a9dd8c34e0bb6308cd3b596b8af208daf4b85721a654594831cb0e2f2e602358114
	HEAD_REF main
)

# Ninja: LLVM's AddLLVM is untested with the FASTBuild generator
vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	GENERATOR Ninja
	OPTIONS
		"-DLLVM_DIR=${CURRENT_INSTALLED_DIR}/share/llvm"
		"-DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=${SPIRV_HEADERS_SOURCE_PATH}"
		-DLLVM_SPIRV_INCLUDE_TESTS=OFF
		-DLLVM_INCLUDE_TESTS=OFF
		-DCCACHE_ALLOWED=OFF
		-DBUILD_SHARED_LIBS=OFF
)
vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()

vcpkg_copy_tools(TOOL_NAMES llvm-spirv AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.TXT")
