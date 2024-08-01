set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/fastbuild
	REF 415d4a33c757e1e1f2c3ef8dae96702295d3f7e3
	SHA512 5bc6a1eea71c4ef715c39cf1d2738b4accbc3f1cd8698fc3bbd985466a664b9142a5d1b15b5cb6fd05f5fb13df713cdf027ea730755df4c4f77dab1b37a222ff
	HEAD_REF master
)

vcpkg_cmake_configure(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-DUSE_CRT_NEW_DELETE=ON
)

vcpkg_cmake_install()

vcpkg_copy_tools(TOOL_NAMES fbuild AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
file(INSTALL "${SOURCE_PATH}/Code/LICENSE.TXT" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
