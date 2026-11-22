# Faster shader compiles: the plan

Status: written 2026-11-17; started 2026-11-17 (the user's call: before the plan's step 3 on the probes, since the
compiles slow down all other work). Steps 0 to 4 and 6 done, 5 dropped by its gate. Each step has a gate, and each is measured before and after on the
same machine, interleaving the runs compared (CLAUDE.md, "Workflow").

## Why

A cold start after an edit to one shader part takes, on the RTX 4070 Ti SUPER's machine (2026-11-17, one edit to
`gi.slang`, salle_de_bain autoloaded, 300 frames): **69 s on Vulkan** (the compile 52 s) and **175 s on D3D12**, 3 s
on either once cached. Shader work is iterated on Vulkan now (the default on Windows since 2026-11-17), but a minute per
edit is still most of an iteration, and every D3D12 check before a commit costs three.

## What it costs now, and why (see docs/notes/layering.md, "The shaders compile in the background")

- **One compile of everything.** `shaders.slang` is one translation unit (its parts included textually after `capi.h`),
  and the main layout's ~80 entry points are one compile request: any edit parses, checks and generates code for all of
  them. The cache is a layout's whole set, keyed by its configuration and its sources.
- **One thread.** Slang isn't thread safe, even across global sessions (the embedded SPIR-V grammar's reference count:
  2 of 59 clients crashed in `~Session` when the main layout compiled in 4 groups at once), so a `ShaderLoader` is the
  only one alive and compiles run in turn. The parallel version was 17.5 s against 28.8 (M4 Pro), at 2.0 GB resident
  against 0.85.
- **D3D12 compiles more.** The request has a SPIR-V target for reflection as well as DXIL (DXC behind slang), and the
  entry points that read the specialization constant are linked and compiled again through DXC for each value
  (`CompileSpecializations`). Its cold compute pipelines took 9.6 s, and `FragmentMain` 10.8 s against Vulkan's 4.4.
- **Everything at full optimization, with full debug information in profile builds** (`kShaderDebugInfoLevel`, for gpu
  captures; the main layout was 81 MB a variant with it, 3.8 MB without).
- **Each profile its own cache.** Scratch profiles, test runs and the user's profile each compile the same sources again.

## Steps

### 0. Measure where the time goes (no behavior change)

Time each phase of a cold compile into the shader log, per layout: the front end (parse and check), each entry point's
code generation per target, DXC per entry point, the specializations, the driver's pipeline creation, and the scene's
load and settle around them. Run once per api.

Gate: a table of the phases for Vulkan and D3D12 that adds up to the measured cold start, and the D3D12 difference
attributed. The order of the steps below may change with it.

**Done (2026-11-17).** `SPEEDO_SHADER_TIMINGS=1` prints each compile's phases and each scene layout's load, and for a
layout of more than 8 entry points first measures it through slang's component api (its front end alone, then each
entry point linked with the module, hashed and its code generated, timed; `MeasureEntryPoints`: it compiles everything
twice, for measuring only). Slang's own `-report-perf-benchmark` printed nothing (it writes to a writer the compiler
doesn't set). A cold start (a fresh profile, salle_de_bain autoloaded, 300 frames, headless, RTX 4070 Ti SUPER):

| | Vulkan | D3D12 |
|---|---|---|
| the whole start | 70 s | 213 s (175 s with an edit, its pipeline cache warm) |
| the main layout (79 entry points) | 51.4 s: one `spCompile` | 207 s: `spCompile` 87.3 s (SPIR-V and DXIL), the specializations 82.2 s |
| its front end alone (parse, check) | 0.67 s | 0.67 s |
| its entry points' code, one at a time | 48.5 s (sum) | 35.9 s (DXIL alone, sum) |
| the slowest entry points' code | FragmentMain 8.1, ComputeMainVisibility 7.9, FragmentTransparent 7.9, FragmentScatter 6.3, ComputeGiResolve 3.3, ComputeGiResolveTraced 3.2, ComputeReflectionTrace 1.7 s | 6.7, 6.5, 5.8, 3.3, 2.2, 2.1, 1.6 s |
| the other three layouts | 1.2 s | 1.6 s |
| the compute pipelines made ahead | 10.3 s (cold driver cache) | 0.45 s (warm) |

