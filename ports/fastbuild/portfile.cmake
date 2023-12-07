set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

if (VCPKG_TARGET_IS_WINDOWS)
	set(FASTBUILD_EXE_SUFFIX ".exe")
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/djohansson/fastbuild/releases/download/v${VERSION}/FASTBuild-Windows-x64-v${VERSION}-djohansson.zip"
		FILENAME "FASTBuild-Windows-x64-v${VERSION}-djohansson.zip"
		SHA512 0
	)
elseif (VCPKG_TARGET_IS_OSX)
	set(FASTBUILD_EXE_SUFFIX "")
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/djohansson/fastbuild/releases/download/v${VERSION}/FASTBuild-OSX-x64+ARM-v${VERSION}-djohansson.zip"
		FILENAME "FASTBuild-OSX-x64+ARM-v${VERSION}-djohansson.zip"
		SHA512 067aeb8289dd2c1e634c578499e6b6c0c1767a5b1b7776675e2d52677d9d138ebf20d9a0436e5ff3eac1a5548bf4a1b1d8cfb0c2b6256872278f6f897bb38320
	)
endif()

vcpkg_extract_source_archive(
	BINDIST_PATH
	ARCHIVE "${ARCHIVE}"
	NO_REMOVE_ONE_LEVEL
)

file(INSTALL "${BINDIST_PATH}/FBuild${FASTBUILD_EXE_SUFFIX}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
file(INSTALL "${BINDIST_PATH}/FBuildWorker${FASTBUILD_EXE_SUFFIX}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
file(INSTALL "${BINDIST_PATH}/LICENSE.TXT" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
