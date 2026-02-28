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

set(TOOLS_ROOT $ENV{TOOLS_ROOT})
#set(CACHE BOOL IS_32_BIT $<IF<$<STRING:MATCH,${CMAKE_SYSTEM_PROCESSOR},^$>:TRUE,FALSE>)
#string(REGEX MATCH "^(x86|i[3456]86)$" _ ${CMAKE_SYSTEM_PROCESSOR})
#string(REGEX MATCH "^(x86_64|x64|amd64|AMD64)$" _ ${CMAKE_SYSTEM_PROCESSOR})
#set(IS_32_BIT $<$<BOOL:${CMAKE_MATCH_1}>:1>)
#set(BOOL IS_32_BIT ${CMAKE_MATCH_1})
#set(BOOL IS_32_BIT FALSE)
#set(MINJECT_EXECUTABLE $<IF:${IS_32_BIT},"${TOOLS_ROOT}/mimalloc/minject32.exe","${TOOLS_ROOT}/mimalloc/minject.exe">)
set(MINJECT_EXECUTABLE ${TOOLS_ROOT}/mimalloc/minject.exe)

# message("CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
# message("CMAKE_MATCH_1: ${CMAKE_MATCH_1}")
# message("MATCH_LENGTH: ${MATCH_LENGTH}")
# message("IS_32_BIT: ${IS_32_BIT}")
message("TOOLS_ROOT: ${TOOLS_ROOT}")
message("MINJECT_EXECUTABLE: ${MINJECT_EXECUTABLE}")

###############################################################################

project(speedo LANGUAGES C CXX VERSION ${SPEEDO_VERSION_STRING})

find_package(Vulkan REQUIRED)
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
	PUBLIC
		SHA-Intrinsics
		cpptrace::cpptrace
		stduuid
		$<$<PLATFORM_ID:Linux>:uuid>
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
	PUBLIC
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
		nfd::nfd
		slang::slang
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
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_custom_command(
		TARGET server POST_BUILD
		COMMAND ${MINJECT_EXECUTABLE} -v -f -i $<$<CONFIG:debug>:--postfix=secure-debug> $<TARGET_FILE:server>
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
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_custom_command(
		TARGET client POST_BUILD
		COMMAND ${MINJECT_EXECUTABLE} -v -f -i $<$<CONFIG:debug>:--postfix=secure-debug> $<TARGET_FILE:client>
		COMMAND_EXPAND_LISTS
		VERBATIM
	)
endif()

###############################################################################
