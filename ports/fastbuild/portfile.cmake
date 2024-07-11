set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/fastbuild
	REF 288903effef0636a402ecb6e28f7701eb59a6f1f
	SHA512 b0f8d01aeceae701b81a0ac4b0b89f7a65dbbecc54cf9c303f43db7fb83812162c06a05a6ca4bfb2415b84de3dc76486cfc5b6580c4f6c5994388bacc632428b
	HEAD_REF master
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${SOURCE_PATH}/Code/LICENSE.TXT" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
