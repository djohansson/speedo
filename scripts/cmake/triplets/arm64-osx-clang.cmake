set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)

set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_ENV_PASSTHROUGH_UNTRACKED LLVM_ROOT LLVM_TOOLS_BINARY_DIR)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang.toolchain.cmake)
set(VCPKG_DISABLE_COMPILER_TRACKING ON) # This target is not compiled yet when vcpkg wants to calculate the compiler hash.