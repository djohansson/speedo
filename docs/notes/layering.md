# Layering: the platform, the file browser, the ImGui renderer and the shaders

This note describes how the layers (core, platform, rhi, gfx, client) divide the work, beyond CLAUDE.md's map: each layer's role (the window system, rhi, gfx, the client's frame loop, graphics api choice and server stub), then the subsystems: presentation and headless mode, the file browser and thumbnails, the ImGui renderer, the shader module, how shaders and pipelines are compiled, cached and shipped without stalling the frame, and the tools. rhi's internals are in rhi.md.

## Contents

- [Layering: core ← platform ← rhi ← gfx ← client](#layering-core--platform--rhi--gfx--client)
  - [The platform](#the-platform)
  - [rhi](#rhi)
  - [gfx](#gfx)
  - [client](#client)
- [The subsystems](#the-subsystems)
  - [Fullscreen and presentation](#fullscreen-and-presentation)
  - [Headless](#headless)
  - [The file browser](#the-file-browser)
  - [Thumbnails and the preview](#thumbnails-and-the-preview)
  - [ImGui's renderer](#imguis-renderer)
  - [Shaders](#shaders)
  - [The shaders compile in the background](#the-shaders-compile-in-the-background)
  - [The shader compiler library and precompiled shaders](#the-shader-compiler-library-and-precompiled-shaders)
  - [Pipelines made without waiting](#pipelines-made-without-waiting)
  - [Tools](#tools)
  - [Tests](#tests)
  - [shadercheck.ps1](#shadercheckps1)

## Layering: core ← platform ← rhi ← gfx ← client

### The platform

`platform` is the window system and the only code that calls glfw (`platform/glfw.cpp`). Its interface knows nothing of graphics, and the server does not link it.

- `platform::Window` holds a native handle and `WindowState`; `platform/capi.h` has the handle types and the C entry points `main.c` calls (`PlatformInit`, `PlatformWaitEvents`, `PlatformWakeMain`, `PlatformTerminate`, `PlatformCreateVulkanSurface`). Callbacks translate glfw's input into core's vocabulary (`core/capi.h`), so nothing above platform includes glfw.
- `platform::WindowedApplication` makes the window, runs `mainCalls` on the main thread, wakes the loop on `RequestExit`, and handles window changes.
- `platform::ImGuiLayer` owns imgui's context, fonts and style, builds frames on the ui thread (`BeginFrame`, `Fade`, `EndFrame`) and keeps a triple buffer of draw data for the draw thread (`TakeFrame`). `platform::imgui` is the glfw backend side; its input callbacks run on the main thread and take a mutex that `NewFrame` holds until `ImGui::NewFrame` is done, since imgui's input queue is not thread safe.

### rhi

`rhi` is the graphics backend abstraction, templated on `GraphicsApi`. It must not include `gfx`, know the application, or hold code of a library above the graphics api. Its objects find their instance and devices through `rhi::GetRHI<G>()`. The RHI is made for a window the platform already made and keeps a swapchain per window.

- The public API speaks `rhi/vocabulary.h` (`Format`, `ImageLayout`, `PipelineStage`, `Extent2d`, ...), converted at the backend boundary with `rhi::vk::ToVk`/`FromVk` (`rhi/vulkan/convert.h`). Add a neutral value and its conversion rather than exposing a Vulkan type.
- The backend's type aliases (`DescriptorType<G>`, ...) are in the global namespace, each naming a member of `BackendTypes<G>`, which a backend specializes (`rhi/vulkan/types.inl`). A neutral rhi type of the same name is ambiguous under `using namespace rhi`: pick another name, or qualify.

### gfx

`gfx` is everything above rhi: importers, `Model`, cameras and `Views`, the renderer and frame graph, `gfx::Scene` and `gfx::ImGuiRenderer`. It names rhi types through the aliases in `gfx/gpu.h` and has no Vulkan calls, enums or types.

gfx and the client are compiled once per graphics api the platform has (Vulkan everywhere, Metal on macOS, D3D12 on Windows), each copy in an inline namespace of its own (`gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE`), and linked into one client. `SPEEDO_GRAPHICS_API` picks the api at run time; the default is Vulkan on Windows and Linux and Metal on macOS. docs/notes/build.md describes the build side.

- `gfx::Scene` (`gfx/scene.h`) is what is drawn and how it is lit: model, image, environment, materials, lights, animation, default resources and the `Renderer`. Loads run in tasks and install on the draw thread; `Update` and `Record` run per frame on it. Its state is an `Impl` in `scene.cpp` whose lambdas capture `this`, so the application destroys it in `Shutdown`, after the tasks and draw calls that may use it.
- `gfx/graphicsqueue.h` has the draw thread's queue helpers (`UpdateBufferOnGraphics`, `TransitionThenBind`, `RetireAfterGraphicsWork`).

### client

`client::WindowedApplication` owns the rhi, views, scene, imgui layer and renderer, and keeps the frame loop (`client/windowedapplication.cpp`). `client::Client` adds the server connection and the task chains.

`ClientCreate` (`client/capi.cpp`) chooses the graphics api once, from `SPEEDO_GRAPHICS_API`, and `ClientMain` and `ClientDestroy` call that api's copy of the client; a name the platform lacks, or an unknown one, fails hard.

The server (`src/server`, an application of its own that links no graphics or platform code) and the client talk through a small RPC stub: the server binds a zeromq REP socket on `tcp://*:5555` and the client's REQ socket connects to `tcp://localhost:5555`. Requests are zpp::bits RPC calls (`server::RpcSay`) made by the `Rpc` task chain on each side, which polls once and requeues itself rather than blocking (core.md, "Parallel work is tasks; callables are templates or tasks, not `std::function`").

`Draw` runs the frame in phases: `InternalPrepare` (outside the graphics queue's lock: window changes, `Scene::Prepare`, the preview), then a `FrameContext` through `InternalUpdate` (per frame buffers, before the command buffer is begun, since updates submit buffers of their own), `InternalRecord` (preview, scene with the ui over it, captures in `client/framecapture.cpp`) and `InternalSubmit`. The passes depend on one another within a frame, so they do not go through the `drawCalls` queue.

Other files: `client/ui.cpp` (menus, windows, autoload variables), `client/loading.cpp` (files by type, zip archives extracted into the user profile, folders), `client/windowedapplicationdetail.h` (shared internals). Menus hold what is picked or switched; a setting with many options is a window a menu item opens (`SPEEDO_SHOW_CAMERAS`, `SPEEDO_SHOW_SKY`, `SPEEDO_SHOW_GLOBAL_ILLUMINATION`, `SPEEDO_SHOW_RAY_TRACING` open them at startup). A checkbox for something that cannot run now is greyed out and unchecked, its setting kept (`SmallCheckbox` with `available`).

## The subsystems

### Fullscreen and presentation

- Fullscreen (Alt+Enter, View > Presentation) follows `WindowState::fullscreenMode` (`FULLSCREEN_MODE_*`, `SPEEDO_FULLSCREEN`): exclusive (default), borderless, or on macOS native.
- The same menu sets the present mode and image count (`Swapchain::Configure`, applied by `Draw`; `SPEEDO_PRESENT_MODE`, `SPEEDO_SWAPCHAIN_IMAGE_COUNT`), within the surface's limits and `SHADER_TYPES_OUTPUT_IMAGE_COUNT`.
- The main thread never waits for the draw thread. `OnResizeFramebuffer` only sets `myResizeRequested`; `Draw` recreates the swapchain.
- Window changes (fullscreen, size, monitor) go through `RequestWindowChange`: `Draw` waits for the gpu and for presents (`Swapchain::WaitPresented`), releases the swapchain, has the main thread make the change (`CallOnMain`), skips frames meanwhile, then makes a new swapchain. Under fifo, frames queued across a display mode change are never shown and the driver hangs on them in swapchain creation or destruction.
- Every other recreation (present mode, image count, out of date, user resize) is from the old swapchain (`oldSwapchain`), without waiting.
- Frames present from the queue `Swapchain::GetPresentQueueType` names: a dedicated compute queue whose family can present (images shared concurrently), else the graphics queue.
- `SPEEDO_PRESENTATION_TEST=<step>[;<step>...]` runs these switches, a step every `SPEEDO_PRESENTATION_STEP_FRAMES` frames, and traps if no frame draws for 10 s; `scripts/assettest.ps1 -Presentation` runs it alone in the foreground.
- On Windows exclusive fullscreen also makes the swapchain exclusive (`Swapchain::SetExclusiveFullscreen`), composited until the window is in the foreground.

### Headless

`SPEEDO_HEADLESS=1` (`assettest.ps1 -Headless`) runs without a display, for locked screens and runners without one.

- glfw's null platform is used (`PlatformIsHeadless`): an imaginary 1920x1080 window. `PlatformWaitEvents` waits on a condition variable that `PlatformWakeMain` notifies; `platform::imgui` supplies imgui's size, scale and time itself.
- The client gives rhi no window (`RHIInitializationData::window` 0: no surface or swapchain) and draws into a `gfx::ImageOutput` with the ui's passes (`drawsUi`), sized as `SPEEDO_WINDOW_SIZE`. Nothing is acquired or presented, and `SPEEDO_CAPTURE` reads the image back.

### The file browser

File > Open... (`client/browser.cpp`, `InternalDrawBrowser`) lists a folder's subfolders and loadable files (models, zip archives, images, panoramas) as tiles, with selection, keyboard stepping, "Include subfolders" and kind filters.

- Only visible rows are built (`ImGuiListClipper`) and only they ask for thumbnails; a slot drawn last frame is never evicted, which made visible tiles flicker. Listings run on the executor and a newer one drops an older one's result.
- The entry last clicked is previewed, one preview load at a time, and shown only once it is the focused entry's.
- Opening goes through `InternalOpen`: one model as is, several as one (`ModelArrangement`), the rest by type, on a ground plane where ticked. File > Open Recent keeps the last `kRecentFileCount` in `<user profile>/recent.txt`.
- Test drivers: `SPEEDO_BROWSE`, `SPEEDO_BROWSE_SELECT`, `SPEEDO_BROWSE_ENVIRONMENT`, `SPEEDO_BROWSE_RECURSIVE`, `SPEEDO_BROWSE_KINDS`, `SPEEDO_BROWSE_GROUND`, `SPEEDO_BROWSE_OPEN`, `SPEEDO_BROWSE_OPEN_FRAME_COUNT`.

### Thumbnails and the preview

`client::Preview` (`client/preview.h`) is a second off-screen scene with its own pipeline, views and `ImageOutput`, lit by the sky, without indirect lighting or model BVHs (`ModelBvh::kNone`). The ui shows it with `ImGuiLayer::AddTexture`; dragging orbits the model (`Views::Orbit`). It lives until `Shutdown`, since its loads install from draw calls and timeline callbacks.

`client::Thumbnails` (`client/thumbnails.h`) is an rgba8 atlas, one imgui user texture whose slots are reused least recently drawn first. Tiles are made on the executor from an image, a panorama, the model's bundled `screenshot/`, or else a capture of its preview (`Preview::RequestCapture`).

- Cached under `<user profile>/thumbnails`, written beside the entry and renamed over it, keyed by the source file's path, size and time (for captures also the files the render reads).
- A capture's header carries its version: the importers' versions and the client's look (`RenderVersion` in `thumbnails.cpp`, a hash of the shader sources plus a `renderer-vN` tag to bump when C++ changes how a model is drawn). An out of date capture is shown until drawn again. Unused entries are pruned after 30 days (`Thumbnails::Prune`).
- The image preview is a second, uncached `Thumbnails` (`RequestPreview`, `GetPreview`), made by `Draw` under the draw lock before it takes the graphics queue's lock.

### ImGui's renderer

`gfx::ImGuiRenderer` (`gfx/imguirenderer.h`) draws imgui on rhi alone; rhi knows nothing of imgui. It has a pipeline of its own and the layout "ImGui" (`imgui.slang`).

- Vertices are pulled from a host visible buffer per frame slot (rhi's pipelines have no vertex input). Each draw's texture is a slot of one array (`gImGuiTextures`, `SHADER_TYPES_IMGUI_TEXTURE_COUNT`; slot 0 a white default). The first vertex is pushed as a constant and the draws' vertex offset is 0, because slang's `SV_VertexID` omits the base vertex.
- imgui's textures are created on the ui thread (`UpdateTextures`); the draw thread binds them, records uploads and releases them once no frame uses them (`PrepareFrame`). A texture id is a slot plus a generation (`TextureId`), so drawing a released or foreign texture fails by name.
- The menu bar, menus and popups are frosted panels: the client collects their rectangles each frame (`UiBlurRect`, `FrameInputs::uiBlurRects`), and the UI Blur X and UI Blur Y passes blur the output under them, a thread per pixel of each panel, before imgui draws its translucent panels over it. The passes run only while a panel is open.
- Assumptions about imgui fail at compile time (`imguirenderer.cpp`) when imgui or `imconfig.h` changes: vertex layout, color channel order, index size, texture id type and the push constants' layout.

### Shaders

Shaders are gfx's: the slang sources and the C header they share with the C++ (`gfx/shaders/`, `capi.h`), and `gfx::ShaderLoader`, which compiles them for `rhi::kShaderFormat` and reflects their bindings into a neutral `rhi::ShaderSet` (`rhi/shaderset.h`). rhi does not link slang. `Pipeline::CreateLayout` turns a `ShaderSet` into descriptor set layouts, and `RHIInitializationData::descriptorPoolSizes` carries the app's pool needs.

- The main layout's module `shaders.slang` is parts by area (`bindings`, `common`, `shading`, `shadows`, `gi`, `tonemap`, ...), included textually after `capi.h` and `sky.h` as one translation unit. Do not make a part a slang module (`__include`, `implementing`): the header's structs would be declared again, ambiguously.
- Entry points are chosen by name and stage, not by `[shader(...)]` attributes.

### The shaders compile in the background

A first launch's compile runs behind a progress bar while the ui runs.

- `Scene`'s constructor starts a task compiling its four layouts (z prepass, main, flares, environment filter) into the shader cache with one `ShaderLoader`. `Scene::Prepare`, called by `Draw` before each frame, finishes the rest once it is done.
- Until then the scene is not ready: `Record` draws a frame graph of its own (cleared output, then the ui), `Update` and `Resize` do nothing, loads import but their installs wait (`WaitUntilReady`), and the preview is not made. The Loading window shows "Compiling shaders".
- Slang is not thread safe, even across global sessions, so a `ShaderLoader` is the only one alive (`ENSUREF` in its constructor) and its users take turns.
- A layout compiles in worker processes (`shadercompileworker`, `SPEEDO_SHADER_WORKER_COUNT`, 0 compiles in process): round one keys each entry point by the hash of its HLSL source, round two compiles the entry points the cache lacks, in parallel (docs/shader-compile-plan.md).
- The cache is `shader-cache` beside the executable, shared by every profile of the build, with round one's results kept as `keys-<hash>.bin` (`KeysRecord` in `shaderloader.cpp`). Nothing in the client removes cache files; `scripts/prune-shader-cache.ps1` does.
- Because clients compile shaders at startup and reload them when their sources change, editing a `.slang` file or `capi.h`, or rebuilding, while clients run breaks them or leaves a batch of runs on mixed shaders.
- Shaders reload when their sources change (`Scene::WatchShaders`, `SPEEDO_SHADER_RELOAD`): compiled on the executor, then between frames the modules are replaced and pipelines remade (`Pipeline::ReplaceShaders`), bindings kept. A failed compile keeps the old shaders; changed resources need a restart. The preview's scene follows the window's reloads (`Scene::FollowShaders`).
- Shader work iterates faster on Vulkan (`SPEEDO_GRAPHICS_API=vulkan`) and is checked on D3D12 before a commit.

### The shader compiler library and precompiled shaders

The client links no slang, so an install compiles nothing and need not ship it.

- Everything that calls slang is the `shadercompiler` shared library (`gfx/shadercompiler.h`/`.cpp`), which `ShaderLoader` loads at its first compile (`core::DynamicLibrary`; `SPEEDO_SHADERCOMPILER_FILE`) and calls through two C entry points, serializing with zpp::bits. It uses core's headers but none of its compiled code, which would duplicate core's globals. `SPEEDO_SHADER_LOG=1` prints a compile's details.
- Every compile checks that each buffer's data has the same layout packed as under std430, since slang lays out Metal's buffers packed, and fails naming the fields (`CheckPackedLayout`; the top of `capi.h` says how to pad).
- `ShaderLoader::Load` reads an install's precompiled shaders where the source would be (`<stem>-<parameters hash>.shaderset`, `PrecompiledPath`), else the profile's cache, else compiles through the library, failing hard where nothing loads.
- `tools/shaderprecompile` writes them for every layout the client may load (`gfx/shaderlayouts.h`, with and without ray queries); `scripts/package.ps1` runs it into `precompiled-shaders`, which the Client component installs in place of the sources. Debug information follows the build (`kShaderDebugInfoLevel`).

### Pipelines made without waiting

Cold pipeline creation can take seconds each in a driver, so the frame never waits for it.

- Compute pipelines are made ahead: `Scene::Prepare` requests the environment layout's and the renderer's (`Renderer::ComputePipelineVariants`) through `Pipeline::RequestComputePipelines`, a task each ("Creating pipelines n of m"). They run in chains (`core::AddDependency`), as many as half the executor's threads, since all at once cold compiles starve the draw thread's task chain.
- Graphics pipelines depend on render target formats set as a pass records, so a pass requests its own (`RequestDrawListPipelines`, `gfx/drawlist.h`; `Pipeline::RequestGraphicsPipeline`, from a copy of the state and bound layout) and records its draws only once none is missing. Without an executor they are made on first use.
- `SPEEDO_AUTOLOAD_EXIT` and its capture restart their frame count while any pipeline is missing (`Renderer::GetGraphicsPipelineProgress`).
- The window's scene keeps the graphics pipelines its passes asked for in the profile's `graphics-pipelines.txt` (`Scene::KeepGraphicsPipelines`) and requests them ahead at the next start (`Renderer::RequestGraphicsPipelinesAhead`).
- The profile's pipeline files are bounded separately (rhi.md, "Pipeline caches: kept in check").
- NVIDIA's driver cache can fill and then accept nothing, so the client points it (`RHIInitializationData::driverCachePath`) at `driver-cache` beside the executable (the profile in an install), empties it at startup when over 90% full, unless `__GL_SHADER_DISK_CACHE_PATH` is set. The driver reads these variables as it loads, so platform must not call `glfwVulkanSupported()` at startup.

### Tools

`src/tools` has the programs beside the client, each its own CMake target in `CMakeLists.txt` linking core or gfx.

- `assettest` (`assettest.cpp`): imports the models and images it is given (files, directories, zip archives) the way the client does and checks the results, one PASS, WARN or FAIL line each, exit 1 if any fails. It also checks the environment kernels, the BVH traversal against brute force and the lens prescriptions on the cpu, through the two kernel sets below. Driven by `scripts/assettest.ps1` (the quick tier, the full sets, CI); docs/notes/assets.md, "assettest".
- `environmentkernels*` (`.h`, `.cpp`, `environmentkernelsbridge.*`): `gfx/shaders/environment.slang`'s kernels (sky, downsample, specular, sheen and irradiance prefilters, the sky's tables) compiled by `slangc -target cpp` into `environmentkernels.generated.cpp`, a build step of `assettest`, and run on the cpu in the order the client's `gfx::EnvironmentFilter` runs them on the gpu, so the tests check what the client draws with. The bridge keeps slang's prelude, which declares many names globally, away from the gfx and glm headers.
- `bvhkernels.slang`, `bvhkernelsbridge.*`: `gfx/shaders/bvh.slang`'s traversal (entry points `BvhTrace` and `BvhTrace2`) compiled to c++ the same way, one thread per ray, so that `assettest` checks the indirect lighting's BVH against brute force.
- `imagecompare` (`imagecompare.cpp`, `capture.h`): measures a capture against a reference and fails by thresholds; what `scripts/visualtest.ps1` and `scripts/researchcompare.ps1` judge by. `--self-test` checks the measures. docs/visual-testing-plan.md, "imagecompare".
- `imagesheet` (`imagesheet.cpp`, `capture.h`): an image's size, a png as a bmp, and compositions of images (each fit to a rectangle) and wrapped labels (a TrueType font through stb_truetype, antialiased; the scripts pass the client's Roboto) into a png, jpg or bmp; what `scripts/researchcompare.ps1` makes its sheets and chart with, the same on every platform.
- `convergence` (`convergence.cpp`, sharing `capture.h`): how long a still view takes to settle, from the captures of one pose repeated: the difference from the settled image per capture, the noise, and the frames until it stays under limits. Run by `scripts/perfbaseline.ps1 -Convergence`; docs/performance-plan.md.
- `rhitest` (`rhitest.cpp`, kernels `rhitest*.slang`): rhi's work on each backend the build has, or those named (`rhitest [shader dir [backend...]]`, from the checkout's root), compared against Vulkan's; exit 0 when every backend passes. A developer's check for backend work, and CI's Metal job runs it; docs/notes/rhi.md, "`rhitest`: rhi checked against Vulkan", and docs/backends-plan.md.
- `neuralbench` (`neuralbench.cpp`, `neuralbench.slang`): what a neural radiance cache costs in plain compute on each backend, against cells of running means; a developer's benchmark, on a gpu doing nothing else. docs/notes/indirect-lighting.md, "A neural radiance cache".
- `shadercompileworker` (`shadercompileworker.cpp`): one shader compile in a process of its own, `shadercompileworker <request file> <response file>`, through the shader compiler library beside it, since slang is not thread safe. Started by `gfx::ShaderLoader`, several at once, not by hand; docs/shader-compile-plan.md.
- `shaderprecompile` (`shaderprecompile.cpp`): writes the precompiled shader sets an install loads, `shaderprecompile <shader dir> <out dir> [format...]`. Run by `scripts/package.ps1`; see "The shader compiler library and precompiled shaders".
- `taskbench` (`taskbench.cpp`): the task system's overheads and the task pool's peak use (submit, call, chain, foreach, nested, alloc). A developer's benchmark; core's tasks are in docs/notes/core.md.
- `filebench` (`filebench.cpp`): ways of reading a file whole (std streams, stdio, mapped files) and, with `--import` or `--zip`, the importers' and `core::zip`'s times. A developer's benchmark; it uses streams and stdio on purpose, being what it measures.
- `macos/`: `mslcheck.mm` compiles MSL files with Metal's runtime compiler (built and run by `shadercheck.ps1 -Validate`); `rtrepro.mm` reproduces Metal 4 ray queries under contention between processes and `contend.ps1` runs several processes at once on the gpu, stopping them at the first gpu restart (docs/notes/indirect-lighting.md, "Hardware ray queries"); `svgrender.swift` renders an SVG to a PNG (`scripts/package/make-icons.ps1`).
- `compat/stdfloat`: a `<stdfloat>` shim on `assettest`'s include path, for slang's c++ prelude, which includes it where libc++ has none.

### Tests

`src/tests` is a Catch2 stub: `main.cpp` runs a `Catch::Session` and `utils.cpp` holds one factorial test case. No target in `CMakeLists.txt` builds it, so it runs nowhere. The testing that counts is the scripts: `scripts/assettest.ps1` (importers, kernels, validated client runs) and `scripts/visualtest.ps1` (image checks), described in docs/visual-testing-plan.md.

### shadercheck.ps1

`scripts/shadercheck.ps1 -Preset <preset> -Target metal|hlsl|dxil [-Work <dir>] [-Validate]` compiles every shader layout the client loads (the scene's with and without ray queries, ImGui's, `EnvironmentFilter`'s) with the install's `slangc` to another backend's target, each entry point alone with the client's options. `-Validate` checks the output: Metal's runtime compiler through `mslcheck` on macOS, DXC for `dxil` on Windows. Run it after a shader change to see that it still compiles for the backends the client is not running on (docs/backends-plan.md).