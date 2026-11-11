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

# curl's libraries in the host tree's lib, two levels up from tools/fastbuild-s3cache
if(APPLE)
	set_target_properties(FBuildS3Cache PROPERTIES INSTALL_RPATH "@loader_path/../../lib")
elseif(UNIX)
	set_target_properties(FBuildS3Cache PROPERTIES INSTALL_RPATH "$ORIGIN/../../lib")
endif()

install(TARGETS FBuildS3Cache fbuild-s3cache
	RUNTIME DESTINATION tools/fastbuild-s3cache
	LIBRARY DESTINATION tools/fastbuild-s3cache)
