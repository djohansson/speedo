# Core: the conventions behind CLAUDE.md's rules

A note of CLAUDE.md (its index names it): what its "Code" section says in a line each, in full, with what each
cost to learn: the application's lifecycle, glm, portability, counts, capacity limits, containers, file I/O, locks and
the task system.

## Application lifecycle: create and destroy while registered in `gApplication`

Much of the code (asset loading, the pipeline cache path, exit requests) finds the application through
`core::Application::Get()`, which locks the `core::gApplication` weak_ptr. Two rules follow from that:

- **Create with `core::CreateApplication<T>(args...)`** (`src/core/application.h`). It publishes
  the application in `gApplication` *before* constructing it (objects created during construction
  need `Get()`), and constructs it exactly once in place. Don't recreate the old
  `make_shared_for_overwrite` + `std::construct_at` pattern: it constructs a default object first
  and never destroys it, which leaked a second `TaskExecutor` and its threads. The default
  constructors of `Application`/`WindowedApplication`/`Client`/`Server`/`RHI` were removed so that
  pattern no longer compiles.
- **Tear down before releasing the last `shared_ptr`.** Once the strong count hits zero, `Get()`
  returns null even though destructors are still running. So `ClientDestroy` calls
  `WindowedApplication::Shutdown()` (joins all tasks, waits for the GPU, flushes timeline callbacks,
  shuts down imgui, releases what holds gpu objects, destroys the RHI) *before* `reset()`, and
  `ServerDestroy` joins the executor first. `~WindowedApplication` only checks that `Shutdown()` ran.

Related gotchas hit while getting shutdown right:

- `TaskExecutor::JoinAll()` waits until nothing is queued *or executing*; call it from a regular
  thread, never from inside a task.
- Destructor bodies run *before* members are destroyed: if a body destroys the parent handle
  (`vkDestroyDevice`, `vmaDestroyAllocator`, `vkDestroyCommandPool`), release the members owning
  children of it first (see `~Device`, `~CommandPool`).
- Release everything holding gpu objects (the model, the views, textures) before the RHI: once it is
  destroyed, `GetRHI<G>()` returns null and objects fall back to null instances and devices.
- Task chains (`Rpc`/`Tick`/`Draw` in client/server) must reach `SetTaskDone` on *every* exit path,
  including errors, and `*Destroy` stops them with `RequestTaskStop`/`WaitTaskStopped` (an
  `exchange`, so an already-ended chain isn't waited on forever).
- The signal handlers re-raise fatal signals with `SIG_DFL`; returning from e.g. SIGSEGV re-runs
  the faulting instruction and loops in the handler forever, which looks like a hang.

## Math: glm, not arrays

Vectors, matrices and quaternions are glm's (`glm::vec3`, `glm::mat4`, `glm::quat`; `glm::dvec3`/`glm::dmat4` where
the importers keep double precision), not `std::array<float, N>` aliases with hand-written `Dot`/`Cross`/`Multiply`
helpers: colors and transforms in `mesh::Material`, `ModelMaterial`, `mesh::Transform`/`SceneMatrix`, the scene nodes'
translation, rotation and scale, `SceneLight`, `SceneCamera`, the environment's sky and dominant lights. Arrays stay
where a layout is fixed elsewhere: the structs `capi.h` shares with the shaders (copied with `glm::value_ptr`),
`TextureRef::transform` (the shader's two rows), rhi's public API (no glm), and lists that aren't vectors (spherical
harmonics coefficients, per channel statistics). glm here doesn't zero vectors by default (no `GLM_FORCE_CTOR_INIT`):
initialize them (`glm::vec3 v{0.0F}`), and a quaternion `glm::identity<glm::quat>()`, since `{}` is zero rather than the
identity. glm's quaternions store x, y, z, w, as gltf does (`glm::make_quat`). zpp::bits serializes glm's vectors by their
members, but not a matrix (its columns are private) or a quaternion (`GLM_FORCE_XYZW_ONLY` doesn't reach it: wherever glm
detects language extensions, `_MSC_EXTENSIONS` with clang on Windows, its members are an anonymous union, which structured
bindings can't bind): `core/glm_extra.h` serializes matrices column by column and quaternions as x, y, z, w (their
members' order, so the caches' layout is the same), so a header whose serialized structs hold one includes it. Changing an imported type from arrays to glm
changed the caches' last bits (glm sums products pairwise), so the importer tags were bumped; renders came out the same
but for a few pixels of skinned models, by 3 of 255 at most.

