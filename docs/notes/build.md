# Building: the toolchain, vcpkg and the sanitizers

A note of CLAUDE.md (its index names each): what the build failed on and how it is set up.

## Sanitizers: the sanitize and tsan presets

`<triplet>-sanitize` (macOS and Linux, from `setup.ps1`) is the profile configuration with `SPEEDO_SANITIZE=address,undefined`
(`CMakeLists.txt`): our own targets compiled with `-fsanitize=` and `-fno-sanitize-recover=all` (the first report aborts),
the vcpkg packages as they are. The runtimes are compiler-rt's, which the host llvm builds (its `compiler-rt` feature,
off Windows; adding it rebuilt llvm, about 50 minutes). The sanitizers replace malloc, so a sanitize build has no
mimalloc: the executables don't link it (its dylib interposes malloc process wide) and `SPEEDO_USE_MIMALLOC` isn't
defined, which takes the code's fallbacks (the Vulkan host allocation callbacks in `instance.cpp`: `aligned_alloc`
there needs a size that is a multiple of the alignment, and on macOS an alignment of at least a pointer, or it returns
null, which failed `vkCreateInstance` and `vkCreateDevice` out of host memory). Run it on the quick tier:
`pwsh scripts/assettest.ps1 -Preset <triplet>-sanitize -Client ...`, which sets `UBSAN_OPTIONS=print_stacktrace=1`
and the host llvm's `llvm-symbolizer`, and fails a run on its reports (`ERROR: AddressSanitizer`, `runtime error:`).
The quick tier passes, assettest and clients.

