# Building: the toolchain, vcpkg and the sanitizers

A note of CLAUDE.md (its index names each): what the build failed on and how it is set up.

## Presets: always configure via `cmake --preset <name>`, never a hand-reconstructed command

`setup.ps1` generates `CMakeUserPresets.json` with the `LLVM_ROOT`/`LLVM_TOOLS_BINARY_DIR`/`PATH`
(and Visual Studio path) environment that `scripts/cmake/toolchains/clang.toolchain.cmake` and
`CMakeLists.txt`'s `find_program(MINJECT ...)` depend on. Copy-pasting the raw
`cmake -D... -S ... -B ...` invocation out of a VS Code CMake Tools log and re-running it by hand
skips that environment and fails in several different, confusing ways (empty `LLVM_ROOT` inside the
toolchain, `minject.exe` not found, etc.). Use `cmake --preset <name> -S <root>` instead — it's not
just shorter, the raw command is missing required state.

The same applies to a bare `cmake --build build/<preset>` from a shell when that dir needs to regenerate (e.g.
after `vcpkg.json`/`ports/` changed, or after *another* preset ran a vcpkg install into the shared `install/`
dir). The auto re-run of CMake doesn't have the preset environment, so vcpkg computes different ABIs, removes
the installed packages from the shared `install/` tree, and then fails rebuilding them (`Apple-clang.cmake`
case warning, glfw3 configure error). Every build dir then misses its dylibs (`Library not loaded:
@rpath/libcargs.dylib`). Recover with `cmake --preset <name>`, which restores the packages from the binary
cache. From a shell, always run `cmake --preset <name> && cmake --build build/<name>` as one command, joined by
`&&`, however recently the directory was configured. Two guards hold it since 2026-11-08 (three wiped install/ trees
in a day): `CMakeLists.txt` stops any configure without the presets' environment (`LLVM_ROOT`,
`LLVM_TOOLS_BINARY_DIR`) before `project()` runs vcpkg, so a bare build that reconfigures fails with that message
instead; and a Claude Code hook (`.claude/settings.json`, `.claude/hooks/require-preset.ps1`) refuses a command that
runs `cmake --build build/<name>` without `cmake --preset <name> &&` before it.

The source files are listed explicitly in `CMakeLists.txt` (`CORE_SOURCE_FILES`, `GFX_SOURCE_FILES`, ...): add a new
`.cpp` there. That changes `CMakeLists.txt`, which reconfigures the build dir (VS Code's CMake Tools does it with the
preset's environment). They were GLOBs, which only a configure re-reads, so a build dir that wasn't reconfigured after a
file was added failed to link (`undefined symbol` for what the new file defines); `CONFIGURE_DEPENDS` didn't fix that with
the FASTBuild generator: it picked up one new file, but twice its glob check didn't run, or the regenerate it triggered
(`cmake --regenerate-during-build`) did nothing.

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
The quick tier passes, assettest and clients. A cold shader compile is slow there: slang isn't instrumented, but its
allocations go through the sanitizer's allocator, which records a stack trace for each, so a client on a fresh profile
took 291 s alone (CornellBox, 2026-11-09) and four side by side 536 to 601 s, against the clients' 600 s timeout: hence
assettest's warm-up (see docs/notes/assets.md).

`<triplet>-tsan` is the same with `SPEEDO_SANITIZE=thread` (tsan combines with neither asan nor ubsan). tsan reports and
goes on, exiting with 66 (an abort, on macOS) once done; `assettest.ps1` sets `TSAN_OPTIONS` (the symbolizer, and
`tsan-suppressions.txt`) and fails a run on `WARNING: ThreadSanitizer`. The quick tier passes. What it found: printing
with `std::cout`/`std::cerr` from several threads races in libc++ (`operator<<` reads and resets the stream's `width()`
unlocked, which the standard says it mustn't): the asset loaders' reimport messages, from parallel texture loads. Print
with `std::println` (`stderr` for errors), one locked `fwrite` a line, which also keeps lines whole: there's no
`std::cout` or `std::cerr` left. `tsan-suppressions.txt` holds what isn't a race (nothing now). Our frames aren't
symbolized there (no line numbers), so a suppression names a function tsan prints, not one inlined into it.

Function local statics: our binaries bound `__cxa_guard_acquire`/`release` from the first C++ dylib exporting them
(libktx's in the client, cpptrace's in assettest: each links libc++abi statically) rather than the system's libc++abi,
whose functions alone tsan interposes (its own guard code is local to its runtime, nothing to forward to). So tsan never
saw a static's initialization published, and the threads reading a static first raced the one that initialized it: mio's
`page_size()` was suppressed, then on 2026-11-08 `gfx::bvh::Version()`'s (the spatial splits switch, read by parallel
model loads, once the BVHs were built on the cpu by default) failed 26 of the quick tier's clients. A tsan build on
macOS now compiles `src/core/tsanguard.cpp` into each executable and shared library (`CMakeLists.txt`, after the
targets): the guards over `std::atomic_ref`, which tsan instruments, releasing the guard's first byte that the compiler's
inline check acquires (`__tsan_atomic8_load`). An object's definition beats a dylib's export in lld, whatever the link
order (`nm -m` shows them defined in the executable). The mio suppression went with it. The same run found one real
race: every `gfx::bvh::Build` set tinybvh's fatal error handler, a plain global, from several threads at once (now once, a
static).

A report can hang a client rather than fail it: tsan symbolizes it holding its report lock, which needs dyld's loader
lock, while a Metal thread (`FPMTLMetricsMTLLayerTracking`, on the frame pacing queue) held that in a `dlopen` and waited
on tsan's lock to allocate. Two of the quick tier's clients sat at 300 to 900% cpu (every other thread spinning on the
report lock) until killed. It only ever follows a report, so the fix is the report's.

About one client run in 50 faulted inside the tsan runtime on macOS 27 (`MetaMap::AllocBlock`, `OnUserFree`: frees of
system frameworks' allocations, CoreUI's libcompression on a dispatch queue), not in our code. Our fatal signal handler
then hung the run (its stack trace allocates, which waited for the tsan lock the faulting thread held): a sanitized
build (`SPEEDO_SANITIZE`, also a compile definition) leaves SIGILL, SIGABRT, SIGFPE and SIGSEGV to the sanitizer, which
reports them itself and exits.

Third-party code compiled into ours (the header-only libraries) whose reports are its idioms goes in
`sanitize-ignorelist.txt`, by function name (`fun:`): FASTBuild compiles preprocessed sources (`-frewrite-includes`), so a
`src:` entry only ever sees the `.cpp`, never the header. FASTBuild doesn't track the ignorelist either: after changing
it, touch the sources it concerns (or rebuild). stb_image_resize2 is the one there: unaligned 8 byte stores, and srgb
tables indexed through a biased base pointer. Where the fix is a line, as upstream's own code elsewhere does it, the
port is patched instead: tinybvh's NEON builder handed each binning slice `slicebinbox[0] + i * 3 * AVXBINS`, indexing
past a 2D array's first row (ubsan's bounds check, `index 72 out of bounds for type 'float32x4x2_t[24]'`, in the full sets
on 2026-11-09, not the quick tier: its binned builder, the moving BVH's and `BuildBinary`'s, slices only nodes past
`MT_SIMD_SLICE_THRESHOLD`, 50000 triangles), where its x86 builder
passes `slicebinbox[i]` (`ports/tinybvh/0001-neon-slice-rows.patch`; the same addresses, so the BVHs and their caches
are unchanged).

