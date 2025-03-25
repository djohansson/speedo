vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO shader-slang/slang
	REF v${VERSION}
	SHA512 a3054f1318952fa28fe4d3fea91d9214459667251f8850f0533de047dfa97fd00f7317ddef33df03e8d40586f7dbd3d9125c0af0a41157a481424d072a0e510b
	HEAD_REF master
	PATCHES
		0001-remove-SPIRV-Headers.patch
		0002-miniz-include-path.patch
		0003-find-lz4-and-miniz.patch
		0004-generators-path-fix.patch
		0005-use-system-unordered_dense.patch
		0006-add-SLANG_USE_SYSTEM_GLSLANG.patch
		0007-change-MSVC-to-WIN32.patch
		0008-msvc-include-crtdgb.h.patch
		0009-temp-lld-patch.patch
		0010-msvc-add-intrin.h-include.patch
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DSLANG_VERSION_FULL=${VERSION}
		-DSLANG_SLANG_LLVM_FLAVOR=DISABLE
		-DSLANG_ENABLE_GFX=OFF
		-DSLANG_ENABLE_DXIL=OFF
		-DSLANG_ENABLE_EXAMPLES=OFF
		-DSLANG_ENABLE_PREBUILT_BINARIES=OFF
		-DSLANG_ENABLE_SLANG_GLSLANG=ON
		-DSLANG_ENABLE_SLANG_RHI=OFF
		-DSLANG_ENABLE_TESTS=OFF
		-DSLANG_USE_SYSTEM_MINIZ=ON
		-DSLANG_USE_SYSTEM_LZ4=ON
		-DSLANG_USE_SYSTEM_VULKAN_HEADERS=ON
		-DSLANG_USE_SYSTEM_SPIRV_HEADERS=ON
		-DSLANG_USE_SYSTEM_UNORDERED_DENSE=ON
		-DSLANG_USE_SYSTEM_GLSLANG=ON
		-DSLANG_SPIRV_HEADERS_INCLUDE_DIR=${CURRENT_INSTALLED_DIR}/include
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(PACKAGE_NAME slang CONFIG_PATH cmake)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(TOOLS slangc slangd)
vcpkg_copy_tools(TOOL_NAMES ${TOOLS} AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")