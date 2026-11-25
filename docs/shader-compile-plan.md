# Faster shader compiles: the plan

Status: steps 0 to 4 and 6 are done, step 5 was dropped by its gate. The goal was to cut the time a cold start spends compiling shaders after an edit, which was most of an iteration on Vulkan and several times that on D3D12. The compile now caches per entry point, runs in parallel worker processes, and a running client reloads shaders when their sources change. Each step had a gate and was measured before and after, with the runs compared interleaved (CLAUDE.md, "Workflow").

## Why

`shaders.slang` is one translation unit (its parts are included textually after `capi.h`), and a layout's entry points (about 80 in the main layout) were one compile request, so any edit regenerated code for all of them. Slang is not thread safe, so only one compile ran at a time. D3D12 compiled more still: a SPIR-V target for reflection as well as DXIL, and the entry points that read the specialization constant compiled again through DXC for each value. Every profile also kept its own cache. Background: docs/notes/layering.md, "The shaders compile in the background".

## The design

- **Keys**: slang's own entry point hash covers every file the module includes, so it changes for all entry points on any edit. The key is instead the hash of each entry point's HLSL source as slang generates it from the linked program, which holds everything the entry point reaches with the definitions applied (and, with debug information, its line numbers). A source that names `kSkippedLayers` marks an entry point that reads the specialization constant, which DXIL compiles once per value.
- **Workers**: a layout compiles in two rounds of worker processes (`shadercompileworker`, beside the client, a small executable around the shader compiler library, started with `core::ChildProcess`). Round one computes the keys (and the reflection, in the first worker); round two compiles the jobs the cache does not hold, divided by the cost each took last time and longest first. The compile task waits for the workers on its own thread.
- **The cache**: each job's code lives in `shader-cache` beside the executable, named by a hash of its key, its specialization and the configuration (slang's version, format, definitions, levels; not the entry point list). Files are written beside and renamed into place. All profiles of a build share it, a profile's whole-layout cache stays in front of it, and an install's precompiled sets in front of that.
- **Pruning**: the client marks an entry used when it takes one (file systems' access times are unreliable), and `scripts/prune-shader-cache.ps1` removes entries unused for a maximum age, then the least recently used past a maximum size, plus what a killed client left behind. CI's kept cache is pruned the same way (docs/notes/build.md, "CI").
- **Worker count**: `SPEEDO_SHADER_WORKER_COUNT` (default half the machine's threads, at most 8); `0` compiles in the client's own process as before, with no entry point cache.
- **Timings**: `SPEEDO_SHADER_TIMINGS=1` prints each compile's phases and each scene layout's load.
- **Reload**: in a checkout, the window's scene (`Scene::WatchShaders`; off with `SPEEDO_SHADER_RELOAD=0`) fingerprints the shader directory every half second between frames and, on a change, compiles its layouts again on the executor through the workers (`ShaderLoader::TryLoad`). A failed compile is reported and the old shaders kept. Resources that differ from the loaded layout's (bindings, push constants) need a restart, which is said, and the shaders are kept. Otherwise, once no pipeline is being made, the graphics queues are waited for and `Pipeline::ReplaceShaders` (`PipelineLayout::ReplaceShaders` per backend) gives each layout new shader modules, keeping layouts and descriptor sets, and destroys every pipeline to be made again. The ui shows a reload's progress and a failed compile's diagnostics, the ImGui renderer reloads with the scene (`ImGuiRenderer::ReplaceShaders`), and the file browser's preview follows the window's reloads (`Scene::FollowShaders`).
- **What is unchanged**: the binaries an install ships (precompiled, full optimization, no debug information in release), the layouts' contents, and the rule that captures compared against each other run the same shaders. SPIR-V from the workers is byte identical to a single process compile; DXIL differs, because DXC now compiles each entry point's HLSL from a program of that entry point alone.

## Steps

### 0. Measure where the time goes (no behavior change)

Done. `SPEEDO_SHADER_TIMINGS=1` times each compile phase per layout. It showed the time is code generation entry point by entry point (the front end is negligible), that on D3D12 the SPIR-V target and the specializations cost more than DXIL alone, and that slang's entry point hash cannot key a cache.

### 1 to 4. Entry points keyed, cached and compiled in worker processes

Done, as one change since the steps share the protocol and the cache: the keys (step 1), the entry point cache (step 1), parallel compiles in workers (step 2), the specializations as separate cached jobs (step 3), and one cache shared by every profile (step 4). A cold start dropped to a fraction of the old time on both APIs, and a changed constant restarts in about the time of compiling its few entry points. Worker count scales until the slowest entry point bounds it. A comment added to a shader recompiles the entry points after it when debug information is on, since their debug information names lines.

### 5. A faster configuration for iterating

Dropped by its gate: neither lower optimization nor dropping debug information made the code generation faster.

### 6. Reload shaders while the client runs

Done, as described above. Not done: a reload waits for the graphics queues rather than retiring the old pipelines and modules, a short hitch once per reload that was judged not worth changing rhi for.

## Also worth looking at

- **Splitting `shaders.slang` into slang modules** (`import`, each part checked and serialized once): not done, since the front end is a small share of a compile and a module cache would key nothing better than the HLSL source does, nor shorten code generation. Slang's per module precompiled code could, but gives up optimizing across modules and is experimental; not tried.
- **Slang's version**: newer releases may compile faster; measure when the port is next updated.
- **Pipeline creation**: the driver's pipeline cache is now the build's (`driver-cache`), round one's results are kept by the sources' contents, and the graphics pipelines a run asked for are asked for ahead at the next start (docs/notes/layering.md).