So the time is the code generation, entry point by entry point, and the front end is negligible: compiling only what
changed (step 1) and in parallel (step 2, bounded below by the slowest entry point, about 8 s) are both worth it. On
D3D12, the SPIR-V target costs as much as Vulkan's whole compile (87.3 against 35.9 for DXIL alone), and the
specializations (5 entry points by 4 values, each linked and compiled through DXC: about 4 s each) more again: step 3.
The driver's pipeline creation is the next largest part on a cold driver cache (Vulkan's 10 s; a shader edit makes only
the changed entry points' pipelines cold).

Measured further through `shaderprecompile` (CPU only, the release build, SPIR-V; the quick tier running meanwhile, so
slower than a quiet machine): the layout without ray queries 15.0 s of code (unoptimized the same, 15.0; with full debug
information 15.6, so neither the optimization nor the debug information is what costs), with ray queries (the client's)
31.0 s, FragmentMain 5.8 s; HLSL source of every entry point 3.6 to 3.9 s (with debug information 4.9).
- **Slang's entry point hash can't key a cache here** (`getEntryPointHash`): it hashes the files the entry point depends
  on, and every entry point depends on every part (one translation unit): a comment added to `gi.slang` changed all 79
  hashes (and none changed from run to run). Step 1 needs a key of its own: each entry point's HLSL source, which slang
  makes from the linked program in a tenth of the code's time, holds everything the entry point reaches, with the
  preprocessor's definitions applied.

### 1 to 4. Entry points keyed, cached and compiled in worker processes

**Done (2026-11-18)**, as one change: the steps share the protocol and the cache. A layout compiles in two rounds of
worker processes (`shadercompileworker`, beside the client: the shader compiler library behind a small executable,
`<request file> <response file>`; `core::ChildProcess` starts them, CreateProcessW or posix_spawn, and waits):

- **Round one, the keys** (`shadercompiler::Action::kKeys`): each worker a share of the entry points (at least 4 each:
  a front end costs half a second), the first one the reflection too (the resources, slots or registers, the included
  files). A worker runs the front end alone (no code), then links each entry point on its own and has slang make its
  HLSL source, whose hash is the entry point's key (`EntryKey`): slang's own entry point hash covers every file the
  module includes, so a comment in `gi.slang` changed all 79 (step 0); the HLSL source holds everything the entry point
  reaches, with the definitions applied and, with debug information, its lines. Whether that source names
  `kSkippedLayers` says whether the entry point reads the specialization constant, which DXIL compiles per value.
- **The cache**: each job's code (an entry point's, or for DXIL a specialization's) in `shader-cache` beside the
  executable, named by a hash of its key, its specialization and the configuration (slang's version, the format, the
  definitions, the levels; not the entry point list, so adding an entry point keeps the others), written beside and
  renamed into place (another process never reads half of one). Shared by every profile of the build (step 4); the
  profile's whole-layout cache (`core::file::LoadAsset`) stays in front of it, and an install's precompiled sets in
  front of that.
- **Round two, the code** (`Action::kCode`): the jobs the cache doesn't hold, divided between the workers by what each
  took the last time (`costs-<format>` in the cache; else by its source's size), longest first to the least loaded; a
  worker makes a front end (a second one, with the constant a link-time one, for DXIL's specialized jobs) and links
  each entry point on its own for its code, as `CompileSpecializations` did for the specializations.
- `SPEEDO_SHADER_WORKER_COUNT` (default half the machine's threads, at most 8): `0` compiles in the client's own process
  as before (the library loaded there, one request for the layout, no entry point cache); otherwise the client loads no
  slang at all.
- The compile task waits for the workers holding its thread, as the compile in its process held it for longer (the
  plan asked for a wait that doesn't; the executor has no means to wait on a process, and the thread is idle meanwhile).

Measured (RTX 4070 Ti SUPER's machine, 32 threads, salle_de_bain autoloaded, 300 frames, headless):

| | before | after |
|---|---|---|
| Vulkan, a fresh profile, the cache empty | 70 s (the main layout 51.4) | 38.8 s (16.2) |
| D3D12, the same | 213 s (207) | 31.8 s (23.6) |
| Vulkan, a fresh profile, the cache warm (another profile compiled it) | 70 s | 21.9 s (the keys 2.6 s; the pipelines 10 s: a fresh profile's pipeline cache is empty) |
| D3D12, the same | 175 to 213 s | 8.1 s |
| Vulkan, the same profile, a constant in `gi.slang` changed | about 69 s | 13.7 s: 3 entry points compiled (4.6 s after 2.6 of keys), their pipelines 3 s |
| Vulkan, the same profile, a comment line added to `gi.slang` | about 69 s | 30.1 s: the 8 entry points whose lines after it moved (their debug information names lines), the four heaviest at 9 s each at once, their pipelines 9 s |
| Vulkan, the same profile, nothing changed | 3 s | 2.5 s |

The workers against their count (Vulkan, the cache empty, each run divided by the sources' sizes): 1: 77.8 s (the main
layout 55.7), peak 0.84 GB; 2: 53.9 (31.7), 1.5 GB; 4: 41.6 (19.4), 3.0 GB; 8: 38.8 (16.2), 4.8 GB; 12: 41.1 (16.1),
6.3 GB. Past 8 the slowest entry point bounds it (FragmentMain about 9 s with the others running).

The binaries: SPIR-V's byte identical to the compile in one process (every set `shaderprecompile` writes: the main
layout's 95 MB with and without ray queries), and a repeatable capture the same to the noise between two runs (0.40 of
255 mean, as between runs of one). DXIL's differ from the first entry point on (DXC compiles each entry point's HLSL as
slang makes it from a program of that entry point alone), and so does the frame: on D3D12 the old compile's frame was
5.9 of 255 from the new one's, 10.8 from Vulkan's, and the new one's 4.7 from Vulkan's; the old one's showed a pattern
over the window that neither the new one nor Vulkan's has. Not explained further.

What a comment costs is the debug information's: an entry point's source names its lines, so an edit that moves lines
recompiles what is after it (correctly: its debug information would be wrong otherwise). Without debug information
(release) it wouldn't.

### 5. A faster configuration for iterating

**Dropped (2026-11-18)**, by its gate: neither costs. Through `shaderprecompile` (SPIR-V, the layout without ray queries)
the code took 15.0 s optimized and 15.0 unoptimized, 15.6 with full debug information; with ray queries 31.0 and 31.8.

### 6. Reload shaders while the client runs

**Done (2026-11-18).** The window's scene (`Scene::WatchShaders`, in a checkout: where `shaders.slang` is; off with
`SPEEDO_SHADER_RELOAD=0`) takes the shader directory's fingerprint (its files' names, times and sizes) every half
second between frames (in `Scene::Prepare`, the draw thread); on a change it compiles its four layouts again on the
executor (`ShaderLoader::TryLoad`: through the workers, the unchanged entry points from the cache; a failed compile
gives its diagnostics rather than trapping). Once they have:
- a failed compile is printed (the diagnostics to the error output) and the shaders are kept;
- resources that differ from the layout's as loaded (bindings, push constants: compared serialized) need a restart,
  said so, and the shaders are kept: what is bound through them would change;
- otherwise, once no pipeline is being made (the ones made ahead done; else again next frame), the graphics queues are
  waited for under their lock and `Pipeline::ReplaceShaders` gives each layout new shader modules
  (`PipelineLayout::ReplaceShaders`, each backend's: the pipeline layout, its descriptor set layouts and push constants
  kept, so every descriptor written stays) and destroys every pipeline (`InternalDestroyPipelines`, which the
  destructors use too: they are made again as asked for, the pipeline cache giving back what didn't change); the
  compute pipelines are asked for ahead again. The scene, its views and the indirect lighting's state are kept.

Measured (RTX 4070 Ti SUPER's machine, salle_de_bain, headless): a constant changed in `gi.slang` on screen 12.3 s after
the edit on Vulkan (its 3 entry points compiled, their pipelines made), 7.6 s on D3D12; a syntax error reported 1.1 to
1.3 s after it, the client running on; put back, 4.2 to 4.5 s (all from the cache). Validation clean (Vulkan's layer,
D3D12's debug layer). The tonemapper's input quartered while running: the frame 69.7 of 255 from the one before (62.0 on
D3D12) and 0.35 (0.11) from a client started with the edit, the noise between runs. Thirty reloads in a release client:
private memory 2.56 GB ready, 2.71 after two, about 2.85 from the eighth on (2.79 to 2.89). The Metal backend's
`ReplaceShaders` and `InternalDestroyPipelines` are written as its constructor and destructor have them, not built
(Windows).

Since (2026-11-19): the ui shows a reload (a row in the loading overlay while it compiles, then for 3 s how long it
took; a failed compile's diagnostics, each error once with the layouts that met it, or a refusal, in a window with a
Copy button until closed or the next reload starts: `Scene::GetShaderReloadStatus`), the ImGui renderer reloads (its
layout compiled with the scene's, one compile at a time, `Scene::AddReloadedLayout`, and replaced between frames,
`ImGuiRenderer::ReplaceShaders`), and the file browser's preview takes the window's reloads (`Scene::FollowShaders`).
Measured headless with the ui drawn, ShadowScene under Vulkan's validation layer: `gi.slang` broken, the window up 1.3 s
after the edit; put back, reloaded 3.9 s after it was seen; `imgui.slang` tinted red, the ui red 0.5 s after; no
validation message. The preview's reload works (the user, 2026-11-19, in a window with the file browser).

Not done, by its measure: a reload waits for the graphics queues before it replaces the shaders, rather than retiring
the old pipelines and modules until the frames using them are done. Bistro's (2026-11-19): the wait 13.8 to 14.1 ms, the
replacement after it 52.7 to 57.4 ms (the pipelines destroyed, the shader modules made again), so a reload is a hitch of
about 70 ms once, after a compile of seconds; retiring rather than waiting would take a fifth of it, and the rest only
by retiring the destruction too, in `Pipeline` and each backend's `PipelineLayout`. Not worth rhi's change for a
checkout's reload.

## Also worth looking at

- **Splitting `shaders.slang` into slang modules** (`import`, each part checked and serialized once: `IModule::serialize`,
  `isBinaryModuleUpToDate`), so an edit re-checks its part and what imports it rather than the whole unit. Large (the
  parts use each other's declarations in any order today), and step 1 may make it unnecessary: only if step 0 shows
  the front end is a large share. **Not done (2026-11-18), by that gate**: the front end is 0.67 s of the main layout's
  51.4 (step 0). Since steps 1 to 4 each worker runs it once a round, at once, so it is about 1.3 s of a changed
  constant's 13.7 s restart, and a module cache would save less than that (an edit re-checks its module and what
  imports it, the main part importing all). It would key nothing better than the HLSL source does (slang's entry point
  hash would still cover every module an entry point imports), nor shorten the code generation (each entry point is
  still linked and generated whole), nor the debug lines' recompiles (a line moved in a file moves it in a module too).
  Slang's code precompiled per module (`precompileForTarget`) could, but gives up optimizing across modules and is
  experimental in slang; not tried.
- **Slang's version**: newer releases may compile faster; measure when the port is next updated.
- **Pipeline creation**: the driver's own caches already keep what didn't change; check after step 1 that only the
  changed entry points' pipelines are cold. **Looked at (2026-11-22)**, as a capture's time went (the client's run
  timings, `SPEEDO_RUN_TIMINGS`): NVIDIA's Vulkan cache had filled at 1 GB and kept nothing new, so every profile made a
  changed shader's pipelines cold (each GI experiment slot 30 to 45 s a variant); round one ran again on every start
  after a swap of variants (4.3 s); and the graphics pipelines waited for the compute ones. Now the driver's cache is the
  build's (`driver-cache`, 10 GB), round one's results are kept by the sources' contents, and the graphics pipelines a
  run's passes asked for are asked for ahead at the next start: a new variant's run in a second slot 2.8 s rather than
  45.5, a swap back 2.6 rather than 6.3, the first slot's pipelines about 7 s sooner (docs/notes/layering.md).

## Not to change

The binaries an install ships (precompiled, full optimization, no debug information in release), the layouts' contents,
and the rule that a capture compared against another runs the same shaders.