## Every graphics api in one build: gfx and the client compiled per api

Every preset builds a client of every graphics api the platform has (Vulkan, and Metal on macOS), chosen at run time by `SPEEDO_GRAPHICS_API` (`vulkan`, the default, or `metal`; docs/environment-variables.md). rhi has every backend already (templated on `GraphicsApi`); gfx and the client name one (`gfx/gpu.h`'s aliases of `gfx::kGraphicsApi`), so they are compiled once per api (`CMakeLists.txt`'s `speedo_gfx_library` and `speedo_client_library`: `gfx` and `clientlib` Vulkan's, `gfx_metal` and `clientlib_metal` Metal's), each copy in an inline namespace of its own (`gfx::vk`, `gfx::metal`, `client::vk`, ...: every `namespace gfx` and `namespace client` is `namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE`, the macro a copy's public compile definition, with `SPEEDO_GRAPHICS_API_METAL` for Metal's, `gfx/graphicsapi.h`), so that both link into one executable without clashing and each stays monomorphic (no virtual calls, no templates through gfx). The executable's C api (`client/capi.cpp`, compiled once) runs the copy the variable names; it links the copies with `$<LINK_ONLY:...>`, since their compile definitions name each its own copy. What can't be compiled twice is outside the copies: the header only libraries' implementations (`gfx/libraries.cpp`, the `gfxlibraries` target: tinybvh, tinyobjloader, stb_image, stb_dxt, stb_image_resize2 and cgltf, whose functions twice don't link; with the code over cgltf's jsmn, `gfx::libraries`), the shader compiler library (`gfx::shadercompiler`, plain), core, platform and rhi. The tools take Vulkan's copy (`gfx`). The cost: gfx and the client compiled twice on macOS, and a larger executable (11.5 MB, profile). Was until 2026-11-13 a preset of its own, `<triplet>-metal` (the `SPEEDO_GRAPHICS_API` cache variable), the backends plan's step 9 replaced it. The backend's design and progress: docs/backends-plan.md.

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

## FASTBuild: precompiled headers with clang-cl overwrite themselves

With clang-cl, CMake's FASTBuild generator (4.4.5, and its master on 2026-11-09) builds no target with precompiled
headers. The `/Yc` PCH compile reports success (fast, ~1s), and the first object using the PCH fails:
```
error: input is not a PCH file: '....pch'
fatal error: file '....pch' is not a valid precompiled PCH file: file too small to contain AST file magic
```
It isn't a race, as these notes said until 2026-11-10 (when caching, the light cache and distribution had been ruled
out). For the MSVC-like compiler families FASTBuild takes the PCH compile's object file from `/Fo` in `.PCHOptions`, as
`%3` (`%2` is the PCH itself). CMake replaces the compile rule's `/Fo%2` with `%3` only when the compiler ID is `MSVC`,
and clang-cl's is `Clang`. So the PCH compile wrote its small object file over the PCH it had just written. A clang-cl
cross-build on macOS reproduces it (`CMAKE_SYSTEM_NAME=Windows`, `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`): `/Fo`
ends up as `$FB_INPUT_2_PLACEHOLDER$` in the `.bff`. CMake's issue #27697 lists the PrecompileHeaders tests among the
clang-cl failures, and its CI excludes them. The fix (replace it for the `msvc` and `clang-cl` families) is offered
upstream (see "vcpkg" below). Until a CMake release has it, turn PCH off for a port built with FASTBuild and clang-cl:
`-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`, in a triplet `if(PORT STREQUAL "<port>")`. The llvm port did, until it moved to
Ninja on 2026-11-09. Not in the Windows host triplet, though, whose every package an edit rebuilds, LLVM's too: there an
overlay of the port turns it off, `ports/glslang` (`-DENABLE_PCH=OFF`, glslang's own switch; mesa[lavapipe]'s
`glslangValidator`, the first Windows CI run with lavapipe failing on it, 2026-11-11). Drop the overlay with that release.

## FASTBuild's cache

The project's objects are cached (the user's ask, 2026-11-11): FASTBuild's cache, a local directory in front of the shared
B2 bucket's `speedo/fastbuild/` prefix, so that a build takes what any machine of the platform compiled, CI's runners
and the user's alike. Until then the cache setup.ps1 configured had never run: caching takes `-cache` or
`FASTBUILD_CACHE_MODE` in FBuild's own environment, and `cmake --build build/<preset>` gave it neither (the presets'
environment is the configure's alone).

