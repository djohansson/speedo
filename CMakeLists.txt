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

# sanitizers (-fsanitize=, e.g. address,undefined or thread) for our own targets: the vcpkg packages aren't instrumented.
# they replace malloc themselves, so the executables are built without mimalloc (whose dylib interposes malloc)
set(SPEEDO_SANITIZE "" CACHE STRING "Sanitizers for our own targets (-fsanitize=), e.g. address,undefined or thread")
if(SPEEDO_SANITIZE)
	add_compile_options(
		-fsanitize=${SPEEDO_SANITIZE}
		-fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/sanitize-ignorelist.txt
		-fno-sanitize-recover=all
		-fno-omit-frame-pointer)
	add_link_options(-fsanitize=${SPEEDO_SANITIZE})
	add_compile_definitions(SPEEDO_SANITIZE)
	set(SPEEDO_USE_MIMALLOC OFF)
	set(SPEEDO_MIMALLOC "")
else()
	set(SPEEDO_USE_MIMALLOC ON)
	set(SPEEDO_MIMALLOC $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>)
endif()

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
find_package(WebP CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_package(libdeflate CONFIG REQUIRED)

if(DEFINED CMAKE_SYSTEM_NAME AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "") AND SPEEDO_USE_MIMALLOC)
	find_program(MINJECT NAMES minject.exe minject32.exe HINTS ENV PATH REQUIRED)
endif()

###############################################################################

set(CORE_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/core/application.cpp
	${SPEEDO_SOURCE_DIR}/core/eventhandlers.cpp
	${SPEEDO_SOURCE_DIR}/core/file.cpp
	${SPEEDO_SOURCE_DIR}/core/math.cpp
	${SPEEDO_SOURCE_DIR}/core/nodegraph.cpp
	${SPEEDO_SOURCE_DIR}/core/process.cpp
	${SPEEDO_SOURCE_DIR}/core/std_extra.cpp
	${SPEEDO_SOURCE_DIR}/core/task.cpp
	${SPEEDO_SOURCE_DIR}/core/taskexecutor.cpp
	${SPEEDO_SOURCE_DIR}/core/time.cpp
	${SPEEDO_SOURCE_DIR}/core/upgradablesharedmutex.cpp
	${SPEEDO_SOURCE_DIR}/core/zip.cpp)
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
		SPEEDO_VERSION="${SPEEDO_VERSION_STRING}" # crash reports name it
		$<$<BOOL:${SPEEDO_USE_MIMALLOC}>:SPEEDO_USE_MIMALLOC>
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
		cargs # the executables' arguments (ParseArguments)
	PUBLIC
		SHA-Intrinsics
		cpptrace::cpptrace
		stduuid
		Tracy::TracyClient
)

###############################################################################

# windows and the window system: the windowed application base (its window, main thread work, exit wake-ups), the file
# dialogs and imgui's platform side, between core and rhi (whose surfaces are made for the platform's windows)
set(PLATFORM_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/platform/glfw.cpp
	${SPEEDO_SOURCE_DIR}/platform/imguiplatform.cpp
	${SPEEDO_SOURCE_DIR}/platform/windowedapplication.cpp)
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
		Vulkan::Headers # glfwCreateWindowSurface's types (PlatformCreateVulkanSurface)
)

###############################################################################

