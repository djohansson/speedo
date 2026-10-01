# CLAUDE.md

Notes for working in this codebase, distilled from real build failures.

## rhi: `Object<T>` / `DeviceObject<T>` base classes

Most `rhi` types (`Buffer`, `Fence`, `Queue`, `Pipeline`, `Window`, ...) derive from
`Object<DerivedType>` or `DeviceObject<DerivedType>` (`src/rhi/object.h`,
`src/rhi/deviceobject.h`). This is a CRTP base, not a normal template: `GetInstance()`
and `GetDevice()` are declared in the base but have **no generic definition** — each
concrete type must explicitly specialize them via macros, once, in its own `.cpp`:

```cpp
IMPLEMENT_OBJECT_GETINSTANCE(Foo<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Foo<kVk>);   // only if Foo derives from DeviceObject
```

If a new type deriving from `Object`/`DeviceObject` is added (or one is spotted missing
these), forgetting this produces linker errors like
`undefined symbol: rhi::Object<rhi::Foo<...>>::GetInstance() const` — it is *not* a sign
that something else is broken, just that the macro invocation is missing. These macros
expand to code that calls `RHIApplication::GetRHI<G>()`, so the `.cpp` needs
`#include <rhi/rhiapplication.h>`.

## Move-via-Swap idiom

Types in this hierarchy that need to be movable (e.g. because they live in a
`std::vector`, like `Device` in `RHI::myDevices`) follow a consistent pattern rather than
defaulted move members:

```cpp
void Foo::Swap(Foo& other) noexcept { SuperType::Swap(other); std::swap(myX, other.myX); ... }
Foo::Foo(Foo&& other) noexcept { Swap(other); }          // default-constructs *this*, then swaps
Foo& Foo::operator=(Foo&& other) noexcept { Swap(other); return *this; }
```

After a swap, the moved-from object holds the *default-constructed* state (because the
move constructor default-constructs `this` before swapping). Destructors on these types
must guard on `IsValid()` (from `Object::IsValid()`, true iff `GetDesc().instance` is
set) before doing any GPU-handle cleanup, otherwise a moved-from object double-frees or
operates on null handles.

Not everything in this hierarchy is meant to be movable — e.g. `Instance` explicitly
`= delete`s its move constructor since it's a true singleton. Check how the type is
actually used (is it stored by value in a container that reallocates?) before assuming
it needs Swap/move support added.

## Gotcha: explicit specialization ordering

When a `.cpp` file both defines an explicit template specialization *and* calls that
same specialization earlier in the file, the compiler implicitly instantiates the
primary template at the call site — and then errors with
`explicit specialization of 'X' after instantiation` when it later hits the `template <>`
definition. Two concrete ways this bites in this codebase:

1. **Swap-based move members**: define `Swap()`'s specialization *before* the move
   constructor / move-assignment specializations that call `Swap(other)` in the same
   file, not after.
2. **`IMPLEMENT_OBJECT_GETINSTANCE`/`IMPLEMENT_DEVICEOBJECT_GETDEVICE`**: if the same
   `.cpp` also defines explicit specializations of functions the macro calls internally
   (e.g. `rhiapplication.cpp` defining `RHIApplication::GetRHI<kVk>()`), the macro
   invocation must come *after* those specializations, not before.

## Gotcha: delegating constructors run their arguments before the base exists

Many types create their GPU handle inside the argument list of a delegating constructor:

```cpp
Foo<kVk>::Foo(CreateDescType&& desc)
	: Foo(std::forward<CreateDescType>(desc), [this, &desc] { /* create handle */ }())
{}
```

Those arguments are evaluated *before* the target constructor initializes the
`DeviceObject` base, so inside them `GetDesc()`, `InternalGetDesc()`, members, and the
no-argument `GetDevice()` (which falls back to `GetDesc().device`) all read uninitialized
memory. Read from the incoming `desc` instead: `desc.device` where a `VkDevice` is enough,
`GetDevice(desc.device)` where the `Device` object is needed (e.g. `.GetAllocator()`), and
`CreateDeviceObjectCreateDesc(name, desc.device)` for child objects. `GetInstance()` is fine
since it does not depend on object state. Don't pass references *into* `desc` either (e.g. a
field bound to a `const&` parameter): `desc` is moved into the base before the target's
members are initialized, so pass a copy.

