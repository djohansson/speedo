# CLAUDE.md

The rules most tasks need. Their details, and each subsystem's notes, are under docs/notes (index at the end): read the note for the area you change first, and keep it current: concise, how it works and why where that helps; no dates, poses, measurements or histories (those go in commit messages).

## Layering: core ← platform ← rhi ← gfx ← client

- `core`: application, tasks and executor, files, containers, math, assertions. No graphics.
- `platform`: windows (glfw only in `platform/glfw.cpp`), imgui's context, presentation. Knows nothing of graphics; nothing above it includes glfw.
- `rhi`: the abstraction over Vulkan, Metal and D3D12, templated on `GraphicsApi`, with its own vocabulary (`rhi/vocabulary.h`; convert at the backend boundary). Never includes gfx, imgui or slang.
- `gfx`: importers, scene, cameras and views, renderer, frame graph, Slang shaders (`gfx/shaders`, sharing `capi.h` with C++). No backend calls or types (aliases in `gfx/gpu.h`).
- `client`: the application (frame loop `windowedapplication.cpp`, ui `ui.cpp`, loads `loading.cpp`).
- gfx and client compile once per graphics api, each in its own inline namespace: open `namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE` (or `client::inline ...`), never a plain `namespace gfx`; a header-only library's implementation goes in `gfx/libraries.cpp`. Details: docs/notes/layering.md.

## Workflow

- Configure and build as one command, joined by `&&` alone: `cmake --preset <name> && cmake --build build/<name>`. Never a raw `cmake` or a bare `cmake --build`: they run without the presets' environment and can make vcpkg empty `install/` (a hook refuses them). New `.cpp` files go in `CMakeLists.txt`'s source lists. Details: docs/notes/build.md, "Presets".
- Run the client from the repo root with a scratch user folder kept between runs: `build/<preset>/client -u temp/user` (one per client running at once: `temp/user-<n>`). The import caches live in the user folder, so a new one reimports every model; give the scripts a fixed `-Work` too (they default to a new temporary directory). Test and experiment runs are headless (`SPEEDO_HEADLESS=1`); windowed (`SPEEDO_BACKGROUND=1`) only where a test needs a window. Variables: docs/environment-variables.md; captures and image checks: docs/visual-testing-plan.md.
- Don't edit `.slang` files or `capi.h`, or rebuild, while clients run: they compile shaders at startup and reload them on change.
- Before committing, run the quick tier: `pwsh -Command '$p = & scripts/fetch-test-assets.ps1 -Quick; & scripts/assettest.ps1 -Preset <preset> -Client -Validate -Headless -Work <scratch>/quick @p'` (the full sets, without `-Quick`, for changes touching many models). A shader change also needs `scripts/visualtest.ps1 -Preset <preset>`, which covers the other ray query variant.
- Commit only after the quick tier passes, one change per commit, the message saying what was measured. A fix for a bug the user reported waits for the user's confirmation.
- Check and validate before committing, always: the change itself, and every claim in the message against its source (the log, the file, the earlier commit), never from memory.
- A push is its own step, never chained to the commit. Before it every change going out is reviewed twice: by you (each commit's diff and message read back) and by the user, whose review is their telling you to push. Never push without being told.
- On macOS, hardware ray queries are opt in (`SPEEDO_RAY_QUERY=1`) and their clients run one at a time, never several. Other independent experiment clients may run in parallel; keep runs that measure time serial.
- Measure on warm pipelines, interleaving the runs compared; on KosmicKrisp use the draw thread's frame time (`SPEEDO_GPU_TIMINGS=1`). Details: docs/performance-plan.md.
- Subagents run on Sonnet (`.claude/settings.json`): delegate to a typed agent, not a fork, which runs on the session's model.

## Code

Each rule's reasons and exemptions: docs/notes/core.md.

- Scripts are PowerShell (`.ps1`), not bash, batch or python; the bootstrap `setup_*` scripts excepted.
- Create the application with `core::CreateApplication<T>(...)`; call `WindowedApplication::Shutdown()` before releasing the last `shared_ptr`.
- Math is glm's, not `std::array` aliases (except fixed layouts: `capi.h`'s structs, rhi's API). glm doesn't zero: initialize vectors and quaternions (`glm::identity<glm::quat>()`).
- Floating point is the same on every platform: IEEE 754, no `-ffast-math`, `-ffp-contract=off` (`std::fma` where a fused one is meant). Math functions are core's (`core::Sin`, `core::Pow`, ... in `core/math.h`), never `std::`'s or glm's that call them (`core/glm_extra.h` has `Perspective`, `Rotation`, `AngleAxis`, `Slerp`). Metal compiles shaders with relaxed math.
- Shaders must compile and work with `HARDWARE_RAY_QUERY` 1 and 0, which picks only how a ray is traced, never whether a feature exists: trace through `bvhtrace.slang`'s interface and keep `#if HARDWARE_RAY_QUERY` inside it; what software tracing costs more is a budget that goes by the device, not a guard. Details: docs/notes/indirect-lighting.md, "Hardware ray queries".
- Portable by default: the general method that performs well everywhere; a platform that can't gets an exception named for what it lacks, decided in rhi (`DeviceLimits`).
- Sizes and amounts are counts and named so (`kChunkCommandCount`); switches aren't.
- A fixed capacity that runs out fails hard with `ENSUREF` naming the limit: never warn and drop, truncate or fall back.
- No `new`/`delete` expressions: heap objects are owned by `std::unique_ptr`/`std::shared_ptr` (exemptions in core.md).
- Associative containers are core's (`core::UnorderedMap`, `core::UnorderedSet`), never `std::map`, `std::set` or `std::unordered_*`.
- Files go through `core/file.h`, not streams or stdio; hand decoders the mapped bytes.
- Locks: `std::mutex`; `core::UpgradableSharedMutex` only where upgrade locks are used.
- Parallel work is tasks on the executor (`core::CreateTask`, `Submit`, `ForEach`, `Join`), never threads of its own; a task never spins or sleeps waiting for another, nor `Join`s holding a lock. `CreateTask` copies its arguments (`std::ref` for references).
- Callables: a template parameter; `core::std_extra::function_ref` for the duration of a call, `move_only_function` when stored; never `std::function`.
- Sleep with `core::SleepUntil`, not `std::this_thread`'s.

## The notes

- docs/notes/core.md: application lifecycle, the conventions behind the Code rules.
- docs/notes/layering.md: platform, presentation, headless, file browser, ImGui renderer, shader compilation and caches.
- docs/notes/rhi.md: the backend abstraction, objects, frames, queues, descriptors, pipeline caches, crash reports.
- docs/notes/rendering.md: frame graph, views, camera and lenses, upscaling, culling, shadows, ray tracing, visibility buffer, lighting.
- docs/notes/indirect-lighting.md: probes, voxel clipmap, surface cache, near field, triangle tracing and ray queries, emitters, world cache.
- docs/notes/sky.md: atmosphere, sun, moon, stars, aerial perspective, fog.
- docs/notes/assets.md: importers, caches, scene unit, exposure and tonemapping, test sets and assettest.
- docs/notes/build.md: presets, toolchain, setup, vcpkg and the binary cache, FASTBuild, CI, releases.
- docs/environment-variables.md, docs/visual-testing-plan.md, docs/performance-plan.md, docs/backlog.md; plans: docs/backends-plan.md, docs/shader-compile-plan.md, docs/world-cache-plan.md, docs/research-scenes-plan.md, docs/research-scenes-review-plan.md.
