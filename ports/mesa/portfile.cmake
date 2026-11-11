# speedo overlay of vcpkg's mesa port: mesa's Vulkan drivers alone, from djohansson/mesa. KosmicKrisp (the kosmickrisp
# feature, macOS's default), mesa's driver over Metal, which the fork adds ray queries to (VK_KHR_acceleration_structure,
# VK_KHR_ray_query, behind MESA_KK_EXPERIMENTAL=ray_query); and lavapipe (the lavapipe feature, Windows'), the swrast
# driver on the cpu over llvmpipe, for headless runs without a gpu (CI). The client takes it as a host dependency: a driver
# the Vulkan loader loads at run time (VK_DRIVER_FILES, its manifest in share/vulkan/icd.d), built with the host LLVM the
# toolchain already builds: mesa_clc compiles KosmicKrisp's OpenCL kernels with it at build time (KosmicKrisp doesn't
# link LLVM), and lavapipe links it, compiling shaders with it at run time. LLVM is static and without RTTI, and so is
# mesa.
# the fork's kk-ray-query branch: upstream mesa (gitlab.freedesktop.org/mesa/mesa) with KosmicKrisp's ray queries and the
# build fixes vcpkg's LLVM needs (clang's resource headers beside its tools, its bitcode archives linked by path)
vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO djohansson/mesa
	REF 93dc1b0ddc03666814b7c7b6cab9b03b38e58962
	SHA512 ab10ddaff9d19a00d1f4d61f56ff9ed08b6bad85257baf80dcc41e0fa8d118b7510de280fd989dfe3328311afc0370404365507b250acd72df3f177ca943dce9
	HEAD_REF kk-ray-query
	PATCHES
		# KK_WORKAROUND_63, local until Apple fixes its bug: a compute kernel with Metal ray tracing, beside other gpu work
		# (another queue or process, WindowServer), hangs the gpu until its firmware restarts it ("CDM Kill timeout"; macOS
		# 27.2, M4 Pro; src/tools/macos/rtrepro.mm `frame 60 rtpudxb` reproduces it in pure Metal, reported to Apple
		# 2026-11-08, beside FB25381275). A counter heap resolve of one entry never written, after every encoder, makes it
		# rare: on devices with ray queries only, off with MESA_KK_DISABLE_WORKAROUNDS=63 (numbered from the top, clear of
		# upstream's). Rarer, not gone: it narrows a race of unknown mechanism. See docs/notes/indirect-lighting.md
		0001-kk-workaround-63-encoder-end-resolve.patch
		# clang-cl (Windows' host triplet, lavapipe) has the _Interlocked* intrinsics but not MSVC's lower-case ones, which
		# u_atomic.h's MSVC path calls (mesa issue 8902): defined from the others under clang, MSVC unchanged. Local until
		# upstream has a fix
		0002-u-atomic-clang-cl-interlocked.patch
)

# Windows: pip bootstrapped here, which x_vcpkg_get_python_packages then takes (it runs get-pip only where vcpkg's Python
# has no Scripts/pip.exe). The scripts setup pins (vcpkg's of 2026-10-28) acquire Python 3.15 there, and the registry
# release's helper (2026.10.16) bootstraps pip with get-pip 24.2, whose pip doesn't run on 3.15 ("module 'typing' has no
# attribute 'no_type_check_decorator'"); upstream's took get-pip 26.2.2 the week after (d90cb72e90), whose archive this is.
# Here rather than an overlay of the helper, whose dependents (mesa and Linux's libsystemd, and theirs) all would rebuild.
# Drop with a registry baseline past that commit
if(CMAKE_HOST_WIN32)
	vcpkg_find_acquire_program(PYTHON3)
	get_filename_component(python_dir "${PYTHON3}" DIRECTORY)
	if(NOT EXISTS "${python_dir}/Scripts/pip.exe")
		vcpkg_from_github(
			OUT_SOURCE_PATH get_pip
			REPO pypa/get-pip
			REF 26.2.2
			SHA512 0e2ae12f15b34cf8c52069e62b0174b0579e8a3c573b6d99706b61708f51ecba80d6b93db56635206e01052d3786e8937c028d5bab64083b5e3aee54005883ac
		)
		set(ENV{PYTHONNOUSERSITE} "1")
		vcpkg_execute_required_process(
			COMMAND "${PYTHON3}" "${get_pip}/public/get-pip.py" --no-warn-script-location
			WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
			LOGNAME "get-pip-${TARGET_TRIPLET}")
	endif()
endif()

x_vcpkg_get_python_packages(PYTHON_VERSION "3" OUT_PYTHON_VAR "PYTHON3" PACKAGES mako packaging pyyaml)
get_filename_component(PYTHON3_DIR "${PYTHON3}" DIRECTORY)
vcpkg_add_to_path(PREPEND "${PYTHON3_DIR}")