Serialized create-descs arrive *without* runtime handles: `ObjectCreateDesc`/`DeviceObjectCreateDesc`
deliberately serialize only `uuid`, never `instance`/`device`. Any code that builds a desc from a
file or cache (shader reflection, `image::detail::Load`, ...) must fill in `instance`/`device`
before the desc is used to construct an object, or `GetDevice()` trips the assert below. Also, a
derived desc with extra fields needs its own `serialize()` (see `ImageCreateDesc`): otherwise it
inherits the base one and silently serializes only the `uuid`. When a serialized layout changes,
bump the `cache-vN` tag in that loader's params hash so stale caches are rebuilt rather than misread.

Related: `DeviceObject<T>::GetDevice()` resolves through
`RHIApplication::GetRHI<G>().GetDevice(handle)`, which only finds devices already stored in
`RHI::myDevices`. That's why `Device` creates its queues and pipeline from the `RHI`
constructor, after the devices are registered, rather than from its own constructor. A
failed lookup trips the `ASSERTF` in `RHI::GetDevice()`.

## Application lifecycle: create and destroy while registered in `gApplication`

Device objects find their instance/device through `core::Application::Get()`, which locks the
`core::gApplication` weak_ptr. Two rules follow from that:

- **Create with `core::CreateApplication<T>(args...)`** (`src/core/application.h`). It publishes
  the application in `gApplication` *before* constructing it (objects created during construction
  need `Get()`), and constructs it exactly once in place. Don't recreate the old
  `make_shared_for_overwrite` + `std::construct_at` pattern: it constructs a default object first
  and never destroys it, which leaked a second `TaskExecutor` and its threads. The default
  constructors of `Application`/`RHIApplication`/`Client`/`Server`/`RHI` were removed so that
  pattern no longer compiles.
- **Tear down before releasing the last `shared_ptr`.** Once the strong count hits zero, `Get()`
  returns null even though destructors are still running, and `GetDevice()`/`GetInstance()`
  silently fall back to null objects ("Invalid device" from the loader). So `ClientDestroy` calls
  `RHIApplication::Shutdown()` (joins all tasks, waits for the GPU, flushes timeline callbacks,
  shuts down imgui, destroys the RHI) *before* `reset()`, and `ServerDestroy` joins the executor
  first. `~RHIApplication` only checks that `Shutdown()` ran.

Related gotchas hit while getting shutdown right:

- `TaskExecutor::JoinAll()` waits until nothing is queued *or executing*; call it from a regular
  thread, never from inside a task.
- Destructor bodies run *before* members are destroyed: if a body destroys the parent handle
  (`vkDestroyDevice`, `vmaDestroyAllocator`, `vkDestroyCommandPool`), release the members owning
  children of it first (see `~Device`, `~CommandPool`).
- Don't `myRHI.reset()`: it nulls the pointer before `~RHI` runs, while objects being destroyed
  still resolve devices through `GetRHI()`. `Shutdown()` does `delete myRHI.get()` then
  `release()`.
