set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)

set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_BUILD_TYPE release)
set(VCPKG_DISABLE_COMPILER_TRACKING ON)
# tracked in the packages' ABI, for the shared binary cache (see setup.ps1): the oldest macOS they run on, the SDK's major
# version's first (MACOSX_DEPLOYMENT_TARGET, see osx.ps1, which every compile reads). The host tools (LLVM, mesa's
# KosmicKrisp) were built for the SDK's own version (27.0 then) and tracked nothing, so the cache would have handed a Mac on
# an older macOS a clang it can't run
set(VCPKG_ENV_PASSTHROUGH MACOSX_DEPLOYMENT_TARGET)
set(
	VCPKG_CMAKE_CONFIGURE_OPTIONS_RELEASE
		-DCMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS=ON
		-DCMAKE_FASTBUILD_USE_LIGHTCACHE=ON
)
set(VCPKG_C_FLAGS_RELEASE "-O3 -flto=thin -Wno-gnu-line-marker -Wno-missing-format-attribute")
set(VCPKG_CXX_FLAGS_RELEASE "-O3 -flto=thin -Wno-gnu-line-marker -Wno-missing-format-attribute")
# vcpkg's llvm port (ports/llvm is it, but for a Windows fix): built with Ninja, since CMake's FASTBuild generator can't
# build it (see docs/notes/build.md); LLVM's libc++ and libc++abi static, since the system's libc++.1.dylib and libc++abi
# aren't ABI compatible with them (the target packages link them statically); clangd without its XPC service, which
# nothing here uses
if(PORT STREQUAL "llvm")
	set(VCPKG_CMAKE_GENERATOR Ninja)
	list(
		APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
			-DLIBCXX_ENABLE_SHARED=OFF
			-DLIBCXX_ENABLE_STATIC=ON
			-DLIBCXX_HERMETIC_STATIC_LIBRARY=ON
			-DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
			-DLIBCXXABI_ENABLE_SHARED=OFF
			-DLIBCXXABI_ENABLE_STATIC=ON
			-DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON
			-DCLANGD_BUILD_XPC=OFF
	)
endif()