glm runs without its intrinsics, on purpose: the compiler already targets the best instruction sets
(`clang.toolchain.cmake`'s fixed baseline, the project's code and the vcpkg ports alike, see docs/notes/build.md:
`-mcpu=apple-m1` on Apple's arm64, `-march=x86-64-v3`, AVX2 and FMA, on x64), and vectorizes
glm's scalar code (on the M4 a `mat4 * mat4` or `mat4 * vec4` is whole columns of `fmul.4s`/`fadd.4s`, as glm's NEON
code would be). glm's own SIMD paths would cost more than they give: `GLM_FORCE_XYZW_ONLY` turns them off (without it,
vectors are anonymous unions, which zpp::bits can't serialize by members), and they only apply to aligned types
(`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` pads a `vec3` to 16 bytes, changing the caches' layouts and every `vec3` array).
If a glm heavy loop ever shows in a profile, use `glm::aligned_vec4`/`aligned_mat4` there. Never `-march=native`: it
ties the binaries to the build machine's cpu, and what a machine builds is shared (packages, the binary cache,
FASTBuild's cache).

## Portable by default, measured on one

This code runs on several platforms and gpus (macOS through KosmicKrisp or MoltenVK, Windows and Linux on desktop
gpus). Measure the choices here, but pick the general method that is likely to perform well across them, not the one a
single driver happens to favor: e.g. the gpu culling compacts its draws and counts them (`DrawIndexedIndirectCount`,
native on desktop gpus) although KosmicKrisp emulates the count. Record the platform's numbers as what it costs there.
Where one platform can't do the general method well, it gets an exception named for what it lacks, decided in rhi
(`DeviceLimits::drawIndirectCountEmulated`), not a different design for everyone, and the exception says when to look
again.

## Naming: counts

A number that is a size or an amount of something is a count, and named so: `SHADER_TYPES_LIGHT_COUNT`,
`kChunkCommandCount`, `SPEEDO_SWAPCHAIN_IMAGE_COUNT` (it was `SPEEDO_SWAPCHAIN_IMAGES`). A switch is not:
`SPEEDO_INDIRECT_DRAWS=0`.

## Capacity limits: hard failures, not warnings

When a fixed capacity runs out (a buffer of draws, slots, lights, materials, textures, samplers, texture views, cluster
light indices, ui panels, the gpu profiler's ring, the task pool), fail hard with `ENSUREF` (`core/assert.h`: it traps
in every build, with the message and a stack trace; `CHECK` only breaks in the debugger), naming the limit. Don't warn
and drop the rest, truncate, or fall back to something slower: that hides the case, and leaves a render that looks
plausible but is wrong. When real content hits a limit, handle it then (a bigger or growing buffer, a budget per view):
an artificial case may keep the hard failure. What stays a budget by design is a policy rather than a capacity: the
shadow atlas gives the lights it can't fit no shadow (see "Shadows"), and the transparency's per pixel node pool drops
fragments past it on the gpu, where no hard failure is possible. The first real case found: NodePerformanceTest's 10000
materials (see "Asset import and testing"), so gMaterialData grows.

## Associative containers: core's, not the standard library's

Don't use `std::map`, `std::set`, `std::unordered_map` or `std::unordered_set` (tools included): use `core::UnorderedMap`
and `core::UnorderedSet` (`core/utils.h`, ankerl::unordered_dense). Where an order matters (e.g. a report), sort a
vector of the keys. Composite keys are small structs with a defaulted `operator==` and a byte hash (`XXH3_64bits`,
`using is_avalanching = void;`, a `has_unique_object_representations` static_assert), floats stored by their bits so
that equal keys hash equal (see `TextureViewKey`, `obj::detail::VertexKey`).

## File I/O: core's memory mapped files, not the standard library's streams

Read and write files through core (`core/file.h`): `core::file::Map` (a whole file mapped for reading) and
`core::file::Write`, or `LoadBinary`/`SaveBinary` and the zpp::bits serializers over them (`InputSerializer`/
`OutputSerializer`), not `std::ifstream`/`std::ofstream`, `fopen`/`fread` or a library's own file loading. Hand third-party
decoders the mapped bytes rather than a path: stb's `_from_memory` functions, `image::Import`/`Decode` and
`environment::Import`'s byte overloads, cgltf's `options.file` callbacks (`gltf::detail::Options`, which map the gltf file
and its buffers and unmap them when cgltf releases them), tinyobj's `LoadObj` from an istream over the mapped bytes
(`obj::detail::MemoryBuffer`, a streambuf over memory, and `MappedMaterialReader` for the mtl files). The zip extractor
maps the archive and writes entries with `Write`. `LoadAsset` maps an asset's source file before calling its source load
op: read the file from the serializer it gets (`in.remaining_data()`), as `LoadTexture` and `LoadEnvironment` do, rather
than opening it again. `src/tools/filebench.cpp` is the one exception, since std streams and stdio are what it measures.

Zip entries are inflated with libdeflate (`core::zip::Inflate`, raw deflate straight into the entry's file, mapped at
its known size) and checked with its crc-32 (`core::zip::Crc32`, the cpu's crc instructions), by a worker per executor
thread taking entries in turn (`core::zip::ExtractAll`, which waits with `TaskExecutor::Join`, so it may run in a task;
directories are created first, on the calling thread). Compared on San_Miguel.zip (511 MiB, 1.8 GiB inflated), libdeflate
inflated in 1.01 s where stb_image's decoder took 2.65 s (zlib 2.33, miniz 2.05, zlib-ng 1.63), and its crc-32 in 24 ms
where the byte-at-a-time table the extractor had took 3.2 s. Extracting it takes 1.0 s on 14 threads (1.7 s on one): one
entry, the 1.1 GB obj, is most of it, and a deflate stream inflates in order. `filebench --zip <archives>` times all of
that.

`Map` asks the os to read the whole file ahead (`fcntl(F_RDADVISE)` on macos, `posix_fadvise(POSIX_FADV_WILLNEED)` on
linux; nothing on windows yet): without it a cold mapped file is read a page fault at a time, which `filebench` measured at
0.6 GB/s against `read()`'s 7 on macos (256 MiB, M4 Pro; `MADV_SEQUENTIAL` doesn't help, `MADV_WILLNEED` only doubles
it). With it, mapping matches `read()` cold and warm (13 to 18 GB/s from the page cache). `std::istreambuf_iterator`
reads, which the image importer used, run at 0.5 GB/s either way. `filebench --import <files>` times the importers:
the change took a 9 MB png's decode from 77 to 58 ms and an 80 MB hdr's from 244 to 227; mesh imports are bound by
parsing (2 to 3% faster). For an image it also times the whole import (decoded, mipmapped and compressed) on one thread
and with an executor compressing: `image::Options::executor` spreads `CompressLevel`'s block rows over it with
`TaskExecutor::ForEach` (`LoadTexture` passes the application's; `std::execution::par` did nothing, libc++ runs it
serially), which took a 2048 Bistro texture's compression from 62 to 23 ms; the decode and the mip resizes are serial.

## Choosing a lock

Measured on this machine (M4 Pro): `core::UpgradableSharedMutex` is the cheapest uncontended (~4-7 ns) and is the
only one with upgrade locks, but every unlock wakes all waiters, so it degrades badly under contention (8 threads
on one exclusive lock: ~430 ns vs `std::mutex`'s ~17 ns). So: `std::mutex` for exclusive-only locks
(`gDrawMutex`, `MemoryPool`'s free list and its shards', the object tracking maps); `UpgradableSharedMutex` where upgrade locks are
used (descriptor set state in `Pipeline`) and as `ConcurrentAccess`'s default, whose locks are short and mostly
uncontended (at 2-3 threads it and `std::shared_mutex` trade wins). Don't lock what can't change: `MemoryPool`'s
`GetPointer` is address arithmetic on chunks that never move once made. The `TaskExecutor` wakes idle threads with
an atomic wake count (`myWakeCount`) rather than a condition variable: submitting takes no lock, and a thread
can't miss a wake between finding the queue empty and waiting. `Pipeline`'s pipeline map is a `std::mutex` and a condition variable
(`PipelineMapSync`): it is only touched when a draw changes pipeline variant (draws are sorted by variant), a few times
per view and phase, so an exclusive lock costs microseconds per frame; an upgrade lock would gain nothing. The code before
took no lock, waiting on the map entry with `std::atomic_ref`, which was only safe while one thread bound pipelines: the
map isn't safe to insert into concurrently, and rehashing moves its entries. Recording isn't expected to go wide enough for
a sharded node map (`phmap::parallel_node_hash_map`, where entries don't move and could be waited on again) to pay.

## Parallel work is tasks; callables are templates or tasks, not `std::function`

- Run parallel and background work as tasks on the application's executor (`core::CreateTask` and
  `TaskExecutor::Submit`, `TaskExecutor::ForEach` for a loop over indices, `core::AddDependency` to run one task after
  another, `Join` or a future's `IsReady` to wait), not on threads of its own (`std::thread`, `std::jthread`,
  `std::async`): the executor's threads are named for the debugger and Tracy and joined at shutdown, a waiting `Join`
  helps run the queue instead of blocking a thread, and a `ForEach`'s caller takes the loop's indices as its workers do,
  then waits for the ones they are running and takes nothing else from the queue. It used to run whatever was ready while
  it waited, as `Join` does: the draw thread, which holds the graphics queue's lock for its frame, ran a texture load in
  `Model::Animate`'s `ForEach` (placing what moves) while the file browser loaded a model, the load's upload asked for
  that lock (`UpgradableSharedMutex` isn't recursive), and the draw thread waited on itself, the loads behind it, and
  the exit behind them (BrainStem animating while the browser previewed Sponza: 3 runs in 3). So don't `Join` while
  holding a lock a task may take. The executor's are the only threads, the tools' included
  (`assettest` makes one executor for its checks, `CheckInOrder`, the environment kernels and zip extraction). Work for a
  particular thread is queued for it as a function, called once in order: `mainCalls` (the main thread's,
  `move_only_function<void()>`, run by `platform::WindowedApplication::Main`) and `RHI::drawCalls` (the draw thread's,
  `DrawCall`, called with its graphics queue's context; `TransitionThenBind` queues its bind). They were tasks, whose
  futures nothing waited on, with the draw context passed as an unchecked parameter. Code that runs work in the background
  takes the executor (`FrameInputs::executor`, which background pipeline creation, `Pipeline::RequestGraphicsPipeline`'s
  task, is submitted to).
- A task doesn't loop until something arrives, nor sleep holding its thread until another task has made something: the
  executor has a thread per core less two, 2 on a 4-core machine (GitHub's runners), where the client's three chains
  (`Rpc`, `Tick`, `Draw`) already want more. `Rpc` looped until the server answered (none does yet) and a load slept until
  the scene was ready, so on 2 threads, with the autoload, nothing drew the frame that makes it ready (the Linux CI's
  lavapipe run, 2026-11-11). `Rpc` polls once and requeues itself; a load waiting for the scene runs the executor's other
  tasks meanwhile (Scene's `WaitUntilReady`, `TaskExecutor::JoinOne`). `SPEEDO_EXECUTOR_THREAD_COUNT=<n>` gives the
  executor n threads, a small machine's.
- The ready queue is moodycamel's, a queue per producing thread, whose dequeue without a consumer token isn't fair: it
  takes from the fullest of the first three non-empty queues, newest first, a tie to the first, and on 2 threads the
  `Tick` and `Rpc` chains, requeued into their threads' queues, won over the draw chain and the shader compile (submitted
  by the main thread) for good. The executor's threads dequeue with consumer tokens of their own, rotating over the queues
  after every task (`TaskExecutor::ReadyQueueTraits`). A FIFO behind a `std::mutex`, fair as well, took a `ForEach` from
  3.1 to 9-20 us on 14 threads (`taskbench`); the tokens cost nothing measurable (submit 522-592 ns a task against
  630-745, foreach 3.2-3.5 us against 3.1-3.2, nested 3.1 ms against 3.3-3.4).
- Take a callable as a template parameter (`template <typename Fn> void Foo(Fn&& fn)`); where it runs later, or on
  another thread, as a task (`core::TaskHandle`, `core::CreateTask`) or a `move_only_function` queued for that thread
  (`mainCalls`, `drawCalls`); where it is called during the call into a .cpp
  that a template can't reach, as a `core::std_extra::function_ref` (c++26's, else `tl::function_ref`: not owning,
  never empty, so an optional one has a default, `core::NeverCancelled`, `NoUi`, `NoMaterial`, or is a `std::optional`);
  and where it is stored and called again (the frame graph's passes) or may be dropped unrun (a task never is: the
  preview's `CaptureDone`), as a `core::std_extra::move_only_function` (c++23's, else a polyfill in `core/std_extra.h`
  that supports `R(Args...) const` too). Not as a `std::function`, which copies, type-erases every call and allocates
  for larger captures. A `function_ref` bound to a lambda dangles once the lambda's full expression ends: bind it in the
  call (or an aggregate built in the call, as `FrameInputs` is), or to a named lambda that outlives it.
- Sleep until a time with `core::SleepUntil` (`core/time.h`), not `std::this_thread::sleep_until`/`sleep_for`, where
  the time matters: on Windows those round up to the system timer's resolution, 15.6 ms unless some process raises it,
  and the client's tick (input and the ui), paced to a millisecond, ran at 64 Hz: the camera and the ui moved at 64 Hz on
  a 120 Hz display while frames were drawn at hundreds a second, a stutter Tracy showed nothing of (the time was a
  sleep). It waits on a high resolution waitable timer there (ticks 1.56 ms apart, 15.4 before).
- The executor's threads are `core::std_extra::jthread`s: `std::jthread` with P2019's thread attributes (the threads'
  name and stack size passed as `jthread::name_hint` and `jthread::stack_size_hint` before the callable), std's where a
  standard library has them (`__cpp_lib_thread_attributes`; none yet, P2019R9 is at LEWG), else a polyfill whose platform
  code is in `core/std_extra.cpp` (pthreads, `_beginthreadex`), so the scheduler has none (its own thread naming went).
  A `std::thread` can't be given a stack size, and macOS gives one 512 KB, which a debug client loading rungholt ran past
  (`SIGBUS` in a stack guard, a client run in the full debug regression): a thread waiting in `Join` runs whatever is
  ready meanwhile, so nested waits stack task upon task (the BVH builder's spatial splits wait on the partitions they
  spawned, running another partition meanwhile, which waits in turn), and the ui's tick landed on top (in a `ForEach`'s
  wait, then), its `PrepareDraw` frame big in a debug build. The task threads' stacks are a main thread's, 8 MB
  (`kThreadStackByteCount`).
- Tasks come from a pool that grows in chunks of 1024 (`core/memorypool.h`, `kTaskPoolChunkSize`), up to about a
  million (past which it's a leak, and traps), and a task stays allocated until it has run and the executor purges it
  (`JoinAll`, or after another task runs). Its handle holds its slot's generation, so a handle used after its task
  was freed traps. Its free slots are in shards, a thread's own, refilled from and drained to the shared free list in
  batches: one free list under one mutex took 12 us an allocation on each of 32 threads (`taskbench alloc`). `ForEach` makes workers for the threads idle at the call (at least one), each recruiting the next as
  it starts: one per thread up front, nested loops on a busy executor (a model's textures, each compressing on every
  thread) left one per thread per loop queued, and Bistro's cold import on 32 threads ran the fixed pool (16384) out.
  `taskbench` (`src/tools`) measures the task system's overheads and the pool's peak.

## Tasks: `core::CreateTask` copies its arguments

`CreateTask(callable, args...)` stores the callable and its arguments decayed (copied or moved), as `std::thread` and
`std::bind` do, in a `move_only_function<void(TaskState&)>`, and calls it once as `callable(std::move(args)...)`: pass a
reference as `std::ref` (the `Rpc` chains' socket and poller, members of the client and server). It stored lvalue
arguments by reference (a `std::tuple` of forwarding references), which dangled once the task ran after the caller
returned (read back as mimalloc's freed-memory pattern `0xdfdf...`), and kept the callable and its arguments in a fixed
256 byte task (at most 128 and 80 bytes) behind hand-written invoke and delete functions. The callable's captures past
`move_only_function`'s inline storage (the polyfill's 32 bytes) are allocated: `taskbench` measured the redesign at a
task's call 336 to 424 ns before, 253 to 300 after (see its commit). A task's dependents are its own (at most 7, a hard
failure past them), and its state, shared with its futures, is the latch alone (and a `Future<T>`'s value).
