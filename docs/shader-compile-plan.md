# Faster shader compiles: the plan

Status: written 2026-11-17; started 2026-11-17 (the user's call: before the plan's step 3 on the probes, since the
compiles slow down all other work). Step 0 done. Each step has a gate, and each is measured before and after on the
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

### 1. Compile only the entry points an edit changed

Slang computes a hash per entry point of everything it depends on, with the target's settings
(`IComponentType::getEntryPointHash`, meant for shader caches). Run the front end once, hash every entry point, and
generate code only for those whose hash isn't in the cache; the set is assembled from cached and new binaries.
Reflection still comes from the whole program. An edit to `gi.slang` then recompiles the entry points that reach the
indirect lighting, not the tonemapper's or the ui's.

- To verify first: that the hash is stable from run to run and changes only with what the entry point uses (a comment
  edit in an unrelated part keeps it), and that the hash call itself doesn't generate code.
- Applies to the specializations too: a DXIL specialization is cached by its entry point's hash and its value.

Gate: an edit to one part recompiles a fraction of the entry points (reported in the log), the binaries are byte
identical to a full compile's, repeatable captures are identical, and the cold start after a `gi.slang` edit is
measured on both apis.

### 2. Compile in parallel, in processes

Slang can't run on several threads, but it can run in several processes. A `shadercompile` tool (the shader compiler
library behind a small executable) takes a request and writes its result, so `ShaderLoader` hands groups of entry
points to N workers at once and merges what they return. Each worker parses the module once, so memory grows with N:
choose N by the machine's cores and memory (2.0 GB for 4 groups in process; to measure per process).

- Rules this must keep (CLAUDE.md, "Code"): the work is tasks on the executor, and a task doesn't block its thread
  waiting: a process's end has to be waited for without holding a thread (core has no process api yet: one is needed,
  on Windows, macOS and Linux, in `core`, and the waiting done by the executor's own means).
- An install compiles nothing (the precompiled shaders), so this is a checkout's alone; `tools/shaderprecompile` can
  use the same workers.
- With step 1, only the changed entry points are split over the workers.

Gate: the same binaries as one process's, no crash in the quick tier's clients over several runs (the in-process
version crashed 2 in 59), the cold start measured with 1, 2, 4 and 8 workers, and the peak memory recorded.

### 3. Find and cut D3D12's extra time

By step 0's numbers. The likely parts:

- **The SPIR-V target compiled only to find which entry points read the specialization constant** (it decorates
  `SpecId 0`). Slang's own metadata for that wasn't available for a compile request's program; with step 1's component
  api (a linked program per entry point) it may be (`IMetadata::isParameterLocationUsed`), and the SPIR-V target goes.
- **The specializations compiled eagerly**, every value for every entry point that reads the constant. They could be
  compiled when a pipeline first asks for one, in the background, as the pipelines are made now.
- **DXC's own time**: its optimization and validation settings, to measure.

Gate: D3D12's cold start within a stated margin of Vulkan's, the quick tier passing on both.

### 4. One shader cache per checkout, shared by its profiles

Keep the compiled sets (and step 1's entry points) in the build directory, keyed by content as now, and let every
profile of that build read and write it: the scratch profiles of captures and tests, and the user's, compile a version
once. The profile's cache stays for installs.

Gate: a second profile's first start after an edit is warm.

### 5. A faster configuration for iterating (optional, by step 0's numbers)

A switch (an environment variable) for a lower optimization level and less debug information while iterating, if
step 0 shows those cost much. It changes the code that runs, so it is off by default, never used for measurements
or captures that are compared, and the quick tier runs without it.

Gate: the time saved is worth having a second configuration; otherwise dropped.

### 6. Reload shaders while the client runs (later)

Watch the shader sources, recompile in the background on a change (steps 1 and 2 make that seconds), and swap the
layouts and pipelines between frames, keeping the scene, the camera and the indirect lighting's state. Removes the
restart and the scene's load; a capture still needs its settle frames. The largest change here, so last.

Gate: an edit is on screen without a restart, nothing leaks over repeated reloads (validation clean), and a reload that
fails to compile keeps the old shaders with the error shown.

## Also worth looking at

- **Splitting `shaders.slang` into slang modules** (`import`, each part checked and serialized once: `IModule::serialize`,
  `isBinaryModuleUpToDate`), so an edit re-checks its part and what imports it rather than the whole unit. Large (the
  parts use each other's declarations in any order today), and step 1 may make it unnecessary: only if step 0 shows
  the front end is a large share.
- **Slang's version**: newer releases may compile faster; measure when the port is next updated.
- **Pipeline creation**: the driver's own caches already keep what didn't change; check after step 1 that only the
  changed entry points' pipelines are cold.

## Not to change

The binaries an install ships (precompiled, full optimization, no debug information in release), the layouts' contents,
and the rule that a capture compared against another runs the same shaders.