- Task chains (`Rpc`/`Tick`/`Draw` in client/server) must reach `SetTaskDone` on *every* exit path,
  including errors, and `*Destroy` stops them with `RequestTaskStop`/`WaitTaskStopped` (an
  `exchange`, so an already-ended chain isn't waited on forever).
- The signal handlers re-raise fatal signals with `SIG_DFL`; returning from e.g. SIGSEGV re-runs
  the faulting instruction and loops in the handler forever, which looks like a hang.

## Queues: aliased queue types share one lock

When a device has no dedicated compute/transfer queue family (e.g. KosmicKrisp/MoltenVK, one
family), `Device::InternalCreateQueues` aliases those queue types to the graphics queue by sharing
the same `std::shared_ptr<QueueTimelineContext>` (the `ConcurrentAccess`, i.e. data *and* mutex).
Consequences:

- Access queues through `Device::GetQueue(type)`. Code that "locks the transfer queue" may really be
  locking the graphics queue, which `Draw()` holds for the whole frame — that serialization is the
  point (before, aliases had separate mutexes and the loader and `Draw()` raced on the same command
  pool).
- **Never hold locks on two queue types at once** unless you know they are distinct (e.g. the
  compute lock inside `Draw()` is only taken when `dedicatedCompute`): `UpgradableSharedMutex` is
  not recursive, so on a single-queue device it self-deadlocks. Lock one at a time, and when
  visiting all queue types dedupe by context (see `RHIApplication::Shutdown()`).

## Descriptor sets: redundant updates consume the pool

Binding a descriptor set that is marked dirty (`BindDescriptorSetAuto` → `InternalUpdateDescriptorSet`)
takes a fresh set from the current `DescriptorSetArray` (16 sets), allocating a new array when it is
full; old arrays are never recycled. So `SetDescriptorData` skips writes whose value is unchanged
(`pipeline::SameBinding`, compared per field since the Vulkan structs have padding) — otherwise
per-frame re-writes of the same data (as the compute pass does) exhaust the pool within seconds
(`VK_ERROR_OUT_OF_POOL_MEMORY`). Genuine changes (loads, resizes) still leak a little until sets are
recycled once the gpu is done with them.

Loaded resources (models/images from the File menu or `SPEEDO_AUTOLOAD_MODEL`/`SPEEDO_AUTOLOAD_IMAGE`)
are installed on the draw thread via `rhi.drawCalls`, stored with `Device::ReplaceResource`, and the
previous resource is freed via `RetireAfterGraphicsWork` once all in-flight graphics work is done.

## Gotcha: `core::CreateTask` stores lvalue arguments by reference

`CreateTask(callable, args...)` keeps `args` in a `std::tuple<Args...>` with `Args` deduced as
forwarding references, so an **lvalue argument is stored as a reference**, not copied. The task
usually runs later, so passing a local or a function parameter as an lvalue leaves a dangling
reference (typically read back as mimalloc's freed-memory pattern `0xdfdf...`). Pass arguments that
must outlive the call as rvalues — `std::move(x)` or an explicit copy like `std::vector(x)` — as
`RHIApplication::InternalOpenFileDialogueAsync` does. Only pass lvalues when the reference is
intended and the referee outlives the task (e.g. the `Rpc` tasks' socket/poller, which are members
of the client/server object).

This is easy to miss with lambdas: a captureless lambda passed by reference happens to work (there's
nothing to read), but it breaks as soon as a capture is added.

## `size_t`-keyed maps expecting hashed names

`Device::GetPipelineLayoutHandles()` (and similar name-keyed lookup tables) key on
`size_t` name hashes, not strings — see `GetPipelineLayoutHandle(std::string_view)` in
`src/rhi/device.h` which does `std::hash<std::string_view>{}(name)`. When inserting into
one of these maps directly (e.g. via `.emplace(...)`), hash the string first; passing a
raw string literal compiles a `pair<size_t, T>` construction attempt that only fails at
template-instantiation depth deep inside `<vector>`/`<unordered_dense.h>`, far from the
actual mistake.

## Always configure via `cmake --preset <name>`, never a hand-reconstructed command

`setup.ps1` generates `CMakeUserPresets.json` with the `LLVM_ROOT`/`LLVM_TOOLS_BINARY_DIR`/`PATH`
(and Visual Studio path) environment that `scripts/cmake/toolchains/clang.toolchain.cmake` and
`CMakeLists.txt`'s `find_program(MINJECT ...)` depend on. Copy-pasting the raw
`cmake -D... -S ... -B ...` invocation out of a VS Code CMake Tools log and re-running it by hand
skips that environment and fails in several different, confusing ways (empty `LLVM_ROOT` inside the
toolchain, `minject.exe` not found, etc.). Use `cmake --preset <name> -S <root>` instead — it's not
just shorter, the raw command is missing required state.

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

FASTBuild generator pitfalls in third-party CMake (see `ports/tracy/0007-*`): a directory as a custom command
`OUTPUT` fails with `File missing despite success`, and custom commands that `DEPENDS` on a *target* name are not
ordered after it, so depend on the produced file (e.g. an `ExternalProject` `INSTALL_BYPRODUCTS`). ld64.lld does not
add `libobjc` implicitly the way Apple's linker does (`undefined symbol: objc_msgSend`, see `ports/tracy/0008-*`).
