# CLAUDE.md

Notes for working in this codebase, distilled from real build failures.

## Layering: core ← rhi ← gfx ← client

- `rhi` is the graphics backend abstraction (instance, devices, queues, buffers, images, pipelines, swapchain,
  `Window`), templated on `GraphicsApi`. It must not include `gfx` or know about the application: rhi objects find
  their instance and devices through `rhi::GetRHI<G>()`, which an `RHI<G>` serves for its whole lifetime (registered
  by its first member, so before its members are created and after they are destroyed).
- `gfx` is everything above it: importers (`obj::Import`, `image::Import`, `zip`), `Model`, `LoadTexture`, cameras and
  `Views`, and `gfx::WindowedApplication` (the windowed, drawing application the client derives from). gfx code is
  not templated on the backend: it names rhi types through the aliases in `gfx/gpu.h` (`rhi::kGraphicsApi`, one per
  build), and contains no backend code: no Vulkan calls, enums or types. gfx also owns the current window
  (`GetCurrentWindow`/`SetCurrentWindow` in `gfx/capi.h`) and the file dialog (`gfx/filedialog.h`).
- rhi's public API speaks its own vocabulary, `rhi/enums.h` (`Format`, `ImageLayout`, `ImageAspect`, `ImageUsage`,
  `BufferUsage`, `PipelineStage`, `Access`, `LoadOp`, `SamplerDesc`, `Extent2d`, `ClearValue`, `PresentResult`,
  ...), converted at the backend boundary with `rhi::vk::ToVk`/`FromVk` (`rhi/vulkan/convert.h`, backend sources
  only). Handles stay `XxxHandle<G>`. Add a neutral value (and its conversion) rather than exposing a Vulkan type.
  Commands without an object of their own go through `CommandEncoder<G>` (viewport, scissor, index buffer, draws,
  dispatch, buffer updates, memory barriers); secondary command buffers inheriting a render target come from
  `CommandPool::SecondaryCommands`; descriptor values are `BufferBinding<G>`/`ImageBinding<G>`, which `Pipeline`
  stores in Vulkan's form (the update templates read them in place).
- ImGui's renderer is rhi's `ImGuiRenderer<G>` (`rhi/imguirenderer.h`): the imgui Vulkan backend plus imgui's
  textures, which it creates and uploads itself on the ui thread and records on the draw thread. gfx owns the imgui
  context, the glfw platform backend (`ImGui_ImplGlfw_InitForOther`) and the triple buffer of draw data snapshots
  between the two threads.
- Shaders are gfx's: the slang sources and the C header they share with the C++ code (`gfx/shaders/`), and
  `gfx::ShaderLoader`, which compiles them for `rhi::kShaderFormat` and reflects their bindings into a neutral
  `rhi::ShaderSet` (`rhi/shaderset.h`: binaries, entry points, and per set the bindings and push constants). rhi
  doesn't link slang. `Pipeline::CreateLayout` turns a `ShaderSet` into the backend's descriptor set layouts (every
  binding partially bound). What the app's descriptor sets need from the pool comes in through
  `RHIInitializationData::descriptorPoolSizes`, since only the app knows its array sizes.
