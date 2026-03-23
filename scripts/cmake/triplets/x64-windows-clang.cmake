set(VCPKG_TARGET_ARCHITECTURE x64)

set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_ENV_PASSTHROUGH LLVM_ROOT LLVM_TOOLS_BINARY_DIR)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang.toolchain.cmake)
set(VCPKG_DISABLE_COMPILER_TRACKING ON) # This target is not compiled yet when vcpkg wants to calculate the compiler hash.