`<triplet>-tsan` is the same with `SPEEDO_SANITIZE=thread` (tsan combines with neither asan nor ubsan). tsan reports and
goes on, exiting with 66 (an abort, on macOS) once done; `assettest.ps1` sets `TSAN_OPTIONS` (the symbolizer, and
`tsan-suppressions.txt`) and fails a run on `WARNING: ThreadSanitizer`. The quick tier passes. What it found: printing
with `std::cout`/`std::cerr` from several threads races in libc++ (`operator<<` reads and resets the stream's `width()`
unlocked, which the standard says it mustn't): the asset loaders' reimport messages, from parallel texture loads. Print
with `std::println` (`stderr` for errors), one locked `fwrite` a line, which also keeps lines whole: there's no
`std::cout` or `std::cerr` left. `tsan-suppressions.txt` holds what isn't a race: a function local static's guard is
released in `__cxa_guard_release`, which our binaries bind from a C++ dylib (cpptrace's, slang's: each exports the
static libc++abi linked into it) rather than the system's libc++abi, so tsan's interceptor never sees it, and the
threads reading a static first race the one that initialized it (mio's `page_size()`, the only one the quick tier
hits). Our frames aren't symbolized there (no line numbers), so a suppression names a function tsan prints, not one
inlined into it.

About one client run in 50 faulted inside the tsan runtime on macOS 27 (`MetaMap::AllocBlock`, `OnUserFree`: frees of
system frameworks' allocations, CoreUI's libcompression on a dispatch queue), not in our code. Our fatal signal handler
then hung the run (its stack trace allocates, which waited for the tsan lock the faulting thread held): a sanitized
build (`SPEEDO_SANITIZE`, also a compile definition) leaves SIGILL, SIGABRT, SIGFPE and SIGSEGV to the sanitizer, which
reports them itself and exits.

Third-party code compiled into ours (the header-only libraries) whose reports are its idioms goes in
`sanitize-ignorelist.txt`, by function name (`fun:`): FASTBuild compiles preprocessed sources (`-frewrite-includes`), so a
`src:` entry only ever sees the `.cpp`, never the header. FASTBuild doesn't track the ignorelist either: after changing
it, touch the sources it concerns (or rebuild). stb_image_resize2 is the one there: unaligned 8 byte stores, and srgb
tables indexed through a biased base pointer.

## Gotcha: `<windows.h>` macros

`core/file.h` includes `<windows.h>` (mio's), so everything reading files sees it, and its macros rename our code:
`LoadImage` becomes `LoadImageW` (and `CreateWindow` `CreateWindowW`: platform's is `CreateNativeWindow`) in the files that include it (a link error, `gfx::Scene::LoadImageW`, where the
definition didn't), and a function like macro named like one of its types rewrites the SDK (`capi.h`'s scalars are
`FLOAT1`/`UINT1`/`INT1` because `UINT(name)` turned `typedef UINT (CALLBACK *YIELDPROC)` into an error in
`mciapi.h`). `core/mio_extra.h` includes mio with `WIN32_LEAN_AND_MEAN`, `NOUSER` and `NOGDI` (mio needs only the
kernel api); other headers keep `<windows.h>` out (see `core/profiling.h`).

Narrow strings are UTF-8 on Windows too: the executables' manifest (`src/windows.manifest`) sets the process's ANSI code
page to UTF-8, which libc++'s `path::string()` and the narrow file APIs (mio, the importers) convert with. With the
system's (1252) it threw on the glTF sample `Unicode❤♻Test` when the file browser listed it. lld-link can't merge a
manifest without libxml2 or mt.exe, so it is a resource (`src/windows.rc`), and `CMakeLists.txt` turns CMake's
`/MANIFEST:EMBED` (in `Windows-Clang.cmake`'s link rule, after the target's flags) into `/MANIFEST:NO`. A new
executable target needs `windows.rc` in its sources.

## `setup.ps1` on Windows: no PowerShell Gallery modules

The Windows setup uses the `winget` CLI and `vswhere.exe` (fixed path under
`${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer`) rather than the
`Microsoft.WinGet.Client`/`VSSetup` modules. `-Scope CurrentUser` modules live in
`Documents\PowerShell\Modules`, which OneDrive often redirects; cloud-only files there are skipped
by module auto-loading, so `Get-InstalledModule` says installed while the cmdlet is "not
recognized". Don't reintroduce modules; also avoid hardcoded install roots (the Windows SDK root
comes from the `KitsRoot10` registry value).

## FASTBuild: PCH can race with sibling objects in the same `ObjectList`

When an `ObjectList()` both builds a precompiled header (`.PCHInputFile`/`.PCHOutputFile`/
`.PCHOptions`) and compiles other files that consume it via `/Yu`, FASTBuild does not reliably
finish the PCH before starting sibling `/Yu` compiles in the same list, despite that ordering being
the documented point of the feature. Symptom: the `/Yc` PCH compile itself reports success (fast,
~1s) but is truncated/corrupt by the time a sibling object reads it:
```
error: input is not a PCH file: '....pch'
fatal error: file '....pch' is not a valid precompiled PCH file: file too small to contain AST file magic
```
Confirmed this is *not* caching (reproduces identically with the local buildtree and the remote
`FASTBUILD_CACHE_PATH` fully wiped), *not* `CMAKE_FASTBUILD_USE_LIGHTCACHE` (reproduces with it OFF),
and *not* distributed builds (FASTBuild's job-slot count matches the local logical core count exactly
— no remote workers involved). Hit building the `llvm` port itself with FASTBuild 1.19 + clang-cl.
Don't re-derive any of the above when this recurs — just disable PCH for the affected port via
`-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` in its `vcpkg_cmake_configure()` `OPTIONS` (see
`ports/llvm/portfile.cmake`). Slower compile, but avoids the race.

## libc++ migration: MSVC STL "leaks" `<cstdlib>`/`<cmath>`/etc., libc++ doesn't

`scripts/cmake/toolchains/clang.toolchain.cmake` builds the `x64-windows-clang` triplet against
LLVM's own `libc++` (`-nostdinc++ -isystem .../include/c++/v1`) instead of MSVC's STL. MSVC's STL
headers transitively pull in a lot (`<string>`/`<windows.h>` commonly drag in `<cstdlib>`, `<cmath>`,
etc.), so third-party code that never explicitly included what it uses compiles fine under MSVC STL
and fails under libc++ with things like:
```
error: no member named 'abort' in namespace 'std'; did you mean simply 'abort'?
error: use of undeclared identifier 'getenv'; did you mean '_wgetenv'?
error: no member named 'pow' in namespace 'std'; did you mean simply 'pow'?
```
This is expected fallout of the libc++ switch, not a sign anything else is broken — and it will keep
surfacing as untouched ports get exercised for the first time under this triplet. Fix by adding the
missing `#include` via a patch on the specific port (see `ports/tracy/0005-*`/`0006-*`). If the port
isn't already a local overlay, copy its `portfile.cmake`/`vcpkg.json` from
`%LOCALAPPDATA%\vcpkg\registries\git-trees\<commit>\` into a new `ports/<name>/` overlay dir and add
the patch there (see `ports/parallel-hashmap/`). Generate patches with `git diff`/`git format-patch`
rather than by hand: an unchanged blank line inside a hunk must be a single space, not an empty line.
An empty one only works while the file has LF endings; with CRLF it is `"\r"` and `git apply` fails
with `corrupt patch at <file>:<line>` (`.gitattributes` now pins `*.patch` to LF as a backstop). The same class of bug can also hit our own code that
happened to rely on the same MSVC-STL leakage (see `src/rhi/vulkan/device.cpp`'s
`std::filesystem::path` → `std::string` conversion, which relied on an implicit conversion libc++
correctly rejects since `path::string_type` is `wstring` on Windows — fix with an explicit `.string()`
call, not a cast).

## `clang.toolchain.cmake`: variables from `vcpkg.cmake` aren't available yet when chainloaded

`VCPKG_CHAINLOAD_TOOLCHAIN_FILE` is `include()`d by `vcpkg.cmake` (`vcpkg/scripts/buildsystems/
vcpkg.cmake:208`) *before* it sets `CURRENT_INSTALLED_DIR`/`CURRENT_HOST_INSTALLED_DIR`/triplet info
(set later, ~line 233+). Code in `clang.toolchain.cmake` that reads those variables only works via a
path that doesn't need them — e.g. the `DEFINED ENV{LLVM_ROOT}` branch, which is always taken in the
real dev workflow because presets set that env var (see above). The `else()`/`elseif()` fallback
branches that read `CURRENT_INSTALLED_DIR` directly are effectively cold-configure-only code that a
normal, preset-driven, warm-cache workflow never exercises, so bugs in them go unnoticed for a long
time. Two found this way: `elseif(DEFINED CMAKE_CROSSCOMPILING AND ${CMAKE_CROSSCOMPILING})` breaks
when `CMAKE_CROSSCOMPILING` isn't defined yet (the empty expansion leaves `AND` with no right-hand
side) — use plain `elseif(CMAKE_CROSSCOMPILING)`. And `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` were
`set(... CACHE FILEPATH ...)` *without* `FORCE`, so once a bad value got cached (e.g. from hitting the
bug above on a cold configure), it stuck across every subsequent reconfigure of that build dir even
after the underlying cause was fixed — pair `CACHE` sets in this file with `FORCE`, like
`LLVM_ROOT`/`LLVM_TOOLS_BINARY_DIR` already do just above.

## Stale `build/<preset>/vcpkg_installed` shadows the real `install/` dir

Some `build/<preset>` directories carry their own `vcpkg_installed/` subfolder (left over from before
`VCPKG_INSTALLED_DIR` was pointed at the shared `install/` dir project-wide) that takes include-path
precedence over the correct shared copy. The symptom looks like a real source/API mismatch (e.g.
imgui symbols "not found" that plainly exist in the installed headers) but is actually just two
different vcpkg install trees on the same include path. Check
`build/<preset>/vcpkg_installed/<triplet>/share/<pkg>/*.list` for a suspiciously old version before
assuming a real API break; fix is deleting the stale `build/<preset>` directory and reconfiguring
(via `cmake --preset`, see above).

## CMake `IMPORTED_LOCATION` vs `CMAKE_MAP_IMPORTED_CONFIG_<custom config>`

For imported targets that only set a generic, unsuffixed `IMPORTED_LOCATION` (e.g. `Vulkan::Vulkan`
from CMake's builtin `FindVulkan.cmake`, unlike vcpkg's own `CONFIG` packages which set
`IMPORTED_LOCATION_RELEASE`/`_DEBUG`), a `CMAKE_MAP_IMPORTED_CONFIG_<CONFIG>` mapping for a custom
config name (we have `profile`, mapped to `release`) does not fall back to that generic property —
`<Target>-NOTFOUND` at link time for the custom config specifically, even though the exact same code
links fine for `debug`/`release` (neither needs a mapping, so they use the generic property
directly). Fix at the call site, not in the shared `FindVulkan.cmake`: after
`find_package(Vulkan REQUIRED)`, explicitly
`set_property(TARGET Vulkan::Vulkan PROPERTY IMPORTED_LOCATION_RELEASE "${Vulkan_LIBRARIES}")`
(see `CMakeLists.txt`).

## CMake presets: setting `"PATH"` in `environment` replaces it, doesn't extend it

Unlike other env vars, a preset `environment` entry named `PATH` is **not** merged with the
inherited process `PATH` unless you explicitly append `$penv{PATH}` yourself — `"PATH": "foo"` sets
the child process's `PATH` to exactly `foo`, dropping `C:\Windows\System32` and everything else.
`setup.ps1` (which generates `CMakeUserPresets.json`) got this right for the Linux `LD_LIBRARY_PATH`
entry (`` `$penv{LD_LIBRARY_PATH}: ``) but missed it for Windows `PATH`, so every build silently ran
with a `PATH` containing only the LLVM/mimalloc tool dirs. Symptom was easy to misdiagnose as harmless
because the overall build still reported success: `applocal.ps1` (vcpkg's app-local DLL deployment,
invoked post-link via a generated `.bat` calling bare `powershell.exe`) failed with `'powershell.exe'
is not recognized...`, and FASTBuild treated that failure as non-fatal. Once `powershell.exe` was
findable again, the *next* layer of the same bug showed up: `applocal.ps1` itself needs
`dumpbin`/`llvm-objdump`/`objdump` on `PATH` to inspect DLL imports, and `llvm-objdump.exe` lives
under `LLVM_TOOLS_BINARY_DIR` (`.../tools/llvm`), not `LLVM_ROOT/bin` — so it also needs to be added
explicitly, it's not implied by `LLVM_ROOT/bin` already being present. Both gaps are fixed in
`setup.ps1`'s `PATH` construction now; if a build "succeeds" but the built `.exe` is missing
sibling DLLs (`vulkan-1.dll`, `cpptrace.dll`, etc. next to it in the build dir), check the build log
for swallowed `applocal.ps1` errors before assuming the DLLs were never needed.

## vcpkg: overlay ports, per-port triplet options, and the fork registry

The registry in `vcpkg-configuration.json` is the `djohansson/vcpkg` fork (upstream release + the FASTBuild
commits). Its `"reference"` must name the fork branch the `baseline` commit lives on: without it vcpkg reads the
versions database from the fork's default branch and fails with `no version database entry for <port> at <ver>`.
Keep `builtin-baseline` in `vcpkg.json`, the registry `baseline`, and the `vcpkg` submodule on the same commit.

Prefer upstream ports over overlays in `ports/`. When an overlay only adds CMake options, put them in the
triplets instead (`if(PORT STREQUAL "<port>") list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS ...)`, see the end of
`scripts/cmake/triplets/*-clang.cmake`); request port features from `vcpkg.json` instead of overlaying the
consumer (e.g. `zeromq[draft]`). Editing a triplet or `clang.toolchain.cmake` changes the ABI hash of every
target package, so expect a full rebuild. Files they merely `include()` (e.g. `clang.rules-override.cmake`) are
*not* hashed: changing one does not rebuild anything, so bump something tracked if packages must be rebuilt.

Toolchain-level fixes that removed the need for port patches:

- FASTBuild preprocesses with `-frewrite-includes`, emitting GNU line markers that fail under a project's
  `-Wpedantic -Werror` (`-Wgnu-line-marker`). A flag in `CMAKE_<LANG>_FLAGS` doesn't help (target options come
  later and `-Wpedantic` re-enables it), so `clang.rules-override.cmake` appends `-Wno-gnu-line-marker` at the
  end of the compile rule.
- `_GNU_SOURCE` is not defined on Darwin: it is a glibc macro and only makes third-party code pick GNU
  variants of APIs, e.g. openssl's `strerror_r` (`incompatible integer to pointer conversion`).
- mimalloc is built with `MI_USE_CXX=OFF`: as C++ it links the static libc++/libc++abi into its dylib and
  exports them. Note that this generally applies to every C++ dylib here (zmq, cpptrace, TracyClient, slang, ...),
  since the host LLVM only ships static `libc++.a`/`libc++abi.a`.
- pdbs: vcpkg's `vcpkg_copy_pdbs` needs `dumpbin`, which isn't on the path for a chainloaded toolchain, and many
  ports install pdbs only under CMake's `MSVC` check, so on Windows no package had any. `x64-windows-clang.cmake`
  replaces the function (llvm-readobj for the path, llvm-pdbutil to check the guid) and runs it after every portfile
  (`VCPKG_POST_PORTFILE_INCLUDES`, `scripts/cmake/copy-pdbs.cmake`). The guid check catches targets sharing a pdb
  path: tidy-html5's `tidy.exe` overwrites `tidy.dll`'s, so that one has none.
- Ports that check `MSVC` take their Linux paths under our GNU style clang on Windows too: slang split its debug
  info with objcopy and strip (`SLANG_ENABLE_SPLIT_DEBUG_INFO=OFF`), and zstd, pugixml and draco need static
  linkage. miniz is static too: only slang uses it, for archive formats we don't use.

A port FASTBuild can't build at all can use another generator from its portfile (`vcpkg_cmake_configure(... GENERATOR
Ninja)`, see `ports/draco`: a shared library linked from object libraries only, and header-only object libraries). Ports
that link executables with GNU link groups (`-Wl,--start-group`) need a patch on apple platforms: ld64.lld has none, and
our clang reports itself as `Clang`, not `AppleClang`, which the ports' checks expect (`ports/draco/0001-*`).

FASTBuild generator pitfalls in third-party CMake (see `ports/tracy/0007-*`): a directory as a custom command
`OUTPUT` fails with `File missing despite success`, and custom commands that `DEPENDS` on a *target* name are not
ordered after it, so depend on the produced file (e.g. an `ExternalProject` `INSTALL_BYPRODUCTS`). ld64.lld does not
add `libobjc` implicitly the way Apple's linker does (`undefined symbol: objc_msgSend`, see `ports/tracy/0008-*`).

`ports/ktx` exists because IT policy blocks msys2's `bash.exe` on Windows: upstream's portfile acquires msys2 bash to
run KTX's `mkversion` script, and `0007-version-without-bash.patch` writes the same `version.h` from CMake instead
(bash is still required for the tests or `KTX_GENERATE_VK_FILES`, which we don't build). Don't add ports that need an
msys2 shell (`vcpkg_acquire_msys` with `bash`, `vcpkg_configure_make`) without patching that out; vcpkg's own
`pkgconf.exe` (`vcpkg_fixup_pkgconfig`) runs fine.

`ports/glfw3` exists for glfw 3.4's Cocoa display modes (`0001-cocoa-hidpi-display-modes.patch`): it lists them
without `kCGDisplayShowDuplicateLowResolutionModes`, so a Retina display's scaled modes are missing (its current one
too), and full screen switched the display to the 1x mode nearest in points; the patch lists them, and sets the HiDPI
mode rather than its 1x duplicate, as SDL does. Retire it once upstream glfw does the same. (Building it found the macOS
triplet setting `VCPKG_FIXUP_ELF_RPATH`, which runs patchelf over Mach-O files to no effect, and failed every port rebuilt
on the Mac since setup stopped installing patchelf: it is Linux's alone now.)

`ports/mimalloc` exists for a macOS bug in mimalloc 3.x, so retire it only once upstream fixes it. dyld allocates
each image's thread-local-variable block from the system zone (`malloc_type_malloc`) and frees it at thread exit
through the pthread key destructor `free`, which is interposed to `mi_free`. mimalloc's own thread locals are
included, so every exiting thread frees a block mimalloc doesn't own. Debug builds print
`mi_free: invalid pointer`, and both builds leak 32 bytes per thread. The patch interposes `free` with a check
that forwards foreign pointers to `malloc_zone_from_ptr(p)`.

`ports/mesa` (an overlay of vcpkg's, whose mesa is the OpenGL one) builds KosmicKrisp alone, the Vulkan driver over Metal,
from the djohansson/mesa fork's `kk-ray-query` branch (github.com/djohansson/mesa: upstream's `main`, from
gitlab.freedesktop.org/mesa/mesa, and the branch on top of it), which adds ray queries to it (`VK_KHR_acceleration_structure`,
`VK_KHR_ray_query`; see "Hardware ray queries"): the client's on macOS, a host dependency, since it is a driver the loader
loads at run time, not a library linked, so it builds with the host LLVM the toolchain builds anyway (its clang, whose
`mesa_clc` compiles KosmicKrisp's OpenCL kernels at build time; the driver links no LLVM). Its manifest
(`install/arm64-osx-release/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`) names the driver by its path from the
manifest, so it holds wherever the tree is; `assettest.ps1` and `perfbaseline.ps1` take it as `VK_DRIVER_FILES` unless one
is set. LLVM is static and without RTTI (`-Dshared-llvm=disabled -Dcpp_rtti=false`), and its static libraries are ThinLTO
bitcode (the host triplet's `-flto=thin`), which meson's architecture check on macOS (`lipo`) can't read, so it rejected
every clang module: the fork links such an archive by its path, as it does clang's resource headers from beside its tools
(vcpkg's LLVM keeps them there). `ports/spirv-llvm-translator` (not in vcpkg's registry: `mesa_clc` needs it, of LLVM's
version) is static, as LLVM is (a shared one with LLVM linked in would register LLVM's options twice), and built against
the SPIRV-Headers commit it pins (`spirv-headers-tag.conf`), put ahead of LLVM's include directory, the install tree's,
which holds vcpkg's older spirv-headers (`0001-*`).

**Updating the mesa fork from upstream.** The fork's `main` is upstream's alone, and `kk-ray-query` is our commits on
top of it. The checkout is `~/Repos/mesa`, with the remotes `origin` (gitlab.freedesktop.org/mesa/mesa, fetched
anonymously: no token needed) and `github` (github.com/djohansson/mesa, which `kk-ray-query` tracks). GitHub can't follow
a GitLab project, so it is updated by hand:

1. `git fetch origin main`, then `git push github origin/main:main`.
2. `git switch kk-ray-query`, `git rebase origin/main` (resolve, then build and test it as below), then
   `git push --force-with-lease github kk-ray-query`.
3. In `ports/mesa`: `REF` the branch's new head (`git rev-parse kk-ray-query`); `SHA512` its archive's
   (`curl -sL https://github.com/djohansson/mesa/archive/<REF>.tar.gz | shasum -a 512`, or the hash vcpkg's mismatch
   error prints); and `version` in `vcpkg.json` mesa's `VERSION` file's, without its `-devel`.
4. `cmake --preset <preset>` builds the port again (the host triplet's tree); run the quick tier on it (the scripts take
   its driver by default), and the ray query checks of "Hardware ray queries" one client at a time.
5. Commit the port and push.

Iterate on the driver itself in the checkout's own build, against Homebrew's LLVM and libraries (`brew install llvm
spirv-llvm-translator spirv-tools meson ninja`, and `.venv`, a Python venv with mako, pyyaml and packaging): once,
`meson setup build -Dbuildtype=debugoptimized -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dplatforms=macos
-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dllvm=enabled -Dtools= -Dvulkan-layers=
--prefix=$PWD/install`; then `ninja -C build install` (seconds when incremental), with
`PATH=$PWD/.venv/bin:/opt/homebrew/opt/llvm/bin:$PATH` and `SDKROOT=$(xcrun --show-sdk-path)`, and run the client on it
with `VK_DRIVER_FILES=~/Repos/mesa/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`. Commit there, push the
branch, then do steps 3 to 5.

## vcpkg's shared binary cache

A new clone restores its packages from a shared binary cache instead of building them (the user's ask, 2026-11-06: the
first setup built LLVM for most of an hour). `setup.ps1` sets `VCPKG_BINARY_SOURCES` (`scripts/binarycache.ps1`, into
`.env.json` and the presets' environment, so a configure's vcpkg run has it too): vcpkg's local default cache, read and
written; the Backblaze B2 bucket `danjo-artifacts` (region `us-west-001`), prefix `speedo/vcpkg/`, read over https by
vcpkg's `http` source (`https://s3.us-west-001.backblazeb2.com/danjo-artifacts/speedo/vcpkg/{sha}.zip`). The bucket is
public: reading needs no account, key or tool, and a missing key answers `NoSuchKey`, a 404, which vcpkg takes as a miss.
`SPEEDO_BINARY_CACHE=0` keeps to the local cache.

Writing is a maintainer's: with `SPEEDO_BINARY_CACHE_WRITE` set, setup adds vcpkg's `x-aws` source
(`x-aws,s3://danjo-artifacts/speedo/vcpkg/,write`, which runs the AWS CLI's `aws s3 cp` and `aws s3 ls`), installs the
AWS CLI (Homebrew's `awscli`, winget's `Amazon.AWSCLI`) and points it at B2 (`AWS_ENDPOINT_URL_S3`, `AWS_DEFAULT_REGION`); the credentials are the
AWS CLI's own, a B2 application key scoped to the bucket in `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` or a profile
(`AWS_PROFILE`), never in the repository. Both sources name an archive `<prefix><abi>.zip`, the package's ABI hash
(vcpkg-tool's `make_object_path` for `x-aws`; `{sha}` is the same hash for `http`). vcpkg uploads what it builds, not
what it restores from another source, so a machine whose packages were built before it wrote seeds the bucket with
`scripts/seed-binary-cache.ps1` (the archives of `install/vcpkg/status`'s packages from the local cache, those the bucket
lacks, each checked readable anonymously after; `-DryRun` lists them).

What makes a package's ABI hash the same on another machine, and different where its binaries would differ (the hash
covers the port's files, the triplet's and the chainloaded toolchain's text, the variables in `VCPKG_ENV_PASSTHROUGH`, and
its dependencies' hashes; the compiler isn't tracked, `VCPKG_DISABLE_COMPILER_TRACKING`, since it is a host package of the
same install):

- the cpu: the ports are built for a fixed baseline (`clang.toolchain.cmake`: `-mcpu=apple-m1` on Apple's arm64,
  `-march=armv8.2-a+crc+crypto` on other arm64, `-march=x86-64-v3` on x86-64), the project's own code for this machine's
  (`SPEEDO_TARGET_CPU=native`, which the presets set). With `-march=native` in the toolchain's text every machine had the
  same hash for different code, and an M4's packages would have handed an M1 instructions it lacks.
- the oldest macOS a package runs on: `MACOSX_DEPLOYMENT_TARGET`, the SDK's major version's first (`osx.ps1`: 27.0 for
  the 27.2 SDK), which every compile reads (clang, CMake and meson take it from the environment) and both osx triplets
  track, the host's too. Without it clang takes the SDK's own version: the target packages built with the 27.2 SDK
  required macOS 27.2, and LLVM and KosmicKrisp, built in October with the 27.0 SDK, required 27.0 while the host triplet
  tracked nothing, so the cache would have handed a Mac on macOS 26 a clang it can't run (the user's ask that the toolchain
  be part of the download). The SDK's path (`SDKROOT`) isn't tracked: the same SDK from Xcode and from the Command Line
  Tools has two paths, so the shared packages would have matched neither. A minor SDK update keeps the packages (their
  headers differ little; an unguarded call to an API newer than the target would only fail on an older minor macOS).
- the compiler: the llvm port's version and port version (`LLVM_PORT_VERSION`, from `ports/llvm/vcpkg.json`), tracked by
  the clang triplets: the target packages are built by that port's clang and link its static libc++, so after an LLVM
  update the shared cache must not hand out the old ones.
- Linux's C library (`GLIBC_VERSION`, `getconf GNU_LIBC_VERSION`), tracked by the Linux triplets, the host's too (LLVM):
  a package built against a newer glibc fails to load with an older one. GitHub's `ubuntu-26.04` has glibc 2.43,
  `ubuntu-24.04` (`ubuntu-latest` until it moves to 26.04) 2.39: a machine seeding the cache for CI runs the same Ubuntu
  release as the job (WSL2's Ubuntu 26.04 here, see "Linux: setup and the toolchain").
- not Windows' toolset and SDK versions (Visual Studio's MSVC tools and the Windows SDK, whose libraries the packages
  link): a package built with older ones links with newer ones (MSVC's binary compatibility runs that way, and the
  packages use only its runtime, libc++ being static), and GitHub's Windows image updates Visual Studio every few weeks,
  each of which would rebuild every package there, LLVM too. A machine whose Visual Studio is older than the one that
  built the packages (VS 2022 Build Tools 17.14, tools 14.44, SDK 22621, on 2026-11-07) may fail to link them: update
  it, or build them (`SPEEDO_BINARY_CACHE=0`). Setup accepts the Windows 11 SDK 22621 or 26100, the hosted image's.
- not the line endings Git checked out: vcpkg hashes the overlay ports' files, the triplets and the toolchain (and the
  post-portfile includes) as they are on disk, and Git for Windows' `core.autocrlf` checks them out with CRLF, so a
  Windows clone's hashes depended on its Git settings (one with it off would have missed every package, LLVM included).
  `.gitattributes` checks `ports/**` and `scripts/cmake/**` out with LF everywhere; the four files committed with CRLF
  keep it (`text=auto`), the same bytes everywhere. The registry's ports are extracted as their blobs are (the fork's
  `* -text`; checked, all 220 here), and vcpkg's own scripts too. The CMake and PowerShell in the hashes are vcpkg's
  downloads, pinned by its tool, not the system's.

The host packages (`arm64-osx-release`: LLVM, the toolchain itself, mesa's KosmicKrisp, the spirv tools) are built by the
system's clang with no cpu flag, so the deployment target was what they lacked. On macOS arm64 the cache holds 86
packages, 1.51 GB: LLVM 1.1 GB, the 74 target packages 0.41 GB. The cpu baseline rebuilt the target packages in 14
minutes on the M4 Pro; the deployment target rebuilt everything, LLVM in 52 minutes (twice the session ending killed
vcpkg partway: setup again picks up where it stopped). `seed-binary-cache.ps1` uploaded the 86 packages, 1.51 GB, in 408 s.
A second clone at another path restored all 84 of its packages from the local cache in 20 s and built none (the keys
don't depend on the clone's path); a fresh clone with an empty local cache (`VCPKG_DEFAULT_BINARY_CACHE` an empty dir)
restored all 84 from the bucket in 2.4 minutes, setup's whole run 165 s, and built none. A fresh clone has no `.vscode`
(it isn't tracked), which `setup.ps1` failed writing `launch.json` into until it created it.

On Windows x64 (2026-11-07, Ryzen 9 7950X) the cache holds 72 packages, 2.20 GB: LLVM 1.8 GB (`x64-windows-clangcl`, built
by Visual Studio's clang-cl), the rest the target packages (`x64-windows-clang`). Pinning the hashed files' line endings
rebuilt them all in 27 minutes, LLVM in 11 (FASTBuild's cache); `seed-binary-cache.ps1` uploaded them in 287 s. A fresh
clone at another path (with `core.autocrlf` on, as Git for Windows installs it) and an empty local cache restored all 72
from the bucket in 4.1 minutes, setup's whole run 286 s, and built none. The CI's first Windows job restored them all and
passed in 7m43s. The Linux toolchain changes (below) rebuilt the target packages again, in 16 minutes with their uploads.

On Linux x64 (2026-11-07, Ubuntu 26.04 in WSL2 on the same Ryzen, 32 threads and 48 GB, `.wslconfig`'s `memory`: half the
machine's 63 GB, WSL's default, is tight for LLVM's ThinLTO links) the cache holds 88 packages: LLVM (`x64-linux-release`,
built in 35 minutes, uploaded in 2) and the target packages (`x64-linux-clang`, about 20 minutes a rebuild). Seeded from
a clone in the distribution's own filesystem (not `/mnt/d`: slow, and Windows' line endings), with the AWS CLI reading the
Windows side's credentials (`AWS_SHARED_CREDENTIALS_FILE=/mnt/c/Users/<user>/.aws/credentials`, `AWS_CONFIG_FILE`,
`AWS_PROFILE`) rather than a copy of the key, `SPEEDO_BINARY_CACHE_WRITE` set on the setup command itself.

A run that fails partway has uploaded what it built, under hashes the fix makes stale (each triplet or toolchain edit
changes every target package's), and every toolchain change leaves the previous packages behind: 587 such archives
(4.6 GB) on 2026-11-07. `scripts/prune-binary-cache.ps1` deletes the archives no platform installs: `-Keep` takes every
platform's list, a status file (`install/vcpkg/status`) or a list of ABIs that `-WriteKeepList <file>` writes on a machine
whose status isn't at hand (the Mac, WSL); `-DryRun` lists what would go, by upload day; more than half of the bucket is
refused without `-Force` (a platform's list left out). Allow it alone in Claude Code's permissions rather than `aws s3api
delete-objects` (a bulk delete from the bucket was blocked without it). B2 keeps a deleted file's versions unless the
bucket's lifecycle keeps only the last, so that setting decides whether pruning frees the storage.

## Linux: setup and the toolchain

Linux setup was first run from scratch on 2026-11-07 (it had only seen a Raspberry Pi, arm64, in 2025). What a fresh
distribution needed, beyond what GitHub's image has:

- `setup_linux.sh` installs the distribution's ICU (`libicu<version>`, found by name): PowerShell, on .NET, aborted
  without it.
- `linux.ps1` installs a compiler and what vcpkg's bootstrap and builds take: `build-essential`, `clang`, `lld`, `llvm`,
  `curl`, `zip`, `unzip`, `tar`, `python3-venv` (meson's ports, libsystemd), `ninja-build` (meson builds with the one on the
  path), `autoconf-archive` and `automake` (autotools' ports: libxcrypt, libmount, gperf), and the AWS CLI for a cache
  writer (AWS's installer: Ubuntu's archive dropped `awscli` in 24.04).
- FASTBuild: its Linux zip was made on Windows and holds no file modes, so the `fbuild` vcpkg extracted wasn't executable
  ("Unable to find FASTBUILD"). The fix belongs in the fork's `vcpkg_find_acquire_program(FASTBUILD)` (a chmod after the
  extraction), but `vcpkg-cmake`'s hash names that script, and so every package's on every platform: `linux.ps1` extracts
  it where vcpkg looks first, executable, by the script's url and SHA-512. Move it into the fork with the next change that
  rebuilds everything anyway (a vcpkg update).
- The host triplets (`x64-linux-release`, `arm64-linux-release`) build with the system's clang, lld and LLVM's archiver,
  as Apple's toolchain is on macOS: their `-flto=thin` is clang's, and ThinLTO's bitcode takes LLVM's linker and archiver.

`clang.toolchain.cmake`'s Linux branch (which changes every target package's hash on every platform, so the Windows and
macOS packages were rebuilt and seeded again after it):

- libc++'s headers and libraries per target triple (`include/<triple>/c++/v1`, `lib/<triple>`), x86_64's or aarch64's:
  it named aarch64's alone, and x64 found no libc++ to link.
- libc++ static (`-Wl,-Bstatic,-lc++,-lc++abi,-Bdynamic -lm`), as on macOS, where LLVM ships it alone: LLVM builds it
  shared too on Linux, and vcpkg makes its installed binaries' rpaths relative, so `slangc` couldn't load `libc++.so.1`.
  One `-Wl` token, since vcpkg's autotools ports strip a library's `.a` and prefix `-l` (`-l:libc++.a` became `-l:libc++`).
- no `CMAKE_BUILD_WITH_INSTALL_RPATH` (macOS has none): with it, what a port runs from its build tree (shader-slang's
  `slang-embed`) couldn't load its dependencies' shared libraries (miniz, lz4).
- the triplets' per configuration flags (`VCPKG_C_FLAGS_RELEASE`, ...) last, after the configuration's own, so a port's can
  override them: libsystemd's meson build refuses `-ffinite-math-only`, which `-ffast-math` implies (`-fno-finite-math-only`
  in its release flags).

Per port, in the Linux target triplets: libpng without its linker version script (FASTBuild links it before the custom
command writing `libpng.vers` has run; macOS's linker has none), zeromq with `<new>` force-included (`std::nothrow`
without it; and `VCPKG_C_FLAGS` set whenever `VCPKG_CXX_FLAGS` is, as `vcpkg_cmake_configure` requires). Overlay patches,
on every platform: draco's `<iterator>` (`std::back_inserter` without it, `0003-include-iterator.patch`), and basisu's SSE
kernels in ktx built with the x64 baseline's AVX2 as the rest of the library (`0008-basisu-sse-with-avx.patch`: they
`#error` with AVX unless `_MSC_VER`, which clang defines on Windows alone; the user's call to keep them, with the best
instruction set, rather than turn SSE off). The project's profile, debug and release builds then pass on Linux.

## CI: GitHub Actions, the cpu's work only

`.github/workflows/ci.yml` (the user's call, 2026-11-06: hosted runners only, no gpu workloads, since the client's are too
heavy for a runner's software Vulkan; and not this machine as a runner) builds and checks on pushes and pull requests to
`master`: setup (`setup.ps1` directly, reading the shared binary cache without credentials: `setup_osx.sh` only adds
PowerShell, which the image has, and upgrades all of Homebrew), `cmake --preset`, the build, and `assettest.ps1` without
`-Client` (the importers, images, environments, the lens check) on the quick tier's assets, fetched with
`fetch-test-assets.ps1 -Minimal` (only what `quick.txt` names: 555 MB in 103 s where the full sets are 4.5 GB; it
never narrows what is there) and kept between runs by `actions/cache`. vcpkg's packages come from the bucket each
run: vcpkg doesn't keep what it restored from a remote in its local archives, so caching those would keep nothing. macOS
first, on GitHub's `xcode-27` image (macOS 27 on Apple silicon, the SDK major version the cache's packages target;
`macos-latest` and `macos-26` are macOS 26, where every package would be built, LLVM too, past the job's two hours).
Windows (2026-11-07) on GitHub's `windows-2025-vs2026` image (Visual Studio 2026 with the components setup requires and
the Windows 11 SDK 26100, CMake 4.4, PowerShell 7.6), restoring the packages built here with VS 2022 (see the toolset
above); the image has no winget, which setup only asks for when it must install something (CMake older than 4.2.3, no
Visual Studio with the components, the AWS CLI for a cache writer). Linux (2026-11-07) on `ubuntu-26.04`, by its own label
(see the glibc above), its apt lists updated before setup. Each job builds the profile, debug and release configurations
in turn (the user's ask, 2026-11-07), the asset checks on the profile build: one job a platform, since the configurations
share their packages and a job each would restore them, about 2 GB, three times. It replaced three workflows from an early trial
(`lukka/run-vcpkg` templates, on a `main` branch this repository doesn't have, so they never ran).

## Releases

The client runs from a checkout or from an install (the user's ask, 2026-11-07: releases on GitHub, packaged for each
platform's installer; the client only, unsigned for now). `ClientCreate` tells them apart: run from a checkout's root
(`resources/` and `src/gfx/shaders/shaders.slang` there), everything is as the scripts expect (the root the working
directory, the user profile in `.speedo`, the test assets in `resources/test-assets`); otherwise the data is beside the
executable (`core::file::GetExecutablePath`): a macOS bundle's `Contents/Resources`, else `<prefix>/share/speedo` (an
AppImage's `usr/share/speedo`, Windows' install directory's `share/speedo`), holding `resources/` and `shaders/`, and the
user's profile and test assets in the platform's user data directory (`core::file::GetUserDataPath`: `~/Library/Application
Support/Speedo`, `%LOCALAPPDATA%\Speedo`, `$XDG_DATA_HOME/speedo` or `~/.local/share/speedo`). `-r` and `-u` choose the
resource and profile directories either way. The environment's `ShaderPath` and `TestAssetsPath` name the shader sources
(compiled at startup, so an install ships them) and the test assets, which the scene, the ImGui renderer, the thumbnails'
render version and the file browser took from the root before. A client copied into `bin/` with its data in
`share/speedo`, run from `/tmp`, found its shaders there, compiled them into its profile and drew the helmet. A first
launch's compile (about 15 s on the M4 Pro) runs in the background behind a progress bar, the ui drawn meanwhile (see
layering.md's shaders), and it offers the test assets to download meanwhile (see assets.md: their manifests are installed
with the resources, `resources/test-asset-manifests`).
