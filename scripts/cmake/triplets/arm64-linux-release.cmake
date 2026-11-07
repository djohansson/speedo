set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_BUILD_TYPE release)
set(VCPKG_DISABLE_COMPILER_TRACKING ON)
# tracked in the packages' ABI, for the shared binary cache (see setup.ps1): the C library's version (GLIBC_VERSION, see
# linux.ps1), as the clang triplets track it. The host tools (LLVM) built against a newer glibc fail to load with an
# older one, so without it the cache would hand a machine on an older distribution a clang it can't run
set(VCPKG_ENV_PASSTHROUGH GLIBC_VERSION)
set(
	VCPKG_CMAKE_CONFIGURE_OPTIONS_RELEASE
		-DCMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS=ON
		-DCMAKE_FASTBUILD_USE_LIGHTCACHE=ON
)
set(VCPKG_C_FLAGS_RELEASE "-O3 -flto=thin -Wno-gnu-line-marker -Wno-missing-format-attribute")
set(VCPKG_CXX_FLAGS_RELEASE "-O3 -flto=thin -Wno-gnu-line-marker -Wno-missing-format-attribute")
