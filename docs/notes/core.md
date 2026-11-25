# Core: the conventions behind CLAUDE.md's rules

Speedo is a real-time physically based renderer in C++26 (sources under `src/`). `core` is its lowest layer: the application, tasks and executor, files, containers and assertions, with no graphics. This note gives the reasoning behind the "Code" rules in CLAUDE.md so they can be applied correctly: the lifecycle and the general rules first (ownership, tasks, locks, containers, files, child processes), then limits and naming, then numeric rules (math, floating point).

## Contents

- [Application lifecycle: create and destroy while registered in `gApplication`](#application-lifecycle-create-and-destroy-while-registered-in-gapplication)
- [Ownership: smart pointers, no `new` or `delete` expressions](#ownership-smart-pointers-no-new-or-delete-expressions)
- [Parallel work is tasks; callables are templates or tasks, not `std::function`](#parallel-work-is-tasks-callables-are-templates-or-tasks-not-stdfunction)
- [Callables: borrowed during a call or owned when stored](#callables-borrowed-during-a-call-or-owned-when-stored)
- [Sleeping: `core::SleepUntil`](#sleeping-coresleepuntil)
- [Scripts: PowerShell, one language on every platform](#scripts-powershell-one-language-on-every-platform)
- [Tasks: `core::CreateTask` copies its arguments](#tasks-corecreatetask-copies-its-arguments)
- [Choosing a lock](#choosing-a-lock)
- [Associative containers: core's, not the standard library's](#associative-containers-cores-not-the-standard-librarys)
- [File I/O: core's memory mapped files, not the standard library's streams](#file-io-cores-memory-mapped-files-not-the-standard-librarys-streams)
- [Child processes and dynamic libraries](#child-processes-and-dynamic-libraries)
- [Capacity limits: hard failures, not warnings](#capacity-limits-hard-failures-not-warnings)
- [Naming: counts](#naming-counts)
- [Portable by default, measured on one](#portable-by-default-measured-on-one)
- [Math: glm, not arrays](#math-glm-not-arrays)
- [Floating point: the same on every platform](#floating-point-the-same-on-every-platform)
- [Other core facilities](#other-core-facilities)

## Application lifecycle: create and destroy while registered in `gApplication`

Code finds the application through `core::Application::Get()`, which locks the `core::gApplication` weak_ptr.

- Create it with `core::CreateApplication<T>(args...)` (`src/core/application.h`): it publishes the application in `gApplication` before constructing it (objects made during construction need `Get()`) and constructs it once, in place. The classes' default constructors are removed so a construct-then-overwrite pattern cannot compile.
- Tear down before releasing the last `shared_ptr`: at a strong count of zero `Get()` returns null while destructors still run. `ClientDestroy` calls `WindowedApplication::Shutdown()` first (joins tasks, waits for the gpu, shuts down imgui, releases what holds gpu objects, destroys the RHI); `ServerDestroy` joins the executor first.
- `TaskExecutor::JoinAll()` waits until nothing is queued or executing: call it from a regular thread, never a task.
- Destructor bodies run before members are destroyed: release members owning children of a handle (`~Device`, `~CommandPool`) before the body destroys that handle.
- Release everything holding gpu objects before the RHI; afterwards `GetRHI<G>()` returns null.
- Task chains (`Rpc`, `Tick`, `Draw`) must reach `SetTaskDone` on every exit path; `*Destroy` stops them with `RequestTaskStop` and `WaitTaskStopped`.
- Signal handlers re-raise fatal signals with `SIG_DFL`; returning from a SIGSEGV handler loops forever, which looks like a hang.

## Ownership: smart pointers, no `new` or `delete` expressions

A heap object belongs to a `std::unique_ptr` (`std::make_unique`) or `std::shared_ptr` (`std::make_shared`), so every exit path frees it in one place. For an aggregate with designated initializers use `std::make_unique<T>(T{.field = value})`; a non-movable type (one with a mutex) is made in place with `std::make_unique<T>(args...)`.

Exempt:

- Placement new into storage the code manages (core's containers and memory pool, `move_only_function`'s buffer, `core::CreateApplication`'s block) and the allocation functions behind it.
- `= delete`, a deleted function.
- `core::std_extra::move_only_function`, which implements a standard library type.
- Objects behind a backend's opaque handles: rhi's handles are raw pointers by design, made with `new` and deleted by the handle's one owner; a handle that is not the owner's is never deleted through.
- An object held in an atomic (the memory pool's chunks).

## Parallel work is tasks; callables are templates or tasks, not `std::function`

- **Tasks, not threads.** Run parallel and background work on the application's executor: `core::CreateTask`, `TaskExecutor::Submit`, `ForEach`, `core::AddDependency`, `Join` or a future's `IsReady`. No `std::thread`, `std::jthread` or `std::async`, tools included. Executor threads are named for debuggers and joined at shutdown, and a waiting `Join` runs queued tasks rather than blocking. Background code takes the executor (`FrameInputs::executor`).
- **Do not `Join` while holding a lock a task may take.** `Join` runs whatever is ready, so a task run inside the wait may need the held lock and deadlock.
- **Work for a particular thread is queued for it**, as a function called once in order: `mainCalls` (run by `platform::WindowedApplication::Main`) and `RHI::drawCalls` (the draw thread's).
- **A task never loops until something arrives, nor sleeps holding its thread until another task has made something.** The executor has one thread per core less two, fewer than the client's chains (`Rpc`, `Tick`, `Draw`) want on a small machine, so a blocked task starves the one it waits for. `Rpc` polls once and requeues itself; a load waiting for the scene runs other tasks meanwhile (`Scene::WaitUntilReady`, `TaskExecutor::JoinOne`). `SPEEDO_EXECUTOR_THREAD_COUNT=<n>` sets the thread count.
- **Fairness.** The ready queue (moodycamel) is one queue per producing thread, and dequeuing without a consumer token is unfair. Executor threads use tokens of their own, rotating over the queues after every task (`TaskExecutor::ReadyQueueTraits`).
- **Callables and sleeping** have their own sections below.
- **Threads and stacks.** Executor threads are `core::std_extra::jthread`s: `std::jthread` with P2019's name and stack size hints, polyfilled in `core/std_extra.cpp`. A `std::thread` cannot be given a stack size and macOS gives it 512 KB, too little for nested `Join`s running other tasks on top; task threads get 8 MB (`kThreadStackByteCount`).
- **The task pool** (`core/memorypool.h`) grows in chunks (`kTaskPoolChunkSize`) to about a million tasks, then traps. A task stays allocated until it has run and the executor purges it. A handle holds its slot's generation, so using it after the task was freed traps. `ForEach` recruits workers one at a time for idle threads, so nested loops do not flood the pool. `taskbench` (`src/tools`) measures the task system.

## Callables: borrowed during a call or owned when stored

The type says how long the callable lives, so no callable is copied or heap allocated by accident (`std::function` copies and allocates, and is never used).

- **A template parameter** where the callable is called inline and the function is a template anyway.
- **`core::std_extra::function_ref`** where it is only called during a call into a .cpp (loader callbacks, draw lists' predicates, the ui's recording). It does not own the callable and is never empty, so an optional one has a default (`core::CancelledFn`'s `core::NeverCancelled`) or is a `std::optional`; it dangles after the call. C++26's `std::function_ref`, else tl's.
- **`core::std_extra::move_only_function`** where it is stored and called again, or may be dropped unrun (the frame graph's passes, the thread queues `mainCalls` and `RHI::drawCalls`). Owning, move only, maybe empty; C++23's, else a polyfill in `core/std_extra.h` that keeps small callables (up to four pointers, nothrow movable) in place and has const signatures, which libc++ lacks.
- **A task** (`core::CreateTask`) where it runs later or on another thread.

## Sleeping: `core::SleepUntil`

`core::SleepUntil(time)` (`core/time.h`) sleeps to within about half a millisecond on every platform. `std::this_thread::sleep_until` is that precise on macOS and Linux but on Windows rounds up to the system timer's resolution (15.6 ms unless a process raises it, and Windows 11 ignores a raise from a process whose windows are all hidden), which paced the client's tick at 64 Hz. On Windows `SleepUntil` waits on a high resolution waitable timer, one per thread (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), falling back to `std::this_thread` where there is none. Elsewhere it is `sleep_until`.

## Scripts: PowerShell, one language on every platform

Scripts (`scripts/*.ps1`, `setup.ps1`) are PowerShell 7.5+, so one language runs the setup, builds, tests and CI on Windows, macOS and Linux (CI's steps use `shell: pwsh`). The `setup_windows.bat`, `setup_osx.sh` and `setup_linux.sh` bootstraps are the exception: they exist to install or upgrade PowerShell (a fresh Windows has only 5.1, which cannot parse `setup.ps1`; Linux needs ICU for .NET) and then run `setup.ps1` with it, which refuses older versions. Tools needing a POSIX shell are avoided elsewhere too (a port patched so as not to need msys2's `bash.exe`, docs/notes/build.md).

## Tasks: `core::CreateTask` copies its arguments

`CreateTask(callable, args...)` stores the callable and arguments decayed (copied or moved), as `std::thread` and `std::bind` do, and calls it once as `callable(std::move(args)...)`. Pass a reference as `std::ref` (for example the `Rpc` chains' socket), since a stored reference would dangle once the task ran after the caller returned. Captures beyond `move_only_function`'s inline storage are heap allocated. A task has at most 7 dependents (a hard failure past that), and its state, shared with its futures, is the latch alone (plus a `Future<T>`'s value).

## Choosing a lock

- `std::mutex` for exclusive-only locks (`gDrawMutex`, `MemoryPool`'s free list, the object tracking maps, `Pipeline`'s pipeline map).
- `core::UpgradableSharedMutex` where upgrade locks are used (descriptor set state in `Pipeline`) and as `ConcurrentAccess`'s default (short, mostly uncontended locks). It is cheap uncontended and the only one with upgrade locks, but every unlock wakes all waiters, so it degrades under contention. It is not recursive.
- Do not lock what cannot change: `MemoryPool`'s `GetPointer` is address arithmetic on chunks that never move.
- `TaskExecutor` wakes idle threads with an atomic wake count (`myWakeCount`), not a condition variable: submitting takes no lock and no wake is missed.

## Associative containers: core's, not the standard library's

Use `core::UnorderedMap` and `core::UnorderedSet` (`core/utils.h`, ankerl::unordered_dense), never `std::map`, `std::set` or `std::unordered_*`, tools included. The standard library's are node based, an allocation per element and a pointer to follow per lookup; unordered_dense keeps its elements in one contiguous vector, which looks up and iterates faster. Where order matters, sort a vector of the keys. Composite keys are small structs with a defaulted `operator==` and a byte hash (`XXH3_64bits`, `using is_avalanching = void;`), floats stored by their bits so equal keys hash equal (`TextureViewKey`).

## File I/O: core's memory mapped files, not the standard library's streams

Use `core/file.h`: `core::file::Map` (a whole file mapped for reading), `core::file::Write`, `LoadBinary`/`SaveBinary` and the zpp::bits serializers. No streams, `fopen` or a library's own file loading; `src/tools/filebench.cpp` is the one exception, since it measures them.

- Give third-party decoders the mapped bytes, not a path: stb's `_from_memory` functions, the byte overloads of `image::Import` and `environment::Import`, cgltf's `options.file` callbacks, tinyobj over an istream on mapped bytes (`obj::detail::MemoryBuffer`).
- `LoadAsset` maps the source file before its load op; read from the serializer it gets (`in.remaining_data()`), not the file again.
- `Map` asks the os to read ahead (`F_RDADVISE` on macOS, `POSIX_FADV_WILLNEED` on Linux; nothing on Windows), since a cold mapped file otherwise reads one page fault at a time.
- Zip entries are inflated with libdeflate and checked with its crc-32 (`core::zip::Inflate`, `Crc32`), one worker per executor thread (`core::zip::ExtractAll`, which waits with `Join` and may run in a task).

## Child processes and dynamic libraries

Work that cannot run in this process is a `core::ChildProcess` (`core/childprocess.h`): started from an executable and arguments (`CreateProcessW`, `posix_spawn`) with the starter's environment and working directory, and waited for (`Wait` returns the exit code). Destroying one that was not waited for waits for it, so none is left running unseen. The shader compiler's workers are the use, since slang is not thread safe (layering.md, "The shaders compile in the background").

`core::DynamicLibrary` (`core/dynamiclibrary.h`) loads a shared library by path at run time and finds its exported symbols by name. It stays loaded until the process ends, since what it returns may be in use anywhere. The shader compiler library is the use (layering.md, "The shader compiler library and precompiled shaders").

## Capacity limits: hard failures, not warnings

When a fixed capacity runs out (draws, lights, materials, textures, samplers, cluster light indices, ui panels, the gpu profiler's ring, the task pool), fail with `ENSUREF` (`core/assert.h`), naming the limit. It traps in every build with a stack trace; `CHECK` only breaks in a debugger. Never warn and drop the rest, truncate, or fall back to something slower: the render looks plausible but is wrong. When real content hits a limit, handle it then (a growing buffer, a budget per view).

A policy is not a capacity: the shadow atlas gives lights it cannot fit no shadow, and the transparency's node pool drops fragments past it on the gpu, where no hard failure is possible.

## Naming: counts

A number that is a size or an amount is a count and named so: `SHADER_TYPES_LIGHT_COUNT`, `kChunkCommandCount`, `SPEEDO_SWAPCHAIN_IMAGE_COUNT`. A switch is not: `SPEEDO_INDIRECT_DRAWS=0`. The suffix makes a name say what it holds: an amount is not mistaken for an index or a switch, and its unit goes before it where it has one (`kThreadStackByteCount`).

## Portable by default, measured on one

Measure, but choose the general method likely to perform well on every platform and gpu, not what one driver favors (gpu culling counts its draws with `DrawIndexedIndirectCount` although KosmicKrisp emulates the count). Record a platform's numbers as what the method costs there. A platform that cannot do the general method well gets an exception named for what it lacks, decided in rhi (`DeviceLimits::drawIndirectCountEmulated`), saying when to look again.

## Math: glm, not arrays

Use glm's types (`glm::vec3`, `glm::mat4`, `glm::quat`; `glm::dvec3` and `glm::dmat4` where importers keep doubles). glm has the operators and functions vector math needs, named and behaving as in the shaders, where an array alias has none and every caller writes its own. Arrays stay only where a layout is fixed elsewhere: structs `capi.h` shares with the shaders, rhi's public API, and non-vector lists (spherical harmonics, statistics).

- glm does not zero-initialize: write `glm::vec3 v{0.0F}` and `glm::identity<glm::quat>()` (`{}` is zero, not identity). Quaternions store x, y, z, w, as glTF does.
- zpp::bits cannot serialize glm matrices or quaternions by members; `core/glm_extra.h` does (matrices by column, quaternions as x, y, z, w). Headers whose serialized structs hold either include it.
- glm runs without its intrinsics: the compiler targets the baseline instruction set (`-mcpu=apple-m1` on Apple arm64, `-march=x86-64-v3` on x64) and vectorizes glm's scalar code, and glm's SIMD needs aligned types, which would pad `vec3` and change cache layouts. Use `glm::aligned_vec4` or `aligned_mat4` only where a profile calls for it.
- Never `-march=native`: build outputs (packages, binary cache, FASTBuild cache) are shared between machines.

## Floating point: the same on every platform

Floating point behaves identically everywhere: consistency over speed.

- **Compiler**: IEEE 754 as written, for the project and packages: `-ffp-contract=off -fno-math-errno`, no `-ffast-math` (docs/notes/build.md). Use `std::fma` where fusing is meant.
- **Math functions**: `core::Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, `Exp`, `Exp2`, `Log2`, `Log10`, `Pow`, `Cbrt` (`core/math.h`) are LLVM libc's (`llvm-libc-math` port) and correctly rounded, so every platform agrees; the platforms' libms each round some inputs differently. `core/math.cpp` includes each function's header alone and configures it to match the flags: no errno, no exception flags, round to nearest only, and not `LIBC_MATH_SKIP_ACCURATE_PASS`, which is what rounds correctly. `sqrt`, `fma`, `floor`, `fmod` and similar are exactly rounded by IEEE 754, so `std::`'s are fine.
- **glm**: its functions that call `std::` math are replaced by `core/glm_extra.h`'s `Perspective`, `Rotation`, `AngleAxis` and `Slerp`. Do not call glm's `perspective`, `rotate`, `angleAxis` or `slerp`.

Package code and the cpu copies of the environment kernels (slang's C++ prelude calls `::sinf` and the rest) still use the platform's functions.

On the gpu, shaders compile with Metal's relaxed math, in the Metal backend and KosmicKrisp (ports/mesa's 0003 patch). It keeps infinities and NaNs, as Vulkan drivers do, where fast math assumes none. Fast functions and reassociation remain: Vulkan leaves their exactness, fusing and denormals to the driver, so gpus never agree bit for bit, and safe math or precise functions cost a large share of the frame.

## Other core facilities

Headers in `src/core` not covered above, one bullet each.

- **`core/loadqueue.h`**: `core::LoadQueue` runs loads on the application's executor, any number at once, and lists those in progress. `Enqueue(name, loadOp)` runs `loadOp(std::atomic_uint8_t& progress)` as a task and returns its future; the load writes progress (0 to 255). `ForEach(fn)` visits the loads running, for the ui's progress display (the loads cannot start or finish meanwhile, so keep `fn` short). A load not started when the application exits is skipped and its future holds a default result. Use it for loads the user watches (`gfx::Scene`, the client's ui).
- **`core/concurrentaccess.h`**: `core::ConcurrentAccess<T, MutexT>` owns a value with its mutex (`UpgradableSharedMutex` by default), reachable only through scopes: `Read()` holds a shared lock and `Write()` an exclusive one for the scope's lifetime, and both give `Get()`, `->` and `*` (through a pointer like `T`). Use it for state that other threads read while one writes, so no access can skip the lock (`LoadQueue`, rhi's queue timeline contexts, `gfx::Scene`'s environment name). Choosing its mutex: "Choosing a lock".
- **`core/circularcontainer.h`**: `core::CircularContainer<T>` wraps a vector as a ring with an atomic head. `Get()` is the current element and `FetchAdd(n)` returns it and advances the head modulo the size, so threads take elements in turn without a lock. Capacity is fixed at construction (`Capacity()`). rhi uses it to rotate over a device's queues.
- **`core/eventhandlers.h`**: input events reach the application through handler objects. `MouseEventHandler::OnMouse` and `KeyboardEventHandler::OnKeyboard` are implemented by the application; `AddMouseHandler` and `AddKeyboardHandler` register one by `shared_ptr`, held weakly (a destroyed handler is skipped). The C functions `UpdateMouse` and `UpdateKeyboard` (`core/capi.h`) call every live handler; the window layer (`platform/glfw.cpp`) calls them, so nothing above platform touches glfw. `core/inputstate.h`'s `core::InputState` is the per tick snapshot of the keyboard (`keysDown` by `Key`), the mouse (positions, buttons, scroll) and `dt`.
- **`core/nodegraph.h`**: `core::NodeGraph` is plain data for a node editor: the nodes (`core::INode`: id, selection, name; `core/nodes/` has `InputOutputNode` and `SlangShaderNode`), the `Link`s between node ids, the editor's saved layout string and a unique id counter. The ui does not use it now (its imgui layer's node editor code is commented out).
- **`core/runtimings.h`**: `SPEEDO_RUN_TIMINGS=1` prints a run's milestones with their time from the process's start and the frames drawn by then. `RunTiming("what")` notes one from any thread (nothing when off), `SetRunTimingFrames` gives the frame counter and `PrintRunTimings()` prints at the end, so an automated run shows where its time goes (docs/environment-variables.md).
- **`core/profiling.h`**: the one include for Tracy (`ZoneScoped`, `ZoneScopedN`, `FrameMark`), active when `SPEEDO_PROFILING_LEVEL > 0` (profile and debug builds) and compiled out otherwise. It redefines the zone and frame macros to record only while `core::profiling::Enabled()` (`SPEEDO_TRACY=0` stops recording: Tracy keeps every event until a profiler connects, so test runs would grow without bound). It also provides `std::is_debugger_present` where the standard library lacks `<debugging>`, without including `<windows.h>`. Use it instead of including Tracy directly. Gpu timing is rhi's (rendering.md).
- **`core/cancelled.h`**: `core::CancelledFn` (a `function_ref<bool()>`) and `NeverCancelled`, how long operations such as importers are told to stop (see "Callables").
- **`core/process.cpp`** (declared in `core/capi.h`): what an executable's `main` sets up, shared by the client and the server: `InstallSignalHandlers` (an interrupt sets `IsInterrupted` and calls the given function from a regular thread, never the handler), argument parsing (cargs) and `SleepSeconds`, which is `SleepUntil`.
- **`core/utils.h`**: `core::UnorderedMap` and `UnorderedSet` (see above) and their hash helpers (`HandleHash`, `HandleCompareEqualTo`, `IntrusiveTypeInfoHash`, `IdentityHash`, `TupleHash`), `core::RangeSet` (a set of integer ranges that merges overlapping inserts, used for descriptor ranges in rhi's pipelines), and the moodycamel concurrent queue include.
- **`core/std_extra.h`**: polyfills and small utilities for the standard library (`function_ref`, `move_only_function`, `jthread` with name and stack size hints, `overloaded`, `aligned_allocator`, `string_literal`, tuple and member count helpers).
- **`core/crc32.h`**: `CORE_COMPILE_TIME_CRC32_STR(literal)`, a `consteval` crc-32 of a string literal (`core::crc32::Crc32<Idx>`), which `SOURCE_LOCATION_DATA` in `core/std_extra.h` turns into a zone color from its tag. The run time crc-32 of zip entries is `core::zip`'s (see "File I/O").
- **`core/path.h`**: filesystem helpers kept apart from `core/file.h` so users do not pull in mio and `<windows.h>`: `GetTimeStamp`, `GetCanonicalPath`, `GetExecutablePath` and `GetUserDataPath` (the per platform directory for a user's data). `core/mio_extra.h` is mio's include trimmed to its kernel part on Windows for the same reason.
- **`core/uuids_extra.h`**: a random (version 4) uuid per thread generator, and zpp::bits serialization of `uuids::uuid` as its 16 bytes.
- **`core/config.h`**: `TRAP`, `DEBUGTRAP` and the preprocessor helpers (`CONCAT`, `COUNTED_VAR_DECLARE`) shared with C code; `core/assert.h`'s macros are described under "Capacity limits".
- **`core/tsanguard.cpp`**: part of thread sanitizer builds on macOS (docs/notes/build.md).
