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

message("VERSION ${SPEEDO_VERSION_STRING}")

###############################################################################

project(speedo LANGUAGES C CXX VERSION ${SPEEDO_VERSION_STRING})

find_package(Vulkan REQUIRED)
find_package(cpptrace CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(xxhash CONFIG REQUIRED)
find_package(SHA-Intrinsics CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(cppzmq CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)
find_package(cargs CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)

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
		SPEEDO_PROFILING_LEVEL=$<IF:$<CONFIG:Release>,0,1>
		SPEEDO_USE_MIMALLOC
	PRIVATE
		UUID_SYSTEM_GENERATOR
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
		mimalloc
		SHA-Intrinsics
		cpptrace::cpptrace
		stduuid
		Tracy::TracyClient
)

###############################################################################

file(GLOB GFX_SOURCE_FILES ${SPEEDO_SOURCE_DIR}/gfx/*.cpp)
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
target_link_libraries(
	gfx
	PRIVATE
		core
		xxHash::xxhash
		imgui::imgui
		glm::glm
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
		SPEEDO_GRAPHICS_VALIDATION_LEVEL=$<IF:$<CONFIG:Debug>,1,0>
	PRIVATE
		UUID_SYSTEM_GENERATOR
)
target_include_directories(
	rhi
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	rhi
	PRIVATE
		core
		xxHash::xxhash
		Vulkan::Vulkan
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
	PRIVATE
		UUID_SYSTEM_GENERATOR
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
	PRIVATE
		cpptrace::cpptrace
		stduuid
		Tracy::TracyClient
		cppzmq
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
target_compile_definitions(
	clientlib
	PRIVATE
		UUID_SYSTEM_GENERATOR
		ZMQ_BUILD_DRAFT_API
)
target_include_directories(
	clientlib
	PUBLIC
		$<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	clientlib
	PRIVATE
		cppzmq
		serverlib
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
		core
		serverlib
		cargs
		$<LINK_LIBRARY:FRAMEWORK,CoreFoundation>
)

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
		core
		gfx
		rhi
		clientlib
		cargs
		glfw
		slang
		nfd::nfd
		$<LINK_LIBRARY:FRAMEWORK,CoreFoundation>
)

###############################################################################