set(GFX_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/gfx/accelerationstructures.cpp
	${SPEEDO_SOURCE_DIR}/gfx/bvh.cpp
	${SPEEDO_SOURCE_DIR}/gfx/camera.cpp
	${SPEEDO_SOURCE_DIR}/gfx/clusters.cpp
	${SPEEDO_SOURCE_DIR}/gfx/drawlist.cpp
	${SPEEDO_SOURCE_DIR}/gfx/emitters.cpp
	${SPEEDO_SOURCE_DIR}/gfx/environment.cpp
	${SPEEDO_SOURCE_DIR}/gfx/environmentfilter.cpp
	${SPEEDO_SOURCE_DIR}/gfx/framegraph.cpp
	${SPEEDO_SOURCE_DIR}/gfx/gltfimport.cpp
	${SPEEDO_SOURCE_DIR}/gfx/graphicsqueue.cpp
	${SPEEDO_SOURCE_DIR}/gfx/imageimport.cpp
	${SPEEDO_SOURCE_DIR}/gfx/imgui_extra.cpp
	${SPEEDO_SOURCE_DIR}/gfx/imguilayer.cpp
	${SPEEDO_SOURCE_DIR}/gfx/indirectlight.cpp
	${SPEEDO_SOURCE_DIR}/gfx/lightgizmos.cpp
	${SPEEDO_SOURCE_DIR}/gfx/meshimport.cpp
	${SPEEDO_SOURCE_DIR}/gfx/model.cpp
	${SPEEDO_SOURCE_DIR}/gfx/objimport.cpp
	${SPEEDO_SOURCE_DIR}/gfx/rayquerywatch.cpp
	${SPEEDO_SOURCE_DIR}/gfx/renderer.cpp
	${SPEEDO_SOURCE_DIR}/gfx/renderoutput.cpp
	${SPEEDO_SOURCE_DIR}/gfx/scene.cpp
	${SPEEDO_SOURCE_DIR}/gfx/sceneanimation.cpp
	${SPEEDO_SOURCE_DIR}/gfx/shaderloader.cpp
	${SPEEDO_SOURCE_DIR}/gfx/shadows.cpp
	${SPEEDO_SOURCE_DIR}/gfx/tangents.cpp
	${SPEEDO_SOURCE_DIR}/gfx/texture.cpp
	${SPEEDO_SOURCE_DIR}/gfx/views.cpp)
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

set(RHI_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/accelerationstructure.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/buffer.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/command.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/commandencoder.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/crashreport.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/descriptorset.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/device.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/deviceobject.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/fence.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/frame.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/image.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/imguirenderer.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/instance.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/memory.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/pipeline.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/pipelinecache.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/pipelinedescriptors.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/pipelinelayout.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/querypool.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/gpuprofiler.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/queue.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/renderimageset.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/rhi.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/sampler.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/semaphore.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/shader.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/swapchain.cpp
	${SPEEDO_SOURCE_DIR}/rhi/vulkan/utils.cpp)
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
)

###############################################################################

set(SERVERLIB_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/server/server.cpp)
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

set(CLIENTLIB_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/client/browser.cpp
	${SPEEDO_SOURCE_DIR}/client/client.cpp
	${SPEEDO_SOURCE_DIR}/client/loading.cpp
	${SPEEDO_SOURCE_DIR}/client/preview.cpp
	${SPEEDO_SOURCE_DIR}/client/thumbnails.cpp
	${SPEEDO_SOURCE_DIR}/client/ui.cpp
	${SPEEDO_SOURCE_DIR}/client/windowedapplication.cpp)
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

set(SERVER_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/server/main.c)
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
		${SPEEDO_MIMALLOC}
		serverlib
		$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation>>
)
if(DEFINED CMAKE_SYSTEM_NAME AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "") AND SPEEDO_USE_MIMALLOC)
	add_custom_command(
		TARGET server POST_BUILD
		COMMAND ${MINJECT} -v -f -i --postfix=secure$<$<CONFIG:debug>:-debug> $<TARGET_FILE:server>
		COMMAND_EXPAND_LISTS
		VERBATIM
	)
endif()

###############################################################################

