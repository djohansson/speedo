# Building: the toolchain, vcpkg and the sanitizers

Speedo is a C++26 real-time renderer built on macOS (arm64), Windows (x64) and Linux (x64, arm64) with its own LLVM toolchain: clang and libc++ come from the vcpkg `llvm` port, and every other dependency is a vcpkg package built with that clang, from upstream's registry plus overlay ports in `ports/`. Configurations are CMake presets that `setup.ps1` generates, builds use FASTBuild with an object cache in a shared public bucket, and vcpkg's packages come from a shared binary cache in the same bucket. Scripts are PowerShell under `scripts/`. CI (`.github/workflows/`) builds and tests on all three platforms, and a tag-triggered workflow packages releases.

## Contents

- [Setup and building](#setup-and-building)
  - [Presets: always configure via `cmake --preset <name>`, never a hand-reconstructed command](#presets-always-configure-via-cmake---preset-name-never-a-hand-reconstructed-command)
  - [Sanitizers: the sanitize and tsan presets](#sanitizers-the-sanitize-and-tsan-presets)
  - [Every graphics api in one build: gfx and the client compiled per api](#every-graphics-api-in-one-build-gfx-and-the-client-compiled-per-api)
- [The toolchain](#the-toolchain)
  - [vcpkg: overlay ports, per-port triplet options, and upstream's registry](#vcpkg-overlay-ports-per-port-triplet-options-and-upstreams-registry)
  - [Floating point in the toolchain](#floating-point-in-the-toolchain)
  - [vcpkg's shared binary cache](#vcpkgs-shared-binary-cache)
  - [FASTBuild's cache](#fastbuilds-cache)
  - [FASTBuild: precompiled headers with clang-cl overwrite themselves](#fastbuild-precompiled-headers-with-clang-cl-overwrite-themselves)
- [Per-platform setup](#per-platform-setup)
  - [`setup.ps1` on Windows: no PowerShell Gallery modules](#setupps1-on-windows-no-powershell-gallery-modules)
  - [Linux: setup and the toolchain](#linux-setup-and-the-toolchain)
  - [macOS: setup and the toolchain](#macos-setup-and-the-toolchain)
- [CI, releases and licenses](#ci-releases-and-licenses)
  - [CI: GitHub Actions](#ci-github-actions)
  - [Releases](#releases)
  - [Licenses](#licenses)
- [Gotchas](#gotchas)
  - [Gotcha: `<windows.h>` macros](#gotcha-windowsh-macros)
  - [libc++ migration: MSVC STL "leaks" `<cstdlib>`/`<cmath>`/etc., libc++ doesn't](#libc-migration-msvc-stl-leaks-cstdlibcmathetc-libc-doesnt)
  - [`clang.toolchain.cmake`: variables from `vcpkg.cmake` aren't available yet when chainloaded](#clangtoolchaincmake-variables-from-vcpkgcmake-arent-available-yet-when-chainloaded)
  - [Stale `build/<preset>/vcpkg_installed` shadows the real `install/` dir](#stale-buildpresetvcpkg_installed-shadows-the-real-install-dir)
  - [CMake `IMPORTED_LOCATION` vs `CMAKE_MAP_IMPORTED_CONFIG_<custom config>`](#cmake-imported_location-vs-cmake_map_imported_config_custom-config)
  - [CMake presets: setting `"PATH"` in `environment` replaces it, doesn't extend it](#cmake-presets-setting-path-in-environment-replaces-it-doesnt-extend-it)

## Setup and building

A bootstrap script, `setup_osx.sh`, `setup_linux.sh` or `setup_windows.bat` (run from the repository root, arguments passed on), installs PowerShell 7.5+ where it is missing or older (Homebrew's cask on macOS, a tarball under `~/powershell` on Linux, winget on Windows) and runs `setup.ps1`. `setup.ps1` is also the way to refresh a machine after an OS or SDK update: where the SDK (macOS) or Visual Studio and Windows SDK versions (Windows) changed, or the vcpkg release moved, it clears the CMake caches of the existing build directories. On every platform it:

- runs the platform's script (`scripts/platforms/<platform>/<platform>.ps1`), which installs what the build needs (see "Per-platform setup"),
- installs the pinned vcpkg tool (see "vcpkg: overlay ports, per-port triplet options, and upstream's registry"),
- records the toolchain's state in `.env.json` (the vcpkg root and downloads directory, the binary cache's sources, the llvm port's version, the SDK versions),
- generates `CMakeUserPresets.json` and the VS Code launch, task and settings files in `.vscode/` (launch configurations per configuration, the sanitizer presets among them, graphics api and target, and clangd from the installed LLVM),
- and installs every dependency, LLVM included, into `install/`: first from the binary cache alone and, where the cache lacks a package, after installing the platform's port build tools, building it (build trees and packages under `temp/vcpkg`).

Then configure and build a preset, `cmake --preset <name> && cmake --build build/<name>` (below), and run the client from the repository root. The programs and their options: README.md.

Scripts are PowerShell under `scripts/`: the setup helpers (`env.ps1`, `platform.ps1`, `platforms/`), the caches (`binarycache.ps1`, `seed-binary-cache.ps1`, `prune-binary-cache.ps1`, `prune-shader-cache.ps1`), packaging (`package.ps1`, `release.ps1`, `version.ps1`, `fetch-warp.ps1`) and the test and measurement scripts (`fetch-test-assets.ps1`, `assettest.ps1`, `visualtest.ps1`, `researchcompare.ps1`, `shadercheck.ps1`, `perfbaseline.ps1`, `update-gltf-manifest.ps1`), described in docs/notes/assets.md, docs/visual-testing-plan.md and the plans.

### Presets: always configure via `cmake --preset <name>`, never a hand-reconstructed command

`setup.ps1` generates `CMakeUserPresets.json`. Its presets carry the environment that `scripts/cmake/toolchains/clang.toolchain.cmake` and `CMakeLists.txt` depend on: `LLVM_ROOT`, `LLVM_TOOLS_BINARY_DIR`, `PATH` (and the Visual Studio path on Windows). They are named `<triplet>-<configuration>`: `debug`, `release` and `profile` everywhere, plus `sanitize` and `tsan` on macOS and Linux. Each has a build preset of the same name, and they inherit two hidden presets, `vcpkg` (the vcpkg toolchain, triplets, `install/` and the cache environment) and `llvm-build` (FASTBuild, `build/<preset>`, the chainloaded clang toolchain).

- Always run `cmake --preset <name> && cmake --build build/<name>`, as one command joined by `&&`, however recently the directory was configured. A raw `cmake -D... -S ... -B ...` copied from a log lacks the environment and fails confusingly.
- A bare `cmake --build build/<name>` that has to regenerate (after `vcpkg.json` or `ports/` changed, or after another preset installed into the shared `install/` tree) re-runs CMake without the preset environment: vcpkg computes different ABI hashes and removes the installed packages from `install/`. Recover with `cmake --preset <name>`, which restores them from the binary cache.
- Two guards enforce this: `CMakeLists.txt` stops any configure without the presets' environment before vcpkg runs, and a Claude Code hook (`.claude/hooks/require-preset.ps1`) refuses `cmake --build build/<name>` without `cmake --preset <name> &&` before it.
- Source files are listed explicitly in `CMakeLists.txt` (`CORE_SOURCE_FILES`, `GFX_SOURCE_FILES`, ...): add a new `.cpp` there.

### Sanitizers: the sanitize and tsan presets

`<triplet>-sanitize` is the profile configuration with `SPEEDO_SANITIZE=address,undefined`: the project's own targets are compiled with `-fsanitize=` and `-fno-sanitize-recover=all` (the first report aborts), the vcpkg packages as they are. The runtimes are compiler-rt's, built by the host llvm. The sanitizers replace malloc, so a sanitize build has no mimalloc. Run it on the quick tier: `pwsh scripts/assettest.ps1 -Preset <triplet>-sanitize -Client ...`, which sets the sanitizer options and `llvm-symbolizer` and fails a run on `ERROR: AddressSanitizer` or `runtime error:`. A cold shader compile is very slow under asan, hence assettest's warm-up (docs/notes/assets.md).

`<triplet>-tsan` is the same with `SPEEDO_SANITIZE=thread` (tsan combines with neither asan nor ubsan); assettest sets `TSAN_OPTIONS` (including `tsan-suppressions.txt`) and fails a run on `WARNING: ThreadSanitizer`.

- Print with `std::println`, never `std::cout` or `std::cerr`: libc++'s `operator<<` races on the stream's width across threads.
- A sanitized build (`SPEEDO_SANITIZE`, also a compile definition) leaves fatal signals to the sanitizer: the application's own handler allocates and can deadlock against the sanitizer's lock.
- Third-party code whose reports are its idioms goes in `sanitize-ignorelist.txt` by function name (`fun:`; FASTBuild compiles preprocessed sources, so `src:` entries only see the `.cpp`). FASTBuild does not track the file: touch the affected sources after changing it. Where the fix is a line, patch the port instead (`ports/tinybvh/0001-neon-slice-rows.patch`).
- On macOS a tsan build compiles `src/core/tsanguard.cpp` into each executable and shared library: guards over `std::atomic_ref`, because the binaries bind `__cxa_guard_acquire`/`release` from a C++ dylib that tsan does not interpose.

### Every graphics api in one build: gfx and the client compiled per api

Every preset builds a client for every graphics api the platform has (Vulkan; Metal on macOS; D3D12 on Windows), chosen at run time by `SPEEDO_GRAPHICS_API` (docs/environment-variables.md). rhi has every backend; gfx and the client name one, so they are compiled once per api (`speedo_gfx_library` and `speedo_client_library` in `CMakeLists.txt`: `gfx`/`clientlib`, `gfx_metal`/`clientlib_metal`, `gfx_d3d12`/`clientlib_d3d12`), each copy in its own inline namespace (`gfx::vk`, `gfx::metal`, ...; `SPEEDO_GRAPHICS_API_NAMESPACE` is a copy's public compile definition). All link into one executable and each stays monomorphic. The executable's C api (`client/capi.cpp`, compiled once) runs the copy the variable names and links them with `$<LINK_ONLY:...>`, which needs policy CMP0131. What cannot be compiled twice lives outside the copies: the header-only libraries' implementations (`gfx/libraries.cpp`, target `gfxlibraries`), the shader compiler library, core, platform and rhi. The tools use Vulkan's copy. Design and progress: docs/backends-plan.md.

## The toolchain

### vcpkg: overlay ports, per-port triplet options, and upstream's registry

**Registry and tool.** vcpkg is upstream's (microsoft/vcpkg), with no clone or submodule. Setup installs the vcpkg tool and its scripts bundle (no ports), pinned with SHA-512s in `scripts/vcpkg-tool.json`, into `~/.vcpkg-tool/<release>` (`SPEEDO_VCPKG_ROOT` for another; the ports' downloads are shared in `~/.vcpkg-tool/downloads`, `VCPKG_DOWNLOADS` for another). Ports come from the git registry that `vcpkg-configuration.json` names at a pinned baseline. The bundle's scripts can be older than the ports: a port calling a newer helper fails, and then the tool release moves. A bump changes the tool release and hashes and the baseline; every package rebuilds, so seed the shared cache from each platform afterwards.

**llvm.** The `llvm` port is vcpkg's own, at the registry's baseline: it provides clang, lld, clang-tools-extra (clangd, clang-format) and libc++, as host packages. Setup reads its version from the registry's `versions/baseline.json` for `LLVM_PORT_VERSION` (see the binary cache), so a baseline move that moves llvm rebuilds every package. It also moves `ports/llvm-libc-math` (LLVM libc's math functions as headers, for core's correctly rounded math, docs/notes/core.md), whose version and archive SHA-512 follow the llvm port's and are updated by hand. CMake's FASTBuild generator cannot build LLVM, so the host triplets build it with Ninja (on Windows with `CMAKE_NINJA_CMCLDEPS_RC=OFF`). The macOS triplet builds libc++ and libc++abi static (the system's are not ABI compatible with LLVM's) and clangd without XPC.

**FASTBuild for ports.** FASTBuild builds the ports through the overlay `ports/vcpkg-cmake` (FASTBuild is the default generator; Ninja where a port asks with `PREFER_NINJA` or names it, or a triplet names one in `VCPKG_CMAKE_GENERATOR`), which takes FBuild from the host port `ports/vcpkg-tool-fastbuild`.

**Overlays versus triplets.** Prefer upstream ports over overlays. When an overlay would only add CMake options, put them in the triplets (`if(PORT STREQUAL "<port>") list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS ...)`, at the end of `scripts/cmake/triplets/*-clang.cmake`), and request port features from `vcpkg.json` instead of overlaying the consumer. Editing a triplet or `clang.toolchain.cmake` changes every target package's ABI hash: expect a full rebuild. Files they merely `include()` (such as `clang.rules-override.cmake`) are not hashed. The triplets are `<arch>-<os>-clang` for target packages and host triplets (`arm64-osx-release`, `x64-linux-release`, `arm64-linux-release`, `x64-windows-clangcl`) for host tools such as LLVM; every triplet disables compiler tracking.

**Toolchain-level fixes** that removed the need for port patches:

- `clang.rules-override.cmake` appends `-Wno-gnu-line-marker`: FASTBuild's `-frewrite-includes` emits line markers that fail under `-Wpedantic -Werror`.
- mimalloc is built with `MI_USE_CXX=OFF`; as C++ it would export the static libc++ from its dylib (the host LLVM ships only static libc++, so this holds for every C++ dylib).
- `_GNU_SOURCE` is not defined on Darwin (a glibc macro that makes ports pick GNU API variants).
- Windows pdbs: `x64-windows-clang.cmake` replaces `vcpkg_copy_pdbs` (which needs `dumpbin`) with a version using llvm tools (`scripts/cmake/copy-pdbs.cmake`).
- Ports that check `MSVC` take Linux paths under our GNU-style clang on Windows: slang's split debug info is off, and zstd, pugixml, draco and miniz are static.

**Generator pitfalls in third-party CMake** (see `ports/tracy/0007-*`): a directory as a custom command `OUTPUT` fails with `File missing despite success`, and custom commands that `DEPENDS` on a target name are not ordered after it (depend on the produced file). ld64.lld adds no `libobjc` implicitly (`ports/tracy/0008-*`) and has no GNU link groups, and our clang reports `Clang`, not `AppleClang` (`ports/draco/0001-*`). A port FASTBuild cannot build can name another generator in its portfile (`GENERATOR Ninja`, see `ports/draco`).

**Other overlay ports**, and why:

| Port | Reason |
| --- | --- |
| `vcpkg-cmake`, `vcpkg-tool-fastbuild`, `fastbuild-s3cache` | FASTBuild for ports and the object cache plugin |
| `llvm-libc-math` | LLVM libc math headers for core's math |
| `mesa`, `spirv-llvm-translator` | mesa's Vulkan drivers (below) and a dependency of its `mesa_clc` that the registry lacks |
| `ktx` | `0007-version-without-bash.patch` (IT policy blocks msys2's `bash.exe`; do not add ports that need an msys2 shell without patching that out); built without KTX's ETC decoder (see "Licenses"); `0008` SSE kernels with AVX2 |
| `glfw3` | lists Cocoa's HiDPI display modes so full screen on a Retina display can use them; retire when upstream does |
| `mimalloc` | macOS: dyld frees thread-local blocks through the interposed `free` that mimalloc does not own; the patch forwards foreign pointers to `malloc_zone_from_ptr`; retire when upstream fixes it |
| `tracy` | libc++ includes (`0005`, `0006`), generator pitfalls (`0007`, `0008`), and `0010`: the UDP broadcast's socket without `getaddrinfo`, whose `dlopen` could deadlock with dyld before `main` on macOS (wolfpld/tracy#1528's diff); retire when a release has it |
| `draco` | Ninja, link groups, `<iterator>` include |
| `ppqsort` | libc++ include |
| `glslang` | no PCH (see the FASTBuild section) |
| `tinybvh` | `0001-neon-slice-rows.patch` (sanitizer-clean NEON builder) |
| `bcdec`, `metal-cpp`, `mio`, `picosha2`, `sha-intrinsics`, `usearch`, `vulkan-loader`, `vulkan-validationlayers`, `vulkan-crash-diagnostic-layer` | not described here; read the port |

**The mesa fork.** `ports/mesa` (an overlay of vcpkg's OpenGL mesa) builds mesa's Vulkan drivers alone, one feature each, from the `kk-ray-query` branch of github.com/djohansson/mesa (upstream's `main` plus our commits). It is a host dependency, since the loader loads a driver at run time.

- `lavapipe` is mesa's cpu driver (`swrast` over llvmpipe), a host package on every platform, for CI's visual checks.
- `kosmickrisp` (the macOS arm64 default) is the Vulkan driver over Metal; the branch adds ray queries (`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`). `assettest.ps1` and `perfbaseline.ps1` pass its manifest as `VK_DRIVER_FILES` unless one is set.
- Patches: `0001-kk-workaround-63-encoder-end-resolve.patch` works around an Apple bug (Metal ray tracing beside other gpu work restarts the gpu; docs/notes/indirect-lighting.md); `0002` replaces MSVC's `_interlocked*64` intrinsics for clang-cl; `0003-kk-relaxed-math.patch` compiles KosmicKrisp's Metal libraries in Metal's relaxed math, which keeps infinities and NaNs (docs/notes/core.md, "Floating point").
- A change to the patches or portfile (comments included) changes the port's hash: every build directory rebuilds mesa on its next `cmake --preset <name>`, and the binary cache must be seeded again. Release packages ship only KosmicKrisp.

**Updating the mesa fork from upstream.** The fork's `main` is upstream's alone and `kk-ray-query` is our commits on top (checkout `~/Repos/mesa`, remotes `origin` = gitlab.freedesktop.org/mesa/mesa and `github` = the fork). Fetch upstream's `main` and push it to the fork's `main` by hand (GitHub cannot follow a GitLab project), rebase `kk-ray-query` onto it, build and test, and push it with `--force-with-lease`. Then in `ports/mesa` set `REF` to the branch's new head and `SHA512` to its GitHub archive's, set `version` in `vcpkg.json` to mesa's `VERSION` without `-devel`, refresh patches that no longer apply (dropping a workaround once its bug is fixed), rebuild with `cmake --preset <preset>`, run the quick tier and the ray query checks (one client at a time), and commit.

### Floating point in the toolchain

Floating point is IEEE 754's on every platform and configuration, for the project and the packages: `-ffp-contract=off -fno-math-errno`, no `-ffast-math` (which reassociated sums, assumed no infinity or NaN, and on Linux linked `crtfastmath.o`, flushing denormals process wide). No contraction means a result does not depend on how an expression was written (`std::fma` where a fused one is meant). The project uses core's correctly rounded math functions (`core/math.h`), and the shaders compile with Metal's relaxed math (docs/notes/core.md, "Floating point").

### vcpkg's shared binary cache

A new clone restores its packages from a shared binary cache instead of building them (LLVM alone takes most of an hour). `scripts/binarycache.ps1` sets `VCPKG_BINARY_SOURCES` (into `.env.json` and the presets' environment): vcpkg's local default cache, read and written, and the Backblaze B2 bucket `danjo-artifacts` (region `us-west-001`), prefix `speedo/vcpkg/`, read over https by vcpkg's `http` source. The bucket is public: reading needs no account or key, and a missing key is a 404, which vcpkg takes as a miss. `SPEEDO_BINARY_CACHE=0` keeps to the local cache.

**What keeps the hashes portable.** A package's ABI hash covers the port's files, the triplet's and the chainloaded toolchain's text, the variables in `VCPKG_ENV_PASSTHROUGH` and its dependencies' hashes (the compiler is not tracked). It must be the same on another machine and differ where the binaries would:

- The compiler: the llvm port's version and port version (`LLVM_PORT_VERSION`, `Get-LlvmPortVersion` in `scripts/binarycache.ps1`), tracked by the clang triplets.
- The cpu: ports are built for a fixed baseline (`clang.toolchain.cmake`: `-mcpu=apple-m1` on Apple arm64, `-march=armv8.2-a+crc+crypto` on other arm64, `-march=x86-64-v3` on x86-64), and so is the project's code (`SPEEDO_TARGET_CPU=baseline`, set by the presets). A build directory keeps the flags of its first configure; `CMakeLists.txt` refuses a directory whose cached `CMAKE_<LANG>_FLAGS_<CONFIG>` differ from the toolchain's, and `cmake --preset <name> --fresh` fixes it.
- Not the line endings Git checked out: vcpkg hashes overlay ports, triplets and the toolchain as on disk, so `.gitattributes` checks `ports/**` and `scripts/cmake/**` out with LF everywhere.
- The oldest macOS a package runs on: `MACOSX_DEPLOYMENT_TARGET`, the SDK's major version's first (`osx.ps1`), tracked by both osx triplets, the host's too. The SDK's path (`SDKROOT`) is not tracked, since Xcode's and the Command Line Tools' differ.
- Linux's C library (`GLIBC_VERSION`), tracked by the Linux triplets, the host's too: a package built against a newer glibc fails to load with an older one. A machine seeding the cache for CI runs the same Ubuntu release as the job.
- Not Windows' toolset and SDK versions: a package built with older ones links with newer ones, and GitHub's image updates Visual Studio often. A machine whose Visual Studio is older than the one that built the packages may fail to link them: update it, or build them (`SPEEDO_BINARY_CACHE=0`).

**Writing** is a maintainer's and CI's. With `SPEEDO_BINARY_CACHE_WRITE` set, setup adds vcpkg's `x-aws` source (`s3://danjo-artifacts/speedo/vcpkg/,write`, which runs the AWS CLI), installs the AWS CLI where missing, and points it at B2. The credentials are the AWS CLI's own (a B2 application key scoped to the bucket, in `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` or `AWS_PROFILE`), never in the repository. An archive is `<prefix><abi>.zip`, named by the package's ABI hash. vcpkg uploads what it builds, not what it restores, so a machine whose packages were built before it could write seeds the bucket with `scripts/seed-binary-cache.ps1` (`-DryRun` lists what it would upload).

**Pruning.** Every triplet or toolchain edit, and every run that fails partway, leaves archives that no platform installs. `scripts/prune-binary-cache.ps1` deletes them: `-Keep` takes every platform's list (a status file, `install/vcpkg/status`, or a list of ABIs that `-WriteKeepList <file>` writes), `-DryRun` lists what would go, and it refuses to delete more than half of the bucket without `-Force` where a platform's list may be missing. B2 keeps a deleted file's versions unless the bucket's lifecycle keeps only the last, which decides whether pruning frees storage.

**CI writes and prunes the cache** where the repository holds a B2 application key (secrets `B2_APPLICATION_KEY_ID` and `B2_APPLICATION_KEY`, with read, write and delete rights; FASTBuild's objects use the same key). `ci.yml` and `release.yml` set `SPEEDO_BINARY_CACHE_WRITE` and the AWS variables from them on every run but a pull request's, so a runner uploads the packages it had to build. After a push whose three platform jobs passed, each having written its installed packages' list (`-WriteKeepList`), the prune job runs `prune-binary-cache.ps1` by those lists with `-MinAgeDays 7`: an archive no list names is kept for a week after its upload, so a maintainer's packages for a change not pushed yet survive until a runner needs them. Without the secrets every run only reads the cache.

**Tracy's profiler app** is a default manifest feature, `profiler-app` (`tracy[gui-tools]` and its dependencies). `SPEEDO_PROFILER_APP=0` leaves it out: setup installs with `--x-no-default-features` plus the other default features, and writes the same into the presets (`VCPKG_MANIFEST_NO_DEFAULT_FEATURES`, `VCPKG_MANIFEST_FEATURES`). CI and release set it, so Tracy is built without the feature there (another ABI) and its dependencies are in no keep list.

### FASTBuild's cache

The project's objects are cached: a local directory in front of the shared B2 bucket's `speedo/fastbuild/` prefix, so a build takes what any machine of the platform compiled, CI's runners and developers' alike.

**How.** FBuild takes a cache plugin only from `.CachePluginDLL` in the `.bff`'s Settings, which CMake's FASTBuild generator cannot write, and `cmake --build` cannot pass `-cache`. The host overlay port `ports/fastbuild-s3cache` therefore builds `FBuildS3Cache` (the plugin) and `fbuild-s3cache`, a launcher that the presets make `CMAKE_MAKE_PROGRAM`: it adds the plugin's lines to the `.bff` and `-cache` to the command line (unless `-cache` or `FASTBUILD_CACHE_MODE` chose a mode), then runs FBuild. It goes once a CMake release can name the plugin.

- The remote's url and region come from the build directory's `CMakeCache.txt` (`SPEEDO_FASTBUILD_CACHE_URL`, `SPEEDO_FASTBUILD_CACHE_REGION`, set by the presets from `scripts/binarycache.ps1`).
- The plugin reads the local directory (FBuild's cache path, else `~/.fastbuild/cache`), else the remote by anonymous GET, keeping it locally. It publishes locally, and remotely only where the machine writes the shared caches (`SPEEDO_BINARY_CACHE_WRITE` plus AWS credentials, as for vcpkg's cache): a PUT signed with AWS Signature V4 through curl.
- `SPEEDO_BINARY_CACHE=0` leaves the remote out and `SPEEDO_FASTBUILD_CACHE=0` the whole cache. The remote failing never fails a build (a miss).
- The local directory grows until trimmed: `cmake --build build/<preset> -- -cachetrim <MiB>`.

**Keys the same everywhere.** A key holds each compile's command line, paths included, so machines share objects only if their commands match: compile commands use relative paths (`CMAKE_FASTBUILD_USE_RELATIVE_PATHS`, set by the presets, the libc++ headers included); the slang kernels assettest compiles to C++ are generated with relative paths (`scripts/cmake/slangcpp.cmake`); `src/platform` does not include `glfw3native.h` (it made FASTBuild's light cache preprocess the file with absolute paths); the project builds for a fixed baseline cpu (see "vcpkg's shared binary cache"); and keys hold the compiler's binaries, so machines share objects when their LLVM comes from the binary cache. Consequences: `__FILE__` is relative, and an object another machine compiled carries that machine's build directory as its debug info's compile directory, so a debugger needs a source map (lldb's `target.source-map`) until it is compiled locally.

**Pruning.** By age, in CI's prune job (`scripts/prune-binary-cache.ps1`; `-FastbuildOnly` alone by hand): an object last dated more than `$FastbuildCacheExpiryDays` (in `scripts/binarycache.ps1`) ago is deleted. A GET does not date an object, so the plugin, where it writes, dates anew an object it retrieves once it is older than half that (`SPEEDO_FASTBUILD_CACHE_REFRESH_DAYS`), with a copy onto itself in the bucket. The job also deletes the older versions and delete markers B2 keeps. Local caches do not expire: use `-cachetrim`.

### FASTBuild: precompiled headers with clang-cl overwrite themselves

With clang-cl, CMake's FASTBuild generator writes the PCH compile's object file over the PCH itself (it substitutes FASTBuild's `%3` for `/Fo%2` only for compiler ID `MSVC`, and clang-cl's is `Clang`). Until a CMake release has the fix, a port built with FASTBuild and clang-cl turns PCH off in a triplet (`-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` under `if(PORT STREQUAL "<port>")`); the Windows host triplet uses the overlay `ports/glslang` (`-DENABLE_PCH=OFF`) instead, so edits do not rebuild LLVM.

## Per-platform setup

Each platform's script installs what restoring the packages and building the project take; `-PortBuildTools` adds what building ports from source takes. Setup installs those only where the binary cache lacks a package: it first installs with `--only-binarycaching`, and on failure runs the platform script with `-PortBuildTools` and installs again. Common to all: CMake (Homebrew's, apt's, or winget's), and with `SPEEDO_BINARY_CACHE_WRITE` the AWS CLI (see "vcpkg's shared binary cache").

### `setup.ps1` on Windows: no PowerShell Gallery modules

The Windows setup needs CMake 4.2.3 or newer and Visual Studio's C++ tools: an instance of any edition with the components `VC.Tools.x86.x64`, `VC.ATL` and `VC.Llvm.Clang` and a Windows 11 SDK (22621 or 26100). Where either is missing it installs them with winget (CMake; Visual Studio 2022 Build Tools with the components and SDK 22621). LLVM's clang links against the MSVC runtime and the SDK's libraries, which also build the ports, so `-PortBuildTools` installs nothing more.

It uses the `winget` CLI and `vswhere.exe` (fixed path under `${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer`), not the `Microsoft.WinGet.Client` or `VSSetup` modules: `-Scope CurrentUser` modules live in `Documents\PowerShell\Modules`, which OneDrive often redirects, and cloud-only files there are skipped by module auto-loading. Do not reintroduce modules, and avoid hardcoded install roots (the Windows SDK root comes from the `KitsRoot10` registry value). winget is needed only when setup must install something.

Two optional steps need an administrator (one UAC prompt; a declined prompt or failure only warns): D3D12's debug layer (opt in with `SPEEDO_D3D12_DEBUG_LAYER=1`, installed with DISM), and Debugging Tools for Windows (cdb, WinDbg, kd, symchk; installed by default where `cdb.exe` is missing, from a debuggers-only download of the SDK installer so Visual Studio's SDK bundle is not touched). The build needs neither.

### Linux: setup and the toolchain

Beyond what GitHub's image has, a fresh distribution needs `libicu<version>` (installed by `setup_linux.sh`; PowerShell aborts without it) and what `linux.ps1` installs: `build-essential`, `curl`, `zip`, `unzip`, `tar`, the X11, Wayland and xkbcommon headers glfw links, `cmake`, and the AWS CLI for a cache writer. With `-PortBuildTools` it also installs what building ports from source takes (the system's `clang`, `lld` and `llvm` for the host triplets, `python3-venv`, `ninja-build`, the autotools, `patchelf`, `bison`, `flex`). The host triplets build with the system's clang, lld and LLVM's archiver, as Apple's toolchain is on macOS: their `-flto=thin` is clang's. Setup records the C library's version as `GLIBC_VERSION`, which the Linux triplets track.

`clang.toolchain.cmake`'s Linux branch (a change to it changes every target package's hash on every platform):

- libc++'s headers and libraries are taken per target triple (`include/<triple>/c++/v1`, `lib/<triple>`).
- libc++ is static (`-Wl,-Bstatic,-lc++,-lc++abi,-Bdynamic -lm`, as one `-Wl` token because vcpkg's autotools ports mangle `-l:libc++.a`), as on macOS: installed binaries' rpaths are relative, so tools such as `slangc` could not load a shared libc++.
- No `CMAKE_BUILD_WITH_INSTALL_RPATH`: with it, tools a port runs from its build tree cannot load their dependencies' shared libraries.
- The triplets' per-configuration flags (`VCPKG_C_FLAGS_RELEASE`, ...) come last, so a port's can override them.

### macOS: setup and the toolchain

`setup_osx.sh` installs or upgrades Homebrew and installs PowerShell from its cask. `osx.ps1` then needs the Xcode Command Line Tools (`xcode-select --install`; the SDK that LLVM's clang compiles against) and installs `cmake` from Homebrew, and `awscli` for a cache writer. With `-PortBuildTools` it installs `pkg-config`, which vcpkg's meson ports (mesa) find in Homebrew.

Setup resolves the SDK's path (`SDKROOT`, naming the SDK version, so that an SDK update is detected) and sets `MACOSX_DEPLOYMENT_TARGET` to the SDK's major version's first (see "vcpkg's shared binary cache"); the presets carry both, with `VCPKG_OSX_SYSROOT` and the architecture. The default graphics api is Metal; for Vulkan the VS Code launch configurations run a task that writes the KosmicKrisp driver's manifest as `VK_DRIVER_FILES` into `~/.speedo/.env`. macOS specifics elsewhere: static libc++ and the `mimalloc` and `glfw3` overlays (see "vcpkg: overlay ports, per-port triplet options, and upstream's registry"), KosmicKrisp (the mesa fork), tsan's guards (see "Sanitizers: the sanitize and tsan presets").

## CI, releases and licenses

### CI: GitHub Actions

`.github/workflows/ci.yml` runs on pushes and pull requests to `master`, on GitHub's hosted runners only (no gpu). One job per platform (`macOS arm64`, `Windows x64`, `Linux x64`) runs `setup.ps1` (reading the shared binary cache, and writing it where the repository holds its key), `cmake --preset` and the build for the profile, debug and release configurations in turn, then `assettest.ps1` without `-Client` on the profile build (the importers, images, environments, the lens check) on the quick tier's assets, fetched with `fetch-test-assets.ps1 -Minimal` and kept between runs by `actions/cache`. The jobs' limit is 240 minutes.

- macOS runs on the `xcode-27` image (macOS 27 on Apple silicon, the SDK major version the cache's packages target; macOS 26 images would build every package).
- Windows runs on `windows-2025-vs2026` (Visual Studio 2026, Windows 11 SDK 26100), restoring packages built with an older Visual Studio. The image has no winget.
- Linux runs on `ubuntu-26.04`, named by its own label (its glibc is part of the binary cache's hash).

**The client on lavapipe.** After the asset checks every platform runs the client itself, headless and validated, on lavapipe (mesa's Vulkan driver on the cpu, from the mesa port): `visualtest.ps1 -Validate` (docs/visual-testing-plan.md), whose scenes go through the load, upload and draw path, plus two research checks against path traced references (docs/research-scenes-plan.md). The macOS runner's gpu is a virtual machine's, so lavapipe is used there too.

- `SPEEDO_EXECUTOR_THREAD_COUNT=2` reproduces a 4-core runner's executor.
- The shader cache between runs is kept with `actions/cache` (keyed by `src/gfx` and the ports, pruned to 1 GB by `scripts/prune-shader-cache.ps1`; docs/shader-compile-plan.md, "The cache").
- The Windows runner's processes are elevated, and the Vulkan loader ignores its path variables (`VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, layer paths) in an elevated process, so the Windows step registers the driver and the validation layer under `HKLM\SOFTWARE\Khronos\Vulkan` on the runner alone. The same holds for any client run as administrator.
- Mesa's disk cache is not implemented on Windows, so every lavapipe client there compiles its pipelines again.

**The client on D3D12, on WARP.** A Windows job of its own (`Windows x64 (D3D12 on WARP)`) runs the visual checks on D3D12 on WARP, Microsoft's cpu driver, from its NuGet package (`scripts/fetch-warp.ps1`, version and sha256 pinned, path in `SPEEDO_D3D12_WARP`) with ray queries on. By its license the package is for testing only and never ships. The runner's own WARP is not used (it compiles each shader at first dispatch on one core and crashes with ray queries). Furnace and upscaling are left out, and the job does not block (`continue-on-error`, logs uploaded on failure).

**The client on Metal.** The macOS job's last checks run on the runner's own gpu (a paravirtual Metal device without ray tracing or gpu timestamps) with `SPEEDO_GRAPHICS_API=metal` and Metal's api validation: `rhitest src/tools metal`, then `visualtest.ps1 -Validate` (its ray query check is skipped on Metal unless `SPEEDO_RAY_QUERY=1`, since Metal's ray tracing restarts the gpu on macOS 27). The step fails where either does but does not block (`continue-on-error`).

**The sanitizer builds.** A job each for `sanitize` and `tsan` on Linux (`Linux x64 (sanitize)`, `Linux x64 (tsan)`) builds that preset and runs `visualtest.ps1` on it on lavapipe, without the validation layer; a sanitizer's report fails the client's run as a validation message does, with stacks symbolized by the host llvm's symbolizer. They read the caches only and do not block (`continue-on-error`).

**The badges.** README.md shows the workflow's own badge plus one per platform and one per rhi backend tested on each, using shields.io's check runs badge (`github/check-runs/<owner>/<repo>/master?nameFilter=`), which takes master's newest commit's check runs by exact name. The platforms' badges take the jobs' names, so renaming a job breaks its badge. A backend on a platform is a check run of its own (`Vulkan (macOS arm64)`, `Metal (macOS arm64)`, `Vulkan (Windows x64)`, `Vulkan (Linux x64)`, `D3D12 (Windows x64)`), made through the checks api (`checks: write`) by two small jobs: `Start backend checks` makes them in progress as the run starts, and `Finish backend checks` completes them from the platform jobs' visual checks outcomes. They only report: the non-blocking D3D12 and Metal checks pass the workflow but show a failure on their badge. They do not run for pull requests (a fork's token cannot write checks).

**The prune job** (`Prune binary cache`) needs all three platform jobs and prunes the binary cache and FASTBuild's objects (see the two cache sections).

### Releases

The client runs from a checkout or from an install. Run from a checkout's root (`resources/` and `src/gfx/shaders/shaders.slang` there), the working directory is the root, the profile is `.speedo` and the test assets are in `resources/test-assets`. Otherwise the data is beside the executable (a macOS bundle's `Contents/Resources`, else `<prefix>/share/speedo`) and the profile and test assets are in the platform's user data directory (`core::file::GetUserDataPath`). `-r` and `-u` choose the resource and profile directories either way. A checkout compiles shaders from their sources at startup; an install's are precompiled (docs/notes/layering.md).

**The version** is written in one place, `src/version.h` (Major, Minor, Patch), which only `release.ps1` changes. CMakeLists.txt reads it for the project's version, the packages' (CPack, Info.plist) and the crash reports' (`SPEEDO_VERSION`); the client's About text includes the header; the scripts read it through `scripts/version.ps1`. `vcpkg.json` has no version of its own.

**Packages.** `scripts/package.ps1 [-Preset <release preset>] [-Out <dir>]` packages a built release preset into this platform's installer in `build/package`: `Speedo-<version>-macos-arm64.dmg`, `Speedo-<version>-windows-x64.msi` (CPack's WiX generator) or `Speedo-<version>-linux-x86_64.AppImage` (appimagetool). The package holds `CMakeLists.txt`'s Client component (installed only by a component install): the client and the shared libraries it loads, its shaders precompiled (no slang or shader compiler ships), fonts, the test assets' manifests, `LICENSE`, `THIRD-PARTY-NOTICES.md` and each port's license as `licenses/<port>.txt`; on macOS also KosmicKrisp with its manifest and the Vulkan loader. Package code is built for the fixed baseline cpu (`package.ps1` refuses an arm64 build configured for native), the macOS bundle is signed ad hoc, and nothing is signed by a developer id, so Gatekeeper and SmartScreen ask at first launch. The icons are committed (`scripts/package/make-icons.ps1` makes them from `speedo.svg`, macOS only). The Windows release client is a windowed program (`WIN32_EXECUTABLE`, still entered at `main`) that writes to a launching terminal (`AttachParentConsole`); debug and profile clients stay console programs. Tracy is not in a package. Test a package from its mounted .dmg, outside the checkout with a scratch `-u` profile and the display awake.

**The release workflow** (`.github/workflows/release.yml`): pushing a tag `v<major>.<minor>.<patch>` checks it against `src/version.h`, makes the tag's draft release (job `Prepare`), and on CI's three runners builds the release preset, packages it and attaches the package and its `.sha256` to the draft, which is published by hand. Run by hand (Actions > Release > Run workflow, no tag) it packages alone and keeps the packages as the run's artifacts. To release: `scripts/release.ps1 -Bump patch|minor|major` (or `-Version <x.y.z>`), on a clean `master`, sets `src/version.h`, commits it (`release: v<version>`) and tags the commit `v<version>` (annotated); `-Push` pushes `master` and the tag, else it prints the push command.

### Licenses

Speedo's own code is proprietary, all rights reserved (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists what the client ships or compiles in and under which license; a package carries each port's copyright file (`licenses/<port>.txt`) and the fonts' licenses. Everything shipped is permissive (MIT, BSD, Zlib, Apache-2.0, the LLVM exception's) except:

- libdwarf, LGPL-2.1 (macOS and Linux, cpptrace's symbolizer): shipped as its own shared library so it can be replaced, with its text and source on offer.
- ZeroMQ, MPL-2.0: file-level copyleft, shipped unmodified as a shared library.
- Attribution: FreeType's FTL (chosen over its GPL) asks for credit in the documentation, the Twemoji font's art is CC-BY-4.0, and Apache-2.0 components' NOTICE files are reproduced. The fonts' OFL forbids selling them alone.
- slang, and the glslang and SPIRV-Tools inside it, do not ship (shaders come precompiled); glslang's generated parser is GPL-3.0 with Bison's exception.
- KTX-Software's ETC decoder is not open source (an Ericsson license for OpenGL products only), so `ports/ktx` builds without it (`KTX_FEATURE_ETC_UNPACK=OFF`).

Tracy is in the debug and profile builds only: release compiles its macros away and links no library, so neither it nor its license ships. The test assets are neither Speedo's nor shipped with it (the client downloads them), and their licenses vary: most are CC-BY or CC0, but some are for research or non-commercial use only.

## Gotchas

Corner cases of the toolchain and the platforms' headers.

### Gotcha: `<windows.h>` macros

`core/file.h` includes `<windows.h>` (through mio), so its macros rename project code (`LoadImage` becomes `LoadImageW`, `CreateWindow` becomes `CreateWindowW`) and a function-like macro named like a project type breaks the SDK (hence `capi.h`'s `FLOAT1`/`UINT1`/`INT1`). `core/mio_extra.h` includes mio with `WIN32_LEAN_AND_MEAN`, `NOUSER` and `NOGDI`; keep `<windows.h>` out of other headers.

Narrow strings are UTF-8 on Windows through `src/windows.manifest`, which lld-link can only embed as a resource (`src/windows.rc`; `CMakeLists.txt` turns `/MANIFEST:EMBED` into `/MANIFEST:NO`). A new executable target needs `windows.rc` in its sources.

### libc++ migration: MSVC STL "leaks" `<cstdlib>`/`<cmath>`/etc., libc++ doesn't

The `x64-windows-clang` triplet builds against LLVM's own libc++ instead of MSVC's STL, whose headers transitively include much more, so third-party code that never included what it uses fails (`no member named 'abort' in namespace 'std'`). Fix it with a patch adding the include (`ports/tracy/0005-*`, `0006-*`); if the port is not yet an overlay, copy it at the registry's baseline into `ports/<name>/`, and rebase overlays on upstream's port when the baseline moves. Generate patches with `git diff` or `git format-patch`, not by hand (a blank context line must be a single space; `.gitattributes` pins `*.patch` to LF). Project code that relied on the leakage needs the same fix (for example `path::string()` explicitly, since `path::string_type` is `wstring` on Windows).

### `clang.toolchain.cmake`: variables from `vcpkg.cmake` aren't available yet when chainloaded

`vcpkg.cmake` includes the chainloaded toolchain before it sets `CURRENT_INSTALLED_DIR`, `CURRENT_HOST_INSTALLED_DIR` and the triplet variables, so toolchain code reading them only works on paths that do not need them (presets always take the `DEFINED ENV{LLVM_ROOT}` branch, so the `else()` fallbacks are cold-configure-only and rot). Use `elseif(CMAKE_CROSSCOMPILING)`, and pair every `CACHE` set in this file with `FORCE`, or a bad cached compiler path sticks across reconfigures.

### Stale `build/<preset>/vcpkg_installed` shadows the real `install/` dir

Older build directories may carry a `vcpkg_installed/` (from before `VCPKG_INSTALLED_DIR` pointed at the shared `install/`) that takes include-path precedence and looks like an API mismatch. Delete the stale build directory and reconfigure with `cmake --preset`.

### CMake `IMPORTED_LOCATION` vs `CMAKE_MAP_IMPORTED_CONFIG_<custom config>`

A target with only an unsuffixed `IMPORTED_LOCATION` (`Vulkan::Vulkan` from `FindVulkan.cmake`) is not found for the custom configuration `profile` (mapped to `release`). `CMakeLists.txt` sets `IMPORTED_LOCATION_RELEASE` on it after `find_package(Vulkan REQUIRED)`; fix such cases at the call site, not in the find module.

### CMake presets: setting `"PATH"` in `environment` replaces it, doesn't extend it

A preset's `PATH` is not merged with the inherited one unless `$penv{PATH}` is appended, so `setup.ps1`'s generated Windows `PATH` includes the system's and `LLVM_TOOLS_BINARY_DIR` (`llvm-objdump.exe` is there, not in `LLVM_ROOT/bin`). If a build succeeds but the `.exe` lacks sibling DLLs, look for swallowed `applocal.ps1` errors in the log: FASTBuild treats them as non-fatal.
