vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/slang
	REF fa49a6eb2dfef2abfe3e92d1993b7a843047c56e #v${VERSION}
	SHA512 23352e5979f591a3ad8d67a6a9ec8bb160f84f9d3fdc422cd76f3071acdcd05cf9003f475c34635137480b79b37d7cf4636c88a9dce57f0320463508db821932
	HEAD_REF master
	# PATCHES
	# 	clang-windows.patch
	# 	clang-windows-2.patch
	# 	remove-external.patch
	# 	0001-find-nvapi.patch
	# 	0002-dxil-support-option.patch
	# 	0003-imgui_impl_win32-include.patch
	# 	0004-cast-to-int64.patch
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DSLANG_VERSION_FULL=${VERSION}
		-DSLANG_ENABLE_GFX=OFF
		-DSLANG_SLANG_LLVM_FLAVOR=DISABLE
		-DSLANG_ENABLE_DXIL=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

#vcpkg_cmake_config_fixup()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(TOOLS slangc slangd)
vcpkg_copy_tools(TOOL_NAMES ${TOOLS} AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")