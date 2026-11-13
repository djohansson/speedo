# speedo overlay: Apple's metal-cpp (not in vcpkg's registry), the Metal backend's api (docs/backends-plan.md). Header
# only: its directories as they are (#include <Metal/Metal.hpp>, <QuartzCore/QuartzCore.hpp>, ...); one translation unit
# of the backend defines NS_PRIVATE_IMPLEMENTATION, MTL_PRIVATE_IMPLEMENTATION and CA_PRIVATE_IMPLEMENTATION before
# including them, and the backend links the Metal, QuartzCore and Foundation frameworks. Apple's release tag
# release/metal-cpp_macOS27.0.1_iOS27.0.1
vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO apple/metal-cpp
	REF 8cdde68ae36a636c84cbb48ebcbc8f3b60251ea0
	SHA512 ca33c06becbd8c8f570c1ddd717651d869516f879446d43436963042b4ee104bccd4fa02d9ee7678e2edf3bdf45fad568972e4a1b4828f1c5ba2fc199e17b1aa
	HEAD_REF main
)

foreach(directory IN ITEMS Foundation Metal MetalFX QuartzCore)
	file(INSTALL "${SOURCE_PATH}/${directory}" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
endforeach()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
