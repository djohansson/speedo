vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/slang
	REF 62b7219e715bd4c0f984bcd98c9767fb6422c78f #v${VERSION}
	SHA512 18f7c11f5675267e9429b683b9f26cca1fd9079668e7582561176e15e9575949c4731b221af41d4300f9a096ae127a8de53c478c2b53924b152adec01b5f93ea
	HEAD_REF master
	PATCHES
		clang-windows.patch
		clang-windows-2.patch
		remove-external.patch
		0001-find-nvapi.patch
		0002-dxil-support-option.patch
		0003-imgui_impl_win32-include.patch
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DSLANG_VERSION=${VERSION}
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