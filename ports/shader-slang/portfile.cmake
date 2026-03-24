vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/slang
	REF fda9068177d1386c079498ceea342af20190cc12
	SHA512 c819c8ec0824df38bb8953208435313b233b833774611e0d22b8267558d4c0b76236b1b9efcaf339756355e1c503be246b35671b69f25e7837a14414b1cc5985
	# PATCHES
	# 	0001-fix-fastbuild-duplicate-neural-shader-targets.patch
	# 	0002-fix-fastbuild-missing-capability-defs-target.patch
	# 	0003-fix-fastbuild-capability-generator-build-order.patch
	# 	0004-fix-fastbuild-static-build-glsl-module-install.patch
	HEAD_REF vcpkg-integration
)

if(VCPKG_TARGET_IS_WINDOWS)
	set(_shader_slang_visual_studio_path "$ENV{VISUAL_STUDIO_PATH}")
	string(REPLACE "\\" "/" _shader_slang_visual_studio_path "${_shader_slang_visual_studio_path}")
	set(_shader_slang_visual_studio_vctools_version "$ENV{VISUAL_STUDIO_VCTOOLS_VERSION}")
endif()

set(_shader_slang_configure_options
	-DSLANG_VERSION_FULL=${VERSION}
	-DSLANG_VERSION_NUMERIC=${VERSION}
	-DSLANG_ENABLE_PCH=ON
	-DSLANG_LIB_TYPE=SHARED
	-DSLANG_EMBED_CORE_MODULE=ON
	-DSLANG_EMBED_CORE_MODULE_SOURCE=OFF
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

if(VCPKG_TARGET_IS_WINDOWS)
	list(APPEND _shader_slang_configure_options
		-DVISUAL_STUDIO_PATH=${_shader_slang_visual_studio_path}
		-DVISUAL_STUDIO_VCTOOLS_VERSION=${_shader_slang_visual_studio_vctools_version}
	)
endif()

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	GENERATOR Ninja
	OPTIONS ${_shader_slang_configure_options}
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_fixup_pkgconfig()
if (VCPKG_TARGET_IS_WINDOWS)
	vcpkg_cmake_config_fixup(PACKAGE_NAME slang CONFIG_PATH cmake)
else()
	vcpkg_cmake_config_fixup(PACKAGE_NAME slang CONFIG_PATH lib/cmake/slang)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(TOOLS slangc slangd slangi)
vcpkg_copy_tools(TOOL_NAMES ${TOOLS} AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")