# LLVM libc's math functions as headers, and only those: libc/shared/math (LLVM 23's header-only interface to them) and the
# support headers they're written in, which core/math.cpp computes every platform's sines, powers and logarithms with.
# correctly rounded, the one right answer, so every platform gets the same, where Apple's, glibc's and Windows' libm each
# round some inputs their own way (Apple's double tan 41% of them, its sin and cos 4%). from the llvm port's source (its
# version and archive, which the llvm host port has downloaded already): keep the two at one version
vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO llvm/llvm-project
	REF llvmorg-${VERSION}
	SHA512 10cb5d9d95d7e8808ee180c6775526b731c69fac5a71342c7a99c0a02013a8e0c823e0e0482624c1eb38d4dbc16732562a9afb4cab4f7707d97fac92f5c53557
	HEAD_REF main
)

# include/llvm-libc-math: the headers by their paths from libc/, as they include each other ("src/__support/...",
# "hdr/..."). what all of shared/math.h includes (clang -M, LLVM 23.1.5): the support directories math is written in, the
# few headers of src/__support's own it takes (not the rest: printf's tables alone are 13 MB), and the type and macro
# headers. a version that includes more fails to compile, naming the header
set(source "${SOURCE_PATH}/libc")
set(destination "${CURRENT_PACKAGES_DIR}/include/llvm-libc-math")
file(INSTALL "${source}/shared/libc_common.h" "${source}/shared/math.h" DESTINATION "${destination}/shared")
foreach(directory IN ITEMS shared/math src/__support/CPP src/__support/FPUtil src/__support/macros src/__support/math hdr
		include/llvm-libc-macros include/llvm-libc-types)
	file(INSTALL "${source}/${directory}/" DESTINATION "${destination}/${directory}" FILES_MATCHING PATTERN "*.h")
endforeach()
foreach(header IN ITEMS big_int.h common.h ctype_utils.h detailed_powers_of_ten.h high_precision_decimal.h
		integer_literals.h libc_assert.h libc_errno.h math_extras.h number_pair.h sign.h str_to_float.h str_to_integer.h
		str_to_num_result.h uint128.h wctype_utils.h)
	file(INSTALL "${source}/src/__support/${header}" DESTINATION "${destination}/src/__support")
endforeach()

vcpkg_install_copyright(FILE_LIST "${source}/LICENSE.TXT")
