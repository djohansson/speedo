# Backends plan: Metal, then D3D12

speedo's renderer (gfx and the client) talks to the graphics api only through rhi, which is templated on a `GraphicsApi` and speaks its own vocabulary (`rhi/vocabulary.h`). This plan covers the second and third backends beside Vulkan's: Metal (macOS) and D3D12 (Windows). Both work: the client runs on each, headless and windowed, with ray queries, timestamps and crash reports. gfx and the client are compiled once per api in inline namespaces, and the api is chosen at run time by `SPEEDO_GRAPHICS_API` (Vulkan by default on Windows and Linux, Metal on macOS). The shaders are slang, compiled to SPIR-V, MSL and DXIL. Remaining: a few Metal leftovers (step 10) and D3D12's WARP gaps (step 7).

## Contents

- [The shaders on slang's Metal and HLSL targets](#the-shaders-on-slangs-metal-and-hlsl-targets)
- [What rhi asks of a backend](#what-rhi-asks-of-a-backend)
- [Exceptions, by "portable by default"](#exceptions-by-portable-by-default)
- [Metal](#metal)
- [D3D12](#d3d12)
- [The plan (Metal)](#the-plan-metal)
- [The plan (D3D12)](#the-plan-d3d12)

## The shaders on slang's Metal and HLSL targets

Every entry point of every layout the client loads compiles to MSL and to DXIL. `scripts/shadercheck.ps1 -Target metal|hlsl|dxil -Validate` checks them (the MSL through Metal's runtime compiler, `src/tools/macos/mslcheck.mm`; the DXIL through DXC), each entry point compiled alone.

- **Parameter blocks.** The shaders declare one `ParameterBlock<T>` per descriptor set (`bindings.slang`): a descriptor set on Vulkan, an argument buffer on Metal, a descriptor table in its own register space on D3D12 (`-parameter-blocks-use-register-spaces`). Fields keep their old names, so gfx still binds by name (`Pipeline::SetDescriptorData`). Slang ignores `[[vk::binding]]` inside a block (fields bind in declaration order), so declaration order is the binding order.
- **Buffer layout.** Slang lays out Metal's buffers packed, Vulkan's as std430. Structs that buffers hold pad explicitly in `capi.h`, and the shader compiler fails any compile where a buffer's layout is not std430 (`CheckPackedLayout`).
- **Slots and registers.** The shader compiler reads each target's own numbering (`AssignSlots` for Metal buffer indices, `AssignRegisters` for DXIL registers) because slang numbers per declaration order or per register class, not by set or binding.
- **Portable shader fixes.** `WavePrefixSum(1u)` replaces `WavePrefixCountBits(true)` (missing from slang's Metal target); every field of `VertexOutput` has a semantic (works around a slang crash on nested structs in Metal vertex outputs).
- **Specialization constant** (`kSkippedLayers`, `[vk::constant_id(0)]`): a Metal function constant; on D3D12 compiled per value (see D3D12).
- **Ray queries** come out as `metal::raytracing` intersection queries and as DXR 1.1 inline queries.

## What rhi asks of a backend

rhi's public headers are in its own vocabulary, and its class templates are generic in the backend `G`. `BackendTypes<G>` (`rhi/types.h`, one `types.inl` per backend) names the backend's types: gfx and the client use only handles, `DescriptorType` and `Flags`; the rest are the backend's structs kept as members. A backend instantiates the templates in `rhi/<backend>/instantiate.cpp`. The second backend proved the abstraction: generic code was cleared of Vulkan structs, and Device, Queue, Pipeline and the descriptor classes' shared halves moved into rhi's generic headers.

What the renderer needs, which a backend provides or has a named exception for: dynamic rendering, timeline semaphores, push descriptors and constants (at most 128 bytes), one specialization constant, descriptor sets (8 sets, 1024 non-uniformly indexed textures, 64 samplers), indirect draws with a count, dispatch indirect, buffer addresses, acceleration structures and ray queries (optional), timestamps, graphics and async compute queues, secondary command buffers, a swapchain with present wait, a pipeline cache, and crash reports with breadcrumbs.

`src/tools/rhitest.cpp` (kernels `rhitest*.slang`) tests rhi generic in the backend: compute, images, the environment kernels, draws, indirect draws and blits, compared against Vulkan.

## Exceptions, by "portable by default"

Each is named in rhi for what the platform lacks, as `drawIndirectCountEmulated` is. Metal has no indirect draws with a count, timestamps only at encoder boundaries, no secondary command buffers (inline encoding instead) and its own acceleration structure instance layout. D3D12 has no specialization constants and no push descriptors for images. None blocks: no feature the renderer needs is missing on either.

## Metal

Sources: `rhi/metal`, using metal-cpp (Apple's header-only C++ wrapper, overlay port `ports/metal-cpp`), so the project stays C and C++.

| rhi | Metal |
| --- | --- |
| queues, timelines | one `MTLCommandQueue` per queue type (graphics, compute, transfer), each with its own timeline and gpu profiler; timelines are `MTLSharedEvent`s (a wait on another queue's timeline is encoded in a command buffer committed before the work) |
| command buffers | one-shot: a pool's `Begin` makes one from its queue; the handle also holds the current encoder, ended and begun as the commands' kind changes |
| secondary command buffers | recorded inline into the primary's render encoder |
| barriers, layouts | hazard tracked resources: a barrier is the compute encoder's `memoryBarrier`; layouts and ownership transfers are no-ops |
| descriptor sets | tier 2 argument buffers: 8 byte fields in declaration order, 16 sets in one shared buffer, bound at the slot the shader compiler reads from the MSL layout; resources made resident at bind (`useResource`); pushed sets via `setBytes` |
| push constants | `setBytes`, padded to a multiple of 16 bytes |
| specialization constants | function constants |
| shaders | MSL, one library per entry point, compiled asynchronously at run time (the first pipeline waits for its library) and precompiled for installs; relaxed math |
| render passes | a render pass descriptor and the command buffer's render encoder; rects cleared inside a pass and scaled blits are draws (Metal clears no part of an attachment and its blit scales nothing) |
| indirect draws with a count | none: the existing exception (`DeviceLimits::drawIndirectCountEmulated`) |
| clip space | rhi's is Vulkan's (y down); the backend flips the viewport |
| acceleration structures | `MTLAccelerationStructure`s on an acceleration structure encoder; rhi's 64 byte instances converted by a kernel into Metal's layout just before each top level build; buffers registered by address (`metal::FindBuffer`) because Metal takes a buffer and offset |
| timestamps | counter samples at encoder boundaries only (Apple gpus), in nanoseconds on the cpu clock |
| swapchain | a `CAMetalLayer` set on glfw's view (`PlatformCreateMetalLayer`); the swapchain's own images are copied into each drawable at present; present wait from the drawable's presented handler |
| crash reports | command buffer errors with encoder execution status; breadcrumbs as encoder labels and debug groups; no fault addresses |
| validation | `MTL_DEBUG_LAYER` (set by `-Validate`), `MTL_SHADER_VALIDATION` by hand |

Notes:

- Ray queries are opt in on macOS because Apple's gpu restarts under Metal ray tracing beside other gpu work (`SPEEDO_RAY_QUERY=1`; see docs/notes/indirect-lighting.md). The ray query switch is made while running, with the scene choosing the shader variant.
- Formats Metal lacks fail hard (24 bit color, depth 24 stencil 8), as do texel buffers, inline uniform blocks, dynamic offsets and combined image samplers until something needs them.
- The graphics api is chosen at run time in one binary (see the overview); docs/notes/build.md has the build's side.

## D3D12

Sources: `rhi/d3d12`, built on Windows. Dependencies: `directx-headers`, `directx-dxc`, `d3d12-memory-allocator`. The device requires shader model 6.8, enhanced barriers and resource binding tier 3, and fails hard naming what is missing.

| rhi | D3D12 |
| --- | --- |
| queues, timelines | direct and compute queues, `ID3D12Fence` timelines; the queue ring has one queue per family |
| command buffers | a command allocator and list each; a pool waits for its lists before resetting its allocators; secondary command buffers record into the primary's list |
| barriers, layouts | enhanced barriers; images track their D3D12 layout; Vulkan's general layout is the queue's common layout |
| descriptor sets | ranges of the device's shader visible CBV/SRV/UAV and sampler heaps, one root descriptor table per set; sampler tables are shared by content |
| push constants | root constants at an explicit register (`b0`) |
| specialization constants | none: each value compiled as its own binary, linked per value (`SHADER_SPECIALIZATION_CONSTANT_LINKED`), the backend's pipeline taking its variant's binary |
| shaders | DXIL from slang through DXC at shader model 6.8; `SHADER_TARGET_NO_POINT_SIZE` and `SHADER_TARGET_CLIP_Y_UP` defined for it |
| render passes | render targets' views set at `Begin`; rect clears are `ClearRenderTargetView`'s; scaled blits (and mip generation) are draws with HLSL compiled by `d3dcompiler_47` |
| indirect draws | `ExecuteIndirect` with a count buffer, so no exception; indirect commands stay Vulkan's layout (6.8's `SV_StartInstanceLocation`) |
| pipeline state | a state per cull mode and front face, made at first draw (D3D12 keeps them in the pipeline state); cache is an `ID3D12PipelineLibrary1` (`pipeline.d3d12.cache`) |
| clip space | flipped in the vertex stage (`ClipSpace`, `gfx/shaders/clip.h`), since D3D12's viewport cannot flip |
| acceleration structures | DXR 1.1 inline ray queries; rhi's instances are already D3D12's layout |
| timestamps | query heaps resolved into readback buffers; direct and compute queues profiled |
| swapchain | DXGI flip model on the window's HWND; own images copied into DXGI's buffers at present (they take no unordered access); fifo, mailbox and immediate (tearing where allowed); present wait on a swapchain fence; alt+enter left to the application |
| crash reports | DRED breadcrumbs and page faults, on unless `SPEEDO_D3D12_DRED=0` |
| validation | debug layer (`SPEEDO_D3D12_VALIDATION`, set by `-Validate`), GPU based validation (`SPEEDO_D3D12_GPU_VALIDATION`) |
| memory | D3D12MA; host visible buffers the shaders read use the GPU upload heap where the system has it, staging and readback buffers stay in system memory |
| headless, CI | WARP (`SPEEDO_D3D12_WARP`) |

Notes:

- Barriers from and to the common layout wait for all of the list's work, and a global barrier's sync and access must suit the list's queue type.
- **WARP from Microsoft's NuGet package.** Windows' WARP crashes in the client with ray queries; the NuGet package's WARP (`scripts/fetch-warp.ps1`, testing only, never shipped) runs the client but fails on a bug that the shader compiler works around by passing DXC `-opt-disable sink -opt-disable gvn` (`gfx::shadercompiler::kDxcOptions`).
- Vulkan is the default on Windows; D3D12 is chosen by `SPEEDO_GRAPHICS_API=d3d12`, and the quick tier runs both.

## The plan (Metal)

1. Done: portable shader fixes (`WavePrefixSum`, semantics on `VertexOutput`), checked on Vulkan.
2. Done: a parameter block per descriptor set in every layout; all entry points pass Metal's compiler and the HLSL check.
3. Done: the backend beside Vulkan (`kMetal`, `BackendTypes<kMetal>`, `ports/metal-cpp`) and rhitest's first dispatch.
4. Done: device, queues, timelines, buffers, images, shaders, compute pipelines and argument buffers; the environment kernels match Vulkan's.
5. Done: render pipelines and passes, draws, indirect draws (issued per command from the host), rect clears.
6. Done: the swapchain, present modes, the ImGui renderer.
7. Done: acceleration structures and ray queries (opt in), timestamps and crash reports. Residency sets remain open (see step 10).
8. Done: the quick tier and the visual checks on Metal; frame times against KosmicKrisp (Metal faster).
9. Done: both backends in one binary, chosen at run time; gfx and the client compiled once per api in inline namespaces (`gfx::vk`, `gfx::metal`), with header-only libraries' implementations in `gfx/libraries.cpp`.
10. Leftovers. Done: crash breadcrumbs and reports, present wait, scaled blits, asynchronous shader library compiles, dedicated compute and transfer queues, `-Validate` on Metal (api validation), installs' MSL precompiled by `shaderprecompile`. Open: indirect command buffers (indirect draws and a count issued from the gpu in one call, which would also remove the `drawIndirectCountEmulated` exception); residency sets (`MTLResidencySet` on the queue in place of `useResource` at each draw and dispatch); the pipeline cache (`MTLBinaryArchive`); MTL4 command queues and barriers; Metal's shader validation in `-Validate`.

## The plan (D3D12)

The steps are Metal's, with Metal's design as the model where the apis agree.

1. Done: the shaders as DXIL (`rhi::ShaderFormat::kDxil`); all entry points pass DXC's validation.
2. Done: the backend (`kD3D12`, `rhi/d3d12/instantiate.cpp`) with rhitest's compute and image checks, matching Vulkan.
3. Done: render pipelines and passes, `ExecuteIndirect` with a count, rect clears and scaled blits; rhitest's draws match Vulkan's bit for bit.
4. Done: the client on D3D12 headless (`SPEEDO_GRAPHICS_API=d3d12`, gfx and the client compiled a third time), captures matching Vulkan's at manual exposure.
5. Done: the swapchain, present modes, present wait and the ImGui renderer; the presentation test passes.
6. Done: timestamps, DRED crash reports, acceleration structures and DXR ray queries.
7. Mostly done: the quick tier passes on D3D12 with the debug layer (and runs on each api by default), the visual checks and frame times match Vulkan's at parity. Open: CI on WARP (Windows' WARP crashes with ray queries; the NuGet WARP's visual checks pass except the furnace, whose emitters' light is short on WARP alone, not yet explained), and untried: the pipeline library's gain on a driver without its own cache, DRED on a real fault.
