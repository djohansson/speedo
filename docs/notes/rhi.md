# rhi

A note of CLAUDE.md (its index names each): the backend's internals and their gotchas.

## Vulkan 1.4

The instance asks for Vulkan 1.4 (`kVulkanApiVersion`, `rhi/vulkan/utils.h`) and only devices that support it are
candidates (`DetectAndCreateDevices`). What 1.1 to 1.4 made core is called and enabled as core: dynamic rendering,
synchronization2, the dynamic cull mode and front face, shader draw parameters, inline uniform blocks, push descriptors,
tooling info (`vkCmdBeginRendering`, `vkCmdPipelineBarrier2`, `vkCmdSetCullMode`, `vkCmdPushDescriptorSetWithTemplate`,
...), not through extensions and function pointers. Features come in `PhysicalDeviceInfo`'s version structs
(`deviceFeatures11Ex` to `deviceFeatures14Ex`, the 1.3 and 1.4 ones chained only where the device has them), then the
extensions' (`deviceFeatureParams`: present id and wait, swapchain maintenance1, the crash reports', the ray queries' where
wanted, see "Hardware ray queries"); the device is made
with the whole chain, so it enables every feature it has, and `Device` checks by name the optional ones the renderer
needs (dynamic rendering, synchronization2, shader draw parameters, timeline semaphores, push descriptors). VMA is told
the version too. Verified on Windows (NVIDIA, 1.4.341); macOS needs KosmicKrisp or MoltenVK to report 1.4, else no device is a
candidate.

## Crash reports

