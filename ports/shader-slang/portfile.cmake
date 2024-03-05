vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/slang
	REF 55f1e5fcbbfa9fd64a0f4eef52e594fdbbab296d #v${VERSION}
	SHA512 0f035c105d35f3a8d67d8003881463b181d95176489264726d43f790792abe492b8a1c90f6f9cfcd4287a9870f02804c22a68683712c210e0bd0013a78230790
	HEAD_REF master
	PATCHES
		clang-windows.patch
		clang-windows-2.patch
		vcpkg.patch
		spirv-tools-windows-link.patch
		slang-lookup-generator-vcpkg.patch
		windows-link.patch
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DSLANG_VERSION=${VERSION}
		-DSLANG_SLANG_LLVM_FLAVOR=DISABLE
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