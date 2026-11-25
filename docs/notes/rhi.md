# rhi

rhi (src/rhi) is the graphics backend abstraction, templated on `GraphicsApi` (Vulkan 1.4, Metal, D3D12). This note covers the abstraction's model, mostly as the Vulkan backend has it: how objects are built, moved, identified and tracked; how frames, command buffers, queues and descriptor sets are owned and shared; how pipelines keep their layouts and caches; what a crash or hang leaves behind; then Vulkan specifics and the C++ gotchas a developer must respect. The Metal and D3D12 backends are described in docs/backends-plan.md.

## Contents

- [The backends](#the-backends)
- [rhi: `Object<T>` / `DeviceObject<T>` base classes](#rhi-objectt--deviceobjectt-base-classes)
- [Move-via-Swap idiom](#move-via-swap-idiom)
- [rhi object identity and tracking](#rhi-object-identity-and-tracking)
- [Frame indices are slots, not swapchain images](#frame-indices-are-slots-not-swapchain-images)
- [Command buffers: one owner each](#command-buffers-one-owner-each)
- [Queues: aliased queue types share one lock](#queues-aliased-queue-types-share-one-lock)
- [Descriptor sets: redundant updates consume the pool](#descriptor-sets-redundant-updates-consume-the-pool)
- [Pipelines own their layouts, by name](#pipelines-own-their-layouts-by-name)
- [Pipeline caches: kept in check](#pipeline-caches-kept-in-check)
- [`rhitest`: rhi checked against Vulkan](#rhitest-rhi-checked-against-vulkan)
- [Crash reports](#crash-reports)
- [Vulkan 1.4](#vulkan-14)
- [Gotcha: explicit specialization ordering](#gotcha-explicit-specialization-ordering)
- [Gotcha: delegating constructors run their arguments before the base exists](#gotcha-delegating-constructors-run-their-arguments-before-the-base-exists)

## The backends

`rhi` is generic in the backend `G` (`GraphicsApi`): its public headers use `rhi/vocabulary.h`, `BackendTypes<G>` (`rhi/types.h`, one `types.inl` per backend) names the backend's types, and each backend instantiates the templates in `rhi/<backend>/instantiate.cpp` (`rhi/vulkan`, `rhi/metal`, `rhi/d3d12`). All three are linked into one client, and which one runs is chosen at start (layering.md, "client"). What a backend provides and how Metal and D3D12 map rhi's objects: docs/backends-plan.md.

## rhi: `Object<T>` / `DeviceObject<T>` base classes

Most rhi types (`Buffer`, `Fence`, `Queue`, `Pipeline`, ...) derive from `Object<DerivedType>` or `DeviceObject<DerivedType>` (`src/rhi/object.h`, `deviceobject.h`), a CRTP base. `GetInstance()` and `GetDevice()` have no generic definition; each concrete type specializes them with macros, once, in its own `.cpp` (which needs `#include <rhi/rhi.h>`):

```cpp
IMPLEMENT_OBJECT_GETINSTANCE(Foo<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Foo<kVk>);   // only if Foo derives from DeviceObject
```

A missing invocation shows as a linker error such as `undefined symbol: rhi::Object<rhi::Foo<...>>::GetInstance() const`.

## Move-via-Swap idiom

Movable types (for example `Device` in `RHI::myDevices`) use `Swap` instead of defaulted moves:

```cpp
void Foo::Swap(Foo& other) noexcept { SuperType::Swap(other); std::swap(myX, other.myX); ... }
Foo::Foo(Foo&& other) noexcept { Swap(other); }          // default-constructs *this*, then swaps
Foo& Foo::operator=(Foo&& other) noexcept { Swap(other); return *this; }
```

A moved-from object holds the default-constructed state, so destructors must guard on `IsValid()` (true iff `GetDesc().uuid` is set) before releasing gpu handles. Not everything is movable: `Instance` deletes its move constructor.

## rhi object identity and tracking

- `ObjectCreateDesc::uuid` identifies a live object; `name` is for debugging. Build descs with the `Create*ObjectCreateDesc(name)` helpers. Validation builds `ENSUREF` that the uuid is non-nil and not held by another live object (`gLiveObjectUuids`), so copying a live object's desc traps.
- `Device`'s resources are keyed by uuid: `CreateResource<T>(desc, ...)`, `GetResource`, `ReplaceResource(previousUuid, resource)` (the caller keeps the new uuid).
- Every Vulkan object is tracked in validation builds by type and handle (the Statistics window shows them, `GetObjectCounts`). Call `Track(device, type, handle, name)` right after creating a handle and `Untrack(type, handle)` right before destroying it; untracking an untracked handle traps. Handles must be non-null and names non-empty (`GetDebugName(desc)`). Instance level objects use `TrackInstance`.
- Buffers, images, views, framebuffers and render passes are tracked by the `Create*` helpers in `rhi/vulkan/utils.h`: destroy them with the matching `Destroy*` helpers, not `vmaDestroy*`/`vkDestroy*`.

## Frame indices are slots, not swapchain images

A frame's index (`FrameInputs::frameIndex`, `PushConstants::frameIndex`, every per frame buffer and descriptor slot) is its slot of `SHADER_TYPES_FRAME_COUNT`, taken in turn (`Draw`'s `myFrameSlot`). `Draw` waits for the graphics timeline value the slot's last frame signaled (`myFrameSlotTimelines`) before anything writes the slot's buffers.

- The swapchain image index (`Swapchain::Flip`) is the output index (`FrameInputs::outputIndex`), used only for what belongs to the image: ComputeMain's and the UI blur's storage views (`gRWTextures` slots below `SHADER_TYPES_OUTPUT_IMAGE_COUNT`), the UI render target and the capture.
- Reason: fifo hands images out in turn, but mailbox and immediate do not, so a frame still on the gpu read buffers the next frame wrote over.
- The queue ring's fences cannot stand in for the slot's wait: the ring is as long as the graphics family's queue count.
- Test state kept from frame to frame under mailbox too, since unthrottled frames reach states fifo's do not.

## Command buffers: one owner each

A `rhi::CommandBuffer` (`rhi/command.h`) is move-only with a single owner. Code that records into it borrows its handle (`CommandBufferHandle`); only the owner can end it and hand it on, to `Queue::EnqueueSubmit(syncInfo, std::move(cmd))`, or, for a secondary buffer, to `CommandEncoder::ExecuteCommands(std::move(secondaries))`. Both end it if still recording and return it to its pool (`CommandBuffer::Release`).

- The pool (`CommandPool`, two per queue, swapped and reset by `Queue::SwapAndResetPool`) hands out buffers (`Begin`, `BeginSecondary`, `Allocate`) and makes them reusable all at once on reset, which traps if any is still held. A buffer dropped unsubmitted is ended and returned unrun, with a warning in validation builds.
- The frame's buffer is `Draw`'s, lent to the scenes, renderer and ui. Uploads and transitions (`UpdateBufferOnGraphics`, `TransitionThenBind`, loader copies) begin buffers of their own. A helper must never end the frame's buffer: that once left the frame recording into an ended buffer.
- A pool's buffers must not be recorded on several threads at once; parallel recording needs a pool per thread.

## Queues: aliased queue types share one lock

When a device has no dedicated compute or transfer family (for example KosmicKrisp), `Device::InternalCreateQueues` aliases those queue types to the graphics queue by sharing one `QueueTimelineContext` (data and mutex).

- Access queues through `Device::GetQueue(type)`. Locking the transfer queue may be locking the graphics queue, which `Draw()` holds for the whole frame; that serialization is the point.
- Never hold locks on two queue types at once unless they are known to be distinct: `UpgradableSharedMutex` is not recursive and self-deadlocks on a single queue device. `Draw()` takes the compute lock inside the graphics one only to present from a dedicated compute family (`Swapchain::GetPresentQueueType`). When visiting all queue types, dedupe by context (see `WindowedApplication::Shutdown()`).
- Command pools are reset without `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`: on KosmicKrisp that frees Metal command allocators that were never reset, and freeing one Metal has not had back yet aborts in the driver. A rare abort in `vkDestroyCommandPool` at exit on KosmicKrisp is unexplained.

## Descriptor sets: redundant updates consume the pool

- Binding a dirty set (`BindDescriptorSetAuto` → `InternalUpdateDescriptorSet`) takes a fresh set from the current `DescriptorSetArray` (16 sets), allocating a new array when full; old arrays are not recycled. So `SetDescriptorData` skips writes whose value is unchanged (`pipeline::SameBinding`), or per frame rewrites would exhaust the pool (`VK_ERROR_OUT_OF_POOL_MEMORY`).
- A layout whose bindings change at every dispatch can hand its sets over (`Pipeline::TakeDescriptorSets(set)`), destroyed once the gpu is done with the commands that bound them. They are freed in a timeline callback on an executor thread while the draw thread allocates from the same pool, so every allocation, free and reset holds `Pipeline::myDescriptorPoolMutex` (Vulkan requires external synchronization).
- The environment filter's set is pushed instead (`Pipeline::CreateLayout`'s `pushDescriptorSets`): its layout has `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` and no partially bound bindings (a push layout allows no binding flags), every binding must be set before the first kernel, and at most 32 push descriptors are allowed. Other layouts hold larger arrays and cannot be pushed.
- A handle equal to the one set is not necessarily the same object: drivers reuse destroyed handles, so objects destroyed and remade with the gpu idle can come back unchanged-looking and leave a set holding destroyed descriptors. `Scene::CreateOutputDependentObjects` therefore calls `Pipeline::InvalidateDescriptorSets` after setting them.
- Loaded resources install on the draw thread via `rhi.drawCalls`, are stored with `Device::ReplaceResource`, and the previous one is freed via `RetireAfterGraphicsWork` once in-flight work is done.

## Pipelines own their layouts, by name

A layout belongs to the `rhi::Pipeline` that created it: `CreateLayout(name, shaderSet)` registers it under its name and `GetLayoutHandle(name)` finds it (`"Main"`, `"VertexZPrepass"`, `"Environment"`); binding another pipeline's layout trips `BindLayoutAuto`'s ENSURE. A pipeline's descriptor sets hold one value per binding, so two scenes cannot share one: each `gfx::Scene` is given its own pipeline (`Scene(rhi, pipeline, views)`; the window's uses `Device::GetPipeline()`).

`Pipeline::ReplaceLayout` makes a named layout again from a set whose resources differ (used to switch the ray queries' shader variants): what was set is carried into the new layout's binding of the same shader variable where the type matches and the descriptor count is at least as large, old layouts and sets are destroyed, pipelines remade, a bound layout rebound. The gpu must be idle; it does nothing while a pipeline is being made.

## Pipeline caches: kept in check

Each backend keeps the pipelines it made in files of the profile (`PipelineCreateDesc::cachePath`). A Vulkan pipeline cache is all or nothing and a D3D12 library never gives names up, so without limits they only grow; they are bounded by `kPipelineCacheByteCount`.

- Vulkan with `VK_KHR_pipeline_binary`: no `VkPipelineCache`; each pipeline's binaries are a file in `pipeline.binaries/` (`rhi/vulkan/pipelinebinaries.h`, `BinaryStore`), named by the pipeline key (`vkGetPipelineKeyKHR`) and headed by the driver's vendor, device and cache uuid. A pipeline whose file exists is made from it (another driver's, or a refused one, is made again); otherwise it is compiled with `VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR` and written. A file's time is its last use; at exit the least recently used go while over budget, down to three quarters. `SPEEDO_PIPELINE_BINARIES=0` leaves the extension out.
- Vulkan without it: the cache file, ignored at load once over budget (`LoadPipelineCache`: "started over"), so the run saves its own pipelines.
- D3D12: the library notes every state used (`PipelineCacheObject::used`); at exit, if over budget, a new library of those alone replaces it (`pipelinecache::Close`).
- The driver's own cache is configured in layering.md, "Pipelines made without waiting".

## `rhitest`: rhi checked against Vulkan

`src/tools/rhitest.cpp` (kernels `rhitest*.slang`) tests rhi generic in the backend: compute, images, the environment kernels, draws, indirect draws and blits, compared against Vulkan.

## Crash reports

A fatal Vulkan result or a hang writes a crash report in every build (`rhi/crashreport.h`, `rhi/vulkan/crashreport.cpp`). `VK_CHECK` checks in every build and hands errors to `vk::crash::ReportFatal`. The first thread to meet one writes `<user profile>/crashes/crash-<time>.json` (`SetCrashReportDirectory`, set by the client) while the others wait, then traps. A report has:

- The kind, result, a signature (a hash of vendor, driver version, kind, result, first pass in flight and fault type, so one cause groups across machines), build, platform, device, driver and tools loaded.
- Breadcrumbs: the frame graph's passes (`CrashBreadcrumb`) written by the gpu as each begins and ends, with `VK_AMD_buffer_marker`, else `VK_NV_device_diagnostic_checkpoints` (sequence numbers, never pointers), else none. `SPEEDO_CRASH_BREADCRUMBS=markers|checkpoints|none` forces one.
- The device fault (`VK_EXT_device_fault`) and the objects bound around the faulting address (`VK_EXT_device_address_binding_report`, through the messenger `crash::CreateMessenger` makes just before a device that enables it), including recently unbound ones, so a write to freed memory names what was freed.
- The last 64 submits and the last 256 application events (`rhi::CrashEvent`), stamped with the frame (`CrashFrame`).

Hangs:

- A wait without a timeout waits at most `SPEEDO_GPU_HANG_TIMEOUT` seconds (default 20, 0 for never) and reports a hang (`vk::crash::WatchedWait`). The test scripts raise it, since many clients starting together stall one another.
- The main thread never waits for the draw thread; it watches it (`rhi::CrashWatch` against `CrashHeartbeat` at each `Draw`), catching a draw thread blocked in a call with no timeout.
- Work that makes progress where no heartbeat can tell (a cold pipeline creation in the driver) is a busy span (`rhi::CrashBusy`), which the watch gives 120 s.
- `SPEEDO_CRASH_TEST=hang` makes frames from the 100th wait for a semaphore nothing signals, to check reports end to end (with `SPEEDO_GPU_HANG_TIMEOUT=3`).

With the Crash Diagnostic Layer loaded, no breadcrumbs are recorded and the report defers to the layer's dump (`<home>/cdl/<time>/cdl_dump.yaml`); the reporter waits 3 s before trapping so the layer can write it. KosmicKrisp's instance host allocations are zeroed (`instance.cpp`) because its vendor id was otherwise uninitialized memory.

## Vulkan 1.4

The instance asks for Vulkan 1.4 (`kVulkanApiVersion`, `rhi/vulkan/utils.h`) and only devices that support it are candidates (`DetectAndCreateDevices`). What 1.1 to 1.4 made core (dynamic rendering, synchronization2, dynamic cull mode, shader draw parameters, push descriptors, tooling info) is called as core, not through extensions.

- Features come in `PhysicalDeviceInfo`'s version structs (`deviceFeatures11Ex` to `deviceFeatures14Ex`), then the extensions' (`deviceFeatureParams`). The device is made with the whole chain, so it enables every feature it has; `Device` checks by name the optional ones the renderer needs.
- On macOS, KosmicKrisp must report 1.4, or no device is a candidate.

## Gotcha: explicit specialization ordering

Calling an explicit specialization before defining it in the same `.cpp` instantiates the primary template and errors with `explicit specialization of 'X' after instantiation`. Define `Swap()`'s specialization before the move members that call it, and place `IMPLEMENT_OBJECT_GETINSTANCE`/`IMPLEMENT_DEVICEOBJECT_GETDEVICE` after any specializations the macro calls (for example `GetRHI<kVk>()` in `vulkan/rhi.cpp`).

## Gotcha: delegating constructors run their arguments before the base exists

Many types create their gpu handle in the argument list of a delegating constructor (`: Foo(std::forward<CreateDescType>(desc), [this, &desc] { /* create handle */ }())`). That runs before the `DeviceObject` base is initialized, so `GetDesc()`, members and the no-argument `GetDevice()` read uninitialized memory there.

- Read from the incoming `desc` instead: `desc.device`, `GetDevice(desc.device)` where the `Device` object is needed, `CreateDeviceObjectCreateDesc(name, desc.device)` for children. `GetInstance()` is safe. Do not pass references into `desc`, which is moved into the base first.
- Serialized create-descs carry only `uuid`, never `instance`, `device` or `name`. Code that builds a desc from a file or cache must fill those in and replace the `uuid` with `uuids::NewUuid()`. A derived desc with extra fields needs its own `serialize()` (see `ImageCreateDesc`). When a serialized layout changes, bump the `cache-vN` tag in that loader's params hash.
- `GetDevice()` resolves through `GetRHI<G>()->GetDevice(handle)`, which finds only devices already stored in `RHI::myDevices`; that is why `Device` creates its queues and pipeline from the `RHI` constructor.
