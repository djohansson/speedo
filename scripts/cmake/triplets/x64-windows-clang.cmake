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
if(PORT STREQUAL "draco")
	# outside of CMake's MSVC check it builds a static and a shared library both named draco, whose import library
	# collides with the static one on windows ("multiple rules generate draco.lib"). link statically.
	set(VCPKG_LIBRARY_LINKAGE static)
endif()
if(PORT STREQUAL "miniz")
	# only slang uses it (zip and riff-deflate archives, which we don't use): no dll of its own to ship
	set(VCPKG_LIBRARY_LINKAGE static)
endif()
if(PORT STREQUAL "ktx")
	# its astc-encoder adds -ffp-model=precise -Werror, which overrides -ffast-math's complex range and so fails with
	# -Woverriding-complex-range
	string(APPEND VCPKG_C_FLAGS " -Wno-overriding-complex-range")
	string(APPEND VCPKG_CXX_FLAGS " -Wno-overriding-complex-range")
endif()

# vcpkg_copy_pdbs reads each dll's pdb path with dumpbin, which isn't on the path here (vcpkg loads no visual studio
# environment for a chainloaded toolchain), so no package got its pdbs. this replaces it (the triplet is included after
# vcpkg's scripts) with llvm-readobj, which reads the same path (lld writes it absolute, into the buildtree). it also
# runs after every portfile: many ports install their pdbs from cmake only under its MSVC check (glfw3, spirv-tools,
# vulkan-loader, ...) and don't call it themselves
set(VCPKG_POST_PORTFILE_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/../copy-pdbs.cmake")
function(vcpkg_copy_pdbs)
	cmake_parse_arguments(PARSE_ARGV 0 "arg" "" "" "BUILD_PATHS")
	if(NOT DEFINED arg_BUILD_PATHS)
		set(arg_BUILD_PATHS "${CURRENT_PACKAGES_DIR}/bin/*.dll" "${CURRENT_PACKAGES_DIR}/debug/bin/*.dll")
	endif()
	if(NOT VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
		return()
	endif()
	find_program(z_llvm_readobj llvm-readobj PATHS "$ENV{LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH REQUIRED)
	find_program(z_llvm_pdbutil llvm-pdbutil PATHS "$ENV{LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH REQUIRED)
	file(GLOB_RECURSE dlls ${arg_BUILD_PATHS})
	set(dlls_without_matching_pdbs "")
	foreach(dll IN LISTS dlls)
		execute_process(
			COMMAND "${z_llvm_readobj}" --coff-debug-directory "${dll}"
			OUTPUT_VARIABLE debug_directory
			ERROR_QUIET
			RESULT_VARIABLE error_code)
		if(error_code EQUAL "0" AND NOT debug_directory MATCHES "PDBGUID")
			continue() # built without one (mimalloc-redirect.dll, which mimalloc ships prebuilt)
		endif()
		set(pdb "")
		if(error_code EQUAL "0" AND debug_directory MATCHES "PDBGUID: ({[^}]+})")
			set(guid "${CMAKE_MATCH_1}")
			# (${CMAKE_MATCH_1} expands before MATCHES runs, so it can't be tested in the same if)
			set(candidate "")
			if(debug_directory MATCHES "PDBFileName: ([^\r\n]+[.][Pp][Dd][Bb])")
				set(candidate "${CMAKE_MATCH_1}")
			endif()
			if(candidate AND EXISTS "${candidate}")
				# some ports (miniz) link both configs to one pdb path: copy it only for the dll it belongs to
				execute_process(
					COMMAND "${z_llvm_pdbutil}" dump -summary "${candidate}"
					OUTPUT_VARIABLE summary
					ERROR_QUIET
					RESULT_VARIABLE error_code)
				string(FIND "${summary}" "GUID: ${guid}" guid_position)
				if(error_code EQUAL "0" AND NOT guid_position EQUAL "-1")
					set(pdb "${candidate}")
				endif()
			endif()
		endif()
		if(pdb)
			cmake_path(GET dll PARENT_PATH dll_dir)
			file(COPY "${pdb}" DESTINATION "${dll_dir}")
		else()
			list(APPEND dlls_without_matching_pdbs "${dll}")
		endif()
	endforeach()
	if(NOT dlls_without_matching_pdbs STREQUAL "")
		list(JOIN dlls_without_matching_pdbs "\n    " message)
		message(WARNING "Could not find a matching pdb file for:\n    ${message}\n")
	endif()
endfunction()