A fatal Vulkan result (a lost device, out of memory, any other error) or a hang writes a crash report, in every build,
release included (`rhi/crashreport.h`, the backend's `rhi/vulkan/crashreport.cpp`): `VK_CHECK` checks in every build
(it discarded every result in release), and `OnCheckFailedDefault` hands an error to `vk::crash::ReportFatal`; another
unexpected result (a timeout, suboptimal) is logged (at most 16) and traps in builds that profile. The first thread to
meet one writes the report, the others wait for it (they used to race, each trapping), to
`<user profile>/crashes/crash-<time>.json` (`SetCrashReportDirectory`, from the client: rhi doesn't know the
application) and a summary line to stderr, then traps. A report has:

- the kind, the result, a **signature** (a hash of the vendor, driver version, kind, result, first pass in flight and
  fault type: the same cause on many machines groups together), the time, frame, version (`SPEEDO_VERSION`), build,
  platform, device and driver, and the tools loaded (`VK_EXT_tooling_info`);
  (KosmicKrisp's vendor id was the uninitialized memory of its instance, another in each process, which also threw
  every pipeline cache away, its header being another vendor's to the next process: the instance's host allocations are
  zeroed, see `instance.cpp`, until the driver allocates it zeroed: mesa's `kk: zero-initialize the instance`, `vk_zalloc`);
- **breadcrumbs**: the frame graph's passes (`CrashBreadcrumb` around each, outside its render pass) written by the gpu
  as each begins (top of the pipe) and ends (bottom), with `VK_AMD_buffer_marker` into a host visible ring of 4096
  slots (device coherent with `VK_AMD_device_coherent_memory`), else `VK_NV_device_diagnostic_checkpoints` (each
  queue's last checkpoints, encoded as sequence numbers, never pointers: the client printed driver markers as strings
  once, an access violation under the layer), else none: the passes in flight (begun, not ended), the last completed,
  how many hadn't begun. NVIDIA's driver has both; `SPEEDO_CRASH_BREADCRUMBS=markers|checkpoints|none` forces one;
- the **device fault** (`VK_EXT_device_fault`): each faulting address and its type, the vendor's codes (its binary
  saved beside the report), and the objects bound around the address (`VK_EXT_device_address_binding_report`, through
  a debug utils messenger made in every build just before a device that enables it is, `crash::CreateMessenger`; made
  with the instance, it asked for the device extension's messages where no device has it, which the validation layer
  reports, every run on KosmicKrisp), the last 1024 unbound ones too (`unboundSecondsAgo`): a write to freed memory names
  what was freed;
- the last 64 submits (queue, command buffers, timeline value) and the last 256 **events**: what the application did
  (`rhi::CrashEvent`: swapchains made, window changes, presentation asked for, models, images and environments
  installed, shutdown), stamped with the frame (`CrashFrame`).

**Hangs**: a fence, semaphore or acquire wait without a timeout waits `SPEEDO_GPU_HANG_TIMEOUT` (20 s, 0 for never) at
most and reports a hang (`vk::crash::WatchedWait`); and the main thread, which never waits for the draw thread, watches
it (`rhi::CrashWatch` in `ClientMain`, against `CrashHeartbeat` at each `Draw`, which runs when frames are skipped too),
which catches the draw thread blocked inside a call without a timeout (a present stuck behind a semaphore never
signaled blocked the draw thread where no wait did). What makes progress where no heartbeat can tell (a pipeline created cold, inside the
driver's call, or one the draw thread waits for a background job to create) is a busy span (`rhi::CrashBusy`), which
the watch gives 120 s from where it began: four clients compiling at once on KosmicKrisp, two waited past the timeout
for their first frames' pipelines. `SPEEDO_CRASH_TEST=hang` makes the frames from the 100th wait for
a semaphore nothing signals, to check reports end to end (with `SPEEDO_GPU_HANG_TIMEOUT=3`).

**The Crash Diagnostic Layer** (see the presentation test's paragraph): where tooling info names it, no breadcrumbs are
recorded, nor the queues' checkpoints read (its own), and the report says its dump has the commands in flight; the
reporter then waits 3 s before trapping, since the layer writes its dump on the thread that met the loss first.

Checked with the descriptor bug of "Descriptor sets" put back: the report named `ComputeMain` in flight, the
presentation change two frames before, a `WRITE_INVALID` fault in a 33 MB image (a 4K swapchain image) unbound 3 s
before, with buffer markers and with checkpoints, and deferred to the layer with it loaded.

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
expand to code that calls `GetRHI<G>()`, so the `.cpp` needs `#include <rhi/rhi.h>`.

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
must guard on `IsValid()` (from `Object::IsValid()`, true iff `GetDesc().uuid` is
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
   (e.g. `vulkan/rhi.cpp` defining `GetRHI<kVk>()`), the macro invocation must come *after* those
   specializations, not before.

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
deliberately serialize only `uuid`, never `instance`/`device`/`name`. Any code that builds a desc from a
file or cache must fill in `instance`/`device`, and
replace the `uuid` with `uuids::NewUuid()` (a cached desc holds the uuid of the object it was saved from),
before the desc is used to construct an object, or `GetDevice()` trips the assert below. Also, a
derived desc with extra fields needs its own `serialize()` (see `ImageCreateDesc`): otherwise it
inherits the base one and silently serializes only the `uuid`. When a serialized layout changes,
bump the `cache-vN` tag in that loader's params hash so stale caches are rebuilt rather than misread.

Related: `DeviceObject<T>::GetDevice()` resolves through
`GetRHI<G>()->GetDevice(handle)`, which only finds devices already stored in
`RHI::myDevices`. That's why `Device` creates its queues and pipeline from the `RHI`
constructor, after the devices are registered, rather than from its own constructor. A
failed lookup trips the `ASSERTF` in `RHI::GetDevice()`.

## rhi object identity and tracking

- **uuids are random and unique.** `ObjectCreateDesc::uuid` identifies a live object (`IsValid()` is
  `uuid != nil`); `name` is for debugging only and need not be unique. Build descs with the
  `Create*ObjectCreateDesc(name)` helpers, which assign `uuids::NewUuid()`. Validation builds `ENSUREF` in
  `Object`'s constructor that the uuid is non-nil and not held by another live object (`gLiveObjectUuids`), so
  copying a live object's desc to create another object traps. `Device`'s resources are a set keyed by each
  object's own uuid: `CreateResource<T>(desc, ...)` returns the object, callers keep its uuid to `GetResource`, and
  `ReplaceResource(previousUuid, resource)` swaps an object out (the caller then keeps the new object's uuid).
- **Every vulkan object is tracked** (validation builds), keyed by type and handle in sharded `phmap` maps, which
  is what the Statistics window shows (`GetObjectCounts`). Call `Track(device, type, handle, name)` right after
  creating a handle and `Untrack(type, handle)` right before destroying it; untracking a handle that was never
  tracked traps, so a missing `Track` shows up immediately. Handles must not be null (destroy paths that may run
  for an object that was never created check that themselves), the device must be valid and the name non-empty:
  use `GetDebugName(desc)` (the name, or the uuid) for desc-based objects. Instance level objects created before
  there is a device (instance, physical devices, surfaces) use `TrackInstance(type, handle, name)` instead, and are
  named through the device once `Device` calls `NameInstanceObjects`. Buffers, images, image views, framebuffers and render
  passes are tracked by the `Create*` helpers in `rhi/vulkan/utils.h`: destroy them with the matching `Destroy*`
  helpers, not `vmaDestroy*`/`vkDestroy*`. VMA's device memory blocks are tracked through its device memory
  callbacks.

## Frame indices are slots, not swapchain images

A frame's index (`FrameInputs::frameIndex`, `PushConstants::frameIndex`, and every per frame buffer and descriptor
slot kept by it) is its slot of `SHADER_TYPES_FRAME_COUNT`, which the frames take in turn (`Draw`'s `myFrameSlot`);
`Draw` waits for the graphics timeline value the last frame in the slot signaled (`myFrameSlotTimelines`) before
anything writes the slot's buffers. The swapchain image `vkAcquireNextImageKHR` handed out (`Swapchain::Flip`) is the
frame's output index (`FrameInputs::outputIndex`, `PushConstants::outputIndex`), only for what is the image's:
ComputeMain's and the UI blur's storage view of it (`gRWTextures` slots below `SHADER_TYPES_OUTPUT_IMAGE_COUNT`), the UI
pass's render target and the capture. The frame index used to be the image: fifo hands the images out in turn, but
mailbox and immediate needn't (an image a newer frame replaced comes back at once), so a frame still on the gpu read
buffers the next frame wrote over: once, on Windows with mailbox (then the default where the surface had it), Sponza's
indirect lighting kept a probe state no frame would have made and stayed black and blown out at one pose; with fifo it
never showed, and KosmicKrisp has no mailbox, so the mac never saw it. A wait for the image's last frame fixed that,
but stalled whenever mailbox handed back the image just drawn. The queue ring's fences (`queues.FetchAdd()`) can't
stand in for the slot's wait: the ring is as long as the graphics family's queue count (16 on NVIDIA), not the slots.
Test what keeps state from frame to frame under mailbox too: unthrottled frames also reach states fifo's don't (e.g.
the indirect lighting's look distance adapts over many more frames, and auto exposure, by time, has less of it in a run
of as many frames).

## Command buffers: one owner each

A `rhi::CommandBuffer` (`rhi/command.h`) is move-only, with a single owner. What records into it borrows its handle (it
converts to a `CommandBufferHandle`, which is all the encoder, images, passes and scenes take), and only its owner can
end it and hand it on: to `Queue::EnqueueSubmit(syncInfo, std::move(cmd))` (or a vector of them; none for a submission
that only waits and signals), or, a secondary one, to `CommandEncoder::ExecuteCommands(std::move(secondaries))` on the
primary, which records it there. Both end it if it is still recording and hand it back to its pool
(`CommandBuffer::Release`). The pool (`CommandPool`, two per queue, swapped and reset by `Queue::SwapAndResetPool`)
hands out single buffers (`Begin`, `BeginSecondary`; `Allocate` for one never begun, as Tracy's setup needs), allocated
8 at a time when it has none free, takes them back once submitted, and makes them reusable all at once on reset, which
traps if any is still held (as does the switch). A buffer dropped without being submitted is ended and handed back
unrun, with a warning in validation builds. So the frame's buffer is `Draw`'s (`auto cmd = pool.Begin()`), lent to the
scenes, the renderer and the ui, and submitted by `Draw`; uploads and transitions (`UpdateBufferOnGraphics`,
`TransitionThenBind`, the loaders' staging copies) begin buffers of their own; the main pass's views are one secondary
buffer, executed into the frame's. `Commands()` used to hand out the frame's recording buffer, shared by reference
count, to anyone who asked, and submits took the pool's pending lists: the file browser's preview ran `Scene::Update`
after the frame's buffer was begun, `UpdateBufferOnGraphics` ended that buffer to submit its upload, and the frame went
on recording into an ended buffer (KosmicKrisp crashed creating a compute encoder, only in profile builds; validation
never saw it). One-shot buffers and a holder count check in `End()` stopped that until buffers had one owner.
(`CommandBufferArray`, buffers in fours sharing a slot in those lists, went with it; allocating several at once is all
it gave, and the pool still does.) The main pass's "draw threads" recorded on one thread, a secondary buffer per level:
a pool's buffers must not be recorded on several threads at once, so recording views in parallel needs a pool per
thread.

## Queues: aliased queue types share one lock

When a device has no dedicated compute/transfer queue family (e.g. KosmicKrisp/MoltenVK, one
family), `Device::InternalCreateQueues` aliases those queue types to the graphics queue by sharing
the same `std::shared_ptr<QueueTimelineContext>` (the `ConcurrentAccess`, i.e. data *and* mutex).
Consequences:

- Access queues through `Device::GetQueue(type)`. Code that "locks the transfer queue" may really be
  locking the graphics queue, which `Draw()` holds for the whole frame — that serialization is the
  point (before, aliases had separate mutexes and the loader and `Draw()` raced on the same command
  pool).
- **Never hold locks on two queue types at once** unless you know they are distinct (`Draw()` takes the compute lock
  inside the graphics one to present, only where the swapchain presents from a compute family of its own, see
  `Swapchain::GetPresentQueueType`): `UpgradableSharedMutex` is
  not recursive, so on a single-queue device it self-deadlocks. Lock one at a time, and when
  visiting all queue types dedupe by context (see `WindowedApplication::Shutdown()`).
- Each queue has two command pools, swapped and reset per use (`SwapAndResetPool`), *without*
  `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`: on KosmicKrisp that frees the pool's Metal command allocators, which
  never get `reset` (their command buffers give the memory back on completion), and freeing one Metal hasn't had back
  yet aborts in `-[IOGPUMetalDeviceShmemPool dealloc]` (`_allocatedSize == 0`), even after the fences were waited on.
  It took switching models with a cold pipeline cache to show (DamagedHelmet, then AnimationPointerUVs: one run in
  three).
- **The same abort at exit, not yet explained (2026-11-09).** About one quick tier client in 500 (AnimationPointerUVs,
  four clients side by side from a new work dir: the model's pipelines cold) aborted in `-[IOGPUMetalDeviceShmemPool
  dealloc]` from `Queue::~Queue`'s `vkDestroyCommandPool`: `kk_DestroyCommandPool` releasing one of the pool's Metal 4
  command allocators with live allocations in it (the first `mtl_release` loop of `kk_cmd_pool_release_mtl_objects`, by
  the return address, `_kk_DestroyCommandPool + 323`). Not reproduced: 0 in 10 single runs of AnimationPointerUVs (all
  its variants, validated) from the warm-up's profile, 0 in 8 beside 12 cpu-bound loops. Ruled out:
  - Metal's completion of committed command buffers lagging the fence waits. KosmicKrisp counts only its re-recorded
    commits in flight (`commits_in_flight`); a normal commit's feedback handler only checks for a lost device. A build
    that delayed the feedback handler by 5 ms had about 250 commits awaiting feedback when the pools went, and didn't
    abort (100 ms stalled the client into the hang detector, the handlers run one at a time): Metal gives a completed
    command buffer's memory back to its allocator whether or not its feedback handler ran, so waiting for them in
    `kk_DestroyCommandPool` wouldn't help.
  - What KosmicKrisp does with allocators, in a standalone Metal 4 program (up to 2000 iterations a case, none aborted):
    an allocator released before a command buffer that encoded with it and was never committed (as KosmicKrisp's
    secondaries: their commands only go to the `vk_cmd_queue`, which `vkCmdExecuteCommands` replays, but each begins
    and ends a Metal command buffer of its own), released right after the queue's shared event was waited on (as a
    fence wait), shared by 64 command buffers in turn (half never committed), or after a command buffer moved on to
    another allocator (a reused command buffer takes whichever allocator tops the pool's free list). A committed command
    buffer evidently keeps its allocator alive until done with it.

  On the way: KosmicKrisp never resets its allocators (`mtl_command_allocator_reset` has no caller), and a busy one's
  heaps (`allocatedSize`) were about 130 MB: 252 MB in all after 30 frames, 264 after 300, a high-water mark rather
  than a leak. A committed command buffer's memory is reused once it completes (64 on one allocator: 4.7 MB), an
  uncommitted one's held until it's begun again or freed (17 MB with half uncommitted).

  To catch it next time (the user's call; not run yet): a KosmicKrisp build that logs, for each allocator just before
  `kk_cmd_pool_release_mtl_objects` releases it, its `allocatedSize`, how many primary and secondary command buffers
  began with it, how many of them were committed and completed (their feedback), and when each last happened, with the
  commits awaiting feedback. Loaded through `VK_DRIVER_FILES` (an ICD json of its own pointing at the build) in quick
  tiers repeated from a work dir holding only the warm-up's profile (`user`: the models' pipelines cold), four clients
  at a time, until a client aborts: the line before the abort names the allocator. vcpkg's meson build dir
  (`temp/vcpkg/buildtrees/mesa/arm64-osx-release-rel` since vcpkg's standalone install; `vcpkg/buildtrees` then) rebuilds the dylib with `ninja` in seconds; back up and restore the
  sources it builds (a `.clean` tree under `src/`, which vcpkg may reuse). Mesa's policy (its `AGENTS.md`) keeps such
  code local, and a report to Apple or to KosmicKrisp's developers is the user's own.

  Seen again 2026-11-11: AnimationPointerUVs, 1 of the quick tier's 61 clients (the project's code just rebuilt for
  `-mcpu=apple-m1` rather than `-march=native`, the mesa port's KosmicKrisp rebuilt from the same source, the
  pipelines warm), the same frame (`_kk_DestroyCommandPool + 323`); 4 of 4 single runs after it passed.

## Descriptor sets: redundant updates consume the pool

Array bindings set element by element (`SetDescriptorData(name, value, set, index)`) store their elements in index
order, which the update template walks: a new index goes before the first range above it. (It used to be appended,
which swapped elements whenever one was set below an existing one: model sampler slot 0 got the default sampler set
earlier in slot 2.)
An acceleration structure array's elements are written an update template entry each (`InternalUpdateDescriptorSetTemplate`):
the validation layer (1.4.350) read a range's handles side by side, past the entries' stride, and reported the bytes after
the first as invalid handles (`VUID-VkWriteDescriptorSetAccelerationStructureKHR-pAccelerationStructures-03580`).


Binding a descriptor set that is marked dirty (`BindDescriptorSetAuto` → `InternalUpdateDescriptorSet`)
takes a fresh set from the current `DescriptorSetArray` (16 sets), allocating a new array when it is
full; old arrays are never recycled. So `SetDescriptorData` skips writes whose value is unchanged
(`pipeline::SameBinding`, compared per field since the Vulkan structs have padding) — otherwise
per-frame re-writes of the same data (as the compute pass does) exhaust the pool within seconds
(`VK_ERROR_OUT_OF_POOL_MEMORY`). Genuine changes (loads, resizes) still leak a little until sets are
recycled once the gpu is done with them. A layout whose bindings change at every dispatch can hand its sets over instead
(`Pipeline::TakeDescriptorSets(set)`: the set's arrays so far, to destroy once the gpu is done with the commands that bound
them, which frees them back to the pool; the next bind takes a fresh one), as the environment filter's did until its set
was pushed (below): 20 sets a sky redraw, every frame while the time of day advances fast, which would have emptied the
pool's storage images within minutes. Those
handed over are freed in a timeline callback, on an executor thread, while the draw thread allocates from the same pool,
which Vulkan requires to be externally synchronized: every allocation, free and reset of a pipeline's pool holds its
mutex (`Pipeline::myDescriptorPoolMutex`, shared with each `DescriptorSetArray`, `DescriptorSetArrayCreateDesc::poolMutex`).
Unguarded, the two corrupted KosmicKrisp's list of the pool's sets while the time of day advanced, and destroying the pool
crashed at shutdown (`kk_descriptor_set_destroy`, unlinking a set already freed). The environment filter's set is pushed
instead (2026-11-05; `Pipeline::CreateLayout`'s `pushDescriptorSets`: those sets' layouts are made with
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` and their bindings not partially bound, since a push descriptor
layout allows no binding flags; `BindDescriptorSetAuto` then pushes the set's bindings into the command buffer with its
template, `InternalPushDescriptorSet`, at each bind, and `TakeDescriptorSets` gives nothing for it): every one of its 9
bindings is set before the first kernel, and 9 is within the 32 push descriptors every device has (KosmicKrisp's
`KK_MAX_PUSH_DESCRIPTORS`, which implements the template push). Nothing else qualifies: the other layouts' sets hold
arrays past that limit (the imgui renderer's 256 textures, the main layout's 1024 and more), and their sets seldom
change (a model installed, a texture added); what changes every frame is in buffers indexed by the frame slot. The
reflection keys the Environment layout's sets 0 (its push constants, `[[vk::binding(0, 0)]]`, a uniform buffer binding
nothing binds) and 1, in that order, so the earlier try's failures (validation's `VUID-...-templateType-00353` and
`-binding-00279`, a crash in `vkCmdPushDescriptorSetWithTemplate` on KosmicKrisp) weren't the order; a push set layout
with the partially bound flag is invalid, likely them. Bistro's street at 600x: 45.9 to 45.3 ms with the indirect
lighting off (KosmicKrisp, 2560x1440).

A handle equal to the one set isn't the same object, though: drivers reuse a destroyed object's handle (NVIDIA's are
addresses), so what is destroyed and made again with the gpu idle (the swapchain's storage views and the frame graph's
transients, at every swapchain recreation) can come back with the old handles, be skipped as unchanged, and leave the
set holding the destroyed ones' descriptors: `ComputeMain` lost the device writing through one, 1 presentation test run
in 10 on Windows, after a present mode change (found with the crash diagnostic layer, below). So
`Scene::CreateOutputDependentObjects` calls `Pipeline::InvalidateDescriptorSets` once it has set them, which writes
every set afresh at its next bind. Loads aren't exposed: what they replace is retired after the new objects exist.

Loaded resources (models/images from the File menu or `SPEEDO_AUTOLOAD_MODEL`/`SPEEDO_AUTOLOAD_IMAGE`)
are installed on the draw thread via `rhi.drawCalls`, stored with `Device::ReplaceResource`, and the
previous resource is freed via `RetireAfterGraphicsWork` once all in-flight graphics work is done.

## Pipelines own their layouts, by name

A pipeline layout belongs to the `rhi::Pipeline` that created it: `CreateLayout(name, shaderSet)` registers it under its
name, and `GetLayoutHandle(name)` finds it (`"Main"`, `"VertexZPrepass"`, `"Environment"`); binding another pipeline's
layout trips `BindLayoutAuto`'s ENSURE. A pipeline's descriptor sets are its own too, and hold one value per binding, so
two scenes can't share one: each `gfx::Scene` is given the pipeline it draws with (`Scene(rhi, pipeline, views)`; the
window's scene uses `Device::GetPipeline()`), and creates its layouts in it. (The device used to keep a map of layout
handles by name hash, which handed out the first pipeline's layouts.)

