# Backends plan: Metal, then D3D12

The user's ask (2026-11-13): prove rhi's abstraction of the graphics api with a second backend for real. A null backend was ruled out on 2026-11-10 ("serves no purpose"), a cpu backend (a non-conformant lavapipe) and OpenGL (macOS stops at 4.1, without compute shaders, and its implicit model would turn rhi's barriers, timelines and queues into no-ops) were weighed and set aside. Metal comes first: an explicit api unlike Vulkan, the platform the client is developed on, and one where every feature the renderer uses is known to map (KosmicKrisp runs the renderer over Metal), so its frames and times compare against KosmicKrisp's on the same machine. D3D12 was investigated alongside (below). This file holds what the shaders and rhi need of each, the decisions, the plan and its progress.

## The shaders on slang's Metal and HLSL targets (2026-11-13)

Every layout the client loads (gfx::SceneShaderLayouts with and without the device's ray queries, the ImGui renderer's, the environment filter's: 172 entry points, as `tools/shaderprecompile.cpp` compiles them) went through slang 2026.22's `slangc` one entry point at a time, with the client's options (`-O3`, column major matrices, `SHADERTYPES_H_GPU_TARGET`). The Metal output was compiled by Metal's own runtime compiler (`MTLDevice newLibraryWithSource`, MSL 3.2, the M4 Pro), as a Metal backend would compile it: Xcode's offline compiler needs the Metal Toolchain component, which isn't installed. The HLSL was emitted for shader model 6.6; DXIL needs DXC (`dxcompiler`), which slang loads and macOS doesn't have, so its validation waits for Windows.

- **The binding model** (the one large item, shared by Metal and D3D12): the shaders declare 91 resources as globals with `[[vk::binding(binding, set)]]` in 8 descriptor sets (`bindings.slang`). Slang gives globals direct Metal argument slots, and Metal has 31 buffers and 128 textures a stage, and no arrays of buffers as direct arguments (the per frame `StructuredBuffer<T> g[SHADER_TYPES_FRAME_COUNT]`): 12 of 168 entry points compiled, every error of those three kinds. In HLSL slang packed all 8 sets into one register space. The portable form is a `ParameterBlock<T>` per set: a descriptor set on Vulkan, an argument buffer on Metal, a descriptor table in its own register space on D3D12 (`-parameter-blocks-use-register-spaces`). Checked on a test file with the renderer's kinds of resources (per frame buffer arrays, the 1024 textures indexed non-uniformly, samplers, storage image arrays, push constants, the specialization constant): its compute, vertex and fragment stages pass Metal's compiler, and its SPIR-V keeps each block's set number with the fields bound 0, 1, 2... in declaration order. gfx binds by the shaders' names (`Pipeline::SetDescriptorData("gVertexBuffer", ...)`, about 200 calls): the blocks' fields keep today's names and the reflection drops the block's prefix, so those calls stay.
- **A slang crash**: its Metal target segfaults on a vertex output holding a nested struct of three or more fields without semantics (two fields compile; HLSL and SPIR-V compile it). `VertexMain` meets it through `VertexOutput.vertex`. Minimal repro: `struct T { float a; float b; float c; }; struct O { T v; float4 hPos : SV_Position; };` returned from a vertex entry point, `slangc -target metal`. A semantic on the nested member, or on each of its fields, avoids it. Not reported upstream yet (2026.22 is the latest release; no matching issue): the user's to file.
- **`WavePrefixCountBits`** isn't in slang's Metal target (`ComputeGiVoxelize`, twice in `gi.slang`). `WavePrefixCountBits(true)` counts the active lanes below, which is `WavePrefixSum(1u)`: `simd_prefix_exclusive_sum` on Metal, an exclusive subgroup add on SPIR-V and HLSL.
- **The specialization constant** (`kSkippedLayers`, `[vk::constant_id(0)]`) becomes a Metal function constant (`[[function_constant(0)]]`), set at pipeline creation as Vulkan's is. D3D12 has none: slang makes it an ordinary shader parameter, so there it is a compile time define (a variant per value) or a root constant.
- **Ray queries** come out as `metal::raytracing` intersection queries (16 entry points), to validate once the bindings compile. Apple's bug that restarts the gpu (Metal ray tracing beside other gpu work, see docs/notes/indirect-lighting.md) is Metal's own (`rtrepro` reproduces it in pure Metal), so on a Metal backend ray queries stay opt in on macOS as they are on KosmicKrisp.
- **HLSL**: all 172 entry points compile to HLSL source at shader model 6.6.

No other kind of error showed: once the bindings are parameter blocks, the Metal check is run again over every entry point (a step of the plan below).

## What rhi asks of a backend

rhi is about 13,500 lines: 30 public headers in rhi's own vocabulary (`rhi/vocabulary.h`: formats, layouts, stages, access, descriptor types, ...) and their Vulkan implementation (`rhi/vulkan`, 33 files). The build picks the backend (`rhi::kGraphicsApi`, `rhi/rhi.h`; `RHI_SOURCE_FILES`); the class templates take it as `G`, and `BackendTypes<G>` (`rhi/types.h`, Vulkan's in `rhi/vulkan/types.inl`) names the backend's types. Of its 89 aliases, gfx and the client use only handles (`CommandBufferHandle`, `ImageViewHandle`, `SemaphoreHandle`, `PipelineLayoutHandle`, `AccelerationStructureHandle`) and `DescriptorType` and `Flags`; the rest are Vulkan structs kept as members of rhi's classes (the feature chains `PhysicalDeviceFeatures11Ex` to `14Ex`, the pipeline create infos, `DescriptorUpdateTemplateEntry`, `RenderPassBeginInfo`, ...). So gfx needs nothing of a second backend; rhi's class templates do: each backend's `BackendTypes` with types of the same roles, or members moved behind the backend's own state where the roles don't map. That is the abstraction the second backend proves or corrects.

What the renderer uses, which a backend must provide or have an exception for: Vulkan 1.4 with dynamic rendering, synchronization2, timeline semaphores, push descriptors (with update templates), push constants (at most 128 bytes, `renderer.cpp`), one specialization constant, descriptor sets from pools (8 sets, the 1024 textures indexed non-uniformly, 64 samplers), indirect draws with a count (`drawIndirectCount`, emulated where `DeviceLimits::drawIndirectCountEmulated`), dispatch indirect, buffer device addresses, acceleration structures and ray queries (optional, opt in on macOS), timestamp queries, a graphics and an async compute queue (with queue family ownership transfers), secondary command buffers (`ExecuteCommands`), a swapchain with present id and wait, a pipeline cache, and the crash reports' breadcrumbs and device faults.

## Metal

| rhi | Metal |
| --- | --- |
| instance, device | `MTLCreateSystemDefaultDevice`, `MTLCopyAllDevices` |
| queues, timeline semaphores | `MTLCommandQueue` (`MTL4CommandQueue` later), the async compute queue a second one; `MTLSharedEvent` signal values, waited on the cpu (`waitUntilSignaledValue`) and the gpu (`encodeWait`) |
| command buffers, the encoder | `MTLCommandBuffer` and its encoders: the backend ends and begins render, compute, blit and acceleration structure encoders as the commands' kind changes; dynamic rendering is a render encoder from an `MTLRenderPassDescriptor` |
| secondary command buffers | parallel encoding: an `MTLParallelRenderCommandEncoder`'s sub-encoders, or command buffers committed in order (`enqueue`) |
| barriers, layouts | none with hazard tracked resources (the default, first); untracked resources with `MTLFence`s or MTL4's barriers later. Image layouts and queue family ownership: no-ops |
| descriptor sets, pools | argument buffers (tier 2 on Apple silicon): a set is a buffer of `gpuResourceID`s and `gpuAddress`es at the offsets slang's reflection gives; resources resident through an `MTLResidencySet` |
| push constants, push descriptors | `setBytes` (up to 4 KB); a per dispatch argument buffer from a ring |
| specialization constants | function constants (`MTLFunctionConstantValues`) |
| shaders, pipelines, cache | MSL from slang, compiled at run time (`newLibraryWithSource`) and precompiled for installs; `MTLRenderPipelineState`, `MTLComputePipelineState`, `MTLDepthStencilState`; `MTLBinaryArchive` as the cache |
| dynamic state | the render encoder's cull mode, winding, viewport, scissor and stencil reference |
| indirect draws with a count | none: the existing exception (`drawIndirectCountEmulated`, KosmicKrisp's today), or indirect command buffers later |
| acceleration structures | `MTLAccelerationStructure` built by an acceleration structure encoder, compacted; instances in Metal's layout (`MTLIndirectAccelerationStructureInstanceDescriptor`), not Vulkan's 64 bytes (`rhi::AccelerationStructureInstance`, which gfx writes): the backend converts them |
| timestamps | counter sample buffers, sampled at encoder boundaries on Apple gpus (as KosmicKrisp's timestamps miss compute passes, see CLAUDE.md's measuring) |
| swapchain | `CAMetalLayer` on glfw's window (`nextDrawable`, `displaySyncEnabled` for fifo or immediate, `maximumDrawableCount`); present wait from the drawable's presented handler |
| crash reports | command buffer errors with encoder execution status (which encoder faulted: the breadcrumbs); no fault addresses |
| validation, names | Metal's api and shader validation (`MTL_DEBUG_LAYER`, `MTL_SHADER_VALIDATION`); `label`s |
| memory | buffers and textures from the device or `MTLHeap`s, shared or private storage (unified memory: uploads through shared buffers); no VMA |

The Metal api from C++: metal-cpp (Apple's header only C++ wrapper, `MTL::Buffer*` handles that gfx's translation units can name, the whole backend in the project's C++ toolchain) or Objective-C++ (no dependency, but the project enables C and C++ only, and the handles in `BackendTypes` would be opaque pointers bridged in `.mm` files). vcpkg's registry has no metal-cpp: it would be an overlay port (a download of Apple's headers, nothing built). Recommended: metal-cpp; the user's call, early, since the handles' types follow from it.

## D3D12

| rhi | D3D12 |
| --- | --- |
| instance, device | DXGI factory and adapters, `D3D12CreateDevice`; shader model 6.6 (descriptor heap indexing, wave intrinsics, 64 bit atomics not needed), resource binding tier 3 (unbounded, partially bound tables) |
| queues, timeline semaphores | direct and compute command queues; `ID3D12Fence` values (a timeline semaphore's model) |
| command buffers | command allocators and lists; secondary command buffers as lists recorded in parallel and executed in order (bundles can't change render targets or barrier) |
| barriers, layouts | enhanced barriers (sync, access and layout: synchronization2's model); queue family ownership: none (state promotion and decay) |
| descriptor sets, pools | ranges of a shader visible CBV/SRV/UAV heap and a sampler heap (2048 samplers; the renderer has 64), copied from cpu heaps (`CopyDescriptorsSimple`, the update templates' role); a root signature of one descriptor table per set |
| push constants, push descriptors | root constants (128 bytes is 32 of the root signature's 64 dwords, with 8 tables 40); root descriptors for buffers, a per dispatch table from a ring for images |
| specialization constants | none: a define per variant, or a root constant |
| shaders, pipelines, cache | DXIL from slang through DXC (vcpkg's `directx-dxc`); pipeline state objects; `ID3D12PipelineLibrary` |
| indirect draws with a count | `ExecuteIndirect` with a count buffer |
| acceleration structures, ray queries | DXR 1.1 inline ray queries; instances in the same 64 byte layout as Vulkan's (`D3D12_RAYTRACING_INSTANCE_DESC`) |
| timestamps | timestamp queries resolved to a buffer |
| swapchain | DXGI flip model; present wait from the frame latency waitable object; exclusive fullscreen (`VK_EXT_full_screen_exclusive`'s role) through DXGI |
| crash reports | DRED: automatic breadcrumbs and page fault data (the breadcrumbs and device faults) |
| validation | the debug layer and gpu based validation; PIX markers |
| memory | D3D12MA (vcpkg's `d3d12-memory-allocator`, 3.3.0), VMA's sibling |
| headless and CI | WARP, Microsoft's cpu driver, lavapipe's role on Windows runners |

Needs: `directx-headers` (in vcpkg, the mesa port's dependency already), `directx-dxc`, `d3d12-memory-allocator`, and for enhanced barriers on older Windows the Agility SDK (not in vcpkg's registry: an overlay port, or Windows 11 24H2's runtime as the minimum). Nothing blocks it; it follows Metal, and the binding work below serves both.

## Exceptions, by "portable by default"

Each named in rhi for what the platform lacks, as `drawIndirectCountEmulated` is: Metal has no indirect draws with a count (the existing exception), timestamps only at encoder boundaries, no secondary command buffers (parallel encoding instead) and its own acceleration structure instance layout; D3D12 has no specialization constants and no push descriptors for images. None blocks: no feature the renderer needs is missing on either.

## The plan (Metal)

1. Portable shader fixes, checked on Vulkan: `WavePrefixSum(1u)` for `WavePrefixCountBits(true)`; a semantic on `VertexOutput.vertex` for slang's crash.
2. Parameter blocks, one per descriptor set, in every layout's shaders; the reflection names a block's fields as today's globals. Vulkan's sets and bindings unchanged (quick tier, visual checks); then the Metal check over every entry point again, and the HLSL one with `-parameter-blocks-use-register-spaces`.
3. The backend chosen at build time: `kMetal` beside `kVk`, a preset option, `RHI_SOURCE_FILES` per backend, `BackendTypes<kMetal>`, the backend's files compiling with every function failing at run time (the null backend's compile time proof, as scaffolding), and metal-cpp's port (or Objective-C++, the user's call).
4. Device, queues and timelines, buffers and images, shaders (MSL compiled at run time), compute pipelines and argument buffers: the environment filter's dispatches.
5. Render pipelines and passes, the headless output: a captured frame, compared with KosmicKrisp's (`imagecompare`).
6. The swapchain (`CAMetalLayer`), present modes, the ImGui renderer.
7. Acceleration structures and ray queries (opt in), timestamps, crash reports, residency sets.
8. The quick tier and the visual checks on Metal; frame times against KosmicKrisp's.

## Progress

- 2026-11-13: the shader check and the rhi survey above; no blockers. Branch `metal-backend`.
