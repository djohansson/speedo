set(VCPKG_TARGET_ARCHITECTURE x64)
#set(VCPKG_CMAKE_SYSTEM_NAME "Windows-Clang")
set(VCPKG_VISUAL_STUDIO_PATH $ENV{VISUAL_STUDIO_PATH})

set(VCPKG_CRT_LINKAGE "dynamic")
set(VCPKG_LIBRARY_LINKAGE "dynamic")

set(VCPKG_ENV_PASSTHROUGH LLVM_PATH LLVM_TOOLS_PATH LLVM_VERSION_MAJOR TARGET_ARCHITECTURE PROCESSOR_ARCHITECTURE)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang.toolchain.cmake)
# VCPKG_LOAD_VCVARS_ENV is needed for libraries using vcpkg through visual studio as a build system (for example tracy).
# must be set _after_ the toolchain file is loaded, since that will set VCPKG_LOAD_VCVARS_ENV to OFF.
set(VCPKG_LOAD_VCVARS_ENV ON) 
