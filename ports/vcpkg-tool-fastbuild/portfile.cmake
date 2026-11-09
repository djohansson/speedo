set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

set(program_version 1.20)

if(VCPKG_CROSSCOMPILING)
    message(FATAL_ERROR "This is a host only port!")
endif()

if(VCPKG_TARGET_IS_LINUX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(download_urls "https://fastbuild.org/downloads/v${program_version}/FASTBuild-Linux-x64-v${program_version}.zip")
        set(download_filename "FASTBuild-Linux-x64-v${program_version}.zip")
        set(download_sha512 5d40a7835fe357a0f8e1cca5cedf96ee49de2f34c3e6d9f02974e7c03de11c5f80246aae06e1675f15a64eee2c6ece0a0e3c29bdbf80322ef3bc829140213696)
        set(program_name fbuild)
    endif()
elseif(VCPKG_TARGET_IS_OSX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64" OR VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(download_urls "https://fastbuild.org/downloads/v${program_version}/FASTBuild-OSX-x64+ARM-v${program_version}.zip")
        set(download_filename "FASTBuild-OSX-x64+ARM-v${program_version}.zip")
        set(download_sha512 aedb1184778f40e0bc7e8f918742f5f15b37c4adf7898361fe8cd8446d823eff1583309c586d9f54e0a0893aaf013dd0cbe4a72fece031e781500a71a03d623e)
        set(program_name FBuild)
    endif()
elseif(VCPKG_TARGET_IS_WINDOWS)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(download_urls "https://fastbuild.org/downloads/v${program_version}/FASTBuild-Windows-x64-v${program_version}.zip")
        set(download_filename "FASTBuild-Windows-x64-v${program_version}.zip")
        set(download_sha512 903635f4869e8d49ac3260e114ac915ad86b77397aee44167e3d85a9d57485c634281215ff68071fd839a1152b5e3eee622b23aab327a8908816df2a78fff36f)
        set(program_name FBuild.exe)
    endif()
else()
    message(FATAL_ERROR "Unsupported host platform ${CMAKE_HOST_SYSTEM_NAME}!")
endif()

vcpkg_download_distfile(
    archive_path
    URLS ${download_urls}
    SHA512 "${download_sha512}"
    FILENAME "${download_filename}"
)

vcpkg_extract_archive(
    ARCHIVE "${archive_path}"
    DESTINATION ${CURRENT_PACKAGES_DIR}/tools/fastbuild.temp
)

file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/fastbuild")
file(INSTALL "${CURRENT_PACKAGES_DIR}/tools/fastbuild.temp/${program_name}"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/fastbuild"
    FILE_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools/fastbuild.temp")