- The backend's type aliases (`DescriptorType<G>`, `PushConstantRange<G>`, ...) are in the *global* namespace (see
  `rhi/vulkan/types.inl`), so a neutral rhi type of the same name hides them inside `namespace rhi`, and is ambiguous
  in code with `using namespace rhi` (gfx's application). Pick another name, or qualify: `::DescriptorType<G>` in
  rhi, `rhi::DescriptorType` in gfx.

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

## Associative containers: core's, not the standard library's

Don't use `std::map`, `std::set`, `std::unordered_map` or `std::unordered_set` (tools included): use `core::UnorderedMap`
and `core::UnorderedSet` (`core/utils.h`, ankerl::unordered_dense). Where an order matters (e.g. a report), sort a
vector of the keys. Composite keys are small structs with a defaulted `operator==` and a byte hash (`XXH3_64bits`,
`using is_avalanching = void;`, a `has_unique_object_representations` static_assert), floats stored by their bits so
that equal keys hash equal (see `TextureViewKey`, `obj::detail::VertexKey`).

## Choosing a lock

Measured on this machine (M4 Pro): `core::UpgradableSharedMutex` is the cheapest uncontended (~4-7 ns) and is the
only one with upgrade locks, but every unlock wakes all waiters, so it degrades badly under contention (8 threads
on one exclusive lock: ~430 ns vs `std::mutex`'s ~17 ns). So: `std::mutex` for exclusive-only locks
(`gDrawMutex`, `MemoryPool`'s free list, the object tracking maps); `UpgradableSharedMutex` where upgrade locks are
used (descriptor set state in `Pipeline`) and as `ConcurrentAccess`'s default, whose locks are short and mostly
uncontended (at 2-3 threads it and `std::shared_mutex` trade wins). Don't lock what can't change: `MemoryPool`'s
`GetPointer`/`GetHandle` are plain address arithmetic on fixed storage. The `TaskExecutor` wakes idle threads with
an atomic wake count (`myWakeCount`) rather than a condition variable: submitting takes no lock, and a thread
can't miss a wake between finding the queue empty and waiting.

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
  visiting all queue types dedupe by context (see `WindowedApplication::Shutdown()`).

## Descriptor sets: redundant updates consume the pool

Array bindings set element by element (`SetDescriptorData(name, value, set, index)`) store their elements in index
order, which the update template walks: a new index goes before the first range above it. (It used to be appended,
which swapped elements whenever one was set below an existing one: model sampler slot 0 got the default sampler set
earlier in slot 2.)


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

## Asset import and testing

Decoding is CPU only and lives in `gfx`: `gfx::obj::Import` (tinyobjloader), `gfx::gltf::Import` (cgltf) and
`gfx::image::Import` (stb_image, stb_image_resize2, stb_dxt). Both model importers produce a `gfx::mesh::Mesh`
(`gfx/meshimport.h`, which also dispatches on the extension: `mesh::Import`, `IsModelFile`, `Dependencies`).
`gfx::Model::Load` and `gfx::LoadTexture` cache the result and fill rhi staging buffers
(`Buffer::CreateStaging`, before taking a queue's lock), which the staging constructors of `Buffer`/`Image` upload; a
failed load prints why and returns null instead of trapping. When an importer changes what it produces, bump its
`objimport-vN`/`gltfimport-vN`/`imageimport-vN` tag in the loader's params hash, or stale caches keep the old output.
Assets outside `RootPath` are cached under `<user profile>/external/<absolute path>`.

glTF: the default scene is flattened into one mesh with the node transforms applied (a mirroring one reverses the
winding *and* must flip the cofactor normal matrix back, which `NegativeScaleTest` catches). `EXT_mesh_gpu_instancing`
is drawn instanced instead: the node's primitives once, in its space, and a transform per instance (the node's times
the instance's translation * rotation * scale) in `Mesh::instances`/`ModelDesc::instances`, which each submesh names a
range of (`firstInstance`, `instanceCount`; instance 0 is the identity, for everything flattened). Each `Model` has an
instance buffer (`ModelInstance`: the transform and its inverse transpose), which `InstallModel` binds as
`gModelInstances`; a submesh is one `DrawIndexed` of its instances, read at `modelInstanceId + SV_InstanceID`. The
mirroring instances (negative determinant) are sorted last (`mirroredInstanceCount`) and drawn separately with clockwise
front faces (`CommandEncoder::SetFrontFace`, dynamic state like the cull mode): they reverse the winding, so a single
draw would cull them, and get double sided shading's front and back the wrong way round.
Points and lines (strips and loops become line lists) are submeshes of their own (`ModelSubmesh::topology`), drawn
with a pipeline per topology: `Pipeline::BindPipelineAuto(cmd, variant)` takes it as a parameter, part of the pipeline
cache key, rather than as state, since the draw threads share the pipeline. Without normals in the file they keep zero
normals, which the fragment shader draws unlit (base color plus emissive, as gltf says). `VertexMain` writes the point
size (Vulkan needs it for point lists) in a struct of its own (`VertexMainOutput`): slang refuses `SV_PointSize` as a
fragment input. Double sided materials
(`ModelMaterial::doubleSided`) are drawn with culling off, set per submesh with `CommandEncoder::SetCullMode` (dynamic
cull mode, `VK_EXT_extended_dynamic_state`, a required device extension), and the fragment shader flips the normal of
back faces (`SV_IsFrontFace`). Alpha modes become
`mesh::Material::alphaCutoff` (`MaterialData::alphaCutoff`, 0 for OPAQUE, which must not alpha test the base color
texture, and for BLEND). BLEND materials (`blend`) are drawn after the opaque submeshes, sorted back to front per view
by `ModelSubmesh::center` (from `Views::GetEyePositions`), with the `BlendMode::kAlpha` pipeline variant (source alpha
over, depth tested but not written). The pipeline variant (`GraphicsPipelineVariant`: topology and blend mode) is a
parameter of `BindPipelineAuto`, part of the pipeline cache key. glTF texcoords already have v = 0 at the top, so unlike obj they aren't flipped, and
normal maps share the obj convention (with `normalTexture.scale` applied to their x and y, as the spec defines it).
Emissive (gltf `emissiveFactor` times `KHR_materials_emissive_strength` and the srgb `emissiveTexture`, obj `Ke` and
`map_Ke`) is added after the lighting, unclamped (CornellBox's lamp, `Ke 17 12 4`, saturates to white); an emissive
texture with a black factor isn't loaded (obj files pair `map_Ke` with `Ke 0`). Occlusion maps (gltf `occlusionTexture`,
by its strength) darken only the ambient term, the stand-in for the indirect light gltf applies them to; they are
imported as `image::Usage::kOcclusion` (the red channel, linear, BC4), since gltf often packs occlusion, roughness and
metallic into one texture's r, g and b, which kMask's luminance would mix. Each texture is a `TextureRef` (`gfx/textureref.h`: path, texcoord set, `KHR_texture_transform` as a 2x3 matrix, and its
sampler as `rhi::SamplerDesc`), which the shader applies: vertices keep both texcoord sets as they are (`texCoord01.xy`,
`.zw`), materials name a `TextureView` per texture (`gTextureViews`: texture and sampler slot, set, transform; 0 is
material 0's, deduplicated per model), and `ViewTexCoord`/`SampleView` sample through them (the normal map's tangent
frame follows its own transformed texcoords). A model's distinct samplers get the 15 sampler slots other than the
default's (`kModelSamplerSlots`), and the previous model's go back to the default. Images embedded in buffers or data uris are written to
`<user profile>/embedded/<name>-<hash>/` (named by content) and loaded like external ones. Only an import writes
them, so `Model::Load` treats a cached model whose extracted images are missing as an unreadable cache, and
`LoadAsset` imports it again. Files requiring draco or
meshopt compression, KTX2/basisu or WebP fail to load with a message naming the extension: those need libraries the
project doesn't have. KHR_node_visibility hides nodes; morph targets are applied at their default weights (the
node's, else the mesh's; position and normal deltas); skins (drawn in bind pose) and animation are ignored with a
warning. Cameras are imported (`SceneCamera`, `ModelDesc::cameras`: world position and forward,
perspective field of view or orthographic height, near and far; none for a set of files) and the views use the first
(`Views::SetScene`); View > Camera picks another or frames the model (`Views::UseSceneCamera`). The views keep no roll,
so a rolled camera loses it, and the file's aspect ratio gives way to the view's. The Khronos glTF-Sample-Assets `Models/` are the test set (see below; `assettest` takes
`.gltf`/`.glb`): there, in the image checks, a 4x4-or-smaller mip only warns about its average (one BC1 block can't hold
more than four colors), and normals below the surface (z < 0, which BC5 can't store) are compared mirrored and warned
about, since both are properties of the asset rather than importer errors.

The loaders don't wait for their uploads: they return the resource with its `gfx::Upload` (the transfer timeline
semaphore and value, and the transfer queue family), and keep it alive from the upload's timeline callback in case the
caller drops it. Whatever uses it on another queue must wait for the upload on the gpu and acquire the resource for its
family (`TransitionThenBind` does both, with `CommandEncoder::AcquireOwnership`): buffers and images are
`VK_SHARING_MODE_EXCLUSIVE`, and the loaders release them to the graphics family. On a single family device (e.g.
KosmicKrisp) the release and acquire are no-ops and the graphics timeline wait covers the upload, so none of this can
be tested there. The transfer queues' timeline callbacks run every frame (in `Draw`) when they aren't the graphics
queues. The staging constructors of `Image` leave it in `kTransferDestination`, and track that: transitioning an
uploaded image from the `kUndefined` it was created with would let the driver discard its contents.

OBJ has no up axis or handedness, so the importer goes by the conventions the renderer expects: counter-clockwise front
faces (the projection flips y, see `Camera`), and `v` flipped so `v = 0` is the first image row. Runs of faces whose
winding disagrees with the file's normals are flipped (mirrored exports), and missing normals are generated, smoothed
within a smoothing group, or within 60 degrees for group 0. Some test scenes are z up (e.g. chestnut) and are loaded as
is.

Rendering is linear: color textures are loaded with srgb formats (`gfx::image::Usage::kColor`, mips filtered in linear
space), the main render target is `R16G16B16A16_SFLOAT`, and `ComputeMain` scales it by the exposure (View > Exposure,
in stops), tonemaps it (Khronos PBR Neutral, which leaves colors up to about 0.76 as they are) and applies the srgb curve
when it copies to the swapchain, which stays unorm since it is a storage image (and imgui's colors are srgb already).
Shading is the glTF metallic-roughness brdf (`Shade` in the shaders: GGX, height correlated Smith, Schlick) over the
lights in `gLights` (`PushConstants::lightCount`; a model's KHR_lights_punctual lights, `ModelDesc::lights`, in lux and
candela, or a default directional light of 2.2 lux), plus a constant ambient radiance (0.3, its specular part by Karis'
environment brdf fit) that occlusion darkens. The default light and ambient light a white matte surface as the fixed
light did before there was PBR. Metallic-roughness textures are `Usage::kMetallicRoughness` (BC5: roughness, the file's
green, in r, and metallic, its blue, in g); obj materials are matte dielectrics (metallic 0, roughness 1), and
`MaterialData` defaults must set roughness to 1 (zero is a mirror). KHR_materials_unlit draws the base color alone. A model's materials
(`ModelCreateDesc::materials`, drawn per `submeshes`) are materials 1 and up in `gMaterialData`. Their diffuse, alpha
(`map_d`, `kMask`: BC4) and normal (`norm`, `kNormal`, else `map_bump`/`bump`, `kBump`: both BC5) textures are loaded with the model and go in `gTextures` slots from 16
(0-3 are the frames' render targets, 15 the texture of material 0 that opening an image loads). Bump textures are height maps in
most mtl files, but some are normal maps: the importer tells them apart by color (normal maps are bluish), turns
heights into normals (scaled by `-bm`), and stores all of them with +y along +v as sampled, i.e. down the image (the
obj importer flips v). Vertices carry gltf's tangents (`VertexP3fN3fTa4fT014fC4f::tangent`: xyz along +u, w the
handedness, so the bitangent `cross(n, t) * w` points *up* the image, i.e. along -v in this convention; mirroring node
and instance transforms flip w). Without them (obj, gltf files without `TANGENT`, or with generated normals, where gltf
says to ignore them) w is 0, and the fragment shader builds the tangent frame from screen space derivatives instead,
corrected by the sign of `dot(cross(ddx(p), ddy(p)), n)`, which is negative here since the framebuffer's y
points down: without it bumps come out inverted. Missing tangents aren't generated (MikkTSpace would need a library).
Either frame follows the normal map's texture transform. NormalTangentMirrorTest checks the vertex tangent path: it
renders right either way, but negating its tangents' w must break it. A quad with a known height map (a dome, which must be lit on the side
the light comes from) is the quickest way to see a sign error. `InstallModel` switches model, textures and materials in one draw
thread step, after the textures are transitioned: every change to `gTextures` takes a new descriptor set (see above),
and the pool only holds 128 copies of that 1024 slot array. Installing a model also sets the views to its first camera,
or frames them on its bounds (`Views::SetScene`, `Views::FrameBounds`).

File > "Open File..." loads models, zip archives and images, by their type (`LoadAndInstallFile`), and "Open
Folder..." a directory's models; `SPEEDO_AUTOLOAD_MODEL` and `SPEEDO_AUTOLOAD_IMAGE` take paths absolute or relative to
the resource directory. Zip archives (opened, or a `.zip` in `SPEEDO_AUTOLOAD_MODEL`, which loads all of its models) are extracted
once into `<user profile>/archives/<name>-<hash of path, size and time>` with `gfx::zip` (stb_image's inflate, no zip
library), then loaded from there like any other files; with several models the user picks one. The extractor reads
each entry by its local header: some archives have stale central directory entries (cube.zip in the McGuire archive),
which unzip ignores too. `assettest` takes zip archives as well, extracting them the same way.

`scripts/assettest.sh <zips or dirs>` runs the `assettest` tool (imports every model and image and checks the result:
index ranges, normals, winding, missing textures, mip chains, unwritten blocks, compression error), and with `--client`
also loads each model in the client (`SPEEDO_AUTOLOAD_EXIT=<frames>` makes it exit after the autoloads finish), failing
on load errors, asserts and validation messages. Only debug enables validation, but it imports slowly: run the
profile preset first, then debug with `--client-only` and the same `--work` dir, whose caches it reuses. The client's
main loop sleeps in `glfwWaitEvents()`, so anything that must end it from another thread goes through
`RequestExit()`, which posts an empty event.

The test sets come from their sources, not from local copies: `scripts/fetch-test-assets.sh` downloads Morgan McGuire's
Computer Graphics Archive (obj, about 2.7 GB) and the Khronos glTF-Sample-Assets models (at a pinned commit, about
2.3 GB) into `resources/test-assets` (gitignored; the client's file dialogs open there; or `$SPEEDO_TEST_ASSETS`), and
prints the paths to test:
`scripts/assettest.sh --client $(scripts/fetch-test-assets.sh)`. The archive publishes no versions or checksums and
does change (several files differ from a 2019 copy), so `scripts/test-assets/mcguire.txt` pins each file's size and
sha256: a changed file is kept as `.unverified` and reported until the manifest is updated, after checking what changed.
`scripts/test-assets/gltf` holds hand-made models for what no downloaded one covers (`SparseIndices.gltf`: index
accessors that are sparse, with and without base values; cgltf's `cgltf_accessor_read_index` and
`cgltf_accessor_unpack_indices` refuse sparse accessors, so `gltf::ReadIndices` applies them;
`InstancingTransforms.gltf`: instances of a single sided, asymmetric triangle under a scaled and moved node, with
normalized short rotations and a mirroring instance, which must face the camera too; `BlendOrder.gltf`: blended quads
listed nearest first, whose overlaps must be tinted by the nearer one), and is always part
of the printed paths. Two archive files are both called `sponza.zip` (Crytek's and Dabrovic's), so the latter is saved as `dabrovic_sponza.zip`,
and Bistro's five zips (the scenes and three texture packs, which the scenes reference as `..\BuildingTextures\...`) are
extracted side by side into `mcguire/bistro/`. Known asset problems that only warn: erato's normals disagree with its
(consistent) winding on a quarter of its area, Bistro and bmw have normals that get replaced, and
`geodesic_dual_classIII_20_10.obj` has 1419 stray vertices (at radius 1140, the others at 1), which `assettest`'s stray
vertex check (more than 100 times the median distance from the median point; ground planes reach about 20) finds.

Archives of several models are sets of variants (geodesic's 86 polyhedra, sphere's tessellations and texture mappings,
the CornellBox variants, ...), not scenes: each file stands alone at the origin, and the files of a set needn't share a
scale. `Model::Load(filePaths)` loads them as one model, side by side in a grid facing the camera, each scaled to the
same size and centered in its cell by its instance transforms (premultiplied by the placement; vertices are copied as
they are), each file still through its own cache entry, merged as staging data before the one upload. Autoloading a zip or a directory loads all of its models that way, opening a zip offers it next to choosing one,
"Open Folder..." loads a directory's, and `assettest.sh` runs the client once per such archive (with the archive's first
image on the default material: sphere.zip's models name materials its mtl file doesn't have), and once per
subdirectory with several models of a directory it is given, rather than once per file: 38 client runs instead of 145
for the McGuire set. The glTF sample models are such subdirectories, of encodings of the same model (glTF, glTF-Binary,
glTF-Embedded, glTF-Quantized, Draco, KTX2, ...): one run each (150 instead of 339), with the encodings the importers
don't support skipped (`mesh::Unsupported`, which only parses the gltf json) rather than failing the set. `assettest`
compares the encodings of each model (files named after the model's directory, in its subdirectories): the same
triangle, vertex, material, submesh and texture counts, and bounds within 1% (quantization rounds). It only warns, since
some differ in the source files: ABeautifulGame's glb is another export (1152 fewer triangles), and StainedGlassLamp's
JPG-PNG variant has 8 materials instead of 13.

## Gotcha: `core::CreateTask` stores lvalue arguments by reference

`CreateTask(callable, args...)` keeps `args` in a `std::tuple<Args...>` with `Args` deduced as
forwarding references, so an **lvalue argument is stored as a reference**, not copied. The task
usually runs later, so passing a local or a function parameter as an lvalue leaves a dangling
reference (typically read back as mimalloc's freed-memory pattern `0xdfdf...`). Pass arguments that
must outlive the call as rvalues — `std::move(x)` or an explicit copy like `std::vector(x)` — as
`WindowedApplication::InternalOpenFileDialogueAsync` does. Only pass lvalues when the reference is
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

The same applies to a bare `cmake --build build/<preset>` from a shell when that dir needs to regenerate (e.g.
after `vcpkg.json`/`ports/` changed, or after *another* preset ran a vcpkg install into the shared `install/`
dir). The auto re-run of CMake doesn't have the preset environment, so vcpkg computes different ABIs, removes
the installed packages from the shared `install/` tree, and then fails rebuilding them (`Apple-clang.cmake`
case warning, glfw3 configure error). Every build dir then misses its dylibs (`Library not loaded:
@rpath/libcargs.dylib`). Recover with `cmake --preset <name>`, which restores the packages from the binary
cache. From a shell, always run `cmake --preset <name>` right before building that preset.

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

`ports/mimalloc` exists for a macOS bug in mimalloc 3.x, so retire it only once upstream fixes it. dyld allocates
each image's thread-local-variable block from the system zone (`malloc_type_malloc`) and frees it at thread exit
through the pthread key destructor `free`, which is interposed to `mi_free`. mimalloc's own thread locals are
included, so every exiting thread frees a block mimalloc doesn't own. Debug builds print
`mi_free: invalid pointer`, and both builds leak 32 bytes per thread. The patch interposes `free` with a check
that forwards foreign pointers to `malloc_zone_from_ptr(p)`.
