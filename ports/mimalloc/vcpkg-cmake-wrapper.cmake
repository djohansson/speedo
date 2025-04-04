_find_package(${ARGS})

if(CMAKE_CURRENT_LIST_DIR STREQUAL ${MIMALLOC_CMAKE_DIR}/${MIMALLOC_VERSION_DIR})
    set(MIMALLOC_INCLUDE_DIR ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include)
    # As in vcpkg.cmake
    set(MIMALLOC_LIBRARY_DIR ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/lib,lib>)
    set(MIMALLOC_OBJECT_DIR ${MIMALLOC_LIBRARY_DIR})
    set(MIMALLOC_TARGET_DIR ${MIMALLOC_LIBRARY_DIR})
endif()

# vcpkg always configures either a static or dynamic library.
# ensure to always expose the mimalloc target as either the static or dynamic build.
if(TARGET mimalloc-static AND NOT TARGET mimalloc)
  add_library(mimalloc INTERFACE IMPORTED)
  set_target_properties(mimalloc PROPERTIES INTERFACE_LINK_LIBRARIES mimalloc-static)
endif()
