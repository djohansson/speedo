set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

set(program_version 1.19)

if(VCPKG_CROSSCOMPILING)
    message(FATAL_ERROR "This is a host only port!")
endif()

if(VCPKG_TARGET_IS_LINUX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(download_urls "https://fastbuild.org/downloads/v${program_version}/FASTBuild-Linux-x64-v${program_version}.zip")
        set(download_filename "FASTBuild-Linux-x64-v${program_version}.zip")
        set(download_sha512 c9f78cd484446bf66bc99a94b57cd7e09ac5576bdc76d869f32558c35db52b8a10e04acb2b5e7efa8b787fb919b74f22a4e2a83cad965afe303d197d8f9dd48b)
        set(program_name fbuild)
    endif()
elseif(VCPKG_TARGET_IS_OSX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64" OR VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(download_urls "https://fastbuild.org/downloads/v${program_version}/FASTBuild-OSX-x64+ARM-v${program_version}.zip")
        set(download_filename "FASTBuild-OSX-x64+ARM-v${program_version}.zip")
        set(download_sha512 7193d6b697d74c178ac5afa6e28f45181bff9ffc3695647c1194435aa411f98215d5924e6cafb9c56b015b22f870cc1076c6b7c81cedfd904ddfee1e265dea9d)
        set(program_name FBuild)
    endif()
elseif(VCPKG_TARGET_IS_WINDOWS)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(download_urls "https://fastbuild.org/downloads/v${program_version}/FASTBuild-Windows-x64-v${program_version}.zip")
        set(download_filename "FASTBuild-Windows-x64-v${program_version}.zip")
        set(download_sha512 c048f93980e4d396d016df944260fe5bac1ca381c79e6fce7ab0ac30f4457413c0020e1d8144f0e338efe319a673be219bbcf368277bfc527f71f4a85095080c)
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
