# speedo overlay, not in vcpkg's registry: for the mesa port (mesa_clc). Static, as LLVM is: a shared library with LLVM
# linked in beside mesa's static LLVM would register LLVM's options twice. Its version follows LLVM's, which mesa
# checks, and it is built against the SPIRV-Headers commit it pins (spirv-headers-tag.conf), not vcpkg's.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO KhronosGroup/SPIRV-LLVM-Translator
	REF "v${VERSION}"
	SHA512 fff2224f63926819e29d5f2daa54015b486497f131ecf4b563cfde75a594812a5c395b67971022be13de42d7429db1fc747c725c15978894baa3a62013d98777
	HEAD_REF main
	PATCHES
		# its pinned SPIRV-Headers before LLVM's include directory, the install tree's, which holds vcpkg's older
		# spirv-headers (SPIRV-Tools' dependency)
		0001-spirv-headers-before-llvm-includes.patch
)

file(READ "${SOURCE_PATH}/spirv-headers-tag.conf" SPIRV_HEADERS_TAG)
string(STRIP "${SPIRV_HEADERS_TAG}" SPIRV_HEADERS_TAG)
if(NOT SPIRV_HEADERS_TAG STREQUAL "575b6512579ebde466ed3dfc04e413439d14d95d")
	message(FATAL_ERROR "SPIRV-LLVM-Translator pins SPIRV-Headers ${SPIRV_HEADERS_TAG}: update the headers' REF and SHA512")
endif()
vcpkg_from_github(
	OUT_SOURCE_PATH SPIRV_HEADERS_SOURCE_PATH
	REPO KhronosGroup/SPIRV-Headers
	REF "${SPIRV_HEADERS_TAG}"
	SHA512 bbff94d40149d529d4c329f0d73597d8f404202198fd4f35316585915919254e7ead5a1caa928dba9f04591423da6bc33f73a308fc26d95de4bfa6eecb47384c
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
