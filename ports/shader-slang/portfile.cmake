vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/slang
	REF 7e7d46cf06c6863520ccd8d7cc26b70141adc145 #v${VERSION}
	SHA512 d0f7bc146107f81b62e81bad3ced8548fd904a82686909de058b902066196daa72d1492e5e638db98518f55b263320ee8d94be97055b719cfd88011a0746d6ab
	HEAD_REF master
	PATCHES
		0001-include-process.h.patch
		#0002-include-intrin.h.patch // TODO: fix patch on win32
		0003-add-crtdbg.h-includes.patch
		0004-cxx20-fixes.patch
		0005-remove-vcpkg-dependency.patch
		0006-add-package-cruft.patch
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DSLANG_VERSION_FULL=${VERSION}
		-DSLANG_ENABLE_GFX=OFF
		-DSLANG_SLANG_LLVM_FLAVOR=DISABLE
		-DSLANG_ENABLE_DXIL=OFF
		-DSLANG_ENABLE_PREBUILT_BINARIES=OFF
	MAYBE_UNUSED_VARIABLES
		SLANG_ENABLE_DXIL
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(PACKAGE_NAME slang CONFIG_PATH lib/cmake/slang)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(TOOLS slangc slangd)
vcpkg_copy_tools(TOOL_NAMES ${TOOLS} AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")