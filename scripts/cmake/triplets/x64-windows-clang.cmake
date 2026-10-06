set(VCPKG_TARGET_ARCHITECTURE x64)

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
		VISUAL_STUDIO_PATH
		VISUAL_STUDIO_VCTOOLS_VERSION
		WINDOWS_SDK_PATH
		WINDOWS_SDK_VERSION
)
set(
	VCPKG_CMAKE_CONFIGURE_OPTIONS
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
		-DCMAKE_MAP_IMPORTED_CONFIG_PROFILE='profile;release'
		-DCMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS=ON
		-DCMAKE_FASTBUILD_USE_LIGHTCACHE=ON
)

# per-port options, so that the upstream ports can be used without an overlay
if(PORT STREQUAL "zeromq")
	list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DPOLLER=epoll)
endif()
if(PORT STREQUAL "spirv-tools")
	# we override the global allocator with mimalloc ourselves
	list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DSPIRV_TOOLS_USE_MIMALLOC=OFF)
endif()
if(PORT STREQUAL "mimalloc")
	# as C++, mimalloc statically links its own copy of libc++/libc++abi into the shared library and exports it
	# (__cxa_throw, operator new, ...), giving the process two C++ runtimes ("mi_free: invalid pointer")
	list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DMI_USE_CXX=OFF)
endif()
if(PORT STREQUAL "zstd" OR PORT STREQUAL "pugixml")
	# they only dllexport under CMake's MSVC check, which is false for clang's GNU driver, so their DLLs export nothing
	# ("undefined symbol: __declspec(dllimport) ZSTD_createDStream" when linking the tracy profiler). link statically.
	set(VCPKG_LIBRARY_LINKAGE static)
endif()
