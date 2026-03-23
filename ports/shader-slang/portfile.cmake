vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/slang
	REF a43b4e194a8788ce77c2e59216343943cb25ad4f
	SHA512 8b086295b682e1020d95220ad191fea393fd8314bbab4e72bbb25b724b9842317dd007bb3a3737396c066df1b2c5bb09d87dcb4900aefd8592ba6f554442921f
	PATCHES
		0001-fix-fastbuild-duplicate-neural-shader-targets.patch
		0002-fix-fastbuild-missing-capability-defs-target.patch
		0003-fix-fastbuild-capability-generator-build-order.patch
		0004-fix-fastbuild-static-build-glsl-module-install.patch
	HEAD_REF vcpkg-integration
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DSLANG_VERSION_FULL=${VERSION}
		-DSLANG_VERSION_NUMERIC=${VERSION}
		-DSLANG_ENABLE_PCH=OFF
		-DSLANG_LIB_TYPE=STATIC
		-DSLANG_EMBED_CORE_MODULE=OFF
		-DSLANG_EMBED_CORE_MODULE_SOURCE=ON
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
		-DSLANG_USE_SYSTEM_SPIRV_TOOLS=ON
		-DSLANG_USE_SYSTEM_UNORDERED_DENSE=ON
		-DSLANG_USE_SYSTEM_GLSLANG=ON
		-DSLANG_USE_SYSTEM_LUA=ON
		-DSLANG_USE_SYSTEM_STB=ON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_fixup_pkgconfig()
if (VCPKG_TARGET_IS_WINDOWS)
	vcpkg_cmake_config_fixup(PACKAGE_NAME shader-slang CONFIG_PATH cmake)
else()
	vcpkg_cmake_config_fixup(PACKAGE_NAME shader-slang CONFIG_PATH lib/cmake/shader-slang)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(TOOLS slangc slangd slangi)
vcpkg_copy_tools(TOOL_NAMES ${TOOLS} AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")