set(CLIENT_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/client/main.c)
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
		${SPEEDO_MIMALLOC}
		gfx
		rhi
		clientlib
		$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation>>
)
if(DEFINED CMAKE_SYSTEM_NAME AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "") AND SPEEDO_USE_MIMALLOC)
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
foreach(entry EnvironmentSky EnvironmentDownsample EnvironmentCopy EnvironmentSpecular EnvironmentSheen EnvironmentIrradiance
		EnvironmentSkyTransmittance EnvironmentSkyMultipleScattering EnvironmentSkyView)
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
# and the indirect lighting's BVH traversal (src/gfx/shaders/bvh.slang), checked against brute force (see
# src/tools/bvhkernelsbridge.h)
set(BVH_KERNELS_SOURCE ${SPEEDO_SOURCE_DIR}/tools/bvhkernels.slang)
set(BVH_KERNELS_CPP ${CMAKE_CURRENT_BINARY_DIR}/bvhkernels.generated.cpp)
add_custom_command(
	OUTPUT ${BVH_KERNELS_CPP}
	COMMAND
		${SLANGC_EXECUTABLE} ${BVH_KERNELS_SOURCE} -I ${SPEEDO_SOURCE_DIR}/gfx/shaders -DSHADERTYPES_H_GPU_TARGET=true
		-target cpp -entry BvhTrace -stage compute -entry BvhTrace2 -stage compute -o ${BVH_KERNELS_CPP}
	DEPENDS
		${BVH_KERNELS_SOURCE}
		${SPEEDO_SOURCE_DIR}/gfx/shaders/bvh.slang
		${SPEEDO_SOURCE_DIR}/gfx/shaders/capi.h
	COMMENT "Compiling the BVH traversal to c++"
	VERBATIM
)
# generated: its warnings aren't ours
set_source_files_properties(${ENVIRONMENT_KERNELS_CPP} ${BVH_KERNELS_CPP} PROPERTIES COMPILE_OPTIONS "-w")
add_executable(
	assettest
	${SPEEDO_SOURCE_DIR}/tools/assettest.cpp
	${SPEEDO_SOURCE_DIR}/tools/environmentkernels.cpp
	${SPEEDO_SOURCE_DIR}/tools/environmentkernelsbridge.cpp
	${ENVIRONMENT_KERNELS_CPP}
	${SPEEDO_SOURCE_DIR}/tools/bvhkernelsbridge.cpp
	${BVH_KERNELS_CPP}
)

target_compile_features(
	assettest
	PUBLIC
		cxx_std_26
)
target_link_libraries(
	assettest
	PRIVATE
		${SPEEDO_MIMALLOC}
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
		${SPEEDO_MIMALLOC}
		core
		gfx
		xxHash::xxhash
)

# how long a still view takes to settle, from its captures (docs/performance-plan.md, step 1), see
# src/tools/convergence.cpp
add_executable(convergence ${SPEEDO_SOURCE_DIR}/tools/convergence.cpp)
target_compile_features(
	convergence
	PUBLIC
		cxx_std_26
)
target_link_libraries(
	convergence
	PRIVATE
		${SPEEDO_MIMALLOC}
		core
)

# the executables' narrow strings are UTF-8 on windows too (windows.manifest sets the process's ANSI code page):
# libc++'s path::string() threw on file names the system's code page can't represent. lld-link can merge a manifest into
# the one it makes only with libxml2 or mt.exe, so the executables embed it as a resource (windows.rc), and the link rule
# (CMake's Windows-Clang.cmake, which ends in /MANIFEST:EMBED after the target's flags) makes none
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	foreach(lang C CXX)
		string(REPLACE "/MANIFEST:EMBED" "/MANIFEST:NO" CMAKE_${lang}_LINK_EXECUTABLE "${CMAKE_${lang}_LINK_EXECUTABLE}")
	endforeach()
	foreach(target server client assettest filebench convergence)
		target_sources(${target} PRIVATE ${SPEEDO_SOURCE_DIR}/windows.rc)
	endforeach()
	# the resource compiler's includes aren't tracked: without this, a changed manifest wasn't embedded
	set_source_files_properties(${SPEEDO_SOURCE_DIR}/windows.rc PROPERTIES OBJECT_DEPENDS ${SPEEDO_SOURCE_DIR}/windows.manifest)

	# the modules slang loads at run time from beside the executable (the build copies only the dlls it imports):
	# without them, compiling the shaders (a new user profile, or a changed shader) fails to load spirv-opt
	set(SLANG_BIN_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:debug>,debug/bin,bin>")
	file(GLOB SLANG_STANDARD_MODULE_DIRS LIST_DIRECTORIES true RELATIVE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
		"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/slang-standard-module-*")
	foreach(target client assettest filebench)
		add_custom_command(
			TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${SLANG_BIN_DIR}/slang-glslang.dll ${SLANG_BIN_DIR}/slang-glsl-module.dll ${SLANG_BIN_DIR}/slang-rt.dll
				$<TARGET_FILE_DIR:${target}>
			VERBATIM
		)
		foreach(moduleDir ${SLANG_STANDARD_MODULE_DIRS})
			add_custom_command(
				TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${SLANG_BIN_DIR}/${moduleDir} $<TARGET_FILE_DIR:${target}>/${moduleDir}
				VERBATIM
			)
		endforeach()
	endforeach()
endif()

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