# the drivers the features ask for: KosmicKrisp (macOS), lavapipe (the swrast Vulkan driver, over gallium's llvmpipe)
set(vulkan_drivers "")
set(gallium_drivers "")
if("kosmickrisp" IN_LIST FEATURES)
	list(APPEND vulkan_drivers kosmickrisp)
endif()
if("lavapipe" IN_LIST FEATURES)
	list(APPEND vulkan_drivers swrast)
	list(APPEND gallium_drivers llvmpipe)
endif()
list(JOIN vulkan_drivers "," vulkan_drivers)
list(JOIN gallium_drivers "," gallium_drivers)

set(additional_binaries "")
if("lavapipe" IN_LIST FEATURES)
	# lavapipe builds its acceleration structures with glsl shaders, compiled at build time (mesa's with_bvh)
	list(APPEND additional_binaries "glslangValidator='${CURRENT_HOST_INSTALLED_DIR}/tools/glslang/glslangValidator${VCPKG_HOST_EXECUTABLE_SUFFIX}'")
endif()

set(platform_options "")
if(VCPKG_TARGET_IS_OSX)
	list(APPEND platform_options -Dplatforms=macos)
elseif(VCPKG_TARGET_IS_WINDOWS)
	# no window system platform beside windows', and not Microsoft's CLC compiler (for d3d12's drivers), which would
	# build where it finds LLVM's clang. flex and bison for mesa's parsers, which windows doesn't have
	list(APPEND platform_options -Dplatforms=windows -Dmicrosoft-clc=disabled)
	vcpkg_find_acquire_program(FLEX)
	vcpkg_find_acquire_program(BISON)
	foreach(program IN ITEMS "${FLEX}" "${BISON}")
		get_filename_component(program_dir "${program}" DIRECTORY)
		vcpkg_add_to_path(PREPEND "${program_dir}")
	endforeach()
endif()

vcpkg_configure_meson(
	SOURCE_PATH "${SOURCE_PATH}"
	OPTIONS
		"-Dvulkan-drivers=${vulkan_drivers}"
		"-Dgallium-drivers=${gallium_drivers}"
		${platform_options}
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
		# the environment's PYTHON* variables and the user's site-packages ignored, as vcpkg's mesa port does, but not -I's
		# isolation: it leaves the script's own directory out of sys.path, and mesa's generators import their siblings
		# (u_format_table.py's u_format_parse, builtin_types_c.py's builtin_types), which failed on Windows
		"python=['${PYTHON3}','-E','-s']"
		"python3=['${PYTHON3}','-E','-s']"
		"llvm-config='${CURRENT_INSTALLED_DIR}/tools/llvm/llvm-config${VCPKG_HOST_EXECUTABLE_SUFFIX}'"
		${additional_binaries}
)
vcpkg_install_meson()

# a manifest names its driver by its absolute path in the package: by its path from the manifest instead, which the
# loader takes, so that it holds wherever the package is installed. the driver is in lib (a dylib) or bin (a windows
# dll, whose path the loader hands LoadLibrary, which takes backslashes alone). read and written as json (string(JSON)),
# which escapes the backslashes: a regex replacement unescaped them, and the loader skipped the invalid manifest
# (VK_ERROR_INCOMPATIBLE_DRIVER, Windows' first lavapipe client runs)
if(VCPKG_TARGET_IS_WINDOWS)
	set(path_separator "\\")
else()
	set(path_separator "/")
endif()
file(GLOB icd_manifests "${CURRENT_PACKAGES_DIR}/share/vulkan/icd.d/*.json")
foreach(icd_manifest IN LISTS icd_manifests)
	file(READ "${icd_manifest}" icd_json)
	string(JSON library_path GET "${icd_json}" ICD library_path)
	string(REPLACE "\\" "/" library_path "${library_path}")
	get_filename_component(library_name "${library_path}" NAME)
	if(EXISTS "${CURRENT_PACKAGES_DIR}/bin/${library_name}")
		set(library_dir bin)
	else()
		set(library_dir lib)
	endif()
	string(JOIN "${path_separator}" relative_path .. .. .. "${library_dir}" "${library_name}")
	# the value as a json string literal: backslashes and quotes escaped
	string(REPLACE "\\" "\\\\" relative_path_json "${relative_path}")
	string(REPLACE "\"" "\\\"" relative_path_json "${relative_path_json}")
	string(JSON icd_json SET "${icd_json}" ICD library_path "\"${relative_path_json}\"")
	file(WRITE "${icd_manifest}" "${icd_json}")
endforeach()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include" "${CURRENT_PACKAGES_DIR}/debug")
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/docs/license.rst")
