cmake_minimum_required(VERSION 3.25)
project(fastbuild-s3cache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL CONFIG REQUIRED)

# FBuild's cache plugin (FASTBuild's CachePluginInterface.h): a module FBuild loads by its path, its functions exported by
# their C names
add_library(FBuildS3Cache MODULE plugin.cpp)
target_link_libraries(FBuildS3Cache PRIVATE CURL::libcurl)
set_target_properties(FBuildS3Cache PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)

# the launcher CMake runs as FBuild (CMAKE_MAKE_PROGRAM)
add_executable(fbuild-s3cache launcher.cpp)

# Windows: curl's header includes <windows.h>, whose min and max macros broke std::max (master's first Windows CI run
# with the refresh, 2026-11-11), and the CRT's fopen is fine here
if(WIN32)
	target_compile_definitions(FBuildS3Cache PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
	target_compile_definitions(fbuild-s3cache PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
	# curl delay-loaded, so that the plugin loads it by its path, from beside itself, before the first call (see LoadCurl)
	target_link_options(FBuildS3Cache PRIVATE "/DELAYLOAD:$<TARGET_FILE_NAME:CURL::libcurl>")
	target_link_libraries(FBuildS3Cache PRIVATE delayimp)
	target_compile_definitions(FBuildS3Cache PRIVATE "CURL_DLL_NAME=$<TARGET_FILE_NAME:CURL::libcurl>")
endif()

# curl's libraries in the host tree's lib, two levels up from tools/fastbuild-s3cache
if(APPLE)
	set_target_properties(FBuildS3Cache PROPERTIES INSTALL_RPATH "@loader_path/../../lib")
elseif(UNIX)
	set_target_properties(FBuildS3Cache PROPERTIES INSTALL_RPATH "$ORIGIN/../../lib")
endif()

install(TARGETS FBuildS3Cache fbuild-s3cache
	RUNTIME DESTINATION tools/fastbuild-s3cache
	LIBRARY DESTINATION tools/fastbuild-s3cache)
# Windows: curl beside the plugin, installed by name, as delay-loaded it isn't among the imports
# vcpkg_copy_tool_dependencies follows (which then finds curl's own, zlib's)
if(WIN32)
	install(FILES $<TARGET_FILE:CURL::libcurl> DESTINATION tools/fastbuild-s3cache)
endif()
