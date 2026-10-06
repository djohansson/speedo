set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# these needs to be set both in cmake presets as well as here for both regular vcpkg invocation and cmake invocations from presets (e.g. vscode cmake extension)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang.toolchain.cmake)
set(VCPKG_DISABLE_COMPILER_TRACKING ON) # This target is not compiled yet when vcpkg wants to calculate the compiler hash.
#

set(
	VCPKG_ENV_PASSTHROUGH_UNTRACKED
		LLVM_ROOT
		LLVM_TOOLS_BINARY_DIR
		FASTBUILD_TEMP_PATH
		FASTBUILD_BROKERAGE_PATH
		FASTBUILD_WORKER
		FASTBUILD_CACHE_PATH
		FASTBUILD_CACHE_PATH_MOUNT_POINT
		FASTBUILD_CACHE_MODE
)
set(
	VCPKG_CMAKE_CONFIGURE_OPTIONS
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
		-DCMAKE_MAP_IMPORTED_CONFIG_PROFILE='profile;release'
		-DCMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS=ON
		-DCMAKE_FASTBUILD_USE_LIGHTCACHE=ON
)

# per-port options, so that the upstream ports can be used without an overlay
if(PORT STREQUAL "spirv-tools")
	# we override the global allocator with mimalloc ourselves
	list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DSPIRV_TOOLS_USE_MIMALLOC=OFF)
endif()
if(PORT STREQUAL "mimalloc")
	# as C++, mimalloc statically links its own copy of libc++/libc++abi into the shared library and exports it
	# (__cxa_throw, operator new, ...), giving the process two C++ runtimes ("mi_free: invalid pointer")
	list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DMI_USE_CXX=OFF)
endif()
if(PORT STREQUAL "ktx")
	# its astc-encoder adds -ffp-model=precise -Werror, which overrides -ffast-math's complex range and so fails with
	# -Woverriding-complex-range
	string(APPEND VCPKG_C_FLAGS " -Wno-overriding-complex-range")
	string(APPEND VCPKG_CXX_FLAGS " -Wno-overriding-complex-range")
endif()