**How.** FBuild takes a cache plugin only from `.CachePluginDLL` in the `.bff`'s Settings, and CMake's FASTBuild generator
(4.4.5) can't write it (no variable; `CMAKE_FASTBUILD_CACHE_PATH`'s value is escaped, rightly) nor pass `-cache` in
`cmake --build`; FBuild reads no variable for it either. So the overlay port `ports/fastbuild-s3cache` (a host tool,
built from its sources there, so that vcpkg's ABI follows them) builds two things: `FBuildS3Cache`, the plugin
(FASTBuild's `CachePluginInterface.h`: `CacheInitEx`, `CacheRetrieve`, `CachePublish`, `CacheFreeMemory`, `CacheShutdown`,
and `CacheOutputInfo` and `CacheTrim` for the local directory), and `fbuild-s3cache`, the launcher the presets make
`CMAKE_MAKE_PROGRAM`. `cmake --build` runs it as FBuild (first for the `rebuild-bff` target, which regenerates the `.bff`,
then the build): it adds the plugin's two lines to the `.bff`'s Settings in place (only after CMake wrote it afresh,
so that FBuild doesn't parse it again), `-cache` to the command line (unless it or `FASTBUILD_CACHE_MODE` chose a mode),
and runs vcpkg-tool-fastbuild's FBuild beside it. On Windows the plugin delay-loads curl and loads it by its path from
beside itself, its zlib with it (`LoadCurl`): imported, FBuild's `LoadLibrary` of the plugin searched FBuild's directory,
the system's, then the working directory, the build directory, before the path (where the launcher had put its own
directory), and took the client's `libcurl.dll` and `z.dll` there, which vcpkg's applocal step copies beside the
executables. Loaded for the whole build, they failed that step once a baseline brought newer ones
(`copy_file(...z.dll...): The process cannot access the file because it is being used by another process`, the first
Windows build after the 2026-11-12 baseline, every build after it). The remote's url and region come from the build
directory's `CMakeCache.txt` (`SPEEDO_FASTBUILD_CACHE_URL`, `SPEEDO_FASTBUILD_CACHE_REGION`, the presets', from
`scripts/binarycache.ps1`). The plugin reads the local directory (FBuild's cache path, else `~/.fastbuild/cache`), else
the remote object (an anonymous GET), keeping it locally; it publishes locally, and remotely where the machine writes
the shared caches (`SPEEDO_BINARY_CACHE_WRITE`, as vcpkg's, with the AWS credentials: `AWS_ACCESS_KEY_ID` and
`AWS_SECRET_ACCESS_KEY`, else `AWS_PROFILE`'s, never written anywhere), a PUT signed with curl's AWS Signature V4.
`SPEEDO_BINARY_CACHE=0` leaves the remote out, `SPEEDO_FASTBUILD_CACHE=0` the cache; the remote failing never fails a
build (a miss; 8 failures in a row leave it out for the rest of the build, a 403 on a PUT its writes). The local
directory grows until trimmed: `cmake --build build/<preset> -- -cachetrim <MiB>` (least recently used first). The
launcher goes once a CMake release can name the plugin (a patch for the user to offer: `CMAKE_FASTBUILD_CACHE_PLUGIN`,
its config, and a cache mode for `cmake --build`).

**Keys the same everywhere.** A key holds each compile's command line, its paths too, and a checkout elsewhere (a clone
at another path, as CI's runners are) took 0 of the profile build's 97 objects from a cache another had filled. What it
took: relative paths in the compile commands (`CMAKE_FASTBUILD_USE_RELATIVE_PATHS`, the presets'), 2 of 97; the libc++
headers relative too (CMakeLists.txt rewrites the toolchain's absolute `-isystem` for the project's compiles: the
toolchain can't, its text being in every package's ABI, and the ports build elsewhere), 93; the slang kernels assettest
compiles to C++ generated from the build directory with relative paths and their prelude's include by name
(`scripts/cmake/slangcpp.cmake`), 96; and no `glfw3native.h` in `src/platform` (its `#import <Cocoa/Cocoa.h>`, in an
`__OBJC__` branch C++ never takes, made FASTBuild's light cache, which reads every branch, preprocess the file instead,
the output's absolute paths in its key: its two functions are declared instead), every object light cached. Nor
`-march=native`, which shares an M4's objects with M1s (see the binary cache's cpu above; the project builds for the
baseline since 2026-11-11). Keys also hold the compiler's binaries, so machines share objects when their LLVM comes from
the binary cache, and a platform's only with its own. Two consequences: `__FILE__` is relative (an assert names
`../../src/platform/glfw.cpp`), and an object another machine compiled has that machine's build directory as its debug
info's compile directory (`DW_AT_comp_dir`, not in the key), so a debugger finds its sources only through a source map
(lldb's `target.source-map`) until it is compiled here. FASTBuild's `.SourceMapping_Experimental`
(`CMAKE_FASTBUILD_SOURCE_MAPPING`) would make that one fixed root on every machine, which every debugger would then map:
not done.

**Measured** (M4 Pro, the profile preset, a clean build, 97 objects): compiled 36 s; with the cache, cold (every object
looked up, 404s, and uploaded) 48 s; from the bucket alone (the local directory emptied) 13 s; from the local directory
0.4 to 0.8 s. 71 MB of objects a configuration. CI's key writes the prefix too (a key for the bucket): the first CI run
with the plugin uploaded its three platforms' objects, 969 with this machine's, 0.62 GB.

**Pruning.** By age, in CI's prune job (`prune-binary-cache.ps1`, after the vcpkg archives; `-FastbuildOnly` alone by
hand): an object last dated more than `$FastbuildCacheExpiryDays` (30, `scripts/binarycache.ps1`) ago goes, every version
of it. A GET doesn't date an object, so the plugin, where it writes, dates anew one it retrieves once it is older than
half that (`refresh_days`, the presets' `SPEEDO_FASTBUILD_CACHE_REFRESH_DAYS`): a copy onto itself in the bucket (S3's
CopyObject, `x-amz-metadata-directive: REPLACE`), one request and no upload. What builds keep using never expires (CI's
runners, starting empty, retrieve everything master uses at least every run); what changed code left behind goes a month
after its last use. Each copy leaves the version before it (B2 keeps a file's versions until deleted by id), which the job
deletes too, as it does delete markers, so the storage is freed whatever the bucket's lifecycle settings. Tested on the
bucket with the refresh age near 0: 97 hits, each a copy (a clean build 20 s from the bucket against 13 without), and the
dry run then counted exactly 97 superseded versions. Local caches don't expire: `-cachetrim <MiB>`.

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
isn't already a local overlay, copy its port at the registry's baseline (see the vcpkg section below) into a new
`ports/<name>/` overlay dir and add the patch there (see `ports/ppqsort/`). When the baseline moves, rebase an overlay
on upstream's port the same way (its newer patches kept, ours put back after them, each checked with `git apply --check`
on the new sources). Generate patches with `git diff`/`git format-patch`
rather than by hand: an unchanged blank line inside a hunk must be a single space, not an empty line.
An empty one only works while the file has LF endings; with CRLF it is `"\r"` and `git apply` fails
with `corrupt patch at <file>:<line>` (`.gitattributes` now pins `*.patch` to LF as a backstop). The same class of bug can also hit our own code that
happened to rely on the same MSVC-STL leakage (see `src/rhi/vulkan/device.cpp`'s
`std::filesystem::path` → `std::string` conversion, which relied on an implicit conversion libc++
correctly rejects since `path::string_type` is `wstring` on Windows — fix with an explicit `.string()`
call, not a cast).

## `clang.toolchain.cmake`: variables from `vcpkg.cmake` aren't available yet when chainloaded

`VCPKG_CHAINLOAD_TOOLCHAIN_FILE` is `include()`d by `vcpkg.cmake` (`<vcpkg root>/scripts/buildsystems/
vcpkg.cmake:208`, line numbers as of 2026) *before* it sets `CURRENT_INSTALLED_DIR`/`CURRENT_HOST_INSTALLED_DIR`/triplet info
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

## vcpkg: overlay ports, per-port triplet options, and upstream's registry

vcpkg is upstream's (microsoft/vcpkg), without a clone since 2026-11-09 (it was a submodule): setup installs the vcpkg
tool and its scripts bundle (`vcpkg-standalone-bundle.tar.gz`: `scripts/` and `triplets/`, no ports), the release
`scripts/vcpkg-tool.json` pins with their SHA-512s, into `~/.vcpkg-tool/<release>` (`SPEEDO_VCPKG_ROOT` for another; not
`~/.vcpkg`, vcpkg's own one-line install, which updates itself), the downloads in `~/.vcpkg-tool/downloads` (shared by
every clone and release; `VCPKG_DOWNLOADS`), and the ports' build trees and packages in the clone's `temp/vcpkg`. The
ports come from the git registry `vcpkg-configuration.json` names (microsoft/vcpkg at 7f7b217d, its master of
2026-11-12, past the 2026.10.16 release for the llvm port's fix; it was the release, 9eec002d, until then), which vcpkg
fetches into `~/.cache/vcpkg/registries` once a machine. The bundle's scripts (ab9a05e7, 2026-10-28, tool 2026-10-29)
are older than the ports since: a port that calls a newer helper of vcpkg's scripts fails there, and the tool's release
moves then. A bump: the tool release and its hashes in `scripts/vcpkg-tool.json` (upstream's
`scripts/vcpkg-tool-metadata.txt` lists the Linux and macOS ones), and the registry's baseline. Every package rebuilds
then (the scripts a portfile uses, and vcpkg's CMake, are in its hash): seed the shared cache from each platform
afterwards (see below). An upstream port to rebase an overlay on: `git show <baseline>:ports/<name>/<file>` in the
registry's cache, or a clone.

Until 2026-11-09 it was the `djohansson/vcpkg` fork (a release plus FASTBuild support: `vcpkg_find_acquire_program(
FASTBUILD)` and its tools entries), as a git registry. Retired at the user's call (no fork to maintain): FASTBuild still
builds the ports, through the overlay of `vcpkg-cmake` (FASTBuild the default generator, Ninja where a port asks for it
with `PREFER_NINJA` or names it, or a triplet names one in `VCPKG_CMAKE_GENERATOR`, as for llvm), which takes FBuild from the `vcpkg-tool-fastbuild` host port (its dependency; FASTBuild
1.20 since 2026-11-09, the project's own builds' too). The overlay's changes are offered upstream (microsoft/vcpkg PRs),
after which it could go. The fork's other changes were unused: compiler detection (every triplet disables compiler
tracking) and `ports.cmake`'s CMake minimum.

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

llvm is vcpkg's own port since 2026-11-12 (23.1.5#1), at the registry's baseline: setup reads its version from the
registry's `versions/baseline.json` at that commit for `LLVM_PORT_VERSION` (see the binary cache below), so a baseline
move that moves llvm rebuilds every package. An override in `vcpkg.json` would pin it; the user decided against one
(2026-11-12: one version fewer to maintain). Until then `ports/llvm` was vcpkg's 23.1.3 port but for one fix, upstream
since (microsoft/vcpkg#54988, merged 2026-11-12): its `libcxx` feature required `libcxxabi`, which LLVM refuses for
MSVC targets (a `FATAL_ERROR` in libcxxabi's `CMakeLists.txt`; vcpkg's CI lists `llvm[libcxxabi](windows)=feature-fails`
and skips llvm), while Windows takes LLVM's libc++ (over the vcruntime ABI) for the clang triplets; so `libcxxabi` is
required only off Windows, and supported only there. The baseline moved past the merge rather than waiting for a vcpkg
release (the user's call). Until 2026-11-09 it was our fork of an older vcpkg port (LLVM 20's, bumped since), whose
differences were FASTBuild workarounds and our options. CMake's FASTBuild generator (4.4.5)
can't build LLVM: a custom target with only file `DEPENDS` and no `COMMAND` (clang's `hmaptool`) depends on
its never-written `CMakeFiles/<target>` and `.rule` (the generator drops command-less custom commands, then lists the
target's sources, those symbolic files among them, as its inputs; the fork patched hmaptool's target out), clangd's XPC
framework is a custom command whose `OUTPUT` is a directory (`File missing despite success`; the fork turned XPC off),
and with clang-cl its precompiled headers overwrite themselves (above; the fork turned PCH off). The host triplets build
it with Ninja (`VCPKG_CMAKE_GENERATOR`), which needs none of them. On Windows, Ninja with clang-cl and rc.exe compiles
`.rc` files through `cmcldeps`, which scans their headers by running the C compiler (clang-cl) on rc's flags, and clang-cl
takes vcpkg's `/c65001` (rc's code page, `CMAKE_RC_FLAGS` in its `windows.cmake`) for a file name (`no such file or
directory: '/c65001'`, where cl.exe only warns): the first Windows setup after the switch failed on LLVM's version
resources, so the Windows host triplet passes `CMAKE_NINJA_CMCLDEPS_RC=OFF` for llvm (rc.exe runs alone, its headers
untracked, which a port's one build doesn't need; checked by rebuilding the failed `.res` files in the build tree with
it). The first and third are CMake bugs, fixed for CMake 4.6 (cmake/cmake!12803 and !12804, merged to its master on
2026-11-12 and 11; the user submitted them from a CMake clone, `~/Repos/cmake`, MR texts in `~/Repos/cmake-mrs`: CMake's
AI policy has a human submit every MR, and lists no AI co-author). With a CMake release that has both, llvm can go back to
FASTBuild with clangd's XPC off. The directory output is FASTBuild's design (an Exec node must write a file:
fastbuild#1024, closed as not a bug), and CMake documents outputs as files, so it stays a port patch or Ninja. The macOS
triplet passes the options the fork had: libc++ and libc++abi static (the system's `libc++.1.dylib` and libc++abi
aren't ABI compatible with LLVM's), and clangd without XPC (nothing here uses it). Upstream's port builds the static
analyzer (the fork turned it and ARCMT off: its tools, scan-build and clang-check among them, are what the install
gained; nothing moved), compiler-rt as a runtime rather than a project (the sanitizers' runtimes are where they were),
and on Windows depends on `atl` (a stub checking that Visual Studio has ATL), which the fork dropped in 2025 for
rebuilding LLVM at every setup, before the binary cache. LLVM builds in 47 minutes with Ninja on the M4 Pro (44 with
FASTBuild before), the whole setup in 60.

`ports/ktx` exists because IT policy blocks msys2's `bash.exe` on Windows: upstream's portfile acquires msys2 bash to
run KTX's `mkversion` script, and `0007-version-without-bash.patch` writes the same `version.h` from CMake instead
(bash is still required for the tests or `KTX_GENERATE_VK_FILES`, which we don't build). Don't add ports that need an
msys2 shell (`vcpkg_acquire_msys` with `bash`, `vcpkg_configure_make`) without patching that out; vcpkg's own
`pkgconf.exe` (`vcpkg_fixup_pkgconfig`) runs fine.

`ports/glfw3` exists for glfw's Cocoa display modes (3.4, and still 3.5.1, which the overlay is on since 2026-11-09) (`0001-cocoa-hidpi-display-modes.patch`): it lists them
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

`ports/mesa` (an overlay of vcpkg's, whose mesa is the OpenGL one) builds mesa's Vulkan drivers alone, a feature each:
KosmicKrisp (`kosmickrisp`, the default on macOS arm64), the Vulkan driver over Metal, and lavapipe (below), from the
djohansson/mesa fork's `kk-ray-query` branch (github.com/djohansson/mesa: upstream's `main`, from
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

Its `lavapipe` feature builds lavapipe (meson's `swrast` Vulkan driver over gallium's `llvmpipe`), mesa's Vulkan driver on
the cpu, for CI's visual checks (see "CI"): a host package on every platform (`vcpkg.json`; on macOS beside KosmicKrisp,
since 2026-11-13), from the same source and LLVM. On Linux without a window system platform (`-Dplatforms=`: CI's clients are headless, and
x11's and wayland's would look for the system's xcb and wayland outside the vcpkg tree), with the port's zlib and the
system's bison and flex (the Linux port build tools). lavapipe links LLVM (`default-targets`: llvmpipe compiles shaders for the cpu at run time),
and builds its BVH builder's shaders with `glslangValidator` (a host `glslang[tools]`, overlaid without precompiled
headers, see the FASTBuild section). Its manifest, `lvp_icd.x86_64.json` in the host triplet's `share/vulkan/icd.d`, names
the driver in `bin` by its path from the manifest, with backslashes (LoadLibrary takes no others), written with
`string(JSON)`, which escapes them: a regex replacement unescaped them once, and the loader skipped the invalid manifest
(`VK_ERROR_INCOMPATIBLE_DRIVER`). Mesa's generators run with python's `-E -s` (vcpkg's port's flags), not `-I`, which
leaves a script's own directory out of `sys.path`, and they import their siblings. Windows needs the port's zlib and
DirectX-Headers (Vulkan's Win32 WSI includes `d3d12.h`, though meson.build finds it optional), and clang-cl a patch of
`u_atomic.h` (`0002`: MSVC's lower-case `_interlocked*64` intrinsics, which clang lacks, mesa issue 8902; the upstream
change is the user's to submit). On Windows the port bootstraps pip itself before
`x_vcpkg_get_python_packages` (mako, packaging and pyyaml for meson): the registry release's helper uses get-pip 24.2,
whose pip doesn't run on the Python 3.15 the newer scripts acquire there (upstream's took 26.2.2 on 2026-10-27,
d90cb72e90); drop it with a baseline past that commit. The feature built on macOS first in a scratch tree (2026-11-10:
the client headless on it, BumpDomes in 17 s); macOS's host package has it since 2026-11-13 (the user's ask: CI's macOS
job runs the visual checks on it too), with the macos platform as KosmicKrisp. The portfile's comments are in the port's
hash on every platform: correcting its first, which called the feature Windows' and Linux's, rebuilt mesa on all three
(the user's call). Release packages don't ship lavapipe (the bundle takes KosmicKrisp's dylib
and manifest by name), and list mesa's license only on macOS, where KosmicKrisp ships.

The port patches the fork's source, locally, with `0001-kk-workaround-63-encoder-end-resolve.patch`: a workaround for an
Apple bug (Metal ray tracing beside other gpu work restarts the gpu, see "Hardware ray queries"), until Apple fixes it,
numbered 63 to keep clear of upstream's `KK_WORKAROUND_#` (18 at the end of October 2026), its reason in the portfile's
comment rather than in Mesa's code. It isn't for upstream as it is: Mesa wants such a change's commit, its code comments and
its entry in `docs/drivers/kosmickrisp/workarounds.rst` in the contributor's own words, and an AI written one labelled
(`Generated-by: LLM`, the tree's `AGENTS.md`). A change to the patch or the portfile changes the port's hash, so every
build directory rebuilds mesa (`cmake --preset <name>`), and the binary cache holds it once seeded again.

**Updating the mesa fork from upstream.** The fork's `main` is upstream's alone, and `kk-ray-query` is our commits on
top of it. The checkout is `~/Repos/mesa`, with the remotes `origin` (gitlab.freedesktop.org/mesa/mesa, fetched
anonymously: no token needed) and `github` (github.com/djohansson/mesa, which `kk-ray-query` tracks). GitHub can't follow
a GitLab project, so it is updated by hand:

1. `git fetch origin main`, then `git push github origin/main:main`.
2. `git switch kk-ray-query`, `git rebase origin/main` (resolve, then build and test it as below), then
   `git push --force-with-lease github kk-ray-query`.
3. In `ports/mesa`: `REF` the branch's new head (`git rev-parse kk-ray-query`); `SHA512` its archive's
   (`curl -sL https://github.com/djohansson/mesa/archive/<REF>.tar.gz | shasum -a 512`, or the hash vcpkg's mismatch
   error prints); and `version` in `vcpkg.json` mesa's `VERSION` file's, without its `-devel` (`port-version` back to 0
   or out). The port's patches must still apply (vcpkg stops at one that doesn't): refresh them against the new source,
   and drop a workaround once its bug is fixed.
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

Writing is a maintainer's and CI's: with `SPEEDO_BINARY_CACHE_WRITE` set, setup adds vcpkg's `x-aws` source
(`x-aws,s3://danjo-artifacts/speedo/vcpkg/,write`, which runs the AWS CLI's `aws s3 cp` and `aws s3 ls`), installs the
AWS CLI where it is missing (Homebrew's `awscli`, winget's `Amazon.AWSCLI`, AWS's installer on Linux; GitHub's images
have it) and points it at B2 (`AWS_ENDPOINT_URL_S3`, `AWS_DEFAULT_REGION`); the credentials are the
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
  `-march=armv8.2-a+crc+crypto` on other arm64, `-march=x86-64-v3` on x86-64), and so is the project's own code
  (`SPEEDO_TARGET_CPU=baseline`, which the presets set since 2026-11-11, the user's call: what a machine builds is shared,
  FASTBuild's cache too; it was `native` before, and cost 0.4% of the M4 Pro's frame at Sponza's benchmark pose, 22.92
  ms against 22.81). A build directory keeps the flags of its first configure (`CMAKE_CXX_FLAGS` is cached from the
  toolchain's `_INIT`), so one configured for native still builds for it whatever the variable says: `CMakeLists.txt`
  refuses it, configure it with `--fresh`. With `-march=native` in the toolchain's text every machine had the same
  hash for different code, and an M4's packages would have handed an M1 instructions it lacks. The toolchain still takes
  `SPEEDO_TARGET_CPU=native` (`-march=native+crc+crypto` on arm64), nothing sets it: taking it out changes the
  toolchain's text, in every package's ABI, so it waits for a change that rebuilds them anyway.
- the oldest macOS a package runs on: `MACOSX_DEPLOYMENT_TARGET`, the SDK's major version's first (`osx.ps1`: 27.0 for
  the 27.2 SDK), which every compile reads (clang, CMake and meson take it from the environment) and both osx triplets
  track, the host's too. Without it clang takes the SDK's own version: the target packages built with the 27.2 SDK
  required macOS 27.2, and LLVM and KosmicKrisp, built in October with the 27.0 SDK, required 27.0 while the host triplet
  tracked nothing, so the cache would have handed a Mac on macOS 26 a clang it can't run (the user's ask that the toolchain
  be part of the download). The SDK's path (`SDKROOT`) isn't tracked: the same SDK from Xcode and from the Command Line
  Tools has two paths, so the shared packages would have matched neither. A minor SDK update keeps the packages (their
  headers differ little; an unguarded call to an API newer than the target would only fail on an older minor macOS).
- the compiler: the llvm port's version and port version (`LLVM_PORT_VERSION`, the version vcpkg installs: an overlay's,
  else an override's, else the registry baseline's `versions/baseline.json` entry, downloaded once a baseline; see
  `Get-LlvmPortVersion` in `scripts/binarycache.ps1`), tracked by the clang triplets: the target packages are built by
  that port's clang and link its static libc++, so after an LLVM update the shared cache must not hand out the old ones.
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
  `.gitattributes` checks `ports/**` and `scripts/cmake/**` out with LF everywhere; the files committed with CRLF
  keep it (`text=auto`), the same bytes everywhere (`git ls-files --eol ports scripts/cmake | grep i/crlf` lists them). The registry's ports are checked out as their blobs are (vcpkg's
  `* -text`; checked, all 220 here, in the fork it was then), and vcpkg's own scripts too. The CMake and PowerShell in the hashes are vcpkg's
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

CI writes and prunes the cache where the repository holds a B2 application key (the user's ask, 2026-11-08): the secrets
`B2_APPLICATION_KEY_ID` and `B2_APPLICATION_KEY`, a key for the bucket with read, write and delete (B2's
`listBuckets,listFiles,readFiles,writeFiles,deleteFiles`; FASTBuild's cache, `speedo/fastbuild/`, is written and pruned
with it too, see "FASTBuild's cache"), never in the repository. `ci.yml` and
`release.yml` set `SPEEDO_BINARY_CACHE_WRITE` and the AWS CLI's `AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY` from them, on
every run but a pull request's, so a runner uploads the packages it had to build (a port or toolchain change pushed
without seeding the bucket first), and the next run on the platform restores them. After a push whose three platform jobs
passed, each having written its installed packages' list (`-WriteKeepList`; one triplet a platform, and the same manifest
features as its machines but Tracy's profiler app, see below), CI's prune job runs `prune-binary-cache.ps1` by those lists with
`-MinAgeDays 7`: an archive no list names is kept for a week from its upload, since master's lists don't name a
maintainer's packages for a change not pushed yet (a port's, the toolchain's), which a runner would otherwise build again
once it is (LLVM on a hosted runner: hours, past the job's timeout). The dry run with the Mac's list alone kept the other
platforms' 162 archives, all uploaded within the week. A toolchain change makes more than half of the bucket stale a
week later, which the script refuses without `-Force`: the prune job fails then, and the maintainer prunes by hand. Without
the secrets every run reads the cache only, and the prune job does nothing.

Tracy's profiler app is a manifest feature of its own, `profiler-app` (`tracy[gui-tools]`, which alone needs 13 packages:
aklomp-base64, capstone, dbus, expat, fp16, md4c, nativefiledialog-extended, nlohmann-json, ppqsort, psimd, pugixml,
tidy-html5 and usearch; the ppqsort and usearch overlay ports are for it), a default one, so a developer's setup installs it
(and the VS Code launch configuration `tracy-profiler`). `SPEEDO_PROFILER_APP=0` leaves it out: setup installs with
`--x-no-default-features` and the manifest's other default features, and writes the same into the presets
(`VCPKG_MANIFEST_NO_DEFAULT_FEATURES`, `VCPKG_MANIFEST_FEATURES`) for the configures' installs. `ci.yml` and `release.yml`
set it (2026-11-09, the user's call: built for local development, not on CI). Tracy itself is then built without the
feature (another ABI), and the 13 are in no CI keep list, so the prune deletes them from the shared cache a week after their
upload: vcpkg's local cache keeps them on a developer's machine, and a new one builds them (small packages).

## Linux: setup and the toolchain

Linux setup was first run from scratch on 2026-11-07 (it had only seen a Raspberry Pi, arm64, in 2025). What a fresh
distribution needed, beyond what GitHub's image has:

- `setup_linux.sh` installs the distribution's ICU (`libicu<version>`, found by name): PowerShell, on .NET, aborted
  without it.
- `linux.ps1` installs what restoring vcpkg's packages and building the project take: `build-essential` (the C
  library's headers and startup files, GCC's runtime, which LLVM's clang links), `curl`, `zip`, `unzip`, `tar`, the X11,
  Wayland and xkbcommon headers glfw links, `cmake`, and the AWS CLI for a cache writer (AWS's installer: Ubuntu's
  archive dropped `awscli` in 24.04). With `-PortBuildTools`, what building ports from source takes besides: the system's
  `clang`, `lld` and `llvm` (the host triplets'), `python3-venv` (meson's ports, libsystemd), `ninja-build` (meson builds
  with the one on the path), `autoconf`, `autoconf-archive`, `automake` and `libtool` (autotools' ports: libxcrypt,
  libmount, gperf), `patchelf`. Setup installs those only where the binary cache lacks a package (2026-11-09): it first
  installs with `--only-binarycaching`, and on its failure runs the platform script with `-PortBuildTools` and installs
  again, building what's missing. vcpkg ends that first install at the first package the cache lacks with its report of
  a failed build (red, advising to update the port files and to report an issue), which read as a failure on a Windows
  setup the cache held nothing for yet (2026-11-10): setup pipes its output (vcpkg prints no colors into a pipe) and
  replaces the report, from its first line ("Please ensure you're using the latest port files"), with one naming the
  package (the last "Installing i/n" line). On macOS that is `pkg-config` (mesa's meson build; the Command Line Tools stay, their
  SDK is what LLVM's clang compiles against and the packages' ABI tracks), on Windows nothing (Visual Studio's C++ tools
  and the SDK are the project's build's too). The host triplets look up `llvm-ar`/`llvm-ranlib` without `REQUIRED`, as
  vcpkg evaluates them for the packages' ABI before any port builds. Tried on macOS: a full hit restored and installed no
  build tools; bcdec's portfile changed (its hash in no cache) took the fallback, installed the build tools and built it.
- FASTBuild: its Linux zip was made on Windows and holds no file modes, so the `fbuild` the fork's
  `vcpkg_find_acquire_program(FASTBUILD)` extracted wasn't executable ("Unable to find FASTBUILD"), and `linux.ps1`
  extracted it first, executable. Gone with the fork (2026-11-09): the ports take FBuild from the `vcpkg-tool-fastbuild`
  host port, which installs it with its modes set (`FILE_PERMISSIONS`).
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

## CI: GitHub Actions

`.github/workflows/ci.yml` (the user's call, 2026-11-06: hosted runners only, no gpu workloads, since the client's are too
heavy for a runner's software Vulkan; and not this machine as a runner; the client headless on lavapipe since 2026-11-11,
below) builds and checks on pushes and pull requests to
`master`: setup (`setup.ps1` directly, reading the shared binary cache, and writing it where the repository holds its key,
see "vcpkg's shared binary cache": `setup_osx.sh` only adds
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

**The client on lavapipe** (the user's asks, from 2026-11-10): every platform runs the client itself after the asset
checks (Linux and Windows since 2026-11-11, macOS since 2026-11-13), headless and validated on lavapipe, mesa's Vulkan driver on the cpu: the visual checks (`visualtest.ps1
-Validate`, docs/visual-testing-plan.md), whose scenes go through the load, upload and draw path. Until 2026-11-12 a step
of client checks ran before them (`assettest.ps1 -ClientOnly -Headless -Validate -Jobs 1`, `SPEEDO_WINDOW_SIZE=320x180`:
BumpDomes' two models), dropped (the user's call) once the visual checks took `-Validate` (assettest's validation layer
settings and problem patterns): they load and draw models the same way, and their first validated run found what the
client checks never had, the GI radiance buffer past lavapipe's `maxStorageBufferRange` (VUID-00333 in the furnace check,
see docs/notes/indirect-lighting.md). The Linux and macOS jobs find the driver in a step of their own, as Windows
registers it. All three take the mesa port's (`mesa[lavapipe]`, a host package, see the mesa port above; no third-party
driver in a job that holds the cache's keys), so that every platform tests the mesa it pins; macOS's beside KosmicKrisp,
which it doesn't run there: lavapipe is the others' driver, with ray queries, and the runner's gpu a virtual machine's. Linux took Ubuntu's (`mesa-vulkan-drivers` from apt, mesa
26.0.8) until the visual checks' ray-queries check crashed on it (2026-11-11): the software BVH's frame (the check's
reference, `SPEEDO_RAY_QUERY=0`, which nothing on Linux had run, its clients taking the ray queries) segfaulted in a
shader llvmpipe compiled, reading a buffer descriptor through a null pointer. With no lane of a divergent descriptor
access active, gallivm took lane 0's, null, and dereferenced it; mesa fixed it in 26.2.0 (63b89856c9e, "gallivm: don't
deref a NULL buffer descriptor with an empty exec mask", MR 42620), not backported to 26.0 or 26.1. Reproduced with
26.0.8 built locally (macOS arm64), which passed with that commit alone applied: the access is legal, the bug the
driver's. What the client checks took (the first client
compiles the shaders): on Linux 1 min 40 s (82 s and 14 s) and 3 min 38 s (187 s and 25 s) in two runs a few hours apart,
on Windows 6 min 19 s (224 s and 153 s, 2026-11-11): Windows' second client starts nowhere near as warm as Linux's, to be
looked into. The validated visual checks, which took their place, took 10 min 17 s on Linux and 20 min 23 s on Windows
(2026-11-12), and 17 min 56 s on macOS (2026-11-13; each check's time in docs/visual-testing-plan.md). The macOS job's
first run on 7a0676ca hung in its asset checks (assettest silent for two hours after the zips' extraction, the job
cancelled at its timeout); its rerun passed them in 1 min 42 s, and assettest on 2 and 3 threads doesn't hang
(SPEEDO_EXECUTOR_THREAD_COUNT, which it honors since): a one-off, to look at if it comes back. No time budget yet (the user, 2026-11-11: measure
first). What it found on the way:

- **A deadlock on 2 executor threads** (a 4-core runner's): the client's `Rpc` task looped until a server answered,
  loads slept in `WaitUntilReady`, and moodycamel's dequeue starved the draw chain; both clients timed out at 30 minutes
  without a frame. Fixed in the executor and the client (see docs/notes/core.md, "Parallel work is tasks");
  `SPEEDO_EXECUTOR_THREAD_COUNT=2` reproduces a small machine.
- **The runner's Windows processes are elevated**, and the Vulkan loader ignores its path variables in an elevated
  process (`loader_secure_getenv`: `VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, the layer paths), so the clients found no driver
  (`VK_ERROR_INCOMPATIBLE_DRIVER`), and `-Validate`'s layer, found through `VK_LAYER_PATH`, would have been skipped with a
  warning only. The Windows step registers the driver and the validation layer in `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`
  and `ExplicitLayers`, where the loader looks then, on the runner alone (assettest leaves a machine's registry alone).
  The same holds for any client run as administrator on a developer's machine.
- **Windows' lavapipe took seven runs to build**: glslang's precompiled headers (the FASTBuild section above),
  get-pip on Python 3.15, zlib and DirectX-Headers missing from the port's Windows dependencies, mesa's generators run
  with python's `-I`, clang-cl lacking MSVC's lower-case interlocked intrinsics (`0002`, offered upstream by the user),
  and the driver's manifest written as invalid json (the mesa port section above).
- **To do (the user's ask, 2026-11-13)**: build the tsan and sanitize presets in CI as well, and run the macOS job's
  client on Metal too (`SPEEDO_GRAPHICS_API=metal`: every build has it since the backends plan's step 9, which replaced
  the metal preset), and give them launch configurations in VS Code (`setup.ps1`'s `$VSCodeLaunchConfiguration`, beside
  the profile, debug and release ones: Metal's a profile client with the variable set); and in CI run the visual
  checks (`visualtest.ps1`, on lavapipe as the profile build's) on the sanitizer builds too, so that what asan, ubsan
  and tsan report about the client's frames is caught as soon as it is pushed.

## Releases

The client runs from a checkout or from an install (the user's ask, 2026-11-07: releases on GitHub, packaged for each
platform's installer; the client only, unsigned for now). `ClientCreate` tells them apart: run from a checkout's root
(`resources/` and `src/gfx/shaders/shaders.slang` there), everything is as the scripts expect (the root the working
directory, the user profile in `.speedo`, the test assets in `resources/test-assets`); otherwise the data is beside the
executable (`core::file::GetExecutablePath`): a macOS bundle's `Contents/Resources`, else `<prefix>/share/speedo` (an
AppImage's `usr/share/speedo`, Windows' install directory's `share/speedo`), holding `resources/` and `shaders/`, and the
user's profile and test assets in the platform's user data directory (`core::file::GetUserDataPath`: `~/Library/Application
Support/Speedo`, `%LOCALAPPDATA%\Speedo`, `$XDG_DATA_HOME/speedo` or `~/.local/share/speedo`). `-r` and `-u` choose the
resource and profile directories either way. The environment's `ShaderPath` and `TestAssetsPath` name the shaders (a
checkout's sources, compiled at startup through the shader compiler library; an install's precompiled, since
2026-11-08: see layering.md's shaders) and the test assets, which the scene, the ImGui renderer, the thumbnails' render
version and the file browser took from the root before. A client copied into `bin/` with its data in `share/speedo`, run
from `/tmp`, found its shaders there and drew the helmet. A first launch shows its progress in the Loading window, the ui
drawn meanwhile: from a checkout the shader compile (about 25 s on the M4 Pro in profile), then from either the driver's
pipelines (about 20 s cold on the M4 Pro), made in the background (see layering.md's shaders), and it offers the test
assets to download meanwhile (see assets.md: their manifests are installed with the resources,
`resources/test-asset-manifests`).

**Packages.** `scripts/package.ps1 [-Preset <release preset>] [-Out <dir>]` packages a built release preset into this
platform's installer, in `build/package`: `Speedo-<version>-macos-arm64.dmg` (Speedo.app beside a link to
/Applications), `Speedo-<version>-windows-x64.msi` (CPack's WiX generator: Program Files\Speedo, a Start menu shortcut,
LICENSE to accept; WiX 5, the `wix` .NET tool, and its UI extension, which the script adds; 6 and later ask for the Open
Source Maintenance Fee's EULA) or `Speedo-<version>-linux-x86_64.AppImage` (appimagetool, fetched into `build/tools`).
What goes in is `CMakeLists.txt`'s Client component (installed only by a component install, `EXCLUDE_FROM_ALL`): the
client and the shared libraries it loads (its runtime dependency set, the system's left out), its shaders precompiled
(the script runs `shaderprecompile` into the build's `precompiled-shaders` first: every variant the client may load, in
place of the sources; no slang nor shader compiler ships, see layering.md's shaders), the fonts the client loads with
their licenses, the test assets' manifests, `LICENSE`,
`THIRD-PARTY-NOTICES.md` and each vcpkg port's license as `licenses/<port>.txt`; on macOS also KosmicKrisp (the mesa
port's, with its patches) and its manifest in the bundle's `Resources/vulkan/icd.d`, where the loader looks, the loader
as `Frameworks/libvulkan.1.dylib` (GLFW loads it by that name) and `Info.plist`. The icon (the user's design, 2026-11-08:
green swim briefs, no logo, with eyes either side of the pouch, on a pool floor's caustics) is `scripts/package/speedo.svg`,
drawn on macOS's icon grid: the AppImage takes it as it is, and `scripts/package/make-icons.ps1` (macOS: AppKit renders
it, through `src/tools/macos/svgrender.swift`, no librsvg or ImageMagick) makes the bundle's `speedo.icns` (Info.plist's
`CFBundleIconFile`) and Windows' `speedo.ico` (PNG images, 16 to 256 pixels, from the tile with a thin margin), which
`windows.rc` embeds as `GLFW_ICON` (the executables' icon in Explorer and the Start menu, and GLFW's windows') and the
installer shows in Add or remove programs (`CPACK_WIX_PRODUCT_ICON`), and `resources/icons/speedo.png` (the tile, 256
pixels), which the client's About window shows beside its text (`LoadUiImage`, client/thumbnails.h: an imgui texture,
loaded when the window first opens; `SPEEDO_SHOW_ABOUT=1` opens it at startup, for a capture) and the package installs
with the resources; the four are committed. On Windows the release
client is a windowed program (`WIN32_EXECUTABLE` in release, still entered at `main` through `/ENTRY:mainCRTStartup`), so
the installed one opens no console window behind its own (the user's report on v1.0.0, 2026-11-08: a console program's
console popped up); started from a terminal it writes to it (`AttachParentConsole`, core/process.cpp: where stdout or
stderr has no handle, so a script's redirects, `assettest.ps1`'s and `perfbaseline.ps1`'s `Start-Process`, stay theirs),
though the shell doesn't wait for it, and ignores that console's ctrl-c. Debug and profile clients stay console programs. A package's code is built
for the toolchain's fixed cpu, not the build machine's: the presets' `SPEEDO_TARGET_CPU=baseline` (`-mcpu=apple-m1`;
until 2026-11-11 they set `native`, `-march=native` on arm64, and the workflow overrode it), and `package.ps1` refuses
an arm64 build configured for native by hand (x86-64 is always `x86-64-v3`). On macOS the project's binaries don't take
the toolchain's rpath to the host LLVM's lib at all (`CMakeLists.txt` drops it: libc++ is static there): since the host
tree has curl, zlib and OpenSSL too (FASTBuild's cache plugin's), v1.0.1's first macOS package failed on it, the bundle's
install finding `libz.1.3.2.dylib` in both trees ("Multiple conflicting paths"), and a client from the build tree loaded
the host's curl. The macOS bundle's remaining absolute rpaths are deleted and it is signed ad hoc (Apple silicon runs only signed code); nothing is signed by a
developer id yet (the user's call, 2026-11-07), so Gatekeeper and SmartScreen ask at first (System Settings > Privacy &
Security > Open Anyway, or `xattr -dr com.apple.quarantine`; More info > Run anyway). Tracy isn't in it (release links
no Tracy, see "Licenses"). The AppImage runs where the C library is at least the build's (Ubuntu 26.04's 2.43).
Tested 2026-11-08 on the M4 Pro: the .dmg (30 MiB) mounted, its app copied out and run from elsewhere on a fresh profile,
the helmet autoloaded with ray queries: a cold first launch (the app's Metal cache, under its bundle id in `getconf
DARWIN_USER_CACHE_DIR`, emptied) first took 80 s with the window frozen twice: the 83 compute pipelines made ahead took
every executor thread (no frame for 20 s, the hang watch tripped) and the generic main pass pipeline was made on first
use (`FragmentMain`, 16.5 s). With the pipelines made in chains and a pass's graphics pipelines made in the background
(see layering.md's shaders), it took 69 s with the hang watch at 15 s and never tripped it, the capture with the helmet;
a second launch 5 to 6 s; no gpu restart. With the shaders precompiled (no slang): the bundle 54 MB rather than 79, the
.dmg 19.9 MiB rather than 30.3, and the cold first launch 33 s (BVHs, the macOS default since then), its first frame
1.76 s after start. Test a package from its mounted .dmg, run outside the checkout with a scratch
`-u` profile (an installed client's default is the user data directory), with the display awake (`caffeinate -u`: glfw
finds no monitor on a sleeping display and the client stops).

**The release workflow** (`.github/workflows/release.yml`): pushing a tag `v<major>.<minor>.<patch>` checks it against
`src/version.h`, makes the tag's draft release, and on CI's three runners (the binary cache's) builds the release preset,
packages it and attaches the package and its `.sha256` (`<hash>  <name>`) to the draft, which is published by hand. Run
by hand (Actions > Release > Run workflow, no tag) it packages alone, the packages kept as the run's artifacts: a test of
the packaging. To release: `scripts/release.ps1 -Bump patch|minor|major` (or `-Version <x.y.z>`), on a clean `master`,
sets `src/version.h`, commits it ("release: v<version>") and tags the commit `v<version>` (annotated); `-Push` pushes
`master` and the tag, else it prints the push command. Tested in a scratch repository (bumps, an older version and a
dirty tree refused). The workflow's first runs (2026-11-08): by hand, all three packages but Linux's (appimagetool
needs `desktop-file-validate`, which the job now installs), then all three (the .dmg 19.3 MB, the .msi 9.6, the AppImage
59.3, as artifacts); `scripts/release.ps1 -Version 1.0.0 -Push` then tagged v1.0.0, whose run made the draft release with
the three packages and their `.sha256`.

## Licenses

Speedo's own code is proprietary, all rights reserved (`LICENSE`, the user's call, 2026-11-07: to keep every option open,
commercial included); the project began in 2018 as a fork of slang, of which no line remains (the four files the history
shares with it, `git blame`d, are all the user's). `THIRD-PARTY-NOTICES.md` lists what the client ships or compiles in,
and under which license; a package carries each vcpkg port's copyright file (`licenses/<port>.txt`) and the fonts'
licenses. Audited 2026-11-07 for a commercial release, from the ports' declared licenses checked against their copyright
files: everything is permissive (MIT, BSD, Zlib, Apache-2.0, the LLVM exception's) but for
- libdwarf, LGPL-2.1: macOS and Linux, cpptrace's symbolizer, shipped as its own shared library (replaceable, as the
  LGPL asks: its text and source on offer, no terms forbidding the reverse engineering that replacing it needs). cpptrace
  can symbolize without it (libbacktrace, BSD, or the platform's tools), which would leave nothing LGPL;
- ZeroMQ, MPL-2.0: file-level copyleft, shipped unmodified as a shared library (changes to its own files would have to be
  published), for the client and server's messages;
- attribution: FreeType's FTL (chosen over its GPL) asks for credit in the documentation, the Twemoji font's art is
  CC-BY-4.0, and the Apache-2.0 components' NOTICE files (where they have one) are to be reproduced;
- slang, and the glslang and SPIRV-Tools inside its glslang module, don't ship since 2026-11-08 (a package's shaders
  come precompiled, see layering.md's shaders); glslang's generated parser is GPL-3.0 with Bison's exception, which let
  it be distributed under any terms when it did; zstd and FreeType are taken under their permissive licenses rather
  than GPL; the fonts' OFL forbids selling them alone;
- KTX-Software's ETC decoder (`etcdec.cxx`) isn't open source: an Ericsson license, for products of OpenGL, OpenGL ES and
  WebGL only, which a Vulkan renderer's isn't plainly among. Only KTX's OpenGL loader used it (speedo reads KTX2 and
  transcodes Basis), so `ports/ktx` builds without it (`KTX_FEATURE_ETC_UNPACK=OFF`, its source removed; found
  2026-11-08 in the package's `licenses/ktx.txt`, which says so).
Tracy is in the debug and profile builds only (the user's call, 2026-11-08): release, the package's build, compiles its
macros away and links no library (`core`'s link), so it doesn't ship, nor its license.
The test assets aren't Speedo's nor shipped with it (the client downloads them from their sources), and their licenses
vary: most are CC-BY or CC0, but some are for research or non-commercial use only (the Stanford models in McGuire's
archive, for one), so a commercial build would have to review what it offers.
