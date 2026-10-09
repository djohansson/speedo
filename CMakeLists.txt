###############################################################################

cmake_minimum_required(VERSION 3.10)

set(SPEEDO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

message("Parsing ${SPEEDO_SOURCE_DIR}/version.h file")

file(READ "${SPEEDO_SOURCE_DIR}/version.h" version)

string(REGEX MATCH "Major = ([0-9]+)" _ ${version})
set(SPEEDO_VERSION_MAJOR ${CMAKE_MATCH_1})

string(REGEX MATCH "Minor = ([0-9]+)" _ ${version})
set(SPEEDO_VERSION_MINOR ${CMAKE_MATCH_1})

string(REGEX MATCH "Patch = ([0-9]+)" _ ${version})
set(SPEEDO_VERSION_PATCH ${CMAKE_MATCH_1})

set(SPEEDO_VERSION_STRING "${SPEEDO_VERSION_MAJOR}.${SPEEDO_VERSION_MINOR}.${SPEEDO_VERSION_PATCH}")

message("SPEEDO - VERSION ${SPEEDO_VERSION_STRING}")

###############################################################################

# SDKROOT is baked into CMakeUserPresets.json by setup.ps1. Fail before vcpkg install runs if the
# installed macOS SDK has changed since then (e.g. after an OS or Command Line Tools update).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND DEFINED ENV{SDKROOT})
	execute_process(
		COMMAND xcrun --sdk macosx --show-sdk-path
		OUTPUT_VARIABLE SPEEDO_CURRENT_SDKROOT
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	file(REAL_PATH "${SPEEDO_CURRENT_SDKROOT}" SPEEDO_CURRENT_SDKROOT)
	file(REAL_PATH "$ENV{SDKROOT}" SPEEDO_CONFIGURED_SDKROOT)
	if(NOT SPEEDO_CURRENT_SDKROOT STREQUAL SPEEDO_CONFIGURED_SDKROOT)
		message(FATAL_ERROR
			"macOS SDK has changed since setup.ps1 was last run:\n"
			"  configured: ${SPEEDO_CONFIGURED_SDKROOT}\n"
			"  installed:  ${SPEEDO_CURRENT_SDKROOT}\n"
			"Run `pwsh ./setup.ps1` to regenerate presets and clear stale build caches.")
	endif()
endif()

###############################################################################

# option(BUILD_DEPENDENCIES "Build dependencies" ON)
# if(BUILD_DEPENDENCIES)
#   list(APPEND VCPKG_MANIFEST_FEATURES client server)
# endif()

###############################################################################

project(speedo LANGUAGES C CXX VERSION ${SPEEDO_VERSION_STRING})

# use -std=c++26 / -std=c23 rather than the gnu++26 / gnu23 dialects
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

# the toolchain's debug flags use -Og (shared with vcpkg dependencies via the triplets' chainload toolchain);
# fully disable optimizations for our own targets so they are easier to step through in the debugger.
# appended after CMAKE_<LANG>_FLAGS_DEBUG, so this -O0 wins over the toolchain's -Og.
add_compile_options($<$<CONFIG:debug>:-O0>)

find_package(Vulkan REQUIRED)
if(TARGET Vulkan::Vulkan)
	# FindVulkan.cmake only sets a generic IMPORTED_LOCATION, which CMake's config-mapping
	# lookup (CMAKE_MAP_IMPORTED_CONFIG_PROFILE -> release) does not fall back to, leaving
	# Vulkan::Vulkan as -NOTFOUND for the profile config.
	set_property(TARGET Vulkan::Vulkan PROPERTY IMPORTED_LOCATION_RELEASE "${Vulkan_LIBRARIES}")
endif()
find_package(cpptrace CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(SHA-Intrinsics CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(cppzmq CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)
find_package(Cargs CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_package(libdeflate CONFIG REQUIRED)

if(DEFINED CMAKE_SYSTEM_NAME AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL ""))
	find_program(MINJECT NAMES minject.exe minject32.exe HINTS ENV PATH REQUIRED)
endif()

###############################################################################

file(GLOB CORE_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/core/*.cpp)
add_library(core STATIC	${CORE_SOURCE_FILES})
target_compile_features(
	core
	PUBLIC
		cxx_std_26
)
target_compile_definitions(
	core
	PUBLIC
		SPEEDO_PROFILING_LEVEL=$<IF:$<CONFIG:release>,0,1>
		SPEEDO_USE_MIMALLOC
)
target_include_directories(
	core
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	core
	PRIVATE
		$<IF:$<TARGET_EXISTS:libdeflate::libdeflate_shared>,libdeflate::libdeflate_shared,libdeflate::libdeflate_static>
	PUBLIC
		SHA-Intrinsics
		cpptrace::cpptrace
		stduuid
		Tracy::TracyClient
)

###############################################################################

# windows and the window system: the windowed application base (its window, main thread work, exit wake-ups), the file
# dialogs and imgui's platform side, between core and rhi (whose surfaces are made for the platform's windows)
file(GLOB PLATFORM_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/platform/*.cpp)
add_library(platform STATIC ${PLATFORM_SOURCE_FILES})

target_compile_features(
	platform
	PUBLIC
		cxx_std_26
)
target_include_directories(
	platform
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	platform
	PUBLIC
		core
	PRIVATE
		glfw
		imgui::imgui
		nfd::nfd
)

###############################################################################

file(GLOB_RECURSE GFX_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/gfx/*.cpp)
add_library(gfx STATIC ${GFX_SOURCE_FILES})

target_compile_features(
	gfx
	PUBLIC
		cxx_std_26
)
target_include_directories(
	gfx
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
# the installed versions of the import libraries, which the asset caches of their output depend on (see
# gfx/importversions.h.in). read from vcpkg's package metadata, since the libraries don't define their versions.
function(speedo_port_version port outVar)
	set(spdxFile "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/vcpkg.spdx.json")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${spdxFile}")
	file(READ "${spdxFile}" spdx)
	string(JSON packageCount LENGTH "${spdx}" packages)
	math(EXPR lastPackage "${packageCount} - 1")
	foreach(packageIndex RANGE ${lastPackage})
		string(JSON name GET "${spdx}" packages ${packageIndex} name)
		if(name STREQUAL port)
			string(JSON version GET "${spdx}" packages ${packageIndex} versionInfo)
			set(${outVar} "${version}" PARENT_SCOPE)
			return()
		endif()
	endforeach()
	message(FATAL_ERROR "No version of ${port} in ${spdxFile}")
endfunction()

speedo_port_version(stb SPEEDO_STB_VERSION)
speedo_port_version(tinyobjloader SPEEDO_TINYOBJLOADER_VERSION)
speedo_port_version(cgltf SPEEDO_CGLTF_VERSION)
speedo_port_version(libwebp SPEEDO_LIBWEBP_VERSION)
speedo_port_version(ktx SPEEDO_KTX_VERSION)
speedo_port_version(draco SPEEDO_DRACO_VERSION)
speedo_port_version(meshoptimizer SPEEDO_MESHOPTIMIZER_VERSION)
configure_file(${SPEEDO_SOURCE_DIR}/gfx/importversions.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/gfx/importversions.h @ONLY)
target_include_directories(
	gfx
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(
	gfx
	PUBLIC
		core
		platform
		rhi
		xxHash::xxhash
		imgui::imgui
		glm::glm
		slang::slang
	PRIVATE
		WebP::webpdecoder
		WebP::webpdemux
		KTX::ktx
		draco::draco
		meshoptimizer::meshoptimizer
		mikktspace::mikktspace
)

###############################################################################

file(GLOB_RECURSE RHI_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/rhi/*.cpp)
add_library(rhi STATIC ${RHI_SOURCE_FILES})

target_compile_features(
	rhi
	PUBLIC
		cxx_std_26
)
target_compile_definitions(
	rhi
	PUBLIC
		SPEEDO_GRAPHICS_VALIDATION_LEVEL=$<IF:$<CONFIG:debug>,1,0>
		GLM_FORCE_XYZW_ONLY
)
target_include_directories(
	rhi
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	rhi
	PUBLIC
		core
		platform
		xxHash::xxhash
		imgui::imgui
		Vulkan::Vulkan
	PRIVATE
		glfw # vulkan surfaces of the platform's (glfw) windows
)

###############################################################################

file(GLOB SERVERLIB_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/server/*.cpp)
add_library(serverlib STATIC ${SERVERLIB_SOURCE_FILES})

set_property(
	TARGET
		serverlib
	PROPERTY OUTPUT_NAME
		server
)
target_compile_features(
	serverlib
	PUBLIC
		cxx_std_26
)
target_compile_definitions(
	serverlib
	PUBLIC
		ZMQ_BUILD_DRAFT_API
)
target_include_directories(
	serverlib
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	serverlib
	PUBLIC
		core
		cppzmq-static
)

###############################################################################

file(GLOB CLIENTLIB_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/client/*.cpp)
add_library(clientlib STATIC ${CLIENTLIB_SOURCE_FILES})

set_property(
	TARGET
		clientlib
	PROPERTY OUTPUT_NAME
		client
)
target_compile_features(
	clientlib
	PUBLIC
		cxx_std_26
)
target_include_directories(
	clientlib
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	clientlib
	PUBLIC
		gfx
	PRIVATE
		serverlib # todo: redesign so that we can remove this (quite unsafe) dependency
)

###############################################################################

file(GLOB SERVER_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/server/*.c)
add_executable(server ${SERVER_SOURCE_FILES})

target_compile_features(
	server
	PUBLIC
		c_std_23
)
target_include_directories(
	server
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	server
	PRIVATE
		$<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
		cargs
		serverlib
		$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation>>
)
if(DEFINED CMAKE_SYSTEM_NAME AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL ""))
	add_custom_command(
		TARGET server POST_BUILD
		COMMAND ${MINJECT} -v -f -i --postfix=secure$<$<CONFIG:debug>:-debug> $<TARGET_FILE:server>
		COMMAND_EXPAND_LISTS
		VERBATIM
	)
endif()

###############################################################################

file(GLOB CLIENT_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/client/*.c)
add_executable(client ${CLIENT_SOURCE_FILES})

target_compile_features(
	client
	PUBLIC
		c_std_23
)
target_include_directories(
	client
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	client
	PRIVATE
		$<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
		cargs
		glfw
		gfx
		rhi
		clientlib
		$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation>>
)
if(DEFINED CMAKE_SYSTEM_NAME AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL ""))
	add_custom_command(
		TARGET client POST_BUILD
		COMMAND ${MINJECT} -v -f -i --postfix=secure$<$<CONFIG:debug>:-debug> $<TARGET_FILE:client>
		COMMAND_EXPAND_LISTS
		VERBATIM
	)
endif()

###############################################################################

# imports models and images like the client does and checks the results, see src/tools/assettest.cpp and
# scripts/assettest.ps1
#
# it runs the environment's prefiltering kernels (src/gfx/shaders/environment.slang) on the cpu, as slang's c++ target
# compiles them: the same kernels the client runs on the gpu (see src/tools/environmentkernels.cpp)
set(ENVIRONMENT_KERNELS_SOURCE ${SPEEDO_SOURCE_DIR}/gfx/shaders/environment.slang)
set(ENVIRONMENT_KERNELS_CPP ${CMAKE_CURRENT_BINARY_DIR}/environmentkernels.generated.cpp)
set(ENVIRONMENT_KERNELS_ENTRY_POINTS)
foreach(entry EnvironmentSky EnvironmentDownsample EnvironmentCopy EnvironmentSpecular EnvironmentSheen EnvironmentIrradiance)
	list(APPEND ENVIRONMENT_KERNELS_ENTRY_POINTS -entry ${entry} -stage compute)
endforeach()
add_custom_command(
	OUTPUT ${ENVIRONMENT_KERNELS_CPP}
	COMMAND
		${SLANGC_EXECUTABLE} ${ENVIRONMENT_KERNELS_SOURCE} -I ${SPEEDO_SOURCE_DIR}/gfx/shaders -DSHADERTYPES_H_GPU_TARGET=true
		-target cpp ${ENVIRONMENT_KERNELS_ENTRY_POINTS} -o ${ENVIRONMENT_KERNELS_CPP}
	DEPENDS
		${ENVIRONMENT_KERNELS_SOURCE}
		${SPEEDO_SOURCE_DIR}/gfx/shaders/capi.h
		${SPEEDO_SOURCE_DIR}/gfx/shaders/sky.h
	COMMENT "Compiling the environment kernels to c++"
	VERBATIM
)
# generated: its warnings aren't ours
set_source_files_properties(${ENVIRONMENT_KERNELS_CPP} PROPERTIES COMPILE_OPTIONS "-w")
add_executable(
	assettest
	${SPEEDO_SOURCE_DIR}/tools/assettest.cpp
	${SPEEDO_SOURCE_DIR}/tools/environmentkernels.cpp
	${SPEEDO_SOURCE_DIR}/tools/environmentkernelsbridge.cpp
	${ENVIRONMENT_KERNELS_CPP}
)

target_compile_features(
	assettest
	PUBLIC
		cxx_std_26
)
target_link_libraries(
	assettest
	PRIVATE
		$<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
		gfx
)

###############################################################################

# compares ways of reading files (std streams, stdio, core's memory mapped files), warm and cold, and times the
# importers (--import) and core::zip (--zip), see src/tools/filebench.cpp
add_executable(filebench ${SPEEDO_SOURCE_DIR}/tools/filebench.cpp)
target_compile_features(
	filebench
	PUBLIC
		cxx_std_26
)
target_link_libraries(
	filebench
	PRIVATE
		$<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
		core
		gfx
		xxHash::xxhash
)

###############################################################################

install(
	TARGETS
		core
		gfx
		platform
		rhi
		serverlib
		clientlib
		server
		client
	EXPORT speedoTargets
	RUNTIME DESTINATION $<IF:$<CONFIG:debug>,debug/bin,bin>
	LIBRARY DESTINATION $<IF:$<CONFIG:debug>,debug/lib,lib>
	ARCHIVE DESTINATION $<IF:$<CONFIG:debug>,debug/lib,lib>
	INCLUDES DESTINATION include/speedo
)

###############################################################################
