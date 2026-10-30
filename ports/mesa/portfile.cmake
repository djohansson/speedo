# speedo overlay of vcpkg's mesa port: KosmicKrisp alone (mesa's Vulkan driver over Metal), from djohansson/mesa, which
# adds ray queries to it (VK_KHR_acceleration_structure, VK_KHR_ray_query, behind MESA_KK_EXPERIMENTAL=ray_query). The
# client takes it as a host dependency: a driver the Vulkan loader loads at run time (VK_DRIVER_FILES, its manifest in
# share/vulkan/icd.d), built with the host LLVM the toolchain already builds, which mesa_clc compiles KosmicKrisp's
# OpenCL kernels with at build time (the driver doesn't link LLVM). LLVM is static and without RTTI, and so is mesa.
# the fork's kk-ray-query branch: upstream mesa (gitlab.freedesktop.org/mesa/mesa) with KosmicKrisp's ray queries and the
# build fixes vcpkg's LLVM needs (clang's resource headers beside its tools, its bitcode archives linked by path)
vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/mesa
	REF 93dc1b0ddc03666814b7c7b6cab9b03b38e58962
	SHA512 ab10ddaff9d19a00d1f4d61f56ff9ed08b6bad85257baf80dcc41e0fa8d118b7510de280fd989dfe3328311afc0370404365507b250acd72df3f177ca943dce9
	HEAD_REF kk-ray-query
)

x_vcpkg_get_python_packages(PYTHON_VERSION "3" OUT_PYTHON_VAR "PYTHON3" PACKAGES mako packaging pyyaml)
get_filename_component(PYTHON3_DIR "${PYTHON3}" DIRECTORY)
vcpkg_add_to_path(PREPEND "${PYTHON3_DIR}")

vcpkg_configure_meson(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		-Dvulkan-drivers=kosmickrisp
		-Dgallium-drivers=
		-Dplatforms=macos
		-Dopengl=false
		-Dgles1=disabled
		-Dgles2=disabled
		-Degl=disabled
		-Dglx=disabled
		-Dllvm=enabled
		-Dshared-llvm=disabled
		-Dcpp_rtti=false
		-Dtools=
		-Dvulkan-layers=
		-Dbuild-tests=false
		-Dzstd=enabled
		-Dvalgrind=disabled
		-Dlibunwind=disabled
		-Dxmlconfig=disabled
	ADDITIONAL_BINARIES
		"python=['${PYTHON3}','-I']"
		"python3=['${PYTHON3}','-I']"
		"llvm-config='${CURRENT_INSTALLED_DIR}/tools/llvm/llvm-config'"
)
vcpkg_install_meson()

# the manifest names the driver by its absolute path in the package: by its path from the manifest instead, which the
# loader takes, so that it holds wherever the package is installed
file(GLOB icd_manifests "${CURRENT_PACKAGES_DIR}/share/vulkan/icd.d/*.json")
foreach(icd_manifest IN LISTS icd_manifests)
	file(READ "${icd_manifest}" icd_json)
	string(REGEX REPLACE "\"library_path\": \"[^\"]*/([^/\"]+)\"" "\"library_path\": \"../../../lib/\\1\"" icd_json "${icd_json}")
	file(WRITE "${icd_manifest}" "${icd_json}")
endforeach()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include" "${CURRENT_PACKAGES_DIR}/debug")
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/docs/license.rst")
