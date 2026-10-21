# tinybvh: a header-only BVH builder and traverser (https://github.com/jbikker/tinybvh), not in vcpkg's registry. gfx
# builds the indirect lighting's ray tracing structure with it (see gfx/bvh.h). its gpu traversal is OpenCL only
# (kernels/*.cl): installed as a reference for the slang port (gfx/shaders/bvh.slang)
vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO jbikker/tinybvh
	REF c9be3103cc9693b456b0a0353927c11cac7d710f
	SHA512 aea59bc14892353d97b9b8a33f6c5c8bc39448c341cba9b7c0872b130c2189158b633f1ebb0a235a3787c112345a02499e48d53e4f88b4f89bcf2350a84c25c2
	HEAD_REF main
)

file(GLOB TINYBVH_HEADERS "${SOURCE_PATH}/tiny_bvh*.h")
file(INSTALL ${TINYBVH_HEADERS} DESTINATION "${CURRENT_PACKAGES_DIR}/include/tinybvh")
file(GLOB TINYBVH_KERNELS "${SOURCE_PATH}/kernels/*.cl")
file(INSTALL ${TINYBVH_KERNELS} DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/kernels")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
