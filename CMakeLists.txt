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

#find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(cpptrace CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
#find_package(SHA-Intrinsics CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(cppzmq CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(cargs CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(SHA-Intrinsics REQUIRED)

###############################################################################

file(
	GLOB
	CORE_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/core/*.cpp
)

add_library(core STATIC ${CORE_SOURCE_FILES})
target_compile_features(core PUBLIC cxx_std_26)
target_compile_definitions(core PUBLIC UUID_SYSTEM_GENERATOR)

target_include_directories(
	core
	PUBLIC $<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}> $<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	core
	PRIVATE cpptrace::cpptrace stduuid Tracy::TracyClient
)

###############################################################################

file(
	GLOB
	GFX_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/gfx/*.cpp
)

add_library(gfx STATIC ${GFX_SOURCE_FILES})
target_compile_features(gfx PUBLIC cxx_std_26)
target_compile_definitions(gfx PUBLIC UUID_SYSTEM_GENERATOR)

target_include_directories(
	gfx
	PUBLIC $<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}> $<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	gfx
	PRIVATE cpptrace::cpptrace stduuid Tracy::TracyClient imgui glm::glm glfw3 Vulkan::Vulkan
)

###############################################################################

file(
	GLOB
	RHI_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/rhi/*.cpp
)

add_library(rhi STATIC ${RHI_SOURCE_FILES})
target_compile_features(rhi PUBLIC cxx_std_26)
target_compile_definitions(rhi PUBLIC UUID_SYSTEM_GENERATOR)
if (WIN32)
	target_compile_definitions(rhi PUBLIC VK_USE_PLATFORM_WIN32_KHR=1)
endif()

target_include_directories(
	rhi
	PUBLIC $<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}> $<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	rhi
	PRIVATE cpptrace::cpptrace stduuid Tracy::TracyClient imgui glm::glm
)

###############################################################################

file(
	GLOB
	SERVERLIB_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/server/*.cpp
)

add_library(serverlib STATIC ${SERVERLIB_SOURCE_FILES})
target_compile_features(serverlib PUBLIC cxx_std_26)
target_compile_definitions(serverlib PUBLIC UUID_SYSTEM_GENERATOR ZMQ_BUILD_DRAFT_API)

target_include_directories(
	serverlib
	PUBLIC $<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}> $<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	serverlib
	PRIVATE cpptrace::cpptrace stduuid Tracy::TracyClient cppzmq
)

###############################################################################

file(
	GLOB
	CLIENTLIB_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/client/*.cpp
)

add_library(clientlib STATIC ${CLIENTLIB_SOURCE_FILES})
target_compile_features(clientlib PUBLIC cxx_std_26)
target_compile_definitions(clientlib PUBLIC UUID_SYSTEM_GENERATOR ZMQ_BUILD_DRAFT_API)

target_include_directories(
	clientlib
	PUBLIC $<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}> $<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	clientlib
	PRIVATE cpptrace::cpptrace stduuid Tracy::TracyClient cppzmq
)

###############################################################################

file(
	GLOB
	SERVER_SOURCE_FILES
	${SPEEDO_SOURCE_DIR}/server/*.c
)

add_executable(server ${SERVER_SOURCE_FILES})
target_compile_features(server PUBLIC c_std_23)

target_include_directories(
	server
	PUBLIC $<BUILD_INTERFACE:${SPEEDO_SOURCE_DIR}> $<INSTALL_INTERFACE:include/speedo>
)
target_link_libraries(
	server
	PRIVATE $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc> cargs core serverlib
)
