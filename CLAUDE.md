# CLAUDE.md

Notes for working in this codebase, distilled from real build failures.

## Layering: core ← platform ← rhi ← gfx ← client

- `platform` is the window system, and the only code that calls glfw: its backend (`platform/glfw.cpp`, `glfw.h`:
  initializing and terminating glfw, the window's creation and destruction, its callbacks, the event loop's wait and
  wake up, and the Vulkan surface), the windows (`platform::Window`: a native handle, its `WindowState` and whether it
  is minimized; `WindowHandle`/`WindowState` in `platform/capi.h`, with `GetCurrentWindow`/`SetCurrentWindow` and the
  C entry points `main.c` calls: `PlatformInit`, `PlatformWaitEvents`, `PlatformWakeMain`, `PlatformTerminate`, and
  `PlatformCreateVulkanSurface` for rhi, its types opaque, `void*`), `platform::WindowedApplication` (makes its window
  and destroys it, runs `mainCalls` on the main thread in `Main`, wakes the main loop on `RequestExit`, tracks
  minimizing in `OnResizeFramebuffer`, and the window changes: fullscreen, sizes, monitors) and imgui's platform side (`platform::imgui`, imgui's glfw
  backend; its input callbacks are installed by `platform::imgui::Init` rather than the backend: they run on the main
  thread, in `glfwWaitEvents`, and fill imgui's input queue, an `ImVector`, which `ImGui::NewFrame` consumes on the ui
  thread, so each calls the window's own callback, then the backend's under a mutex that `NewFrame` returns held until
  `ImGui::NewFrame` is done; unlocked, the two once crashed in `FindLatestInputEvent` with an index out of range). It
  links glfw and imgui privately (and Vulkan's headers, for `glfwCreateWindowSurface`'s types), and its interface knows
  nothing of graphics (no native file dialogs: the client's file browser replaced nfd's): the server, which has no
  windows, doesn't link it. Its callbacks translate glfw's keys, buttons, actions and modifiers into core's input
  vocabulary (`enum Key`, `MOUSE_BUTTON_*`, `INPUT_ACTION_*`, `INPUT_MOD_*` in `core/capi.h`, which `InputState` and the
  views' movement keys use): nothing above platform includes glfw (the client and gfx used its key codes, rhi made the
  surface with it, and `main.c` held the window, its callbacks and the loop). The executables' `main.c` (client and
  server) are a few lines each: core's `InstallSignalHandlers(onInterrupt)` (`core/process.cpp`: SIGINT sets
  `IsInterrupted` and calls `onInterrupt` from a regular thread, the self-pipe trick off Windows, here the client's
  `PlatformWakeMain`; fatal signals are logged and raised again, but those a sanitizer reports), `ParseArguments` (cargs:
  `-r`, `-u`, `-h`), and their loop (the client's around `PlatformWaitEvents`, the server's polling with
  `SleepSeconds`); both had their own copy of all of it. Alt+Enter (and View > Presentation > Fullscreen)
  goes fullscreen as `WindowState::fullscreenMode` says (`FULLSCREEN_MODE_*`, set by View > Presentation or
  `SPEEDO_FULLSCREEN`), on the monitor the window is on (`platform::WindowedApplication::ToggleFullscreen`; it was
  always the primary monitor's): exclusive (`glfwSetWindowMonitor`, the default; on Windows the swapchain also takes
  the monitor, `VK_EXT_full_screen_exclusive`, application controlled, `Swapchain::SetExclusiveFullscreen` with the
  window's `PlatformGetWin32Monitor`: without it NVIDIA's driver composited the window covering the monitor, and the
  display's variable refresh rate never engaged, under fifo or mailbox; a lost exclusive mode recreates the swapchain,
  as out of date), borderless (an
  undecorated window covering the monitor, on every platform; on macOS with the menu bar and dock hidden, and composited)
  or, on macOS, native (its own space). Exclusive and native
  let macOS scan the swapchain out directly, which flickered with KosmicKrisp from mesa 26.2 (frames out of order), so
  borderless was the only mode for a while; with mesa main (8fc4981) all three work. The same menu sets the swapchain's
  presentation (`Swapchain::Configure`: `rhi::PresentationMode`, FIFO, FIFO relaxed, mailbox or immediate, of those the
  surface has, and an image count, applied by `Draw`, which recreates the swapchain; KosmicKrisp has FIFO and immediate,
  and takes exactly 3 images; at startup the swapchain takes mailbox, else FIFO relaxed, else FIFO), as
  `SPEEDO_PRESENT_MODE` and `SPEEDO_SWAPCHAIN_IMAGE_COUNT` do at startup. The counts asked for stay within the surface's
  limits and the application's (`RHIInitializationData::maxSwapchainImageCount`, the client's
  `SHADER_TYPES_OUTPUT_IMAGE_COUNT`, 8: ComputeMain's storage view of each image is a `gRWTextures` slot), which
  `Swapchain::GetImageCountLimits` gives the menu; the swapchain's frames are the images the driver made, which may be
  more than asked for (it used to trap).

  Window changes and the draw thread (NVIDIA's Windows driver, an RTX 5070, where all of this hung; found by
  `SPEEDO_PRESENTATION_TEST`, below). The main thread never waits for the draw thread: `OnResizeFramebuffer` (called
  inside the window system's calls, e.g. a fullscreen switch's `glfwSetWindowMonitor`) only asks `Draw` to recreate the
  swapchain (`myResizeRequested`). It used to take the draw lock and recreate it itself, while `Draw` held the lock
  recreating it, which on Windows waits for the window's thread to handle its messages: a switch out of fullscreen hung
  2 runs in 3. What the application changes about the window itself (a fullscreen switch, a size, a monitor) goes
  through `RequestWindowChange`: `Draw` stops, waits for the gpu and for what it presented to be shown
  (`Swapchain::WaitPresented`), destroys the swapchain (`Swapchain::Release`), and has the main thread make the change
  (`CallOnMain`, which wakes it), skipping frames until it has; then makes a new swapchain, from none. With fifo, frames
  queued for the coming vertical blanks when exclusive fullscreen changed the display mode were never shown, and the
  driver waited for them forever in `vkCreateSwapchainKHR` or `vkDestroySwapchainKHR` (every switch into exclusive
  fullscreen under fifo), and a swapchain made after leaving fullscreen hung the same way when the window was resized
  with frames queued. Every other recreation (a present mode, an image count, out of date, a resize by the user) is
  from the previous swapchain (`oldSwapchain`), without waiting for presents: releasing first hung a fifo swapchain in
  `vkDestroySwapchainKHR` on resizes, 5 runs in 5. The switch is handled while minimized too (Windows minimizes exclusive
  fullscreen when it loses the focus, and the switch waited for a frame). The main loop waits for events with a timeout
  (`glfwWaitEventsTimeout`): a fullscreen switch waiting for its main call stalled twice until the next input, as if
  `glfwPostEmptyEvent`'s wake up had been missed. Each frame's present semaphore is kept by its frame slot
  (`myPresentSemaphores`) until the slot comes round again and the present's fences
  (`VK_EXT_swapchain_maintenance1`) have signaled (`InternalRetirePresents`): a timeline callback per frame waited for
  the present with `vkWaitForPresentKHR`, which would have blocked an executor thread a refresh or more under fifo, but
  never did: `VK_KHR_present_id` was looked for among the instance's extensions (it is a device's), so presents never
  had ids, nor the semaphores a wait. Frames are presented from the queue `Swapchain::GetPresentQueueType` says: a
  dedicated compute queue whose family can present to the surface (NVIDIA's), the swapchain's images then shared by the
  graphics and compute families (`VK_SHARING_MODE_CONCURRENT`), else the graphics queue (whose family the rhi picks for
  presenting). They were presented from the compute queue without either: its family never checked for present
  support, the images exclusively the graphics family's and never transferred (invalid; nothing reported it,
  validation included). From the graphics queue instead, a swapchain hung in `vkDestroySwapchainKHR` when a fullscreen
  switch released it (4 runs of the presentation test in 8, against 0 in 12 from the compute queue), so the compute
  queue stayed, made valid; the concurrent sharing costs nothing measurable (Sponza's frame 4.94 ms against 4.83).

  `SPEEDO_PRESENTATION_TEST=<step>[;<step>...]` (`PrepareDraw`) runs View > Presentation's options and the switches, a
  step every `SPEEDO_PRESENTATION_STEP_FRAMES` (20) drawn frames once the autoloads are in: `fullscreen` (in or out),
  `fullscreen=exclusive|borderless|native`, `present=fifo|fifo_relaxed|mailbox|immediate`, `images=<count>`,
  `size=<width>x<height>`, `monitor=<index>`; it traps when no frame is drawn for 10 seconds (and turns glfw's
  auto-iconify off: an automated run doesn't control the focus). `scripts/assettest.ps1 -Presentation` runs a sequence of
  them on the primary and the second monitor, alone and in the foreground (they take over the display); the fullscreen
  hangs above failed it every time. Once they were fixed, one run in ten crashed inside the driver or lost the device:
  validation (below) found `gRWTextures`' slots past a swapchain's image count still naming the views of one destroyed
  since (4 images, then 3), which the set's next whole update handed the driver; every output slot is written now (the
  first image's past the count). It then passed 12 runs in 12 with Sponza, and clean under validation. A debug client
  compiling the shaders on Windows ran past 20 minutes without drawing a frame: the profile client with
  `VK_LAYER_PATH=<install>/x64-windows-clang/bin` and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` validates too (the
  layer prints to stdout when there is no messenger). For a lost device, the Crash Diagnostic Layer (the
  `vulkan-crash-diagnostic-layer` overlay port, installed beside the validation layer:
  `VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_crash_diagnostic`) dumps the submits and commands the gpu hadn't finished, with
  the shader and descriptor sets of the last one started, to `<home>/cdl/<time>/cdl_dump.yaml` (its `CDL_*` variables
  weren't taken from the environment here: the dump's settings show what was). With the layer loaded, the client's own
  crash report defers to it (see "Crash reports").
- `rhi` is the graphics backend abstraction (instance, devices, queues, buffers, images, pipelines, swapchains),
  templated on `GraphicsApi`. It must not include `gfx` or know about the application: rhi objects find
  their instance and devices through `rhi::GetRHI<G>()`, which an `RHI<G>` serves for its whole lifetime (registered
  by its first member, so before its members are created and after they are destroyed).
  The RHI is made for a window the platform already made (`RHIInitializationData::window`): its surface picks the
  devices that can present to it, and it keeps a swapchain per window (`RHI::GetSwapchain(WindowHandle)`).
- `gfx` is everything above it: importers (`obj::Import`, `image::Import`), `Model`, `LoadTexture`, cameras and
  `Views`, the renderer and frame graph, `gfx::Scene` and `gfx::ImGuiLayer`. `gfx::Scene` (`gfx/scene.h`) is what is
  drawn and how it is lit: the installed model, image and environment, their materials, textures, lights and animation,
  the default resources standing in for what they don't have, and the `Renderer`. Its loads (`LoadModels`,
  `LoadImage`, `LoadEnvironment`) run in load tasks and install on the draw thread; `Update` and `Record` are the draw
  thread's per frame; the ui changes its `Settings` and reads its animation, scenes and statistics. Its state is an
  `Impl` in `scene.cpp` (what were the application's file statics), whose lambdas capture `this`: the application
  destroys the scene in `Shutdown`, after the tasks and draw calls that may use it. `gfx::ImGuiLayer` is the ui's imgui:
  its context, fonts and style, its platform binding and renderer, and the triple buffer handing a frame's draw data
  from the ui thread (`BeginFrame`/`EndFrame`) to the draw thread (`PrepareFrame`/`Draw`). `gfx/graphicsqueue.h` has
  the draw thread's graphics queue helpers (`UpdateBufferOnGraphics`, `TransitionThenBind`, `RetireAfterGraphicsWork`).
- `client` is the application: `client::WindowedApplication` (a `platform::WindowedApplication`) owns the rhi, the
  views, the scene and the imgui layer, and keeps the frame loop (`client/windowedapplication.cpp`: `Draw`, fences,
  pool resets, draw calls, present, frame statistics; construction, resizes and `Shutdown`), the ui (`client/ui.cpp`:
  `PrepareDraw`'s menus and windows, and the autoload variables) and the load policy (`client/loading.cpp`: files by
  type, zip archives extracted into the user profile, folders). What the three share is in
  `client/windowedapplicationdetail.h` (internal): `App()`, the frame statistics, the archive choice, the extensions
  the dialogs filter by and the load functions.
  The file browser's preview is `client::Preview` (`client/preview.h`): a second, off-screen scene with its own pipeline,
  views and `ImageOutput`, lit by the sky, without the indirect lighting or the models' BVHs (`Scene::Settings::modelBvhs`, `ModelBvh::kNone`:
  no file's read from its cache, no set's built, which took San Miguel's archive seconds that a preview given up went on
  with; so the atlas's shadows alone and no emitters' light), shown by the ui with `ImGuiLayer::AddTexture`; dragged, it orbits
  the model (`Views::Orbit`: around the bounds' center, at the camera's distance). The browser itself
  (File > Open..., `client/browser.cpp`, `WindowedApplication::InternalDrawBrowser`; its path a button per folder, the
  ones that don't fit behind "...") lists a folder's subfolders and the
  files the client loads (models, zip archives, images, panoramas) as tiles (a long name keeps its start and end, "..."
  between: `ElideMiddle`), with click, ctrl or cmd click and shift
  click selection, double click (or enter) to open or enter, and the arrow keys to step through them (scrolled into
  view); "Include subfolders" lists every loadable file below the folder instead of its folders (by their path from it),
  and checkboxes filter by kind (models, archives, images, panoramas): by default every model and zip archive below `resources/test-assets`
  (else the resource dir); only the rows in view are built (`ImGuiListClipper`), and only they ask for thumbnails (all
  of them asking each frame made every one count as just drawn, and past the atlas's slots each new thumbnail evicted
  one in view: they flickered; a slot drawn last frame is never taken now, a thumbnail waits for one rather, and the
  atlas holds more than a browser filling a 4K window shows, about 300 tiles, which the 256 slots it had didn't); listings run on the task executor (a big tree takes
  a while), a newer one dropping an older one's result; it previews the entry last clicked if it is a model, a zip archive (its
  models side by side, extracted as opening it does: `ExtractArchive`, `FindModels`) or a panorama (one preview load at
  a time, the latest selection's next; a model after a panorama brings the sky back), and shows it only once it
  is the focused entry's: the scene's file path is the name `Scene::LoadModels` returned for it (a model's path, or a set's
  name); until then an empty square ("Loading..."), and nothing for an entry without a preview (a folder), or an archive
  without models ("Nothing to preview"): the last preview stayed up, which looked like the new entry's; and
  opens the selection: one model as it is, several as one (`ModelArrangement`: side by side, the grid `Model::Load` of
  several always used, or as authored, each file where it is), the rest each by its type. `SPEEDO_BROWSE=<dir>`,
  `SPEEDO_BROWSE_SELECT=<path>[;<path>...]` (paths from the folder), `SPEEDO_BROWSE_ENVIRONMENT=1`, `SPEEDO_BROWSE_RECURSIVE=0`,
  `SPEEDO_BROWSE_KINDS=<models,archives,images,panoramas>` and `SPEEDO_BROWSE_OPEN=side-by-side|as-authored` drive it at
  startup, for tests and screenshots (`SPEEDO_BROWSE_OPEN_FRAME_COUNT=<n>` opens once the preview has loaded the selection
  that many frames: a double click, whose first click started it; a preview is given up when the browser closes, its
  loads cancelled). Tiles show thumbnails (`client::Thumbnails`, `client/thumbnails.h`): rgba8 in
  one imgui texture registered with `ImGui::RegisterUserTexture` (an atlas of 160 pixel slots, 24x24, reused least
  recently drawn first: imgui's 1024 descriptor sets wouldn't hold a texture each), made on the task executor (4 at a
  time, newest asked for first, skipped once scrolled away): an image decoded (`image::Decode`) and scaled down, a
  panorama imported small and tonemapped, a model's bundled screenshot (`<model>/screenshot/screenshot.*`, as the
  glTF-Sample-Assets have), else, for a model without one or a zip archive (`Source::kArchive`, keyed by the archive and
  both importers' versions), a capture of its preview once drawn 20 frames
  (`Preview::RequestCapture`: the draw thread copies the preview's image to a host visible buffer after recording it,
  `CommandEncoder::CopyImageToBuffer`, and a graphics timeline callback hands the pixels over); cached on disk under
  `<user profile>/thumbnails` (`THM1`: a header, then the pixels; written beside the entry and renamed over it, since
  a model's encodings share their screenshot's entry and make it at once: written in place, one could read it half
  written, and on Windows the write failed while another had it mapped), keyed by what each is made from, so that changing any
  of it makes a new one: the file's path, size and time (an image's, a panorama's, a screenshot's), and for a capture
  also the files its render reads (`mesh::Dependencies`, and `mesh::ImageFiles`: the textures a gltf's uris or an mtl's
  maps name). A capture's version is in its header instead (`THM2`): the importers' versions (`Model::ImportVersion`,
  `TextureImportVersion`: the tags their caches are keyed by too) and the client's look (`RenderVersion` in
  `thumbnails.cpp`: a hash of the shader sources under `RootPath`, plus a `renderer-vN` tag to bump when C++ changes how
  a model is drawn); a capture of another version is shown until its preview is drawn again, which captures it again
  (`NeedsCapture`). In the key, every shader change (pulled, or a constant's comment edited) lost every capture, and an
  archive's thumbnail is only ever one: archives' thumbnails disappeared on restarts. A hit marks its entry used (its write time), and
  making a cached `Thumbnails` prunes the entries unused for 30 days on the task executor (`Thumbnails::Prune`): what
  changes to files and shaders leave behind. Being a user texture, the
  atlas's updates are reset each frame by its owner (imgui does that for its own atlases only: `Thumbnails::Update`),
  and queued with `ImTextureDataQueueUpload`. A second `Thumbnails` of one 768 pixel slot, not cached, is the image
  preview. The ui requests it (`RequestPreview`); `Draw` makes it under the draw
  lock, before taking the graphics queue's (the scene's constructor takes that), and publishes it (`GetPreview`). In the
  frames the ui shows it (`SetPreviewVisible`), `Draw` updates and records it before the window's scene (see "Command
  buffers: one owner each" for the crash its update once caused). It is kept until `Shutdown`: its loads install from draw calls and timeline callbacks (an image's transition
  queues another draw call), which only the shutdown's settling waits for, so destroying it earlier isn't safe.
  `client::Client` adds the server connection and the tick and draw task chains. It names gfx's aliases, which
  conflict with rhi's templates under both using-directives, with using-declarations in `namespace client`.
- gfx code is not templated on the backend: it names rhi types through the aliases in `gfx/gpu.h` (`rhi::kGraphicsApi`, one per
  build), and contains no backend code: no Vulkan calls, enums or types.
- rhi's public API speaks its own vocabulary, `rhi/enums.h` (`Format`, `ImageLayout`, `ImageAspect`, `ImageUsage`,
  `BufferUsage`, `PipelineStage`, `Access`, `LoadOp`, `SamplerDesc`, `Extent2d`, `ClearValue`, `PresentResult`,
  ...), converted at the backend boundary with `rhi::vk::ToVk`/`FromVk` (`rhi/vulkan/convert.h`, backend sources
  only). Handles stay `XxxHandle<G>`. Add a neutral value (and its conversion) rather than exposing a Vulkan type.
  Commands without an object of their own go through `CommandEncoder<G>` (viewport, scissor, index buffer, draws,
  dispatch, buffer updates, memory barriers); secondary command buffers inheriting a render target come from
  `CommandPool::SecondaryCommands`; descriptor values are `BufferBinding<G>`/`ImageBinding<G>`, which `Pipeline`
  stores in Vulkan's form (the update templates read them in place).
- ImGui's renderer is rhi's `ImGuiRenderer<G>` (`rhi/imguirenderer.h`, drawing into a swapchain's frames): the imgui
  Vulkan backend plus imgui's textures, which it creates and uploads itself on the ui thread and records on the draw
  thread. Its platform side is `platform::imgui` (`Init`, `NewFrame`, `Shutdown`). gfx owns the imgui context (made
  before `platform::imgui::Init`, destroyed after `Shutdown`) and the triple buffer of draw data snapshots between the
  two threads.
- Shaders are gfx's: the slang sources and the C header they share with the C++ code (`gfx/shaders/`), and
  `gfx::ShaderLoader`, which compiles them for `rhi::kShaderFormat` and reflects their bindings into a neutral
  `rhi::ShaderSet` (`rhi/shaderset.h`: binaries, entry points, and per set the bindings and push constants). rhi
  doesn't link slang. `Pipeline::CreateLayout` turns a `ShaderSet` into the backend's descriptor set layouts (every
  binding partially bound). What the app's descriptor sets need from the pool comes in through
  `RHIInitializationData::descriptorPoolSizes`, since only the app knows its array sizes. The main layout's module,
  `shaders.slang`, is parts by area (`bindings`, `common`, `debug`, `packing`, `vertex`, `texturing`, `ibl`, `brdf`, `shading`,
  `shadows`, `lights`, `transmission`, `scatter`, `fragment`, `tonemap`, `composite`, `uiblur`), which it includes textually after
  `capi.h` and `sky.h`: one translation unit, so the header's declarations exist once (each part including it as a part
  of the module, `__include` and `implementing`, declared its structs again: ambiguous). A part may use what any other
  declares, in any order. Slang's dependency list names every part, so editing one recompiles. Entry points are chosen
  by name and stage (`ShaderLoader`), not by their `[shader(...)]` attributes; one had strayed onto `TonemapAces`.
- The backend's type aliases (`DescriptorType<G>`, `PushConstantRange<G>`, ...) are in the *global* namespace (see
  `rhi/vulkan/types.inl`), so a neutral rhi type of the same name hides them inside `namespace rhi`, and is ambiguous
  in code with `using namespace rhi` (gfx's application). Pick another name, or qualify: `::DescriptorType<G>` in
  rhi, `rhi::DescriptorType` in gfx.

## Vulkan 1.4

The instance asks for Vulkan 1.4 (`kVulkanApiVersion`, `rhi/vulkan/utils.h`) and only devices that support it are
candidates (`DetectAndCreateDevices`). What 1.1 to 1.4 made core is called and enabled as core: dynamic rendering,
synchronization2, the dynamic cull mode and front face, shader draw parameters, inline uniform blocks, push descriptors,
tooling info (`vkCmdBeginRendering`, `vkCmdPipelineBarrier2`, `vkCmdSetCullMode`, `vkCmdPushDescriptorSetWithTemplate`,
...), not through extensions and function pointers. Features come in `PhysicalDeviceInfo`'s version structs
(`deviceFeatures11Ex` to `deviceFeatures14Ex`, the 1.3 and 1.4 ones chained only where the device has them), then the
extensions' (`deviceFeatureParams`: present id and wait, swapchain maintenance1, the crash reports'); the device is made
with the whole chain, so it enables every feature it has, and `Device` checks by name the optional ones the renderer
needs (dynamic rendering, synchronization2, shader draw parameters, timeline semaphores, push descriptors). VMA and
imgui's backend are told the version too (imgui then loads the core dynamic rendering functions: the KHR ones aren't
enabled). Verified on Windows (NVIDIA, 1.4.341); macOS needs KosmicKrisp or MoltenVK to report 1.4, else no device is a
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

## Math: glm, not arrays

Vectors, matrices and quaternions are glm's (`glm::vec3`, `glm::mat4`, `glm::quat`; `glm::dvec3`/`glm::dmat4` where
the importers keep double precision), not `std::array<float, N>` aliases with hand-written `Dot`/`Cross`/`Multiply`
helpers: colors and transforms in `mesh::Material`, `ModelMaterial`, `mesh::Transform`/`SceneMatrix`, the scene nodes'
translation, rotation and scale, `SceneLight`, `SceneCamera`, the environment's sky and dominant lights. Arrays stay
where a layout is fixed elsewhere: the structs `capi.h` shares with the shaders (copied with `glm::value_ptr`),
`TextureRef::transform` (the shader's two rows), rhi's public API (no glm), and lists that aren't vectors (spherical
harmonics coefficients, per channel statistics). glm here doesn't zero vectors by default (no `GLM_FORCE_CTOR_INIT`):
initialize them (`glm::vec3 v{0.0F}`), and a quaternion `glm::identity<glm::quat>()`, since `{}` is zero rather than the
identity. glm's quaternions store x, y, z, w, as gltf does (`glm::make_quat`). zpp::bits serializes glm's vectors by their
members, but not a matrix (its columns are private) or a quaternion (`GLM_FORCE_XYZW_ONLY` doesn't reach it: wherever glm
detects language extensions, `_MSC_EXTENSIONS` with clang on Windows, its members are an anonymous union, which structured
bindings can't bind): `core/glm_extra.h` serializes matrices column by column and quaternions as x, y, z, w (their
members' order, so the caches' layout is the same), so a header whose serialized structs hold one includes it. Changing an imported type from arrays to glm
changed the caches' last bits (glm sums products pairwise), so the importer tags were bumped; renders came out the same
but for a few pixels of skinned models, by 3 of 255 at most.

glm runs without its intrinsics, on purpose: the compiler already targets the best instruction sets
(`clang.toolchain.cmake`: `-march=native+crc+crypto` on arm64, `-march=x86-64-v3`, AVX2 and FMA, on x64), and vectorizes
glm's scalar code (on the M4 a `mat4 * mat4` or `mat4 * vec4` is whole columns of `fmul.4s`/`fadd.4s`, as glm's NEON
code would be). glm's own SIMD paths would cost more than they give: `GLM_FORCE_XYZW_ONLY` turns them off (without it,
vectors are anonymous unions, which zpp::bits can't serialize by members), and they only apply to aligned types
(`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` pads a `vec3` to 16 bytes, changing the caches' layouts and every `vec3` array).
If a glm heavy loop ever shows in a profile, use `glm::aligned_vec4`/`aligned_mat4` there. `-march=native` ties the
macOS binaries to the build machine's cpu: anything distributed needs a fixed one (e.g. `-mcpu=apple-m1`).

## Portable by default, measured on one

This code runs on several platforms and gpus (macOS through KosmicKrisp or MoltenVK, Windows and Linux on desktop
gpus). Measure the choices here, but pick the general method that is likely to perform well across them, not the one a
single driver happens to favor: e.g. the gpu culling compacts its draws and counts them (`DrawIndexedIndirectCount`,
native on desktop gpus) although KosmicKrisp emulates the count. Record the platform's numbers as what it costs there.
Where one platform can't do the general method well, it gets an exception named for what it lacks, decided in rhi
(`DeviceLimits::drawIndirectCountEmulated`), not a different design for everyone, and the exception says when to look
again.

## Naming: counts

A number that is a size or an amount of something is a count, and named so: `SHADER_TYPES_LIGHT_COUNT`,
`kChunkCommandCount`, `SPEEDO_SWAPCHAIN_IMAGE_COUNT` (it was `SPEEDO_SWAPCHAIN_IMAGES`). A switch is not:
`SPEEDO_INDIRECT_DRAWS=0`.

## Capacity limits: hard failures, not warnings

When a fixed capacity runs out (a buffer of draws, slots, lights, materials, textures, samplers, texture views, cluster
light indices, ui panels, the gpu profiler's ring, the task pool), fail hard with `ENSUREF` (`core/assert.h`: it traps
in every build, with the message and a stack trace; `CHECK` only breaks in the debugger), naming the limit. Don't warn
and drop the rest, truncate, or fall back to something slower: that hides the case, and leaves a render that looks
plausible but is wrong. When real content hits a limit, handle it then (a bigger or growing buffer, a budget per view):
an artificial case may keep the hard failure. What stays a budget by design is a policy rather than a capacity: the
shadow atlas gives the lights it can't fit no shadow (see "Shadows"), and the transparency's per pixel node pool drops
fragments past it on the gpu, where no hard failure is possible. The first real case found: NodePerformanceTest's 10000
materials (see "Asset import and testing"), so gMaterialData grows.

## Associative containers: core's, not the standard library's

Don't use `std::map`, `std::set`, `std::unordered_map` or `std::unordered_set` (tools included): use `core::UnorderedMap`
and `core::UnorderedSet` (`core/utils.h`, ankerl::unordered_dense). Where an order matters (e.g. a report), sort a
vector of the keys. Composite keys are small structs with a defaulted `operator==` and a byte hash (`XXH3_64bits`,
`using is_avalanching = void;`, a `has_unique_object_representations` static_assert), floats stored by their bits so
that equal keys hash equal (see `TextureViewKey`, `obj::detail::VertexKey`).

## File I/O: core's memory mapped files, not the standard library's streams

Read and write files through core (`core/file.h`): `core::file::Map` (a whole file mapped for reading) and
`core::file::Write`, or `LoadBinary`/`SaveBinary` and the zpp::bits serializers over them (`InputSerializer`/
`OutputSerializer`), not `std::ifstream`/`std::ofstream`, `fopen`/`fread` or a library's own file loading. Hand third-party
decoders the mapped bytes rather than a path: stb's `_from_memory` functions, `image::Import`/`Decode` and
`environment::Import`'s byte overloads, cgltf's `options.file` callbacks (`gltf::detail::Options`, which map the gltf file
and its buffers and unmap them when cgltf releases them), tinyobj's `LoadObj` from an istream over the mapped bytes
(`obj::detail::MemoryBuffer`, a streambuf over memory, and `MappedMaterialReader` for the mtl files). The zip extractor
maps the archive and writes entries with `Write`. `LoadAsset` maps an asset's source file before calling its source load
op: read the file from the serializer it gets (`in.remaining_data()`), as `LoadTexture` and `LoadEnvironment` do, rather
than opening it again. `src/tools/filebench.cpp` is the one exception, since std streams and stdio are what it measures.

Zip entries are inflated with libdeflate (`core::zip::Inflate`, raw deflate straight into the entry's file, mapped at
its known size) and checked with its crc-32 (`core::zip::Crc32`, the cpu's crc instructions), by a worker per executor
thread taking entries in turn (`core::zip::ExtractAll`, which waits with `TaskExecutor::Join`, so it may run in a task;
directories are created first, on the calling thread). Compared on San_Miguel.zip (511 MiB, 1.8 GiB inflated), libdeflate
inflated in 1.01 s where stb_image's decoder took 2.65 s (zlib 2.33, miniz 2.05, zlib-ng 1.63), and its crc-32 in 24 ms
where the byte-at-a-time table the extractor had took 3.2 s. Extracting it takes 1.0 s on 14 threads (1.7 s on one): one
entry, the 1.1 GB obj, is most of it, and a deflate stream inflates in order. `filebench --zip <archives>` times all of
that.

`Map` asks the os to read the whole file ahead (`fcntl(F_RDADVISE)` on macos, `posix_fadvise(POSIX_FADV_WILLNEED)` on
linux; nothing on windows yet): without it a cold mapped file is read a page fault at a time, which `filebench` measured at
0.6 GB/s against `read()`'s 7 on macos (256 MiB, M4 Pro; `MADV_SEQUENTIAL` doesn't help, `MADV_WILLNEED` only doubles
it). With it, mapping matches `read()` cold and warm (13 to 18 GB/s from the page cache). `std::istreambuf_iterator`
reads, which the image importer used, run at 0.5 GB/s either way. `filebench --import <files>` times the importers:
the change took a 9 MB png's decode from 77 to 58 ms and an 80 MB hdr's from 244 to 227; mesh imports are bound by
parsing (2 to 3% faster). For an image it also times the whole import (decoded, mipmapped and compressed) on one thread
and with an executor compressing: `image::Options::executor` spreads `CompressLevel`'s block rows over it with
`TaskExecutor::ForEach` (`LoadTexture` passes the application's; `std::execution::par` did nothing, libc++ runs it
serially), which took a 2048 Bistro texture's compression from 62 to 23 ms; the decode and the mip resizes are serial.

## Choosing a lock

Zip entries are inflated with libdeflate (`core::zip::Inflate`, raw deflate straight into the entry's file, mapped at
its known size) and checked with its crc-32 (`core::zip::Crc32`, the cpu's crc instructions), by a worker per executor
thread taking entries in turn (`core::zip::ExtractAll`, which waits with `TaskExecutor::Join`, so it may run in a task;
directories are created first, on the calling thread). Compared on San_Miguel.zip (511 MiB, 1.8 GiB inflated), libdeflate
inflated in 1.01 s where stb_image's decoder took 2.65 s (zlib 2.33, miniz 2.05, zlib-ng 1.63), and its crc-32 in 24 ms
where the byte-at-a-time table the extractor had took 3.2 s. Extracting it takes 1.0 s on 14 threads (1.7 s on one): one
entry, the 1.1 GB obj, is most of it, and a deflate stream inflates in order. `filebench --zip <archives>` times all of
that.

Measured on this machine (M4 Pro): `core::UpgradableSharedMutex` is the cheapest uncontended (~4-7 ns) and is the
only one with upgrade locks, but every unlock wakes all waiters, so it degrades badly under contention (8 threads
on one exclusive lock: ~430 ns vs `std::mutex`'s ~17 ns). So: `std::mutex` for exclusive-only locks
(`gDrawMutex`, `MemoryPool`'s free list, the object tracking maps); `UpgradableSharedMutex` where upgrade locks are
used (descriptor set state in `Pipeline`) and as `ConcurrentAccess`'s default, whose locks are short and mostly
uncontended (at 2-3 threads it and `std::shared_mutex` trade wins). Don't lock what can't change: `MemoryPool`'s
`GetPointer`/`GetHandle` are plain address arithmetic on fixed storage. The `TaskExecutor` wakes idle threads with
an atomic wake count (`myWakeCount`) rather than a condition variable: submitting takes no lock, and a thread
can't miss a wake between finding the queue empty and waiting. `Pipeline`'s pipeline map is a `std::mutex` and a condition variable
(`PipelineMapSync`): it is only touched when a draw changes pipeline variant (draws are sorted by variant), a few times
per view and phase, so an exclusive lock costs microseconds per frame; an upgrade lock would gain nothing. The code before
took no lock, waiting on the map entry with `std::atomic_ref`, which was only safe while one thread bound pipelines: the
map isn't safe to insert into concurrently, and rehashing moves its entries. Recording isn't expected to go wide enough for
a sharded node map (`phmap::parallel_node_hash_map`, where entries don't move and could be waited on again) to pay.

## Frame indices are slots, not swapchain images

A frame's index (`FrameInputs::frameIndex`, `PushConstants::frameIndex`, and every per frame buffer and descriptor
slot kept by it) is its slot of `SHADER_TYPES_FRAME_COUNT`, which the frames take in turn (`Draw`'s `myFrameSlot`);
`Draw` waits for the graphics timeline value the last frame in the slot signaled (`myFrameSlotTimelines`) before
anything writes the slot's buffers. The swapchain image `vkAcquireNextImageKHR` handed out (`Swapchain::Flip`) is the
frame's output index (`FrameInputs::outputIndex`, `PushConstants::outputIndex`), only for what is the image's:
ComputeMain's and the UI blur's storage view of it (`gRWTextures` slots below `SHADER_TYPES_OUTPUT_IMAGE_COUNT`), the UI
pass's render target and the capture. The frame index used to be the image: fifo hands the images out in turn, but
mailbox and immediate needn't (an image a newer frame replaced comes back at once), so a frame still on the gpu read
buffers the next frame wrote over: once, on Windows with mailbox (the default where the surface has it), Sponza's
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

## The frame graph: passes declare, the graph synchronizes

A frame is recorded by `gfx::Renderer` (`gfx/renderer.h`) as a `gfx::FrameGraph` (`gfx/framegraph.h`): passes, added
once in the order they run, each declaring what it does with which resource (`ResourceUse`: an image's layout, and the
stages and accesses), with an execute callback that records only its own commands. The passes now: Cull Clear and Cull (where the gpu culls; Depth Pyramid and Cull Late after the Prepass, and Prepass Late and Main Late, with occlusion culling; GI Clear, GI Shift, GI Voxelize, GI Keep, GI Light Static, GI Restore, GI Voxelize Moving and GI before Main with indirect lighting), OIT clear, Shadows, Scatter Back, Scatter, Scatter Blur, Scatter Denoise, Prepass, Shadow Trace Clear, Shadow Classify, Shadow Trace, Shadow Denoise X and Y (with ray traced shadows), GI Near, GI Near X and Y and GI Near Temporal (the indirect lighting's near field), Emitter Sample, Emitter Shade, Emitter X and Y and Emitter Temporal (emissive surfaces as lights), Main,
Transmission (blit and mips), Main Transmissive, Exposure Clear, ComputeMain (or, with temporal upscaling, Composite,
the Upscale passes and Sharpen), Exposure Readback, UI Blur X and Y, UI Uploads, UI, Present. Don't add barriers or transitions in a pass's callback for what it declared; declare the use instead.

Where a scene's frames go is a `gfx::RenderOutput` (`gfx/renderoutput.h`), which the scene and its renderer are given
rather than branching on: its extent, image count and per image storage views (ComputeMain's output, bound up front),
the target a frame is drawn into, `Prepare` after it is (re)created, and the passes that end a frame
(`AddFinalPasses`, which see the frame being recorded through `RecordingFrame`). `SwapchainOutput` is a window's
swapchain (looked up each time, since the rhi recreates it): the UI Uploads, UI and Present passes end its frames, and
`Prepare` sets its frames' load and store ops. `ImageOutput` is an image (RGBA8 unorm, storage, sampled and color
attachment, srgb encoded as the swapchain's) with a `RenderImageSet` over it, one for every frame (frames are
serial on the graphics queue): an Output pass leaves it in `kShaderReadOnly` for the UI to sample (`GetView()`), as
`Prepare` does before its first frame; its owner recreates it (`Resize(extent)`) and then has the scene resize. The
output's owner keeps it alive longer than the scene. The UI shows a GPU image it doesn't own through
`ImGuiLayer::AddTexture(view)` (an `ImTextureID` for `ImGui::Image`; the view must be in `kShaderReadOnly` whenever a
frame drawing it runs, as an `ImageOutput`'s is) and `RemoveTexture(id)`, both on the ui thread: the descriptor set is
released as imgui's own textures are, once the draw thread has drawn a ui frame published after the removal and the gpu
is done with it, and freed back on the ui thread (the backend's pool, 1024 sets, is the ui thread's).

The ui's frosted panels, as macOS's: every top level window shown with a background (the main menu bar's, the dialogs
such as the file browser and Lights, menus, popups, tooltips; not child windows, which are inside theirs, nor windows
without a background, which would show the blur alone; a collapsed one by its title bar) is translucent (`WindowBg`,
`TitleBg*`, `MenuBarBg` and `PopupBg` with alpha) and rounded, over its backdrop blurred, with the controls in a macOS
dark palette: faint white buttons, fields and scrollbars, macOS's blue for what is hovered, open, selected or pressed,
rounded frames (all `ImGuiLayer`'s style). `ImGuiLayer::EndFrame` collects their windows' rectangles and corners (in
framebuffer pixels: `UiBlurRect`) with the frame's draw data, so a panel and its blur are of the same ui frame; `Draw`
takes the frame (`TakeFrame`) before recording and hands them to the scene (`FrameTarget::uiBlurRects`). `ComputeMain`
also writes its finished linear color within the blur's reach of a panel to a transient
(`SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE`), and the UI Blur X and Y passes (`uiblur.slang`: a 24 pixel gaussian in 25 reads
per pass, the taps paired into one linear filtered read between each two, at the offset weighing them as the gaussian
does, then a little more saturation, as macOS's vibrancy; the same image as 49 reads, to 3 in 255) write it back over
the output inside each panel's rounded rectangle, antialiased (the most coverage of any panel, so overlapping panels
write the same), before the UI pass. Each pass is one dispatch of a thread per pixel of the panels in turn
(`UiBlurRect::firstThreadX`/`Y`, filled in by the renderer; X's over the blur's reach above and below each too). The
swapchain images aren't sampled images, and reading them as storage images needs `shaderStorageImageReadWithoutFormat`,
hence the copy. `BeginMainMenuBar`'s window draws its own `WindowBg` (85% opaque in the classic style) under the bar:
the ui clears it. ImGui draws menu items' highlights square (macOS's are rounded and inset), and what the ui draws
itself behind the windows (the light gizmos, a background draw list) shows through them sharp, not blurred. A full
window of blur (the browser, 2400 by 1600 pixels) cost no measurable frame time (3000 frames of Bistro unthrottled, 4.38
s either way). `SPEEDO_OPEN_MENUS=View/Presentation` holds menus open, for screenshots.

The ui fades out when it isn't in use (`PrepareDraw`): a second after the mouse leaves the menu bar with no other window,
menu, popup or tooltip up, over half a second, and back in over 0.15 s when the mouse is over the menu bar or one comes
up (a shortcut's, a load's choice). `ImGuiLayer::Fade(opacity)` fades the windows begun after it, by imgui's style alpha
(its colors are multiplied by it as they are drawn), and their frosted backdrops (`UiBlurRect::opacity` scales the
blur's coverage); the ones begun before keep theirs, so the client draws what stays first: the loading bars, the FPS and
TPS overlay and the move speed. Which windows are whose comes from imgui's begin order (`BeginOrderWithinContext`
against the active window count when `Fade` ran), not their names. `ShowedFadingWindows` says whether the last frame had
a faded window other than the main menu bar, which keeps the ui up; it counts hidden windows too, since imgui hides a
window begun at a style alpha of 0 (a browser opened by its shortcut once the ui had faded out stayed open, invisible,
and the next press closed it). Faded windows still take input: the invisible menu bar is under the mouse when it fades
back in.

The menus' shortcuts (Ctrl, cmd on macOS: Open, Open Environment, Exit, the layouts) are checked in `PrepareDraw` with
`ImGui::Shortcut(..., ImGuiInputFlags_RouteGlobal)`: a `MenuItem`'s shortcut is only a label. Escape closes the focused
window (`CloseOnEscape`, called between its `Begin` and `End`: `Shortcut`'s default route is the focused window's).

- **Barriers come from tracked state, at run time.** `Execute` keeps each resource's last write and the reads since (across
  frames too, which are serial on the graphics queue), and before each pass emits one batched memory barrier for the
  hazards and an `Image::Transition`/`IRenderTarget::Transition` per layout change (whose masks come from the layouts).
  So a disabled pass (`SetEnabled`: the transmission passes without transmissive materials) changes nothing else.
  Layouts are the image's own tracked ones: a callback may leave an image elsewhere (`GenerateMips` leaves
  `kShaderReadOnly`), and the next transition starts from there.
- **Transients** (`CreateImage`/`CreateBuffer`: the main color and depth, the transmission texture, the OIT lists) are
  created by `Compile` for the frame's extent (again on resize, with the gpu idle) with the usages their uses imply,
  and placed in `rhi::MemoryBlock`s (images and buffers apart, per memory type set), largest first at the lowest offset
  overlapping no placed transient whose pass lifetime overlaps (`Image`/`Buffer`'s placing constructors,
  `vmaCreateAliasing*2`; requirements from a temporary resource, since Vulkan 1.2 has no desc query). Their contents
  don't outlive the frame: the first use in a frame must write them (`Compile` checks: no read, no `kLoad`), and is
  preceded by `Image::Discard` and a barrier from whatever last used the same bytes, but a persistent one's
  (`ImageDesc::persistent`: the shadow atlas, whose tiles are kept from frame to frame), which is placed where no other
  transient's bytes are and keeps what the last frame left, once a frame has used it after `Compile` made it
  (`IsPersistentValid`). Views exist for sampled or storage transients only. The Statistics window shows the bytes,
  aliased and not.
- **Imported** resources (the swapchain, the exposure histograms) are bound each frame (`Bind`); passes writing them are
  never culled (others: only if something needed reads what they write, or `SideEffects`).
- **Depth stencil formats** attach both: `RenderTarget`'s dynamic rendering took a combined format's depth branch
  alone, so the stencil was never attached and what draws wrote to it went nowhere (found by the depth reprojection,
  whose stencil read garbage). The stencil's load and store ops are the depth's (the frame graph sets both).
- **Raster passes** declare attachments (`ColorAttachment`/`DepthAttachment` with load and store ops): all transients
  (the graph makes a `RenderImageSet` over the shared images, one per attachment list) or all of one bound render target
  (the swapchain). The graph sets the ops, begins it (inline or secondary contents) around the callback and ends it.
  `RenderImageSet` shares its images (`std::shared_ptr<Image>`), rather than owning them. One render target now serves consecutive
  frames (there used to be one per frame index), so nothing it holds may be recreated while earlier frames' command
  buffers reference it: `InternalUpdateAttachments` recreated the depth view on every transition (it compared the
  aspect before reducing depth and stencil to depth), and KosmicKrisp crashed resetting the command pool (an
  `objc_release` of the freed view, at random in debug). Each pass is a gpu scope of its own (see "GPU timings"):
  don't add a `GPU_SCOPE` in a pass's callback.
- **Draws** are data: `BuildDrawList` turns the model into `DrawItem`s for a main pass phase (pipeline variant, cull mode,
  front face, material slot, instance range, skin and morph bases), and `RecordDrawList` records them for a view, which
  the main pass does per view in the queue pool's secondary command buffers. A draw's per draw values are a `DrawRecord`
  (material, first instance, skin and morph bases) in the frame's `gDrawRecords[frame]` (host visible, per frame index),
  written in `Renderer::Record` once the frame's lists are built (`WriteDrawRecords`, which sets each item's `record`).
  The buffer starts at `SHADER_TYPES_DRAW_RECORD_COUNT` and grows to the next power of two that holds a frame's draws
  (NodePerformanceTest's two encodings side by side have 40000), the old one kept until the frames in flight, whose
  descriptor sets name it, are done (`myRetiredBuffers`). Each draw names its record by its first instance, which the
  vertex stage reads as `SV_StartInstanceLocation` (shader draw parameters, `VkPhysicalDeviceShaderDrawParametersFeatures`;
  `SV_InstanceID` doesn't count it), and the vertex stage hands the material on (`VertexOutput::materialId`, flat); the
  push constants only say the view (`viewId`). Each run of draws with the same pipeline and state is one
  `DrawIndexedIndirect` of their commands, which the frame index's `DrawStream` hands out in recording order from chunks of
  `kChunkCommandCount` host visible commands, made as a frame needs them, so there is no limit to what a frame records
  (`SPEEDO_INDIRECT_DRAWS=0` records direct draws instead). A slot table (slot to record, read at `SV_DrawIndex`) came
  first, but a frame's recorded draws are only known once recorded, so it couldn't grow, and the shadow views multiply
  them. Renders are the same either way. On KosmicKrisp, measured on Bistro: multi-draw indirect costs nothing over
  direct draws (Shadows 4.18 to 4.03 ms looking down a street, 3.22 to 3.12 framed), `DrawIndexedIndirectCount` (the
  count read from a buffer) about 0.8 us per draw more (Shadows 4.03 to 4.53 ms: its four cascades' 600 draws), and a
  draw of no instances in a plain indirect draw about 0.2 us: Metal has no draw count indirect, which KosmicKrisp
  emulates. Elsewhere (desktop Vulkan, D3D12) the count is read natively, so the gpu culling (TODO.md) compacts the
  draws it keeps and counts them, the general method, and KosmicKrisp's cost is known rather than designed around.
  Renders came out the same as with push constants, but for one byte of ScatteringSkull's (by 2 of 255, in every run).
- **The gpu's culling** (wherever the draws are indirect; `SPEEDO_GPU_CULLING=0` hands it to the cpu, `SPEEDO_CULLING=0`
  turns culling off on either path). Each list's items are cut into runs of the same pipeline variant, cull mode and front face
  (`PlanDrawRuns`, at most `maxDrawIndirectCount` each; a specialized variant whose pipeline isn't made yet draws its run
  with the generic one). `InternalPlanCulling` writes the candidates (`CullCandidate`: bounds, cullable, record, the
  command's values, its run and the run's first item), a job per list and view drawing it (`CullJob`: the main pass's
  phases and the scatter lists by the main views, the shadow list by the shadow views) and the views' planes (`CullView`).
  The Cull Clear pass empties the counts, the Cull pass (`ComputeCull`, `cull.slang`, compute entry point 5) runs a
  thread per candidate and job: one in view takes the next place of its run (`InterlockedAdd`) and writes its command
  there, and the drawing passes (which declare `ResourceUse::Indirect()` on the commands and counts) draw each run with
  one `DrawIndexedIndirectCount` (`RecordCulledDrawList`). The candidates, commands and counts grow as the records do
  (`GrowingBuffers`). The order of a run's draws is the order threads got their places, which only shows between
  coplanar opaque surfaces; renders came out identical to the cpu's (seven models, ScatteringSkull and DragonAttenuation
  among them); the debug quick tier passes with it, under validation. Where the count is emulated
  (`DeviceLimits::drawIndirectCountEmulated`, rhi's: KosmicKrisp, by its driver id), or missing, nothing is compacted
  or counted (below), since there a count costs: `kk_CmdDrawIndexedIndirectCount2KHR` (mesa's `kk_cmd_draw.c`) runs a compute kernel per call that zeroes the
  commands past the count (`kk_predicate_draws`: in a render pass, a Metal encoder split, about 55 us with the shadow
  atlas) and then draws every one of `maxDrawCount`. ThousandsOfLights' 256 shadow views, a call each: Shadows 1.0 ms to
  15.1; Bistro's Main 4.03 to 4.31 framed. Mesa main of 2026-10-19 (44fd0fde) still does that, with a TODO to build Metal
  indirect command buffers on the gpu instead: once it does, measure again and drop the exception. Uncompacted
  (`CullJob::compact` 0, `CulledDraws::counted`), every draw keeps its place, of no instances if culled, and each run is
  a plain `DrawIndexedIndirect`: on KosmicKrisp that costs nothing measurable (ThousandsOfLights' 256 shadow views and
  Bistro's, the cpu's culling against it: the same Shadows time to 0.02 ms), so every list is culled on the gpu there too
  (the shadow and scatter lists stayed the cpu's for a while). By the frustum alone the gpu culling saves no gpu time
  anywhere (the cpu's draws are the same ones): it saves the cpu's recording, and occlusion (below) is where it pays.
- **Occlusion culling** (two phase, `SPEEDO_OCCLUSION_CULLING=0` turns it off; where there is a count, emulated or not,
  and culling is on): the main pass's opaque list is culled on the gpu in two jobs per view. The Cull pass's
  (`CULL_JOB_EARLY`) keeps the draws seen last frame (`gCullVisibility`, a word per draw and main view, one buffer kept
  from frame to frame: the main list comes first, so its place holds while its draws and the views do; it grows, which
  loses it once), and the Prepass draws them (see "The pre-pass", below: it took this role from Main, which shades over
  its depth, the early draws in Main and the late in Main Late). The Depth Pyramid pass (`ComputeDepthPyramid`, compute entry point 6) reduces the
  main depth (`SHADER_TYPES_MAIN_DEPTH_TEXTURE`) into an `R32Sfloat` mip chain of the farthest depth, half its size at
  level 0 (`FrameGraph::ImageDesc::downscale`), each texel the farthest of 3x3 below so odd sizes are covered, a dispatch
  per level with barriers between, written and read as storage images of a format (`gDepthPyramid`, binding 1 of the rw
  textures' set: reading without one needs `shaderStorageImageReadWithoutFormat`, which KosmicKrisp lacks). Cull Late
  (`CULL_JOB_LATE`) tests every draw in view against it (`CullOccluded`: the box's corners projected into the view's
  viewport, its nearest depth against the farthest of the at most 2x2 texels of the level where its rectangle spans two;
  a box reaching behind the eye is never hidden), keeps the ones it doesn't hide that weren't drawn first, and writes
  each one's visibility for the next frame; Prepass Late draws them, loading the attachments (and Main Late shades
  them). Blended draws
  (`CullCandidate::late`) are only ever drawn late, after every opaque draw (the transparency's lists depth test when
  written), and the transmissive phase's list is tested against the pyramid alone (`CULL_JOB_PYRAMID`). Renders are identical with and without (eight models, BlendInstances among them),
  and with a still camera Main Late draws nothing. Measured on KosmicKrisp by the frame's gpu time (the `Frame` scope,
  see "GPU timings"), off to on: Sponza 5.34 to 4.83 ms (Main 3.78 to 1.78), Bistro 7.67 to 7.94 framed, 8.58 to 9.05
  and 11.67 to 11.79 in its streets, ThousandsOfLights 9.78 to 10.26. Where little is hidden it costs: the second render
  pass loads and stores the main color and depth (a tile based gpu's cost; a desktop gpu's is lower), the pyramid takes
  0.14 ms (0.28 from a full size level 0), and the cull passes. It is the general method, so it stays on.
- **The shadow views' occlusion** (`SPEEDO_SHADOW_OCCLUSION_CULLING=1`; off by default): the same two phases in each
  light's view, before Scatter and Main sample the atlas. Shadows draws the casters each shadow view saw last frame
  (`CULL_JOB_EARLY`, the visibility after the main views'), the Shadow Depth Pyramid pass reduces the atlas
  (`gShadowDepthPyramid`, half its size at level 0, `PushConstants::depthPyramidSource` 2), Cull Shadows Late tests the
  rest against it and Shadows Late draws them, loading the atlas. One pyramid serves every tile: tiles are power of two
  squares aligned to their size (`shadows::Plan`, checked), and a view reads no level coarser than its tile is a texel of
  (`CullView::maxLevel`; `CullView::pyramid` says which pyramid). The cull jobs are bounded by design (`kCullJobCount`:
  the main lists' per main view, the shadow list's early and late per shadow view); ThousandsOfLights' 256 shadow views
  overflowed it before it counted both. Renders are identical with and without, and to the cpu's culling. It is off
  because it hides almost nothing at the chunks' size: a shadow view sees few chunks behind others (Bistro 6 to 11 of
  560 to 800 draws, ThousandsOfLights and ManyLights none), while the atlas's second render pass and its pyramid cost
  0.5 to 1 ms a frame on KosmicKrisp (Bistro framed 8.2 to 9.1 ms, Sponza 4.8 to 5.1). Smaller chunks for shadows, or a
  pyramid of the tiles that are drawn only, might change that.
- **Occlusion modes** (`SPEEDO_OCCLUSION_MODE`): `two-phase` (the default, above), `reprojected` and `hybrid`, which take
  the last frame's depth instead of drawing first what it saw. Each frame saves its depth of what doesn't move (Depth
  Save, `ComputeDepthSave`, compute entry point 7, into `gPreviousDepth`, a float per pixel kept from frame to frame and
  remade on resizes): what moves is at the far plane, since its depth is last frame's place, marked in the main depth's
  stencil (`CommandEncoder::SetStencilReference`, dynamic: 1 for draws that aren't cullable, written by every pipeline
  that writes depth; `gMainStencil` reads it). The next frame pushes each saved pixel to where its point is seen now
  (Depth Reproject, `ComputeDepthReproject`, entry point 8: the view's last inverse view projection, then the current
  view projection; `CullView::previous*`), the nearest landing on a pixel kept (`InterlockedMax` of the float's bits, the depth being reversed, into
  `gReprojectedDepth`, a transient cleared to the far plane, 0), and builds the pyramid from it (Depth Pyramid Reprojected,
  `PushConstants::depthPyramidSource`). Every point pushed is on a surface that doesn't move, so it is no nearer than
  what the pixel sees now; pixels nothing lands on (revealed, or the gaps when coming closer) hide nothing; rounding is
  made up for by a bias farther and the pyramid's 3x3. `reprojected` culls once (Cull Reprojected, `CULL_JOB_PYRAMID`)
  and draws once: no Main Late, no second render pass. `hybrid` draws first what the reprojected depth doesn't hide
  (`CULL_JOB_HYBRID_EARLY`, marked in the visibility), then a pyramid of what was drawn tests the rest
  (`CULL_JOB_HYBRID_LATE`), drawn in Main Late: exact as two-phase. All three render identically to culling off. The
  cull kernel counts its decisions (`gCullStatistics[frame]`, `CULL_STATISTIC_*`: drawn early, late, late blended,
  hidden), which the Statistics window shows and `SPEEDO_GPU_TIMINGS=1` averages at exit; hybrid's opaque late draws
  are what the reprojection alone would have hidden wrongly (popping in `reprojected`). `SPEEDO_CAMERA_TURN=<degrees
  per second>` and `SPEEDO_CAMERA_WALK=<units per second>` move the views in automated runs (`Views::Turn`). Measured on
  KosmicKrisp (the frame's gpu time, still, turning, walking, and both, on Sponza, Bistro framed and in a street, and
  ThousandsOfLights): hybrid's opaque late draws were 0 in every run, so the reprojection never hid what was seen;
  `reprojected` costs the same as `two-phase` (within 0.1 ms either way; it was 1 ms faster in one Sponza session,
  the same in another: the gpu clock drifts between sessions), and hides less while the camera turns (Sponza 0.4 draws
  against 1.3, Bistro's street 2.8 against 6.8: the revealed edges hide nothing); `hybrid` costs 0.35 to 0.45 ms more
  than either (both pyramids and the reprojection, and still the second render pass). On a tile based gpu the second
  render pass is what two-phase pays; the reprojection pays as much in Depth Save and the scatter's atomics. A
  `reprojected` miss would show as a draw missing for a frame, never for longer: its pixels then save the depth behind
  it.

Sync validation is available (`SPEEDO_VALIDATE_SYNC=1` in a debug build, with `VK_LAYER_PATH` set as `assettest.ps1`
does) but sees no hazards on the main layout's descriptors (all partially bound), even with the graph's barriers
removed, so it can't vouch for them; compare renders instead (identical before and after the graph).

## Debug views and the Statistics window

View > Debug View (`SPEEDO_DEBUG_VIEW=<key or number>`; Ctrl/Cmd+[ and ] step through them, +0 back to the light)
shows something other than the light in the main pass: `PushConstants::debugView`, `DEBUG_VIEW_*` in `capi.h`, keys in
`client/ui.cpp`'s `kDebugViews`, helpers in `debug.slang`. The surface's values (base color, normals, roughness,
metallic, occlusion, a color per material, texcoords) return early from `ShadeFragment` (`DebugSurfaceValue`); the
lighting's (direct alone, ambient alone, its diffuse on white and its specular, the shadows' share of the direct light,
the cluster's light count as a heat map) and the indirect lighting's (from `DEBUG_VIEW_GI_FIRST`, `DebugViewGi`, which
the renderer also writes to `GiData::debug` for the references' compute) return from `Shade`, without emission and the
scattering's light. Views of values (`DebugViewIsData`) skip exposure and tonemapping in `ComputeMain` (stored as
`pow(v, 2.2)` where a value should read as itself after the srgb curve) and have a dark backdrop instead of the sky.
The scatter pass is never in a debug view. An overlay at the bottom left names the view and what its colors mean.
Indirect lighting's views beyond the per pixel lookups: Voxel World (radiance, albedo, normal: `ComputeMain` marches
each pixel's ray through the clips' voxels, `GiTraceVoxels`, what the probes' rays see) and Probes (the 8 probes
around a point, trilinear, without the resolve's weights). The traced shadows' (see "Ray traced shadows"), of the first
traced light, magenta where none is traced: as the main pass takes them (`traced-shadow`, filtered), the rays as traced
(`traced-shadow-raw`, a ray a pixel), and a reference (`traced-shadow-reference`: 256 rays a pixel over the light's
disk where the atlas isn't sure, `SHADER_TYPES_SHADOW_REFERENCE_RAY_COUNT`; seconds a frame in Bistro, so capture it
after a few frames). The last two go through the filter's passes unfiltered into the mask, which is what the main pass
reads: the rays' image is a transient whose memory the mask takes once the filter has read it.

For comparing renders of a move: `SPEEDO_CAMERA_PATH=<x,y,z,pitch,yaw>[;...]` (or `@<file>` of them, separated by `;`
or lines: a still pose repeated for 1200 frames is more than a Windows environment variable holds) places the views at a pose a frame,
the last kept (`Views::FollowPath`, applied by `Upload`: the draw calls the ui queues needn't run in their order, since
`RHI::drawCalls` keeps it per producer thread only and the ui's frames run on any of the executor's, so placements
queued in different frames landed out of order); `SPEEDO_CAPTURE_PATH=<directory>` saves each frame of the path
(`pose-NNNN.bmp`; `SPEEDO_CAPTURE_PATH_FIRST=<pose>` from that pose on, to measure a still pose's frames once what
settles over frames has; `SPEEDO_CAPTURE_PATH_STRIDE=<n>` every n-th of those, for a long move); `SPEEDO_GI_EYE_PATH=<x,y,z>[,<look x,y,z>][;...]` (or `@<file>` of them, one a line: an
environment variable on Windows holds 32767 characters) centers the clips on a point a frame instead of the camera, so
that under a still camera what changes is what the clips' moves change, and with the look a run's clips replayed exactly
(`SPEEDO_GI_EYE_LOG=1` prints each frame's as it takes them, and each clip's scroll); `SPEEDO_GI_REVOXELIZE=1` voxelizes every clip again every
frame, what their scrolling must come to. `SPEEDO_TIME_FRAME_SECONDS=<s>` advances the time of day by that many real
seconds a drawn frame once a model is installed (times `SPEEDO_TIME_SPEED`), rather than by the real time between frames,
so that two runs have the sun where it was at the same frame whatever their loads and frames took.

The Statistics window (debug and profile builds) has the pass timings in two columns, then: per draw list its draws
and triangles; the culling of the main and the shadow views (`Renderer::GetFrameStatistics` for what was tested, the
cpu's plan of the last frame recorded, and `GetCullStatistics` for the gpu's decisions of the last frame it finished,
a frame or two behind): tested, outside the frustum, occluded, drawn early and late, triangles drawn. The cull kernel
counts `CULL_STATISTIC_FRUSTUM` in the job that decides last on a draw (frustum, late, pyramid and hybrid late jobs) and
`CULL_STATISTIC_TRIANGLES` wherever a draw is kept; frustum only jobs count their draws as early (they were left out
before, so a list without occlusion counted nothing). Then the lights, shadows and clusters (most lights in a cluster,
the mean where any), and the indirect lighting's clips (voxel counts and sizes, how many were voxelized this frame,
what moves). `SPEEDO_GPU_TIMINGS=1`'s exit summary has the frustum and triangle counts too. The cpu culling path
(`SPEEDO_GPU_CULLING=0`) has no gpu statistics.

## GPU timings and shader tiers

Gpu work is timed by one path: each queue's `rhi::GpuProfiler` (`rhi/gpuprofiler.h`, `Queue::GetProfiler`; only with
`SPEEDO_PROFILING_LEVEL > 0`: debug and profile, else null), a ring of 4096 timestamps (KosmicKrisp's query pools are
Metal counter heaps of at most 4096) in the order they were recorded. A scope (`Begin`, ended when dropped or by
`Scope::End`) writes a timestamp at each end and opens and closes a Tracy gpu zone at record time (Tracy's C api: a
context of the queue's, `___tracy_emit_gpu_new_context` with a first timestamp, uncalibrated, as `TracyVkCtx` was).
`GPU_SCOPE(cmd, queue, tag)` is one with a static name; the frame graph opens one per pass, named after it and in the
graph's timing group (`FrameGraph::GetTimingGroup`), so its callbacks have none of their own. `Collect` (in `Draw`,
after the frame's fences) reads what the gpu is done with (up to the first timestamp that isn't, and never half a
grouped scope), gives Tracy every one, frees them from the host (`vkResetQueryPool`, Vulkan 1.2), and keeps each grouped
scope's duration, which `Renderer::Record` takes (`TakeTimings`), shows in the Statistics window, and with
`SPEEDO_GPU_TIMINGS=1` prints each pass's median of at exit, and the draw thread's median frame time.
`SPEEDO_CAPTURE=<file>` saves the frame an autoload exit ends on (`SPEEDO_AUTOLOAD_EXIT`) as a bmp, read back from the
swapchain image (its images are transfer sources for it): renders can be compared without the screen, which a locked
one hides from screenshots. `SPEEDO_UI=0` leaves the ui out of the frames (and its panels' blur; it still runs: the
autoloads, the camera's placement and the capture itself), so a capture is the scene alone. `SPEEDO_WINDOW_SIZE=<width>x<height>`
makes the window's framebuffer that many pixels whatever the display's scale (the default, 1280x720 in the window
system's units, is 2560x1440 pixels on the Mac's display and 1280x720 on a Windows display at 100%), so that
measurements match across machines. `Renderer::Record`
also opens a `Frame` scope (in the same group) around the whole graph, whose timestamps are outside every pass: it is
timed where passes aren't (KosmicKrisp times neither compute passes nor Main Late). It misses most compute work there
too, though: Bistro's evening frame read 43.4 ms by it and 67.3 ms by the draw thread, which a sampling profiler showed
waiting on its fences (the gpu's time), and leaving out any GI pass didn't move it, where San Miguel's agreed within 2
ms. So on KosmicKrisp measure a frame by the draw thread's frame time (fifo, printed at exit; check that the draw thread
waits on its fences; the half seconds the lighting warms up in, `Renderer::IsLightingWarmingUp`, the surface cache's
first 256 frames of 4 rays a voxel, are printed apart, as "frame time warming up": they cost Bistro 16 ms a frame, and a
run of 400 frames measured mostly them), and a pass by the frame time it frees left out (`SPEEDO_SKIP_PASSES=<pass>[;...]`, the GI pass's
steps by name: `framegraph::Skipped`; a pass whose output decides another's work changes that too). The costs of compute
heavy features recorded here by the `Frame` scope are undercounts on KosmicKrisp: the near field's 0.9 ms was 16 to 19
ms at the benchmark poses. `scripts/perfbaseline.ps1` measures features, passes and how long the lighting takes to
settle (`build/<preset>/convergence`, from captures) at the benchmark poses (docs/performance-plan.md, with the M4 Pro's
baseline). The frame time unthrottled is no substitute for the gpu's on macOS: presentation stays at the display's rate even with
`SPEEDO_PRESENT_MODE=immediate` (in the background, at least), so frames take one or two refreshes. A timestamp is plausible if it is not 0, not before the one
before it in its command buffer, and not more than 250 ms after it: Tracy gets the one before in place of the others,
and a grouped scope is timed only if both of its are plausible. KosmicKrisp only writes timestamps where its own Metal
command encoders begin and end: passes inside one encoder read 0, and the others are unwritten, which it reports as
available (`VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, which `QueryPool::Read` asks for, per timestamp) with 0 or a value
written frames before. Taking those, the UI pass's time grew with the run time (its begin stale, its end current), and
reading without availability (`VK_NOT_READY` for the whole range) dropped every pass of most frames, so the Statistics
window flickered. Where the timestamps go matters there: outside a render pass KosmicKrisp writes them from a compute
encoder (Metal 4's counter heap, resolved when the encoder ends), and once that encoder also holds dispatches its
samples read 0 (`ComputeMain`, the UI blur passes), and the last before the UI render pass a value about a second old.
Inside a render pass with inline contents it samples the render encoder's stages: so a raster pass recorded inline has
its scope inside its render target (`InternalRecord`: top of pipe after it begins, bottom of pipe before it ends), its
own work, and the others around it (one begun for secondary command buffers, as the main pass's, can hold none). The UI pass reads 0.015 ms with the menu bar alone, 0.09
with the file browser's grid, and Shadows 3.67 rather than 3.9 (without the transitions around it). The Statistics
window (View > Statistics, or `SPEEDO_SHOW_STATISTICS=1`; in debug and profile builds, the vulkan object counts in debug
only) shows each pass's moving average over the frames that timed it (factor 0.1), for 2 seconds after it was last
timed; `SPEEDO_GPU_TIMINGS` prints the raw frames' medians. The GPU's clock drifts between runs, so compare a pass
against a pass the change doesn't touch (e.g. Main against Shadows).

Shader tiers: `GraphicsPipelineVariant::specialization` is the shaders' specialization constant 0, `kSkippedLayers` in
`bindings.slang`, the layer groups (`SHADER_TYPES_LAYERS_*`) a pipeline compiles away. `SHADER_TYPES_LAYERS_EXTENDED`
(clearcoat, sheen, transmission, anisotropy, iridescence, diffuse transmission, gated by `Layer()`) is skipped for the
draws of materials without any of them (`LayersOf`, counting animated layers), so a plain PBR material doesn't pay the
uber-shader's register allocation for them: Sponza's main pass took 13 to 23% less (its time over the shadow pass's,
across runs; single runs are noisy, one pair showed almost no difference). `SPEEDO_SHADER_TIERS=0` draws with the
generic pipelines. Pipelines take 15 to 50 ms each to create cold on KosmicKrisp, so a specialized one is never waited
for: `RecordDrawList` asks `Pipeline::RequestGraphicsPipeline`, which returns it if it exists, or the first time a job
creating it from a copy of the current graphics state (`GraphicsSnapshot`, its pointers aimed at its own storage), which
runs on the task executor (`FrameInputs::runInBackground`) while the generic pipeline draws. The pipeline map is guarded
by a mutex and a condition variable (`PipelineMapSync`): a null handle is a pipeline being created, which
`BindPipelineAuto` waits for. Each phase's draws are sorted by pipeline variant (opaque ones depth test, blended ones are
order independent), so pipelines switch once per variant.

## Views: uploaded per frame

`Views` (`gfx/views.h`) keeps a camera per view, which the input, resizes, the grid, framing and the scene's cameras
change from any thread, and a view buffer per frame index (`gViewData[frame]`). `Upload(frameIndex)` writes only the
recording frame's buffer, from the cameras as they are then, and returns those cameras: `Scene` calls it while recording
(after the frame's fence), and culls, fits the shadows and plans the clusters by them, so the cpu and the gpu see one
camera. It replaced `UpdateBuffers`, which wrote every frame index's buffer whenever a camera changed, including frames
still in flight, so a frame could draw with a newer camera than it had been culled by (a chunk at the screen's edge
missing for a frame while the camera turned). Nothing else uploads the views: a change shows in the next frame.

## Depth: reversed

Depth is reversed: 1 at the near plane, 0 at the far (or at infinity), the cameras' and the shadow views' projections
taking gl's clip space to vulkan's as `kClip` does (`camera.cpp`, `shadows.cpp`: y down, and z' = (w - z) / 2), the depth
test keeping the greater (`rhi::DepthTest::kNearer`, `VK_COMPARE_OP_GREATER`; `kEqual`, the main pass's over the
pre-pass's depth, greater or equal) and depth cleared to 0 (`ClearValue`'s default, the frame graph's, `RenderImageSet`'s,
the shadow tiles'). A float's exponent then holds the precision where a perspective depth has none, near 0: the distance
two surfaces can be told apart at grows with the distance, not with its square. Standard (0 near, 1 far, a float's
mantissa near 1 alone), a window's shutter and the lit interior a few centimetres behind it fought 50 m away (the user's
night time lapse over Bistro, `-827.3380,155.1481,33.8635,-9.897,72.297`, the window's mean change between frames 6.49 of
255 at native and 0.74 at performance, 0.12 at both reversed), as did a sign's lettering and its board, and dots on a
ledge. What reads depth by its convention: the sky (`depth <= 0.0`: nothing drawn), the depth pyramid (the farthest is the
smallest: `min`) and occlusion (`CullOccluded`: a box's nearest depth is its largest, hidden when below the farthest
under it; a corner nearer than the near plane, `clip.z > clip.w`, isn't), the depth reprojection (an `InterlockedMax` of
the bits, a transient cleared to 0, the bias farther a factor), temporal upscaling (FSR's inverted depth:
`FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH`'s comparisons, its reconstructed depth an atomic maximum), the shadow atlas
(a blocker's depth is larger than the point's; `ShadowViewDistance` unprojects a depth along its view), the transparency's
sort key (the depth's float bits taken from the near plane's, increasing with the distance) and the backdrop's rays (from
depth 1, the near plane, to 0.5). The frustum's planes swap: `planes[4]` (z >= 0) is the far one and `planes[5]` the near,
which the clusters slice from. Unprojections through the inverse view projections need nothing, and are more precise:
what starts from a point unprojected from the depth (the traced shadows' rays, the near field's and the emitters') was
off by a standard depth's step along the view, 7 mm at 30 m with a near plane a ten-thousandth of the far (the views'
framing, `Views::FrameBounds`), and Sponza's framed sun shadows came out a pixel lower along its facade than they do now,
closer to where the atlas alone puts them (which shades the fragment's own position). Against the same tree
with standard depth (2560x1440, captures of 14 scenes and settings: Bistro at night and in its streets, Sponza under each
occlusion mode, CornellBox, ShadowScene, ManyLights, the blending, scattering and transmission tests): the same means,
each scene within 0.03 of 255 a pixel where nothing fought (Bistro 0.1 to 0.3, its noise), and the same draws culled and
hidden (Bistro through its arch 46.8 a frame two-phase, 46.5 reprojected and hybrid; the shadow views' occlusion 11.9).

## Temporal upscaling

View > Upscaling (`SPEEDO_UPSCALING=off|native|quality|balanced|performance`, `gfx::Upscaling` in `gfx/upscaling.h`;
Copy Pose writes it where it isn't the default; balanced by default since 2026-10-29, the user's call once FSR 3.1 was
ported, `kDefaultUpscaling`, the file browser's preview too; `SPEEDO_UPSCALING=off` for measurements of the frame at the
output's size): the frame rendered at a share of the output's size each way (`RenderScale`: 1,
0.77, 0.67, 0.5) and reconstructed at it over frames by AMD's FSR 3.1 upscaler, ported (`upscale.slang`); native
reconstructs at the output's size, its antialiasing alone. `Scene::Settings::upscaling`; a change resizes the render
targets before the next frame (`Scene::NeedsResize`, which the client's `Draw` checks with the gpu idle, as for a
window's resize). The frame graph has two extents (`FrameGraph::Compile(device, extent, outputExtent)`): the transients
are the render size's, but those sized after the output (`ImageDesc::outputSized`: the ui's frosted panels' backdrop).
The views' viewports are scaled into the render target (`ScaleViewport`, in `Views::Upload` for `ViewData::viewport` and
in the scene for `FrameInputs::viewports`, the same rounding), and their projections offset within a pixel, a point of
Halton's (2, 3) a frame (`Views::Upload`'s jitter: 8 points at the output's size, more the fewer pixels a frame renders,
FSR's phase count, `ViewData::jitter`, this frame's and the last's in normalized device coordinates; the cameras `Upload`
returns, which culling and the shadows fit to, aren't), and the materials' textures sample their mips by the output's
size, half a level finer still (`ViewData::textureLodBias`, `SampleView`'s `SampleBias`; FSR recommends a whole level,
which shimmered more). Then ComputeMain composites at the render size (the Composite pass: `PushConstants::upscale`'s
`UPSCALE_COMPOSITE`, into `SHADER_TYPES_COMPOSITE_TEXTURE`, linear, before exposure, its alpha the blended layers'
coverage, FSR's reactive mask; the exposure and distance histograms and the surface cache's block marks as before), the
Upscale passes reconstruct it, and Sharpen (`ComputeSharpen`) tonemaps that into the swapchain, sharpened by Lottes'
RCAS (FSR's).

The Upscale passes are FSR 3.1's (the FidelityFX SDK 1.1.4's fsr3upscaler, MIT licensed, its notice in `upscale.slang`),
each a compute entry point: Upscale Clear (the reconstructed previous depth, `gUpscaleDepth`, a float's bits per pixel,
to the far plane), Prepare Inputs (the dilated depth and motion, the nearest of each pixel's 3x3 and its motion; that
depth pushed to where the point was, an atomic maximum; the farthest depth; the luma), Shading Change Pyramid and Shading
Change (how the luma changed since the last frame where each point was: the smallest relative difference between sorted
sets of 5 lumas, so that an edge the jitter moved hasn't changed, its mean and its sign's over three levels from half the
render size, one pass through shared memory where FSR's single pass downsampler makes six; noise, whose signs disagree,
is no change), Prepare Reactivity (the disocclusion, by the reconstructed depth; the motion's divergence; the shading
change with the reactive mask; the accumulation, a third more a frame up to whole, none after a shading change, a third
less after a disocclusion; and new locks where thin features are), Luma Instability (a pixel's luma unlike the last
frame's but like one of the three before, from a history of 4 lumas) and Accumulate (at the output's size: the history
where the point was, Lanczos 2 over 4x4; this frame's 3x3 by an approximated Lanczos 2 as wide as the render pixels are;
the history clamped onto the ellipsoid of the 3x3's deviations, 3 of them, down to 1 where the point moves, is near,
hasn't accumulated, reacts or its shading changed, luma counting 1.7 times, but kept where it is locked or its luma
flickers; blended by the accumulation against this frame's sixteenth). What they keep from frame to frame, two of each by
the frame's parity (`UPSCALE_PARITY`; `UPSCALE_HISTORY` whether the last holds a frame at this size), is the luma, the
accumulation and the luma history at the render size and the color (its alpha the lock) at the output's; the new locks
are one image at the output's. What is ours: the motion (`UpscaleMotion`: what moves by the pre-pass's motion, the rest
by its depth through the view's last view projection, the jitters taken out), the views (viewports of one render target,
each pixel's jitter and history its view's), the exposure (the frame's, FSR's `Exposure()`, with a pre-exposure of 1),
depth inverted (near 1, see "Depth: reversed"), the scene's units as meters, FSR's default settings, and no transparency and composition mask. What moves
(the main depth's stencil) keeps a history by its own motion, since its depth reprojects where it is, not where it was:
the pre-pass writes each pixel's motion since the last frame (`SHADER_TYPES_PREPASS_MOTION_TEXTURE`, its second color
attachment, RG16F, in the render target's pixels, the jitters taken out: `FragmentPrepass`), from where the vertex stage
places what moves in the last frame (`VertexOutput::previousPosition`: `WorldPosition` with the last frame slot's
instances, joints and morph weights, for the draws `DRAW_FLAG_MOVES` marks; the rest where they are).

Ours too, beyond FSR: each render pixel keeps its log2 luma's running mean and variance (by a sixteenth a frame, with
the accumulation, `Upscale Accumulation` RGBA16F, where the point was; started over without a history or where
revealed), and the shading change counts only as far as the frame's luma strays past 3 of its deviations
(`UpscaleGateShadingChange`), and the history is kept where the luma swings (FSR's luma instability, as much as the
deviation passes a quarter of a stop, all of it at three quarters: `kUpscaleLumaSwing*`). A feature thinner than a render
pixel is hit by some of the jitter's samples and missed by others (a vertical one whole), and FSR took that for the
light changing every frame: the shading change took its locks (made at the output pixel a sample lands on, another as
the jitter moves, they never built) and its accumulation, and the history was clamped onto the frames that missed it,
so it showed each frame's samples as they were: AnisotropyBarnLamp's filament through its bulb (the user's pose
`0.1543,-0.1653,0.3783,-5.337,-31.820`, balanced) jumped in 5867 pixels over 40 still frames and now in 471 (native 881,
now none), changing by 0.080 of 255 a frame and 0.097 over 20, now 0.020 and 0.038, drawn as whole glowing strands
rather than dashes; the thin features at the user's night pose in Bistro's street crept 0.77 over 20 frames, now 0.51. A
change past the deviations moves the mean there at once and the variance by at most what it allows (a swing that keeps
coming back grows it in about ten frames, a step barely does): taking the whole step in, a model appearing over the
backdrop was within its pixels' deviations, its shading change gated and its history kept, and DamagedHelmet faded in
over half a second (CubeVisibility's toggles are a frame, as before). And neither the history is kept nor the shading
change gated where the luma drifts rather than swings (`UpscaleDrifting`: the running mean of its differences from its
mean, by an eighth a frame, the statistics' fourth channel, against its deviation: a swing's cancel, a moving shadow's or
object's add up): kept there, moving shadows' edges trailed their light and faded out over frames, bright bands along
every one at the user's pose over Bistro's square at 13:21 at 600 times real time (`-790.9853,245.9946,216.9426,-0.435,66.449`):
against the same frames with upscaling off (`SPEEDO_TIME_FRAME_SECONDS` alike), a mean of 7.80 of 255 off before the
gate, 8.27 with it, 7.60 with the drift too; the filament's 471 jumping pixels 555. Still and turning renders are
otherwise the same (Sponza 0.05 of 255, DamagedHelmet 0.14, Bistro's street 0.15; turning at the same angles 0.10).

FSR 2.2 came first (its depth clip, locks and lock luma, luma instability at the output's size, a box of 1 deviation at
the output's size and up to 20 at a quarter of its pixels, whose clamp cut the accumulation to a tenth): steadier than
the resolve it replaced at performance, but at native it flickered at every edge twice as much (a center-weighted 1
deviation box clamped the history wherever the jitter moved an edge pixel's sample across). The resolve before it, ours,
was a TAA (a 3x3 gaussian, a box of 1.25 deviations, a history weight of at most 16).
Stability, by the mean change of the output between consecutive frames of a still pose (frames 300 to 305, 0 to 255, the
user's Bistro pose `-994.4661,340.8679,-53.6291,-8.370,78.594` in the evening, `SPEEDO_EXPOSURE=10`, the old binary built
beside the new and run in the same session): FSR 3.1 against the resolve, performance 0.432 and 0.602, at 600 times real
time (the sun moving) 0.776 and 1.210; native 0.434 and 0.458, at 600 times 0.674 and 0.832 (FSR 2.2: performance 0.554
and 0.964, native 0.956 and 1.616). What is left is the traced features' grain (the near field's and the emitters', a
texel per 2 or 4 pixels: see their sections), which no upscaler takes out.
Measured by the draw thread's settled frame time at the benchmark poses (2560x1440 output, GI on), with the SBVH and
the shadow atlas's cache, warm pipelines (the first run after a shader change draws with the generic ones while the
specialized compile): off, balanced and performance with the resolve before FSR, Bistro 57.6, 42.2 and 36.3 ms; San
Miguel 68.3, 46.3 and 39.1; Sponza 29.4, 24.4 and 21.6: a quarter to a third of the heavier scenes' frame at balanced,
short of the pixels' 55% since the probes, the voxels and the surface cache don't scale with the pixels. FSR 3.1 costs
0.8 ms more than the resolve did at Bistro's performance (37.1 against 36.3 ms, the history reprojected by its approximated
Lanczos rather than the reference, FSR's option, which took 37.5). With upscaling off the images are as before (the frame graph's Composite, Upscale and Sharpen passes
disabled, ComputeMain's as it was).

## Culling: chunks, and frustums per view

The importers make one submesh per material, which can span a whole scene (Bistro: 2.8 M triangles in 132 submeshes, a
material's walls on every side of the camera), so a submesh's bounds cull nothing. `mesh::Chunk` (`gfx/meshimport.h`,
run by `Model`'s load after `mesh::Import`, and by `assettest`) cuts each static triangle submesh into chunks of at most
`kChunkTriangles` (16384): the triangles' centroids ordered along a space filling curve (`meshopt_spatialSortRemap`), cut
into even runs, each run's triangles back in the file's order (their vertex reuse). A chunk is a submesh like the one it
came from (material, instances, ...), with its bounds: its vertices at each of its instances, in world space
(`Submesh::bounds`). What moves (skinned, morphed, or instances that follow animated nodes: `Submesh::moves`) stays whole, and
is culled by where its animations take it (`AnimatedBounds` in `model.cpp`, from the same samples that grow the model's
bounds: its linked instances' or its skin's joints' reach, a morphed submesh's targets' reach at weights from 0 to 1, and a
tenth of its size more, since a crossfade turns joints between two animations' poses; a skinned or linked submesh's
morph reach is only at rest). `DrawItem::moves` keeps marking it in the stencil and voxelizing it each frame; it used to
be `!cullable`. Renders are the same with and without culling (CesiumMan, Fox, BrainStem, AnimatedMorphCube,
MorphStressTest, RiggedFigure at frozen times). The chunk size is part of the model cache key (`chunks-N`). `assettest` reports the chunks and fails a
chunk whose vertices leave its bounds; its encodings comparison counts the submeshes before chunking.

`RecordDrawList` takes the view's `gfx::Frustum` (`gfx/frustum.h`: six planes from a view projection, Gribb and Hartmann;
an infinite far plane comes out as (0, 0, 0, near), which culls nothing) and skips the draws whose bounds are outside a plane. The
main pass culls by `FrameInputs::viewProjections` (the cameras `Views::Upload` wrote to the frame's view buffer, so the
cpu culls by what the gpu draws with), the shadow pass by each shadow view's (no depth clamp, so a caster in front of
a shadow view's near plane casts nothing anyway). View > Culling (`SPEEDO_CULLING=0`) turns it off. Each draw costs about a
microsecond of gpu time on KosmicKrisp (Bistro framed, nothing culled: Main 3.72 ms unchunked, 4.34 in chunks of 4096), so
`RecordDrawList` sets cull mode, front face and push constants only where they change (a submesh's chunks share them), and
the chunks are big. Bistro's gpu time (Main + Shadows), before and with chunks of 16384 culled: 11.9 to 10.9 ms looking
down a street through an arch, 11.0 to 7.8 in a shopping street, 9.1 to 7.3 framed (the shadow views gain most: each
cascade skips what it doesn't see); chunks of 4096 were 7.5 in the street but 7.5 framed. `SPEEDO_CAMERA=x,y,z,pitch,yaw`
(degrees; positive pitch looks down, positive yaw turns from -z towards +x) places the views once the autoloads have
installed (`Views::Place`), for such measurements: Bistro's were `3430,170,2200,0,-62`, `564,170,-2436,0,30`, and framed.
It keeps the near and far planes of the camera the views had (a model's own camera's, e.g. a chase camera's far plane of
1 m in DiffuseTransmissionPlant); `SPEEDO_CAMERA=frame` frames the model instead, as View > Camera > Frame model.
`SPEEDO_LAYOUT=1x1|1x2|2x1|2x2` starts with that grid of views, as View > Layout.

## Shadows

The punctual lights cast shadows from one depth atlas (`gfx/shadows.h`: 4096 texels, a grid of 128 texel cells; a frame
graph transient, `SHADER_TYPES_SHADOW_ATLAS_TEXTURE`), planned on the cpu each frame (`shadows::Plan`, in
`Renderer::Record`, after the frame's fence): directional lights first (4 cascades of 1024 texels, Zhang's practical
splits over the first view's frustum up to the scene's bounds, each a bounding sphere seen along the light, depth from
the scene's nearest caster, snapped to whole texels), then point lights (6 cube faces, a little wider than 90 degrees so
the filter near a face's edge stays in it) and spot lights (the outer cone plus a margin), by brightness at the camera.
A point or spot light whose range is outside the camera's frustum lights nothing seen and gets none. The others' tiles
are sized by how big their range looks from the camera (`CellsFor`: its share of the view's height; at most 512 texels a
face, 1024 a spot), then budgeted: while they need more than the atlas, the lowest priority light of those with the
biggest tiles gets them halved (down to one cell; the cascades keep theirs), and once all are one cell the lowest
priority ones go without. The tiles are placed biggest first (aligned power of two squares then pack without gaps), all
of a light's or none, up to `SHADER_TYPES_SHADOW_VIEW_COUNT` (256) views. It used to stop at 4 point and spot lights,
each a fixed size, while every shadow view drew the whole scene (before culling): `ManyLights.gltf` (24 point and 8 spot
lights between pillars) shows all of them shadowed, the Shadows pass taking 0.46 ms (0.31 with 4), the main pass 5.6
(4.3: each pixel filters a shadow per light in reach, 4 to 9 here). A light's tile size changes by octaves as the camera
moves, which shows as its shadow sharpening or softening at once. The views go in `gShadowViews[frame]` (`ShadowData`: view projection, atlas rectangle, texel size) and each
light's first in `gLightShadows[frame]`, host visible buffers per frame index. The Shadows pass draws each view in its
tile with `BuildShadowDrawList` (opaque and alpha masked triangles; not blended or transmissive; both faces of every
caster, as the traced shadows' rays and the indirect lighting's voxels have a single sided triangle block light from
behind: culled as the main pass draws them, CornellBox's ceiling cast nothing and the box's inside was lit where the rays
found it in shadow; 0.5 ms more of Shadows on Bistro, 3.69 to 4.15 in its street), `VertexMain` taking the
view from `PushConstants::shadowView` and `FragmentShadow` (the layout's third fragment entry point) cutting out alpha
masks. A pipeline without color attachments writes depth if its variant's first blend mode is `kOpaque`, and pipelines
are cached per render target attachment formats too (the shadow pass made the main pass's default variant for the depth
only target, so the main pass drew nothing). `ShadowFactor` in the light loop picks the cascade (the nearest whose
filter stays in its tile) or face, offsets the point along the geometry's normal by 1 to 3 texels by the angle to the
light (1.5 alone left acne, a gray cast on lit faces, since the filter reaches 2 texels), on the side facing the light
(a leaf lit from behind, diffuse transmission's or the scattering's light, pushed away from it was behind itself as the
light sees it: DiffuseTransmissionPlant's fireflies shone through leaves in a star, where the cube faces meet), and filters 3x3 bilinear
lookups (`GatherRed`, four comparisons each). The rule holds for whatever replaces this, shadow maps or rays: a point
pushed off its surface goes to the side its query goes to (a shadow test's light, a reflected ray's viewer, a refracted
or transmitted ray's far side), not along the normal regardless, which faces the viewer on double sided materials; on
the wrong side, the surface occludes or hits itself (missing transmitted light, black speckles in refractions). View > Shadows toggles them (`SPEEDO_SHADOWS=0` starts without). View > Lights... (`SPEEDO_SHOW_LIGHTS=1`) lists gLights (the model's,
then the environment's dominant lights, or the sky's sun, unless merged: a model sun more than 5 degrees from
the sky's makes two sets of shadows) and draws them over the views as an imgui overlay (no shader or pipeline of its own):
the renderer keeps a copy of a frame's lights, shadow views and scene bounds while it is open (`FrameInputs::captureLights`,
`Renderer::GetLightCapture`), `gfx::BuildLightGizmos` turns it into world space lines (a directional light's arrow into the
scene's center, spot cones, point ranges, each shadow view's frustum: its clip space box through the inverse view
projection) and markers, and the ui projects them with each view's camera, a directional light's marker where it is in the
sky (the direction at w = 0), at the view's edge when it is out of it, dimmed when behind the camera.
The atlas keeps its tiles from frame to frame (a persistent transient, loaded rather than cleared): a view that the last
frame also had in the same tile (its view projection, tile and texel size bytewise, the same model and shadow draws)
isn't drawn again, unless something that moves is in its frustum (`Renderer::InternalPlanShadowCache`); the others'
tiles are cleared (`IRenderTarget::ClearRect`) and drawn, and the pass is skipped when every view is kept.
`SPEEDO_SHADOW_CACHE=0` draws them all every frame. Point and spot lights keep theirs while the camera moves (their
tiles change size by octaves), the sun's cascades while it is still (they follow the view, snapped to texels: the far
ones, of big texels, change less often). San Miguel's still pose 86.9 to 68.4 ms (its two sided foliage four times over;
more than the 9.9 ms the baseline's left out Shadows pass freed, not yet understood), Sponza's 30.35 to 29.37,
Bistro's evening the same (its sun is down); renders identical, still and along a path.
`resources/test-models/gltf/ShadowScene.gltf` has a sun, a point and a spot light over boxes on a ground plane.

## Ray traced shadows

Lights have a size (`LightData::size`): a directional light's angular radius in radians, a point or spot light's radius
in world units (the sphere it emits from), 0 for none (hard shadows). The model's directional lights take the real sun's,
0.00465 (0.27 degrees, `environment::kSkySunRadius`), its point and spot lights none; View > Lights... sets each (degrees
for a directional light), as does `SPEEDO_LIGHT_SIZES=<light>=<size>[,...]` (the model's lights by index; cleared when a
model is installed, `Scene::GetLightSizes`). The sky's sun is the real one too, and each dominant light of a
panorama gets its own (`DominantLight::angularRadius`: its texels' mean squared angle from its center, weighed by their
light, is a uniform disk's r^2 / 2; at least half a texel; `environment-v7`).

**The pre-pass** (Prepass, and Prepass Late with occlusion culling): the main list's opaque draws first, their depth
and their geometry normals toward the side seen (`FragmentPrepass`, the layout's fifth fragment entry point,
`kPrepassFragmentShader`; `PrepassOutput`'s alpha says whether the fragment takes light from behind, diffuse transmission
and the scattering's), `SHADER_TYPES_PREPASS_NORMAL_TEXTURE`. Main and Main Late shade over that depth, each pixel once
(`GraphicsPipelineVariant::depthTest`, rhi's `DepthTest::kEqual`: greater or equal, the depth being reversed, no writes); blended draws depth test as
before, and the transmissive phase isn't in the pre-pass (it must see the opaque scene behind it). Renders are identical,
and frames are faster for the overdraw no longer shaded (Bistro's street, GI on, 25.15 to 22.9 ms).

**The traced shadows** (View > Ray Traced Shadows, on by default; `SPEEDO_TRACED_SHADOWS=0` for the atlas alone, `all`
to trace every pixel): for up to `SHADER_TYPES_SHADOW_TRACE_SLOT_COUNT` (4) lights a frame (the directional ones first,
brightest first, then the point and spot lights the atlas has views of, by their light at the camera; none of zero
light), where the model has a BVH (see "Tracing the triangles"; `Animate` places the moving one for them too), a ray per
pixel of the pre-pass's depth and traced light toward a point of the light's disk, through the static BVH (alpha masks
cut out, `GiAlphaMask`) and the moving one (`ShadowOccluder`), filtered over the penumbra's width
(`tracedshadows.slang`, `gShadowTrace[frame]`: `ShadowTraceData`, the slots' lights; the images are the frame graph's
transients, a slot a channel). Main's opaque fragments take them for the traced lights (`ShadowFactor`, by
`PushConstants::prepassImages`' `PREPASS_IMAGE_SHADOW_MASK`); everything else is the atlas's.

- **Classify** (Shadow Classify, `ComputeShadowClassify`, compute entry point 18): where the atlas is sure, no ray
  (`ShadowMapClassify`): its 3x3 filter all lit or all shadowed, and none of 24 taps up to 24 texels away says otherwise
  within the penumbra it would make (a blocking texel within the half width its depth gives, plus 2 texels, of a lit
  pixel; an unblocked one within its own blocker's reach, of a shadowed one), each compared with the pixel's tangent
  plane where the texel is (`ShadowReceiverGradient`: a surface sloping toward the light isn't its own blocker), and
  the 2x2 texels where the pixel itself is (without the filter's normal offset) agreeing with the verdict within a
  texel's slope of its tangent plane (none blocking a lit one, all a shadowed one): where they disagree, the verdict is
  the bias's. a contact shadow within the offset's reach was lit and sure (Bistro's cornice band under its ledge, the
  user's pose `2991.8064,394.9334,1467.5278,-0.016,5.767`: a sawtooth of the atlas's texels along the ledge, which
  tracing every pixel didn't have), and a concave corner's other surface, nearer than the pixel's own depth but within
  a texel's slope of its plane, shadowed and sure (the dark lines along CornellBox's edges). The cost: Bistro's street
  75.1 to 76.1 ms, the cornice pose 79.8 to 82.4 (the pixels traced; a 4x4 footprint traced the mouldings' curves too,
  77.2 and 84.8). A faint dashed highlight is left along one of the cornice's creases. For a lit verdict a texel there
  blocks within a quarter of a texel's slope, if it is a texel's size or more before the plane: a facade in a far
  cascade under a high sun slopes so steeply in the light's view that a texel's slope was as deep as its eave's overhang,
  and the band under the eave was lit and sure between traced pixels, teeth again (the user's pose
  `-1033.9612,281.2515,-184.0673,-3.357,86.270` at 12:23); about a percent more pixels traced, Bistro's street at noon
  37.6 to 38.0 ms (a quarter of the slope without the size, every frame and moulding traced, 38.3). The
  rest are listed (`gShadowTraceList`: a count, then the pixels) and the 8x8 tiles within the filter's reach of them
  flagged (`gShadowTraceTiles`). Bistro's street lists 136000 pixels (4%), mostly along grazing sunlit facades. A thin
  occluder can fall between the taps: such a pixel is decided wrongly, which the filter takes as one more sample.
- **Trace** (Shadow Trace, `ComputeShadowTrace`, entry point 19): `SHADER_TYPES_SHADOW_TRACE_THREAD_COUNT` threads take
  the listed pixels in turn (traced where they were classified, a group waited for its slowest pixels: as costly as
  450000 rays). The disk's point (`ShadowDiskPoint`): one of 16 a 4x4 tile, its radius band and quarter of the circle
  from two orthogonal latin squares over GF(4), so every row and column has all of both, within them a (0,2,2)-net,
  turned per tile (by the order of a Vogel spiral each row of a tile had a band of its own, and a narrow penumbra came
  out in steps along the rows). The ray starts off the surface toward the light by 1.5 pixels (more far away) and keeps
  what it met's distance (`SHADER_TYPES_SHADOW_BLOCKER_TEXTURE`, -1 where the light doesn't reach: facing away).
- **Filter** (Shadow Denoise X and Y, `ComputeShadowDenoiseX`/`Y`, entry points 20 and 21; flagged tiles only): rows,
  then columns, a gaussian of the distance along the surface whose deviation is a third of the penumbra's half width
  (`ShadowPenumbraHalfWidths`: from the pixel's blocker, else the mean of those 16 taps around met, as
  `ShadowPenumbraHalfWidth` gives it: d tan(a) of a disk, r d / (s - d) of a sphere) and at least the step's footprint
  on the surface (`ShadowFootprint`: a surface at a slant isn't blurred further across than along), up to 16 pixels,
  weighed by the neighbor's distance off the pixel's tangent plane and its normal's agreement, leaving out where the
  light doesn't reach (a pixel facing away keeps its 0: its neighbors beyond its edge alone lit it). A pixel no ray
  around met a blocker takes the floor (its taps' places are fixed, and rays lit by chance left single pixels of
  penumbrae unfiltered, as bright as the light). Against the reference (`traced-shadow-reference`, see "Debug views"),
  mean error over the reference's penumbrae, of 255: ShadowScene under a 2 degree sun 7.7, its point and spot lights of
  0.4 units 4.1 and 5.2, Sponza at the user's pose (`0.9128,4.5351,-2.4096,-2.036,-83.129`) 9.6, Bistro's street 11.0;
  a deviation of 0.7 of the half width in pixels facing the eye, as it was, widened ShadowScene's by three fifths
  (15). The real sun's penumbrae a pixel or two wide are 33 off: one ray a pixel can't resolve them, and their edges
  wobble by a pixel from tile to tile, which only more rays (accumulated over frames) would fix.
- **The atlas where nothing is traced** (the lights past the traced ones, blended, transmissive and scattering
  fragments, a model without a BVH): percentage closer soft shadows for a light with a size (`ShadowMapSoft`, Fernando's:
  16 taps for the blockers within the widest penumbra the point can have, 16 bilinear comparisons over the one their
  mean distance makes, turned per pixel by interleaved gradient noise, at most 16 texels; the 3x3 filter where the
  penumbra is under a texel and a half). Lit renders with it alone against the traced ones: a mean difference of 0.03 to
  0.09 of 255, a little blockier. Point and spot lights have no size by default, so they cost nothing more.

Cost, by the frame's gpu time (the `Frame` scope; 2560x1440, GI on, interleaved runs agreeing to 0.1 ms): Bistro's street
31.1 ms traced, 26.0 with the atlas soft (`SPEEDO_TRACED_SHADOWS=0`), 23.1 with its 3x3 filter alone; Sponza's inside
(`0,2,0,0,90`) 19.2, 19.3 and 16.7; ManyLights 16.4 traced (4 of its 32 lights, without sizes) against 13.9. Bistro's
is its rays along sunlit facades seen at a grazing angle (136000 a frame, about 35 ns each against its 2.8 M triangles):
letting the atlas decide the grazing pixels saved 1.2 ms of it, and was dropped. On the way there (GI off, Bistro's
street, 11.0 ms with the atlas alone): every pixel traced 52.7, the classification 26.4, by the penumbra's reach 20.7,
the listed pixels taken by persistent threads 20.3, the filter over flagged tiles only 18.3.

## Clustered lighting

Each fragment loops over only the lights that may reach it (`ClusterLights` in the shaders, for `Shade` and
`ScatterLight`). Each view's frustum is a grid of clusters (`SHADER_TYPES_CLUSTER_TILES_X` by `_Y` tiles of normalized
device coordinates, 16 by 9, and `SHADER_TYPES_CLUSTER_SLICES` slices of depth, 24, logarithmic: `log2(1 + d / unit)` of
the distance from the near plane, over 1023 units to the scene's farthest point or the far plane). `clusters::Plan`
(`gfx/clusters.h`, in `Renderer::Record` after the shadows' plan) bins the lights on the cpu each frame: a directional
light into every cluster; one without a range as far as the scene's farthest corner (nothing beyond the scene is lit, as
the shadows' reach: an unranged spot light used to reach every cluster); a point light into the box of clusters around its range's sphere, a
spot light around its cone's (`LightReach` in `gfx/lightbounds.h`: the cone capped by the range, in its smallest
bounding sphere, Wronski's; shadows skip and size spot lights by it too) (its corners projected, the whole screen if one
is behind the eye; a light outside the frustum into none), widened by a thousandth of a tile or slice against rounding.
Per frame index, host visible: `gClusterViews` (a `ClusterView` per view: the depth plane and slicing; the tile comes
from the view's own view projection, which is the one the clusters were planned with, see `Views::Upload`), `gClusters`
(per cluster its first index and count) and `gClusterLights` (the indices, at most
`SHADER_TYPES_CLUSTER_LIGHT_INDEX_COUNT`, a hard failure past it: ThousandsOfLights in four views fits; `enabled` 0, all
lights looped over, is for View > Clustered Lights off).
View > Clustered Lights (`SPEEDO_CLUSTERED_LIGHTS=0`) turns it off. Every light the loop rejects cost about 60 us of
main pass per frame (ManyLights with 223 more lights out of reach: 20.7 ms, clustered 5.2, as without them; ManyLights
itself 5.8 to 5.2, 4.9 with its spot lights bounded by their cones, orthographic 6.3 to 5.3; Bistro with one light 4.10
to 4.15). Renders with and without are identical. The cpu binning stays cheap: 6 us a frame for Bistro's one light, 42
for ManyLights' 33, 80 (105 at the 90th percentile) for the 4000 of `ThousandsOfLights.gltf` in one view, so it hasn't been
moved to a compute pass; that scene's main pass takes 8.9 ms clustered, and unclustered more than the 250 ms a timed pass
may (its timings are dropped). `SHADER_TYPES_LIGHT_COUNT` (gLights) is 4096.

## Indirect lighting: irradiance probes over a voxel clipmap

View > Indirect Lighting (on by default since 2026-10-22, the user's call once the artifacts that followed the camera
were gone; `SPEEDO_GI=0` to start without it, and per frame:
`FrameInputs::indirectLighting`, off drops the volumes, on voxelizes again) gives the diffuse ambient term from world
space irradiance probes (`gfx/indirectlight.h`, `gi.slang`; `GiData` in `capi.h`), as DDGI (Majercik et al. 2019, RTXGI)
has them, over a voxel clipmap the probes' rays march: each probe traces a set of rays a frame through the voxels'
radiance and blends what they see into what it holds (hysteresis), its irradiance along the axes (an ambient cube) and a depth map
for a visibility test; the shading interpolates the 8 probes around a point, each by whether it sees the point. Lighting
lags by the blend (about a second to settle, nothing else is temporal: no reprojection, so no ghosting), and a still
scene flickers by the frame's rays' share of the blend (measured below). It replaced world space radiance cascades
(2026-10-21): a fixed set of 36 directions per probe sampling a scene's small bright parts (CornellBox's lamp, Sponza's
arches) landed differently from probe to probe, which showed as splotches a fifth of a room wide that no amount of
tuning took below a third of the error (CornellBox-Sphere against a 2048 ray reference, `mean |log2|` at 48 pixels of
blur: 0.13 against the reference's own 0.046 with 512 rays; the merge's parallax 0.03 of it, the levels 0.01, the rest
the direction grid), and their cost grew with the clips (every clip's cascades reach across the last: CornellBox-Sphere
29.9 ms a frame on the day it was dropped, 9.3 with the probes). What it does, per frame:

- **The clipmap** (`gi::MakeClipmap`, `SHADER_TYPES_GI_CLIP_COUNT`): `SPEEDO_GI_CLIP_COUNT` volumes (3 by default, at
  most 4), finest first. The last is over the scene's bounds (`SPEEDO_GI_RESOLUTION`, 128 by default, voxels along its
  longest axis; View > Indirect Lighting Resolution offers `gi::kResolutions`, 64 to 384, and a change makes the clipmap
  again, `FrameInputs::giResolution`. Finer resolves light through narrower openings: the sky under Sponza's curtains,
  a gap of a few tens of centimetres, which 128 drew as spots at the corridor's end that faded into longer, dimmer
  patches as the eye neared them (the coarser clips' probes, 1.8 m apart, giving way to the finer's; the surface
  reference has a band along the curtains' lower edges), and 256 as the reference does; at the user's pose
  `7.7949,0.8419,-3.7077,0.531,-91.763` the frame takes (without the surface cache, below) 2.1 ms at 64, 2.6 at 96, 3.5 at 128, 5.4 at 192, 8.2 at 256, 11.9 at 320 and 17.8 at
  384 (RTX 4070 Ti SUPER, 1920x1080; Bistro's street 18.6 at 384); a margin of a probe spacing, so that a layer of probes stands outside its outermost surfaces; a whole
  spacing puts the bounds halfway between two planes of probes in every clip, the probes being half a spacing from a
  cell's corner: at one and a half, a plane of the last clip's probes lay in the outermost surfaces, CornellBox's floor,
  back and left walls, half of each one's rays leaving through the surface it was on and the relocation unable to tell
  which way was off it, the surface's voxels being on both sides of it, and they lit the floor along the back wall
  with the sky; Sponza's error against its surface reference went from 0.385 to 0.341); each before it is a cube of about its voxel count (a multiple of 4 voxels a side, the flags' blocks; no
  bigger than the last along an axis: Bistro's and Sponza's 92) of voxels half the size of the next, centered ahead of
  the eye toward what the camera looks at (below) on a grid of its probes and kept inside the last. As slabs of the last clip's shape (Bistro's 122x53x128
  voxels made the finest 14.7 m tall at 27.7 cm, Sponza's 4.9 m), the ground fell out of them from a high viewpoint
  (the user's over Bistro's square, 13 m up), and the square was lit by the last clip's probes, 8.9 m apart, of a cell
  half under the ground and one probe inside the building by its entrance. A clip's center is ahead of the eye, toward
  what the camera looks at, by `SPEEDO_GI_LOOK_AHEAD` of its half size at most (0.5 by default; 0 centers it on the
  eye): how far the camera looks is the median distance of the frame's drawn pixels from the eye (`ComputeMain`
  unprojects the main depth into 64 bins of log2 distance after the exposure histogram's, `SHADER_TYPES_DISTANCE_BINS`,
  read back with it and smoothed as the exposure is: `FrameInputs::lookDistance`), and the centers lie along the view's
  forward, so each clip stays inside the next, and the eye stays in the finer clips, two thirds of the way to their
  fade. Centered on the eye, half of each clip was behind it, and from the roof over Bistro's square (27 m from the
  hotel's entrance) the square was the last clip's; with the look ahead the ground is clip 1's (4.4 m probes), the
  entrance's facade still mostly the last's at 0.5 (its weight in clip 1 is 0.2 there). The buffers a new clipmap gets
  are cleared first (the GI Clear pass: every GI buffer and the volumes filled with 0 when they were replaced,
  `myGiClearBuffers`): the resolve writes only the blocks near a surface, and a lookup's filter reaches into the ones
  beside them, which held whatever their memory had (cyan and red over Bistro's square, in one run of two). Each has its own voxels, radiance, probes
  and flags, at its offsets in the shared buffers (`GiData::voxelFirst`, `radianceFirst`, `probeFirst`,
  `lightCellFirst`, `flagFirst`; the resolve's volumes stack the clips' slabs along z, `GiData::volumeImage`: the
  images' texels per axis and the clip's first along z;
  `gGi[frame][clip]`), and the kernels take theirs from `PushConstants::giClip`. The flags' offset is in words
  (`SHADER_TYPES_GI_FLAG_WORD_COUNT` a block): in blocks, each clip's second half of flags was the next clip's first,
  whose kernels rewrote them (its bits, and other blocks' stamps), so in the finer clips' far halves blocks near a
  surface went unresolved and probes untraced, at random (found 2026-10-22 reading the flags from the rays' walk). A
  clip's rays (`GiMarch`) and shadow rays (`GiOccluderDistance`) that leave it go on through the coarser ones (a box is
  never entered again). All are made again when the model, its bounds or its draws change; a clip that moves scrolls
  (`GiData::scroll`, in voxels, whole probe cells): GI Shift (`ComputeGiShift`) gives each voxel the static one kept at
  its place before, or empty where it scrolled in, and only that is voxelized (its jobs are the static draws whose
  bounds reach the slabs that scrolled in, `GiClipJobs`; a triangle whose voxels in the clip were all kept skips, and a
  row of its samples only covers the slab); the probes shift the same way (below); a move of its size or more, or a
  change of the scene, voxelizes all of it (a scroll of its whole size). Voxelizing (the GI Shift and GI Voxelize
  passes, only then): a thread per triangle of the main list's static draws (`gGiJobs`: per draw its first triangle of
  all, first index, `DrawRecord` and instances; `gIndexBuffer`, so index buffers are storage buffers too) samples its
  surface at under half a voxel apart into the voxel half a voxel behind each of its faces (`GiVoxel`: albedo and
  occupancy, normal facing away from the face, emission; `InterlockedMax`, so that the order doesn't matter), in rows
  of samples, at most 512 (`kGiVoxelizeSampledSteps`); a longer triangle (over 256 voxels) is rasterized along its
  normal's dominant axis instead (`GiVoxelizeRasterize`: each voxel column whose square its projection overlaps, the
  voxels its plane crosses there). Sampled at most 512 rows apart, as it was, a long triangle was sampled a little more
  than a voxel apart and skipped voxel columns: at resolution 384 Sponza's corridor floor (triangles some 10 m long,
  voxels of 2 cm in the finest clip) had lines of holes across it, which the surface cache drew as bright lines along
  the floor; sampled half a voxel apart however long, its voxelizing took 13 ms rather than 4.4 when a clip scrolled
  (a triangle is a thread), rasterized 3.4 (Bistro's street 3.5 to 3.0). Both
  faces, always: a double sided material's either face can be the one seen, and a triangle's winding, its vertex
  normals and the side it is drawn from needn't agree (CornellBox's generated normals point out of the box, whose walls
  are seen from inside). Which faces wrote a voxel is kept (`GiVoxelFaces`: 1 a front face, behind which it stands for the
back side, 2 a back face, 3 both), and whether a double sided triangle did (`kGiVoxelDoubleSided`; a job's bit 30, from
its draw's cull mode), and a drawn side's normal wins the voxel's (`GiPackVoxelNormal`: the front side's, either of a
double sided triangle's, by the normal's top bit in the atomic maximum). A voxel a single sided triangle's front face
wrote holds a side never drawn, and the resolve gives it no light (see the BVH's paragraph below). Each side's voxel is
a shell for the lookups and the relocation; for the rays (`GiMarch`,
  `GiOccluderHit`: the probes' rays and depth, the shadow rays, the Voxel World view) the voxel a back face alone wrote
  (`GiVoxel::emissionB`'s faces 2: outside a solid, by the importers' outward winding) is passed, and is the surface
  met if its twin behind is hit within two voxels (its radiance and normal), so a surface is a voxel thick for them
  rather than two, and an opening a voxel wider a side. At 0.29 m voxels Sponza's first floor arcade lost its
  openings' lower 0.6 m to the shells: no sun reached its floor in the last clip and its probes held no light (their
  rays' mean 2e-5 against the next clip's 4e-3), so the corridor was black beyond the finer clips' boxes, the third
  artifact that followed the camera (the user's poses `-7.7534,4.8126,-3.5046,0.426,448.765` and
  `-2.3323,4.8044,-3.5225,0.779,449.768`), and the surface reference agreed, reading the same voxels. With the shells
  passed its far corridor at exposure 6 reads 0.045 (the reference 0.033; before 0.017 and 0.011), the mid floor 0.18
  (0.09; 0.07 and 0.04) and the near 0.42 (0.39): it still darkens with the distance, the last clip's openings being
  what they are (two clips at 256 voxels light it, 0.13, for Sponza's 13 to 24 ms a frame, 29 with three, and three at
  192 0.16 for 21: too much; TODO.md). A probe's rays walk from the finest clip whose box holds it (`GiFinestClipHolding`, in `ComputeGiTrace`),
  not from its own, so the clips' probes agree where they overlap: by their own voxels, the finest clip's sun patches
  lit its probes and not the coarser clip's beside them, so its box's edge was a step of 12% on the vault (the user's
  "sharp discontinuity", in the surface reference as much as the probes' render, whose fade band is narrow that far
  out), moving with the eye; the step is 8% now, over a probe spacing, and the far corridor reads 0.075 (the coarser
  clip's probes nearest the box walk the fine voxels too). What is left of it is the coarser voxels' own lighting
  beyond the finest box. The one voxel shell has corner gaps only where a plane passes voxel corners exactly, which the second shell
  covered: the samples' half voxel offset keeps it face connected elsewhere. The voxels' albedo is the base color times 1 minus the metallic factor times its texture's
  (by the factor alone, Sponza's voxels were all black). Those voxels are kept (GI Keep, into a buffer of their own:
  the clips voxelized, the others' voxels holding what moved last frame), lit by the lights (GI Light Static, below),
  and every frame with something that moves (skinned, morphed, or following an animated node: not cullable) they are
  copied back (GI Restore) and what moves is voxelized over them (GI Voxelize Moving: its jobs after the static ones',
  by `PushConstants::cullJobBase`), placed by `WorldPosition` (`vertex.slang`: morphed, skinned, then by the instance,
  as `VertexMain` does): it occludes, is lit and bounces light, in the frame.
- **Moving clips**: a clip that follows the eye moves by whole snaps (`gi::kSnapProbes`, one probe: 8 voxels; clamped
  to whole snaps at the last clip's sides too), so everything a point inside it is lit by stays where it is in the
  world as it snaps: its probes are laid out from its corner (`GiData::probeOrigin`), a whole number of snaps from the
  clipmap's, and keep what they hold across a snap (the update reads each probe's history from where it was, by the
  scroll); the flags' blocks are rounded up (`GiLevelVoxels`, `gi::LevelVoxels`); and each clip has a box around its center that it always holds well
  inside (`GiData::fadeCenter`, `fadeExtent`: half its size less half a snap; with a snap of 4 probes and a spacing and
  a voxel more of margin, Sponza's thin clips, 68 voxels tall, kept a box a third of their height, and the finest clips
  lit nothing on its ground floor: everything there was the last clip's, 23 cm voxels and 1.9 m probes), which
  the shading fades it out toward over a third of its half size (`GiClipWeight`) and rays leave it at
  (`GiFadeBoxExit`), so what changes as a clip snaps changes with the eye, smoothly. The shading takes each clip by its
  weight of what the finer ones leave (`GiSampleAt`). Under a still camera with the clips moving 6 m through Sponza's
  arcade (`SPEEDO_GI_EYE_PATH`, 120 frames), the frames' means differ from the still run's by 0.1% at most.
- **Lighting the voxels** (`GiDirectRadiance`): each voxel's emission plus its albedo over pi times the punctual lights
  of its cell and the emissive surfaces where they are lights (`EmitterVoxelIrradiance`, see "Emissive surfaces as
  lights": the rays that read a voxel's radiance then take its emission out) (`gi::BinLights`: a 16^3 grid of cells over the volume, as the clusters bin lights), from its front only
  (a wall's voxel isn't lit by the sun outside it), each shadowed by a ray through the voxels (the shadow atlas covers
  only what the camera sees; `GiOccluderDistance` visits each voxel the ray crosses once, Amanatides and Woo). The
  shadow rays start a voxel out along the voxel's normal, the bounce's lookup 1.5 voxels and the point its probes are
  tested from half a voxel, and the surface cache's rays a voxel along its open side (`GiCacheOrigin`), each no
  further than halfway to a surface facing back at it there (`GiOpenAlong`: a ray through the static BVH, 3 voxels at
  most, once a voxelizing in GI Light Static, kept in the direct light's alpha, in voxels; the cache tracing a ray of its
  own each update cost Sponza 0.3 ms a frame): in the slot between salle_de_bain's bench and its wall (0.66 units, a last clip's voxel and a half), the bench
  back's voxel faces the wall 0.26 units off, and its shadow rays started past the wall, outdoors, where the sun lit it,
  and its bounce read the volume outdoors; whatever read it (the cache's rays in the slot, the probes', the near field's)
  drew the slot as a band brighter than the room (488 of 765 against the wall's 330 at the user's pose
  `13.6266,14.4950,35.7340,16.977,4.625`, the slot's wall seen from above), now 66 to 133, fading out toward its opening.
  A voxelizing's GI Light Static took 0.14 to 0.24 ms in Sponza, 0.92 to 1.41 in Bistro's street. The
  static voxels' is kept (GI Light Static, `ComputeGiLightStatic`, into `GiData::directFirst` of `gGiRadiance`) and
  made again when a clip is voxelized or the lights change (`Renderer::myGiLights`, compared bytewise: the environment's
  rotation turns its dominant lights, which are among them, and the sky's sun moves with the time of day). The part the
  lights don't change, the emitters' light and how far each voxel is open (`GiOpenAlong`), is kept apart
  (`GiData::keptFirst`) and taken as it is where the lights changed alone (`GiData::lightsOnly`): remade with them, the
  sun moving every frame cost Bistro's street 72 ms a frame (its emitters' samples and shadow rays per voxel), 10 now. A
  change of the same lights turned by under 2 degrees and dimmed by under a quarter is gradual
  (`Renderer::myGiLightsGradual`): the surface cache keeps what it has rather than starting over, sampling as where
  something moves (32 samples at most) for `kGiLightsFollowFrameCount` (64) frames after, so that it follows the sun
  rather than restarting every frame. Each frame GI Light (`ComputeGiLight`) gives every voxel
  that (a moving voxel's, which differs from the static one at its place, made then: a moving voxel doesn't shadow the
  static ones' kept light), plus what its albedo reflects of the probes' light at it as the last frame resolved it
  (`GiData::bounce`, `SPEEDO_GI_BOUNCE=0` for the lights' light alone): the light bounces on, a bounce a frame, settling
  with the blend; not for the first 32 frames after a reset (`kGiBounceDelayFrameCount`): the probes' depth maps are
  provisional then, and what leaked through them into the voxels fed the probes, which fed the voxels, with the loop's
  time constant, `1 / ((1 - hysteresis) (1 - albedo))` frames: Sponza's arcade started 8 times too bright and darkened
  for 200 frames; it rises to its level in 100 now. The shadow rays every frame cost CornellBox-Sphere 2.9 ms of its 3 clips' lighting, the kept light 1.3.
  What moves does shadow the kept light: the moving voxelizer stamps the frame into the blocks it writes (the flags'
  second word, `SHADER_TYPES_GI_FLAG_WORD_COUNT`), and a static voxel whose line to a light that lights it crosses a
  stamped block (`GiMovingShadows`: a walk over the blocks, `GiMovingBetween`) traces its shadow rays that frame; only
  with something moving in the frame (`GiData::moving`).
  The **flags** (`gGiFlags`, `ComputeGiFlags`): per block of 4x4x4 voxels
  (`SHADER_TYPES_GI_FLAG_LEVEL`) the lighting stamps the frame into the blocks holding something (a plain store: an
  atomic maximum kept whatever a new buffer held, and such a block never counted again, which blacked out Sponza), then
  bit 0 says whether anything is in the 3x3x3 blocks around it and bit 3 whether that is within 4 blocks along each axis
  (three separable steps): the resolve skips the blocks without bit 0 (the lookups are within a few voxels of a surface)
  and the probes without bit 3 (a probe is only ever looked up from within its cell of a surface's lookup), which is
  what makes a clip's cost that of its surfaces rather than its volume. A block's third word is the last frame a pixel's
  lookup read it, and bit 4 of its first whether the surface cache works on it this frame (see the surface cache, below).
- **The probes** (`GiData::probe*`): a lattice every `SPEEDO_GI_PROBE_SPACING` voxels (8 by default) from the clip's
  corner (a multiple of 8, so that the resolve's blocks of 4 voxels never span two cells), centered on voxel boundaries
  (so every point of a voxel interpolates the same 8), of which only those near a surface trace (the flags, below):
  CornellBox-Sphere 6400, Sponza's inside 4200, Bistro's street 4500, DamagedHelmet 10400 (its surface fills its last
  clip). The rays per probe are `SPEEDO_GI_RAY_COUNT` (128) at most, and `SPEEDO_GI_RAY_BUDGET` (500000) over the
  probes traced last time (in eights, at least 16: a ray costs 4 to 8 ns here, so DamagedHelmet's 10400 probes at 128
  cost 10 ms; at the budget's 46 they cost 6.5, and Sponza keeps 112, Bistro 104, CornellBox 72). Each probe is at its
  lattice point plus an offset it keeps (coefficients 2 to 4's w; DDGI's relocation, `GiProbeOffset`,
  `GiProbePosition`: the rays trace from where it was, `GiProbeHistory`, and the update moves it): out of the voxels at
  its corner along their normals' mean by 0.45 of a spacing at once (`GiCornerAway`; a probe on a wall's voxel shell
  saw nothing and was skipped, and the points nearest it took the mean of the others: Sponza's inside against its
  surface reference from 1.24 to 0.95 mean |log2|; a step of one and a half voxels, stopping as soon as the corner was
  clear, left a probe's harmonics the surface's and drew dots on CornellBox's ceiling; the move is taken only where its
  target is clear of voxels, else the probe stays where it is: taken wherever the normals pointed, a probe behind
  Bistro's hotel facade, whose two targets were each in the other's voxels, the facade's and the window frames' at
  28 cm, jumped between them every frame, one of them inside the hotel, whose emissive interior is 30 times brighter
  than the street, so the wall by the window lit yellow on alternate frames, the user's "heavy flicker": found by
  dumping the volumes along a line into the wall, 1 to 4 and back, with every probe's light, depth and position of the
  cell in front steady, and then the probes of the cell behind), else, where more than a quarter
  of its rays met a surface's back (`GiOccluderHit`: the voxel's normal along the ray, unless both of a triangle's faces
  wrote the voxel, a thin wall's, whose normal is either's: the voxelizer keeps which faces wrote in
  `GiVoxel::emissionB`'s bits 16 and 17), away from the nearest such by its distance and a tenth of a spacing, a frame at
  a time, never more than 0.45 of a spacing from the lattice point, and only while the probe settles (its first two
  cycles of ray phases, again whenever it starts over); a probe still inside after that (Sponza's outer walls are 0.6 m
  thick and empty between their shells) is marked (DDGI's classification: face 5's w holds the share of its rays that
  met a back, blended over its frames, inside above a quarter, `kGiInsideShare`) and the resolve skips it. Both used to
  go by the frame's rays alone, and each frame's are another set: a probe near a quarter of its rays on backs moved
  every frame within its reach and was skipped on alternate frames, which with the BVH's rays (they meet single sided
  walls from behind, where the voxels' walk passed them) flickered salle_de_bain's niches by up to 100 of 255 (mean
  change 2.64 a frame, 0.11 after; Sponza's error against its reference the same, 0.377). It costs: a wall on a lattice plane makes a plane of probes trace that returned dead before
  (CornellBox +3 ms, Sponza +1). A probe that hugs a surface sees a bright emitter in it at a steep angle: as
  spherical harmonics (bands 0 to 2, until 2026-10-22) its light rang, a yellow halo and a dark ring on CornellBox's
  ceiling around its lamp, and a bright compact source at a grazing angle kept 3/32 of its strength along the surface's
  normal (the bands' 1/4 + cos/2 + 5/16 P2 at 90 degrees; a Hanning window gained 5% on CornellBox and lost 8% on
  Sponza, and can't help: the leak is band 0's quarter, which only the higher bands cancel): Bistro's lit interiors
  and lamps, a hundred times the sky, lit the square before the hotel's entrance yellow. The probes hold an ambient
  cube instead (`GiIrradianceBasis`: 6 faces, the irradiance over pi along +-x, +-y and +-z, each ray weighed by its
  cosine to the axis where it faces it, times 4 over the ray count; `GiEvaluate`: the faces a direction's components
  point at, by the components squared): a grazing source gives a face nothing, at some accuracy between the axes (a
  uniform sky gives a 45 degree slope 0.75 of the zenith's irradiance where 0.854 is right). CornellBox against its
  surface reference reads 0.21 mean |log2| either way; Bistro's entrance from the roof 0.62 to 0.60 (the rest of its
  yellow was a lamp beside a probe, below). Each frame (the GI pass, GI
  Trace then GI Probe Update) each probe near a surface traces `SPEEDO_GI_RAY_COUNT` rays (128), a Fibonacci sphere of
  directions turned by the frame's phase (`GiRayDirection`: a quaternion from Roberts' R3 sequence over
  `SPEEDO_GI_RAY_PHASE_COUNT` phases, 32, then the same sets again, so what a probe holds converges to a fixed value
  rather than wandering, as it would under DDGI's random rotation a frame; every 32nd point of one set 32 times as
  big, tried first, was a coarse spiral of its own, and the phases' light differed by a quarter: a 32 frame pulse of
  5%), the same directions for every probe (each probe turning them its own way made each probe's error its own, which
  showed as dots where the resolve's weights single a probe out) but each probe at its own phase of the cycle
  (`GiProbePhase`: the frame's plus a hash of its place in the clipmap's lattice, the same wherever its clip has
  scrolled to): the blend's ripple (the latest subset weighs 3% of what a probe holds, the oldest of the 32 1.2%, so it
  pulses with the cycle by the subsets' differences) was the same across the probes of a region, which pulsed together,
  a 32 frame flicker of Sponza's walls that the user saw under auto exposure; at their own phases the ripples of a
  cell's 8 probes cancel in the lookup (frames 16 apart at exposure 9 differed by 94 of 255 at most, 17 after). A ray
  walks the voxels to the first one something is in (`GiMarch`: each voxel it crosses visited once, Amanatides and Woo,
  through its clip to its box around the eye and on through the coarser ones; a block of 4x4x4 voxels nothing is in
  this frame, by the flags' stamps, is crossed in one step: DamagedHelmet's frame 12.6 ms walking every voxel, 9.7
  skipping, Bistro's street 17.5 and 16.0), the environment past the last clip (`EnvironmentRadiance` at
  roughness 0.6, without the dominant lights, which light the voxels themselves), and to the first occupied voxel as
  far as the depth map reaches (`SHADER_TYPES_GI_DEPTH_REACH`, 3 spacings: the points a probe is tested for are within
  its cell), into `gGiRays` (rgba16f: radiance, distance in spacings). Where emissive surfaces aren't lights (View > Emissive Lights off), a probe's ray that meets an emitter nearer than `SPEEDO_GI_EMISSION_REACH` spacings (0.5; 0 for none:
  `GiData::emissionReach`, `GiMarch`'s `emissionReach`) takes its emission as from that far away (scaled by the square
  of the distance over it, as a small source's irradiance falls), since what the probe holds lights the points of its
  cell, mostly about half a spacing from it, where a source beside the probe is that far (instant radiosity clamps a
  virtual light's contribution the same way; the references, from the points themselves, don't): a street lamp's head
  beside one of Bistro's last clip's probes (8.9 m apart, at 4 m over the square before the hotel's entrance) lit the
  square and the entrance's facade yellow, the probe's faces 2.4 where its neighbors' were 0.06, where the surface
  reference has a pool under the lamp and the interior's light at the windows beside the arch; with it the entrance
  region reads 0.48 mean |log2| against the reference (0.60 before; its mean 111 of the reference's 115, 137 before),
  and CornellBox's lamp, 8 voxels over its probes, changes nothing (0.218 against 0.210). The rays used to march a mip chain of the
  radiance past two probe spacings (anisotropic, 6 faces a voxel composited front to back, a level per four times the
  distance: a cone of a constant angle, half voxel steps each taking half a voxel's share of its opacity), through which
  a wall a voxel thick is half empty (a quarter at the level above), so every ray that met one kept a share of its
  transmittance and brought the sky in behind it: Sponza's arcades were lit from outside by a thousandth of the sky
  (their voxels hold nothing: the probes' rays read 7e-4 in the finest clip, 1.3e-4 in the next and 4e-5 in the last,
  whose rays go coarse later), and the finest clip's box edge was a dark band that followed the camera along them (the
  user's pose `0.7013,1.0767,-4.0779,0.008,90.314`; its corridor at exposure 10 against the surface reference's 0.000:
  0.16 to 0.31 before, 0.007 to 0.026 now, and no band; walking the corridor after the probes settled, the far end
  brightens evenly as it nears). Walking every voxel costs nothing in the interiors (Sponza's inside 13.1 ms against
  13.7 with the levels, CornellBox-Sphere 9.1 against 11.8; their kernel is gone), and the open scenes paid for rays
  walking to the sky until the block skip. A still frame at the lion wall steps 13 of 255 at most between frames at
  exposure 9 (17 before). The references walk without it (`skipEmptyBlocks`):
  with the blocks' reads the surface reference's fragment shader ran past Metal's command queue timeout
  (`MTL_COMMAND_QUEUE_ERROR_TIMEOUT`, `VK_ERROR_DEVICE_LOST` within seconds, with three clips; under validation too,
  which reported nothing else). The walk's loop has no `continue`: with one, KosmicKrisp didn't keep the block last
  read across it, so a probe's ray starting on a block's face (the probes are at voxel corners, and a spacing of 8 is
  a multiple of the blocks' 4) with a hair of its direction into it (`-0.000874, 0.9999, 0.013`: the push of a
  thousandth of a voxel lost to float rounding) found the same empty block anew every iteration, its exit at 0, and
  hung the gpu: the draw thread in `Fence::Wait` for good, no device loss (and once the machine, 2026-10-22, at the
  lion wall pose `-6.9604,1.3018,-0.3295,7.269,-86.774`). A step cap (4096 a clip) ends such a walk, counts it in
  `gGiStatistics` (`GI_STATISTIC_CAPPED_RAYS`, with the ray and where it was) and the renderer prints it after the
  frame: a hang mustn't take the machine down again, and a capped ray must be seen. The cap's writes are only where the
  walk skips: in the main pass's fragment shader (which holds the surface reference's walk) a storage write, even on a
  path never taken, turned its early depth test off, and Sponza's Main took 10 ms rather than 4.7. The update (a group per probe, a thread per texel
  of its depth map, the rays through shared memory; it counts the probes traced into `gGiStatistics[frame]`, host
  visible, read back when the frame index comes round, which `SPEEDO_GPU_TIMINGS=1` averages at exit) blends them into
  the probe: its irradiance faces (`SHADER_TYPES_GI_IRRADIANCE_COUNT`, 6 a color, `gGiIrradiance`; face 0's w counts the
  frames it has), and its depth map (`gGiProbeDepth`: `SHADER_TYPES_GI_DEPTH_SIDE` 16 by 16 octahedral texels of the
  mean distance and the mean of its square, half2, DDGI's, and the nearest the rays within about 20 degrees met,
  blended as the mean is (packed in the one word with the mean: the mean a half, the spread and the nearest over the
  mean 8 bits each, `GiPackDepth`; a second word doubled what the resolve streams per block, every probe's 256
  texels, for 2 to 3 ms a frame) (a running minimum relaxing by a percent a frame stepped whenever a nearer
  ray came, and the weights with it: the lion wall's still frames stepped 28 of 255 rather than 14; see below); a texel takes the rays near its direction by `cos^50` of
  their angle, `SHADER_TYPES_GI_DEPTH_SHARPNESS`, four texels a thread, and keeps what it had where none is near: at
  8x8 and `cos^10` a texel toward a wall mixed the wall's distance with what is beyond it, and a sunlit probe outside
  the wall lit the points at its base inside, and Sponza's walls showed the probe cells as blotches; 16x16 cost 2 ms on
  Sponza. A texel's first rays count whole, not by the probe's age: blended in by it, a texel first hit at age 10 held
  a tenth of its distance, and the probe failed the test that way for a second, dark patches that followed the camera
  as the clips scrolled; and a texel no ray is near yet takes a wide cone's rays, `cos^4`, as a provisional value, its
  mean negated, until rays near it replace it whole: a hole was a direction the probe saw nothing in, which the test
  took as seeing nothing at all, and with every probe of a cell new after a scroll a point took their mean unweighted,
  the sunlit probes beyond the walls too, the leaks that followed the camera. The depth rays walk the probe's own clip
  to its edge rather than to its box around the eye (`GiOccluderHit`'s `box`, which the shadow rays keep: what they
  see of the coarser voxels should change with the eye), and in a coarser clip the voxel they start in doesn't count,
  since a point outside a surface's shell in the finer clip is inside the thicker shell of the coarser: a probe over
  the floor below the finest clip's box traced its depth in the next clip from the first step, whose shell around the
  floor it stood in, so its depth read 0 that way and it was dead), by `SPEEDO_GI_HYSTERESIS` (0.97 at 60 frames a
  second: the frame's rays are 3% of it; raised to the frames' length in sixtieths otherwise, smoothed over about ten,
  so that a probe follows a change in the same time at any frame rate: `Renderer::myGiTimeBased`, `SPEEDO_GI_TIME_BASED=0`
  per frame; by frames, five times as long at San Miguel's 12 frames a second, and with the cache off Sponza at 27 came
  within 5% of settled after 220 frames, 170 by time), or by the
  frames it has while fewer (so a probe starts from its first frame's rays and settles in 30), or, with
  `SPEEDO_GI_ADAPT=1`, a quarter at a time through a change of the light (DDGI's adaptive hysteresis: each probe keeps
  how much its frames' mean radiance usually differs from what it holds, coefficient 1's w, and a difference of four
  times that and half of what it holds is a change rather than noise; a light stepping from 0 to 200 cd, eight frames
  later the probes are at 139 of the instant blend's 167, 105 with the hysteresis alone). It is off by default: on a
  still scene the same ray set returns every `rayPhaseCount` frames, and a ray that hits a sunlit patch tripped the
  test each time, a jump of a quarter every 64 frames in Sponza's arcade (70 of 255 at exposure 9, where the plain
  hysteresis steps 10, in a 32 frame ripple of the phases). A slow change (AnimatedCameraLight's 12% over 100 frames)
  lags 7% either way, by design. What the irradiance is blended towards is not the frame's rays but the mean of
  the probe's last whole cycle of ray phases (`GiData::cycleOffset`: past both halves of the irradiance, the cycle's
  running sum and the last cycle's mean; the running mean until the first whole cycle, and a change found by
  `SPEEDO_GI_ADAPT` starts the cycle again from the frame): a bright source smaller than a ray set's spacing (the sky
  through the gaps between Sponza's curtains) is met by one set of the 32, and blending each frame's rays in jumped the
  probe once a cycle and decayed it for the other 31 frames, a 32 frame sawtooth on the floor it lit (the user's pose
  `-9.1511,1.4025,-3.5391,26.261,106.832`, pixels from 134 to 84 of 255 and back; 3% of the pixels of the indirect
  diffuse view stepped by 32 or more); a still scene's cycles are the same directions, so not one pixel changes from
  frame to frame there now. A change of the light reaches the target once the probe's cycle ends, up to 32 frames
  later, then the blend's; no measurable cost (the frame 3.51 ms either way). The probes' buffers hold
  two halves, the frame's and the last's (`GiData::probeFirst`, `probePreviousFirst`, swapped each frame), and a
  probe's history is the probe at its place last frame (by the scroll), a probe new to the clip taking the coarser
  clip's light where it is (its previous half, at its place then, trilinear) as one frame's worth; on a reset (the
  scene changed, or the buffers were replaced: `GiData::reset`) every probe starts over and the voxels take no bounce.
  Memory per probe: 2 x (3 x 96 B of faces + 256 B of depth) + 128 rays x 8 B; Sponza's 3 clips of 16x8x10 probes
  take 5 MB.
- **The resolve and the shading**: after the probes, GI Resolve (`ComputeGiResolve`, a group per block of 4x4x4
  voxels, with its 8 probes, or 27 where it spans two cells an axis, in shared memory) gives each voxel of each clip
  the irradiance faces of the 8 probes around its center, each weighed as DDGI weighs them: by how much it faces the voxel's
  open side (its occupancy's gradient over the 3 voxels each way, the nearer counting more, plus a surface's voxel's
  own normal: by the normal alone a floor's voxel at a wall's base was tested from a point straight up, as close to the
  wall as before; over the 6 neighbors alone, the voxels a lookup lands in, a voxel or two off a surface, had no side,
  so there every probe counted by its visibility alone, and a probe behind Bistro's hotel facade, whose depth toward
  the street mixes the facade's with the window's beside it, which Chebyshev lets through, lit the wall by the window
  with the hotel's interior: the air voxels in front of the wall resolved to 1.0 against the wall's own 0.26, 0.4 with
  the reach, for 0.3 ms of Sponza's frame; wrapped,
  `facing^2 + 0.02`: DDGI's 0.2 for the probes behind let the probes below Sponza's floors, in the clipmap's margin and
  lit by the environment through its bottom, in where every probe of the corridor failed), by whether it sees the
  voxel's point (out that way by a fifth of a spacing, at least a voxel: DDGI's normal bias; a probe's depth map along
  the direction to the point, bilinear across the octahedral map's folds, and Chebyshev's inequality where the point is
  farther than the mean, cubed, at least a thousandth; and a point well behind the nearest thing the texel's rays
  met, past one and a half times its distance and a tenth of a spacing, is behind the nearer of two things the texel
  sees, the wall with the window beside it, which the mean and the variance can't tell: nearer than the mean the test
  let the point through, and beyond it the mix's variance did; a probe inside the building by Bistro's square, its
  texel toward the ground before the entrance at a mean of 0.39 spacings with the wall at 0.066, saw the ground at
  0.54 at 0.55 and, alone of its cell's probes to pass, lit the square and the entrance with the interior (the user's
  "yellow lights that don't make sense"); such a probe takes a tenth of the floor below too, since a point every probe
  fails takes their mean, which one behind a wall, lit by what is there, pulled its way; by a tenth of a spacing
  alone, a texel toward a floor at a grazing angle failed its probe too, meeting the floor nearer than the point all
  the way, a continuum, where the point is within half again of the nearest. DDGI's twentieth of a sunlit probe outside a wall lit a band
  along the floors inside, and the floor only needs to be more than nothing, since the lookup normalizes; a probe whose
  depth that way is nothing sees nothing at all, being inside a surface's voxels, and is skipped, as is one marked
  inside, above), crushed below a fifth (cubed, so that a point few see takes the mean of them rather than nothing),
  then a floor of a thousandth (after the crush: a floor before it was crushed to 1e-11, which the volumes' half floats
  hold as 0, so a point no probe saw had no light at all, black patches wherever every probe of a cell failed), and
  only then by where the point is between them (trilinear: crushing the product crushed every probe at a cell's center,
  where each is an eighth, and the facing ones least, which drew the probes' cells as a checkerboard across every
  wall), premultiplied, and the sum of the weights. A surface's voxel is resolved as the voxel out along its open side
  would be (its center moved a voxel that way for the trilinear weights and the point, within the block's cell: past
  it, the probes read were not the ones in shared memory, black and colored garbage on the floor behind Sponza's
  fountains): the lookups land in it where their offset is small (in a finer clip's voxels, below), and its own cell's
  weights admitted a probe the voxel above rejected, a bright band along Sponza's floors at their walls that survived
  everything else (found by sampling each clip's own voxels, which has none; one or two clips have none either). The
  Probe State debug view (`GiProbeStateDebug`, a data view) shows, of the 8 probes around a point in the clip that
  lights it, the share marked inside (red), the share whose depth toward the point is empty (green) and the share
  failing the test (blue): every camera following artifact so far showed in it as green or blue where the lit view
  showed nothing yet. `SPEEDO_GI_DUMP=<x,y,z>` (or `pixel:<x>,<y>`: the point under a pixel, which `ComputeMain` unprojects into
  `GI_STATISTIC_DUMP_POINT` for the next frame's dump, `GiData::dumpPixel`: what found the entrance's probes) prints, at exit, per clip the probe nearest the point: its position,
  offset, age, backface count, its lattice point's voxel coordinates and the column of voxels through it, its depth
  map, the first rays' depths, its light along the axes, a walk straight down from it (`GiOccluderHitDebug`), the
  volumes at its voxel and along a line from the point into -z (`GiData::dumpPoint`: occupied, red along +x and
  the weight per voxel, which is what found the alternating cell): what found the dead floor probes and the probe
  beyond the end wall; and the surface cache entry of the point's voxel (`GI_DUMP_CACHE_FIRST`: its samples and inside
  rays, its rays' origin, the voxel's open side and normal, its light per face, and up to 8 of the frame's rays, their
  distance and light: what found the slot behind salle_de_bain's bench lit by the sun, see `GiOpenAlong`). View > Camera > Copy Pose (ctrl/cmd+shift+c)
  copies the view as the variables that reproduce it (`SPEEDO_CAMERA`, `SPEEDO_AUTOLOAD_MODEL`, the environment's path,
  and the settings that aren't their defaults, `ui::ApplyPoseSettings`'s: the exposure, the tonemapper, the shadows,
  `SPEEDO_GI` and the indirect lighting's, the upscaling, the sky's time, `SPEEDO_DEBUG_VIEW`; then `# window <w>x<h>`,
  the framebuffer), for reports: an artifact is a pose first. View > Camera > Paste Pose (ctrl/cmd+shift+v;
  `SPEEDO_PASTE_POSE_FRAME=<n>` once n frames are drawn, for tests) applies such a text from the clipboard
  (`ui::ParsePose`; a shell's `export`/`env` words are skipped) to the running client: the settings by the same function
  the startup takes them from the environment with, those it leaves out back to their defaults, the model and the
  environment loaded where they aren't the ones installed (by the scene's file path, scene and variant, and
  `Scene::GetEnvironmentPath`), the window's framebuffer resized to the comment's size unless fullscreen, and the camera
  placed as `SPEEDO_CAMERA`'s once the pasted model is installed: Bistro pasted over DamagedHelmet came out as a run
  started with the same variables (a mean of 1.7 of 255, the lighting's frames). They go in 5 `R16G16B16A16Sfloat` 3D images
  (`gGiVolumes`: per color its faces +x, -x, +y and -y, then red's and green's +z and -z, then blue's and the weight, so the
  shading reads vectors, see `GiSample`; the clips stacked along z, a lookup clamped inside its clip's slab), written
  as storage images (`gGiVolumeImages`) and sampled with the clamping sampler, always in the general layout (the
  voxels' lighting samples them in the pass that writes them; Main declares a storage read). rhi's 3D images:
  `ImageCreateDesc::depth` above 1 (not serialized: the cached images are 2D) and `ImageViewCreateDesc::volume`. `Shade`
  takes `GiSampleSurface` in place of the environment's irradiance where a clip covers the point (the environment's
  elsewhere): the finest clip holding it more than a probe spacing from its edges, blended into the next over 8
  voxels; there the 8 voxels around the point offset 0.75 voxels along its normal and 1.5 toward the eye (the line of
  sight is clear; the normal alone led into the other surface's voxels at a concave edge), in each clip's own voxels
  (in the finest clip's for every clip, a coarse clip's lookup at a thin wall landed within the filter's reach of the
  wall's outer shell voxel, resolved from a point outside, where a probe outside the building passes: the end wall of
  Sponza's corridor lit white by the sky beyond it, the leak that followed the camera as the coarse clip took over;
  with one clip it was dark, which is what found it), twice as far where that finds no probes, filtered, their
  premultiplied faces over their weights (`GiEvaluate`). The specular takes the same
  lookup: the environment by the indirect light's share of the environment's irradiance (luminance, at most 1; all of
  it in the open), and, by roughness (a smoothstep from 0.2 to 0.9), the faces along the reflection. Kept as arrays
  of the values (28 as harmonics) in the main pass's lookup, they spilled: Bistro's main pass took 31 ms. The lookups cost Sponza's
  main pass 1.5 ms (3.5 to 5.0) and Bistro's street 2.4 (3.8 to 6.2); the volumes' reads were 0.6 of Sponza's as harmonics (bands 0
  and 1 alone, 4 volumes: 4.45; the cube's 5 volumes hold 19 values where the harmonics' 7 held 28), the rest the lookup
  itself (the clips' weights, two attempts, the specular's second evaluation).
  Its debug views (View > Debug View > Indirect Lighting, see "Debug views"; `SPEEDO_GI_DEBUG=<n>` is the n-th,
  `GiData::debug`): 1 the indirect diffuse alone, 2 the resolved weight (how much a point sees of the probes around it),
  3 and 4 the radiance of the voxel behind or in front of a point, 5 its albedo; 6 a reference: each probe's light
  from 512 rays through the voxels straight to the environment, nothing blended (seconds a frame); 13 the surface
  reference (`GiPixelReference`: `SPEEDO_GI_REFERENCE_RAYS` cosine weighted rays from the point itself, 256, fewer
  where a frame of it runs past Metal's queue timeout, Bistro's long walks to the sky at 64; its spiral turned per point by a hash,
  else its fixed directions made radial streaks; the rays shared out among the clips that hold the point by the
  weights the shading blends them with, each ray one clip's, so a pixel costs the same however many hold it: from the
  finest alone, the edge of its box cut the vault in two shades, and every ray in every clip ran past Metal's queue
  timeout; an origin in an occupied voxel, an adjacent surface's shell at a concave edge, moves out of it along the
  voxel's normal turned toward the eye, a voxel and a half at a time, `GiReferenceOrigin`: along the normal as
  stored, an inner shell voxel's led further into its solid, and the vault's ribs drew black lines; seconds a frame:
  capture it after a few frames); 7 which clip lights a point, 8 the indirect diffuse
  with the finest clip's voxel and probe planes drawn over it (`GiClipDebug`); and the Voxel World and Probes views (the
  8 probes around a point as they hold them, trilinear, without the resolve's weights). The kernels dispatch rows of
  `SHADER_TYPES_GI_ROW` threads (`GiThread`), so a volume's size isn't capped by a dispatch.

**Tracing the triangles** (the default since 2026-10-21, the user's choice; View > Indirect Lighting Rays switches between
the triangles and the voxels at run time, `Scene::Settings::traceTriangles`, `FrameInputs::traceTriangles`;
`SPEEDO_GI_TRACE=voxels` starts with the voxels, and View > Camera > Copy Pose writes it): a model loaded gets a BVH of what the voxels hold of it (`gfx::bvh::Build`, `gfx/bvh.h`: its static triangle lists that aren't
blended or transmissive, at each instance, in world space), tinybvh's compressed wide BVH (CWBVH; the `ports/tinybvh`
overlay, not in vcpkg's registry, pinned to a commit of its main branch), built with spatial splits (tinybvh's `BuildHQ`, an
SBVH: a triangle spanning several nodes' space is referenced from each, so they overlap less and rays visit fewer;
`SPEEDO_BVH_BUILD=fast` binned SAH alone, `Build`, as it was, a cache entry of its own: Bistro's frame 66.0 to 57.6 ms,
San Miguel's 88.7 to 87.1, Sponza's 31.8 to 30.4, at 214.6 MB of BVH rather than 193 and 0.58 s of build rather than
0.4) on the executor (`TINYBVH_NO_BUILTIN_POOL`,
its spawn, barrier and parallel for hooks over the `TaskExecutor`) when the model is imported and kept in its asset cache
like the rest of it (after the staging buffers; `bvh::Version`, tinybvh's version and a `bvh-vN` tag, is part of the
cache key: bump it when `Build` changes), a merged set's built at upload from the staged triangles (its files are placed
when merged), and uploaded with the model whichever rays are chosen, so switching is immediate (`gBvhNodes`, `gBvhTriangles`; a default
node's worth of zeros while a model has none). The rays traverse
it in `bvh.slang` (tinybvh's OpenCL kernel, `kernels/traverse_cwbvh.cl`, ported to slang, with its dev branch's stack of a
uint per entry, the parent node and the hit bits left, dropping groups past 24 rather than writing past the stack): the
probes' rays (one closest hit answers the radiance and the depth, `GiTraceBvh`), the voxels' shadow rays (any hit,
`GiVisibility`) and both references. Where a ray hits, the radiance is the voxels' (`GiRadianceAtHit`: the shell voxel half
a voxel off the surface on the ray's side, in the finest clip whose box around the eye holds the point), so the lighting,
the bounce and the probes are as before; what changes is what a ray meets: the triangles, with no surface a voxel thick
and no opening narrowed by its shells. With the triangles the resolve doesn't test the probes by their depth maps either: whether each of a voxel's 8 probes
sees it is a ray between them through the static BVH (to the voxel's center, a surface's voxel's half a voxel out along
its normal, stopping a twentieth of a voxel short of it: a quarter left a surface crossed within it untested, and a
probe over CornellBox's ceiling saw a corner voxel's point 4 mm under it, a streak along the ceiling's edge; a back face
met means the probe is behind a wall, and it is skipped, a front face hides it), kept per voxel
(`gGiVisibility`: 2 bits a probe and a valid bit, in two halves as the probes, by the scroll) and traced again only for
the blocks where a probe moved or is new (`gGiSharedProbeMoved`). The resolve is then a kernel of its own (`ComputeGiResolveTraced`,
`GiResolve<false>`), which neither reads the depth maps nor holds them in shared memory: the generic one, a group
loading its 8 probes' 256 texels each, 8 KB of shared memory, whatever the rays traced, cost Bistro's frame 2.4 ms, San
Miguel's 1.5 and Sponza's 2.5 at the benchmark poses (43.65 to 41.26, 47.10 to 45.57 and 26.02 to 23.52 ms, balanced),
the renders the same. Every probe is traced on a retrace, the ones the
resolve then skips as inside too: skipped before the trace, their bits stayed visible, and a probe whose inside share
hovered at the line lit what it couldn't see once it fell below, in some runs and not others. A probe weighs its
facing's 4th power (nothing behind the voxel's open side), a thousandth of that if hidden; and the hidden ones bring
the least light of theirs (per face and color), not their mean: a voxel no probe sees can confirm none of it, and a
pocket no probe sees into is a shadowed one. salle_de_bain's niches, 67 units out in the last clip (3.5 m probes, 0.44 m
voxels), took all of that: by their depth texels, half a metre wide there, the probes behind the back wall (in the
closed space the window's emitter lights, 40 times the room's light) passed for the niches' points, and with the
traced test no probe sees into the niches at all (the room's hug the wall plane), so their voxels took the hidden
probes' light, and those behind the wall are straight along the niches' open side; neither the face a hiding ray meets
nor how far from the voxel it meets it tells a wall between from a niche's lip. The niches went from white blotches
that flickered (the largest pixel step a frame 24 of 255) to as dim as the walls around (the statue's base 0.21 to
0.055 against the walls' 0.068, then 0.077 against 0.058 once the last clip's margin was a whole spacing, as in the
surface reference, whose niches are as bright as its walls; steps of 2 at most). A test in the shading's lookup of which voxels are of the point's side (a
voxel's open side, kept per voxel, read for its 8 voxels, the filter by hand where any is not) cost Sponza's main pass
6.3 ms and wasn't needed once the voxels were right. Traced every frame it cost Bistro's street 12.6 ms; refreshed a
voxel in 8 a frame as much (a few threads of every simd group trace), a block in 32 a frame 2.6 ms (the dispatch waits
for its slowest group, whose threads trace 8 rays each in turn); settled, none are traced (4,700 blocks a frame while
Sponza's probes settle, none from frame 80). All of it, with the back sides, the margin below and the shorter rays,
against before (interleaved builds, the `Frame` scope): salle_de_bain 11.4 to 11.55 ms, Sponza 22.4 to 23.6, Bistro's
street 25.15 to 26.5, CornellBox-Sphere 9.1 to 9.9; of that the whole spacing of margin is Sponza's 0.5 and
CornellBox's 0.27 (its probes trace now, where they lay dead in the outer surfaces). Sponza against its
reference 0.386 to 0.390. The voxels the back sides of single sided triangles wrote (outside the room, inside a solid)
are given no light: resolved for that side, the row over salle_de_bain's ceiling (a single sided triangle 0.08 m under
the row's centers) was lit from the sky through the space over it, and a wall's top, whose lookup's filter reaches
that row, drew a bright line along every wall (0.56 against the wall's 0.13); at a wall's top the wall's room side and
the space over the ceiling share a voxel, whose open side the occupied voxels below it turned up, so they go too
(CornellBox the same in both ray modes; Sponza 0.385). Drawn sides' normals winning the voxel's took the room's level
down a tenth (the light from outside it). A hit carries its normal toward the ray, its side and its flags (`BvhHit`). A
backface is a single sided triangle seen from behind (the compressed triangles' transform divides the normal by its
largest component, whose sign `Build` keeps in their last float4's x, with `BVH_TRIANGLE_FLAG_*` and the material's index
in y and the first index in z). Alpha masked triangles (`BVH_TRIANGLE_FLAG_ALPHA_MASKED`) are cut out as the main pass's
alpha test does (`GiAlphaMask`, an `IBvhFilter` the traversal asks of each hit: the base color's alpha or the mask's at the
hit's texcoords, `BVH_ALPHA_LEVEL`), so light passes through foliage's gaps, which the voxels hold whole (0.2 ms on Sponza,
1 ms on Bistro's trees). What moves has a BVH of its own (`bvh::Moving`: what the voxels hold of it, placed each frame on
the cpu in `Model::Animate` as `WorldPosition` places it, morphed, skinned and by the instance, and refit; per frame,
host visible, `gBvhMovingNodes`, `gBvhMovingTriangles`; placed only while the rays trace triangles, `Animate`'s
`placeBvh`), traced after the static one (`GiTraceTriangles`), except by the
static voxels' kept light (`ComputeGiLightStatic`), which nothing moving may shadow for good (`GiVisibility`'s `moving`).
Its leaves are split to at most 16 triangles before the conversion (`BVH_GPU`'s leaf references count 7 bits, and
tinybvh clamps a bigger leaf, whose rest is never tested). One BVH for all of it, not one per submesh: BrainStem's dozens of overlapping parts each took a traversal (its frame 14.6
ms, 11.6 with one). It is tinybvh's binary layout in Aila and Laine's form (`BVH_GPU`, `BvhTraverse2`, a port of
`kernels/traverse_bvh2.cl`): refitting the binary tree and copying it costs BrainStem's 61000 triangles 1 ms a frame,
where encoding a CWBVH again took 3 (the refit is of the wide tree's: converting the binary tree again collapses it by the
new bounds, into another node count). For the static BVH the CWBVH stays: the binary layout took Bistro's frame from 25.2
to 28.3 ms (Sponza the same). Animated models against the voxels' walk (the `Frame` scope): CesiumMan 7.3 to 7.1 ms, Fox
the same, BrainStem (all of it moving, so every voxel's light traces the moving BVH each frame) 9.5 to 12.1.
tinybvh's dev branch stores CWBVH triangles uncompressed (3 float4: edges and a vertex, whose cross product faces the right
way), so moving the port to it means `BvhTraverse`'s triangle test follows (`bvh.cpp` doesn't compile without
`CWBVH_COMPRESSED_TRIS`); they would also fix the compressed triangles' precision on tiny ones (under a millimetre in a room:
living_room, salle_de_bain, San Miguel), whose barycentrics come out wrong by tens of percent and which a ray may miss for
one behind, where the binary layout's test is right (`CheckBvh` counts those apart and warns). A hit's normal, side and flags are read once the traversal is done (`BvhFinish`): read at
every accepted hit, the loop carried them all and Bistro's frame took 2 ms more. Bistro's cost tried and kept: the dev branch's stack of a uint an entry (no change); ordering the
probes' threads by ray (no help: each probe turns its ray set by its own phase, so neighbors' rays don't share
directions). Measured (Sponza's inside, Bistro's street, CornellBox-Sphere, as below, interleaved): the
indirect lighting's share of the frame (the `Frame` scope less a run without it) 9.9 ms walking the voxels, 10.5 tracing;
7.6 and 11.2 (Bistro's 2.8 M triangles, 193 MB of BVH, built in 0.4 s at import, without spatial splits; Sponza's in 40 ms); 4.9 and 3.6.
`assettest` checks each model's BVHs in both layouts (`Build`, `BuildBinary`) as `bvh.slang` traverses them (compiled to
c++, `src/tools/bvhkernels.slang`) against testing every triangle: nearest hits, distances, sides, barycentrics and any
hits (`CheckBvh`; a triangle tied with a coplanar one, e.g. back to back, isn't a disagreement). Each method's
probes against its own surface reference (Sponza's inside at exposure 3, mean |log2| at 48 pixels): voxels 0.45, the BVH
0.38; the two references differ by 0.78, the BVH's 48% brighter (the voxels' shells take that much of the light through
the arcades and onto the curtains). The quick tier passes with it, profile and debug (validation).

**The surface cache** (View > Indirect Lighting Surface Cache, on by default since 2026-10-23, the user's call; `SPEEDO_GI_CACHE=0`
to start without it, `FrameInputs::giSurfaceCache`; `gGiCache`, `GiData::cache*`, the cache section of `gi.slang`): the
probes, 8 voxels apart (1.8 m in Sponza's last clip), spread the light through a gap of a few tens of centimetres (the sky
under Sponza's curtains, at the user's pose `7.7949,0.8419,-3.7077,0.531,-91.763`) into spots a probe cell wide, which
changed as the finer clips took the floor over; per voxel near a surface, the cache measures it at the voxel's size, a
band along the curtains' lower edges down the whole corridor (`docs/reference/sponza-corridor-surface-cache.png`: the
probes alone against the cache, lit, and the floor, as the user approved it). An entry is 3 uint4: the 6 faces' rgb as
half floats, the samples and the rays traced 16 bits each, the mean of the samples' squares (`GiCacheSample`, the light
along the voxel's open side), and the open side (octahedral, 8 bits a coordinate) and the rays that started inside
something. Only the blocks of 4x4x4 voxels that hold entries and were read lately have them, a page of 64 each from a
pool (`gGiCache`, `GiCachePopPage`): each clip has a page table a frame (`gGiCacheBlocks`, two halves swapped each frame,
so that a clip that scrolled finds its blocks' pages where they were; two words a block: the page, with a bit its voxels
holding an entry set in the resolve, and the last frame a lookup read it, `GiCacheTableWord`). A block keeps its page
while it is near a surface, held an entry last frame and was read within `SPEEDO_GI_CACHE_KEEP_FRAMES` (1024;
`GiCacheKeeps`): the release (`ComputeGiCacheRelease`) frees the others' pages onto a free list (`gGiCacheFree`), and
the resolve takes pages for the blocks that wanted one last frame and are read now (a frame late: a block's first
frame near a surface has no page), never both in one dispatch; the pool starts at a thirty-second of the clipmap's
blocks and grows to a quarter more than the resolve counted (`GI_STATISTIC_CACHE_BLOCKS`), its pages copied over (the
GI Cache Grow pass, the new pages pushed by `ComputeGiCachePrepare`). The resolve (`GiCacheUpdate`) carries the voxel's
entry over in place (a voxel new to its clip from the coarser clip's, as 8 samples, where the coarser block keeps its
page, so that it is still its own) and traces `SPEEDO_GI_CACHE_RAYS` (1) rays from the voxel (Roberts' R2, offset per
voxel by its place in the world, three quarters of them over the hemisphere its open side faces and the rest over the
other, each sample weighed by its side's density, `kGiCacheForwardShare`, `GiCacheEstimate`: over the sphere evenly,
half the rays added little to the faces the lookups read; over the open side's hemisphere alone, Sponza's interior came
out 6% darker, the lookups reading the other side's faces at edges and corners; split, Sponza came within 5% of settled
after 620 frames rather than 960, its noise 0.0052 rather than 0.006, and settled as before to 0.025 mean |log2|; a surface's voxel's out along its open side by a voxel, but no
further than halfway to a surface facing back at it there, `GiCacheOrigin`: CornellBox's sphere a voxel off its wall had
the voxel out along the wall's open side inside it, the entries were inside, and the wall drew a black crescent along
the sphere, stepped at the voxels, at the user's pose `0.1488,-1.1952,0.7514,12.998,14.697` at resolution 192), as the
probes' rays are traced, into a running mean of at
most `SPEEDO_GI_CACHE_SAMPLES` (1024; 32 where something moves), and at most half the frames since it started over
(at least 32; `SPEEDO_GI_TIME_BASED=0` without), tracing 4 rays a voxel for its first 256 frames
(`kGiCacheWarmFrameCount`, then `SPEEDO_GI_CACHE_RAYS`): its first frames' samples, read while the probes and the
bounce build up and overshoot, then fall out sooner (Sponza from a cold start within 10% of settled after 280 frames
rather than 840, 460 with one ray; 4 rays throughout, 240 but 4.8 ms a frame and noisier;
the tail, a mean of a light still settling, fades as 1 over the frames either way: Sponza's still changed 2% 2000 frames
in, see docs/performance-plan.md); a ray meeting a back face within a voxel and a half started inside something, and a
voxel with more than a quarter of those is no light (the probes' alone). The samples and those rays are kept over one
window, scaled down together past the cap (the inside rays in sixteenths): clamped apart, the samples stopped at the cap
while the inside rays went on to it, so a voxel with a tenth of its rays inside crossed a quarter after 3400 frames. Lights changing
or a new buffer start it over. `ComputeGiCacheFilter` then filters it as SVGF filters a frame (Schied et al. 2017): a pass
estimating each voxel's noise (its samples' variance pooled over the 3x3x3 on its surface, over its own samples: by its
own alone, a voxel that missed a source smaller than its rays' spacing measured no noise and refused its lucky
neighbours' light, and the curtains' light broke into dim blobs), then `SPEEDO_GI_CACHE_FILTER_PASSES` (3) passes of an
a-trous filter (taps 1, 2 and 4 voxels apart) weighing each tap by the kernel, the open sides' agreement (to the 8th), its
distance off the voxel's plane, its confidence, and how its light differs against `SPEEDO_GI_CACHE_SIGMA` (8) deviations
of both voxels' noise, the noise of a mean of at most 64 of their samples (`kGiCacheFilterCount`: as the variance of the
mean of all of a voxel's samples, it shrank as they grew to 1024, the filter smoothed less and less, and the light it had
spread from bright voxels into darker ones withdrew for as long: Sponza's interior darkened 2000 frames after a cold
start, its probes settled by 300; held, it came within 5% of settled after 960 frames rather than 1480) (by the voxel's alone, noisy bright voxels took their quiet neighbours' light and not the other way,
and the floor's band lost a quarter of its light; a sigma of 4 left blotches in it). The last pass moves the volumes'
light toward the filtered light by the voxel's samples over them and 16 (`SPEEDO_GI_CACHE_SHOW=1`: all of it), and weighs
the voxel in the lookups by that where the probes see it less (premultiplied by a probe weight of a ten thousandth, at
resolution 64, where the last clip's probes don't see into the corridor, the cache's light went to half floats'
subnormals and came out in pink, yellow and blue blotches). The passes ping-pong between the halves of the records
(`gGiCacheRecords`), which only the blocks a lookup read take (`GiCacheTakeRecord`, grown by
`GI_STATISTIC_CACHE_ACTIVE_BLOCKS`). Only what is looked up is traced and
filtered: ComputeMain stamps the blocks each pixel's lookup reads (`GiMarkSeen`, the flags' third word, in each clip that
lights the pixel), and the resolve marks a block active (`SHADER_TYPES_GI_FLAG_CACHE_ACTIVE`) where it or a block around
it was read in the last 2 frames (at its place then, by the scroll; a thread each of the 27 blocks: by the first alone,
one after another, the frame took 0.16 to 0.34 ms more); the others trace a ray every 8th frame and take their
own light unfiltered, so that the bounce doesn't change with where the camera looks (`SPEEDO_GI_CACHE_EVERYWHERE=1` does
them all: a turn in place differs from it by a mean of 1 to 2 of 765). Once its window has all its samples (twice
`SPEEDO_GI_CACHE_SAMPLES` frames after it started over, `SPEEDO_GI_CACHE_SETTLED_FRAMES` to say when) and the sun isn't
being followed, a voxel where nothing moves traces every other frame (`GiData::cacheSettled`): the same samples over
twice the frames, of a light that doesn't change (Bistro 57.5 to 56.3 ms, San Miguel 68.3 to 67.4: its rays are a
fifth of its 11 ms in Bistro's resolve, the rest the entries' work). Costs (RTX 4070 Ti SUPER, 1920x1080, the `Frame`
scope): Sponza's corridor 3.6 ms without it, 5.7 with it (8.2 everywhere), Bistro's street 6.5, 9.4 (12.4),
CornellBox-Sphere 2.6, 4.7 (4.9); everywhere, a ray a voxel was 1.9 ms of Sponza's and a filter pass 0.4. Its storage exists only while it is on, and holds the blocks that hold entries and were
read lately: at Sponza's corridor, near nothing at resolution 128 and 0.3 GB at 384. An entry for every voxel of every
clip, two halves and the filter's scratch, as it was, took 0.3 and 6.4 GB (Sponza then used 14.9 GB of the GPU's 16);
paged for every block near a surface (25,000 of 32,500 at 128, 290,000 of 730,000 at 384, half of their voxels holding
an entry, a tenth of them read in a frame), 0.1 and 1.1 GB. Paged, the renders are the same as the dense cache's (to a
mean of 0.2 to 0.8 of 765, still, walking and turning); taking pages only for what is read, a block nobody looks at
carries the probes' light into the bounce rather than its cache's (a mean of 1.3 of 765 still), and an area seen for
the first time starts from the probes' light, which showed nothing visible in a turn in place (a mean of 1.4 to 3.7 of
765 against the dense cache's). The frame's time is the same. A still camera changes by 0.026 of 255 a pixel a frame (the probes alone, 0). Interiors come out darker than the
probes alone (light measured at the surface, where corners and overhangs shade it, rather than in the open cells around
it), and the CornellBox's and salle_de_bain's probe blotches go. The surface reference (debug view 13) is no use as a
reference (the user, 2026-10-24: too slow, and it doesn't look like one): seconds a frame, one bounce on its first frame
(the voxels' starts after 32 frames), noisier than the cache; judge by A/B captures instead, until a real one exists
(settled, accumulated over frames, or path traced).

**The near field** (View > Indirect Lighting Near Field, on by default since 2026-10-24; `SPEEDO_GI_NEAR=0` to start without
it, `FrameInputs::giNearField`; `ginear.slang`, `GiData::near*`; where the rays trace the BVHs): where a solid rests on a
surface, the surface's voxels at the contact are also the solid's never drawn inside, which gets no light (see the
resolve), and the gap beside it is thinner than a voxel, all its surfaces lit by the gap's own unlit voxels, so the
volume's lookups there found no light, in blocks a voxel wide (the user's CornellBox poses, the 11 models side by side:
`0.0062,-2.2486,1.2961,23.077,13.130`, black rectangles under the spheres; `0.1488,-1.1952,0.7514,12.998,14.697` at
resolution 192, red and black blocks where the sphere touches the green wall). Every other pixel each way of the
pre-pass's depth (View > Near Field Rays, `FrameInputs::giNearStride`, `SPEEDO_GI_NEAR_STRIDE`, 2; 4 traces a sixteenth of
the pixels: 4 to 11 ms less at the benchmark poses, coarser at contacts, docs/performance-plan.md; a change starts the
frames' history over) traces a ray from its surface
through the BVHs (GI Near, `ComputeGiNear`), as far as
`SPEEDO_GI_NEAR_REACH` (8) voxels of the clips that light the point, its direction one of 16 over a 4x4 tile of traced
pixels (the traced shadows' disk points lifted onto the hemisphere, cosine distributed), and takes the light what it
meets reflects (the voxels' there less their emission, `GiHitReflected`, read a voxel and a half off the pixel's tangent
plane along the hit's surface: within it the voxels at the hit are the pixel's surface's too, and the floor under the
left sphere drew its own lit light as a tan crescent; moved as far along it as reaching that height takes, and a
surface facing the pixel's that doesn't get there within 3 voxels, a sphere's underside near the floor it rests on, is
in a gap thinner than the voxels, which hold neither side's light there, and gives none: moved a voxel and a half
whatever the slope, the underside rose a fraction of that, the reads along its normal crossed the floor into its
underside's voxels, lit by the gap below, and each sphere had a bright crescent around its contact, the user's pose
`0.8869,-1.0542,1.2337,17.751,7.611`; reading the floor's own voxel there, the sun's patch beside a sphere lit its
contact) or, where it meets nothing, the volume's light along it from its
end. Emitters' light is the volume's, which averages it over its probes' rays and frames: with the voxels' emission, a
ray meeting anything in an emitter's voxel took all of it, and Bistro's lanterns (a bulb of a few centimetres, 3.5, in
voxels of 30 cm with their frames and glass) lit the walls around them yellow in blotches, a hundred times what the bulb
gives (the user's pose `3002.7041,404.1237,1410.4180,10.076,5.141`); with the bulb's own, rays rarely met it and each
that did was a white speck. The emitters light the scene themselves now (see "Emissive surfaces as lights"); where they
don't, the probes' rays take a hit's own emission (`GiHitRadiance`: the voxel's reflected light and the triangle's
emission, its material's times its emissive texture at the hit), not its voxel's. (Only the alpha masked triangles
carried their material then, so every other emitter a ray met gave nothing: EmissiveStrengthTest's cubes lit nothing
around them. Every BVH triangle carries it now, `bvh-v3`.) GI Near X
and Y filter it at the traced pixels alone (at every pixel, as they were, the filter and the frames cost Bistro 5.4 ms,
San Miguel 8.5 and Sponza 9.3 at the benchmark poses; at the traced pixels the frames take 2.1, 1.8 and 3.2 ms less, the
main pass's upsampling taking some back, and differ from before by about twice the frames' noise, a contact a few pixels
wide a little lighter), over the traced pixels within 16 along rows, then columns, on the pixel's surface (by the plane and the
normal, as the traced shadows' filter), by a tent of the distance (a box of the 17 traced pixels around a traced one and
the 16 around the others drew stripes a pixel wide and a bright ray as a square). GI Near Temporal
(`ComputeGiNearTemporal`; `SPEEDO_GI_NEAR_TEMPORAL=0` without it, View > Indirect Lighting > Near Field Over Frames),
at every pixel of the pre-pass's depth, upsamples that from the traced pixels (`GiNearUpsample`: bilinear over the 2x2
around, each by how much it is on the pixel's surface, as the emitters' light, with their fallbacks) and blends it into a
history reprojected by the pixel's point through the view's last view projection (`ViewData::previousViewProjection`,
`previousEyePosition`, which `Views::Upload` keeps from its last upload, w 0 where the view moved in the target, 2 where
the view didn't move; both frames' jitters taken out, `PixelHistoryAt`, see the emitters'), bilinear over the 4 pixels
around where it was, each only if its point was as far from the last eye as this one's and its normal within 25 degrees
(by the pixels' surfaces, `SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE`: RGBA32F, distance, packed normal and the frames
the history holds; within a
fifth of a percent and two pixels' depth along the surface), else the 3x3 around that are (`PixelHistory`), none for what moves (the
main depth's stencil), by 1 over `SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT` (16), a frame far brighter than what the history
holds taken as at most 8 times that, or that and a quarter of the frame's white more (`HistoryFireflyClamp`, the
emitters': a traced pixel's ray that meets what is bright, the voxels by a lit window's interior from a window frame's side
across the opening, is a sixteenth of the pixels around at a stride of 4, and faded over the 16 frames, white fireflies
along Bistro's window frames at night, the user's pose `724.7479,659.6074,-2775.4233,-21.034,93.844` at 02:14: pixels
that jumped over 80 frames 2291 to 582, 393 with the near field off; in its light alone 1568 to 244, its mean the same),
into two images each, the frame's by its parity (`GiData::nearFrame`, fixed slots: a descriptor written every frame takes a set
from the pool); the rays turn every frame (`GiNearDirection`), each pixel taking every one of its tile's 16 directions
over 16 frames. Fixed, the rays' grain stayed on the screen as the camera moved (the user: "fixed to the camera"; their
pose under Sponza's curtains, `2.8260,0.3324,-2.7619,27.122,-513.942`); over frames it is smooth, and a slow turn
smears nothing. It costs 0.9 ms a frame on KosmicKrisp by the `Frame` scope (Sponza 30.1 to 31.0, Bistro's street 57.3
to 58.3), which misses compute work there: by the draw thread's frame time, 18.9 ms at Bistro's evening benchmark pose
and 16.4 at San Miguel's, its rays 10, its filter 4 to 8 (every pixel) and its frames 1 (docs/performance-plan.md). The frames were
the traced pixels', which the main pass upsampled, until temporal upscaling's jitter made the pixels upsampled from a
traced pixel at an edge change as a block, as the emitters' did (see "Emissive surfaces as lights"): per pixel, the
near field's light alone (the indirect diffuse debug view with `SPEEDO_GI_NEAR_SHOW=1`) at the user's San Miguel pose
`7.6715,4.3834,1.7963,-22.087,-88.462` (performance, a stride of 4, resolution 64) went from 870 pixels a frame changing
by more than 32 of 255 to 447, and by more than 64 from 200 to 41, and its leaves and branches came out crisper. Main's
opaque fragments (`PushConstants::prepassImages`' `PREPASS_IMAGE_GI_NEAR`) take what the rays gathered at their pixel
(the frames' pass runs whether or not over frames, blending by 1 without), by how many met another surface nearer than a
contact (`FrameInputs::giNearContact`: View > Indirect Lighting Near Field Contact, `SPEEDO_GI_NEAR_CONTACT`, 2.5 voxels), the
volume's lookup by the square of the share that met none (`SPEEDO_GI_NEAR_POWER`), and the rays' light alone where the
lookup saw no probe (`GiNearIrradiance`). Blended by anything met within the rays' reach, the rays' light took every
junction over: Sponza's columns between the curtains lit from the courtyard through gaps the voxels close, and a broad
darkening along every wall. At a contact of 1.5 voxels the rectangles' edges stayed faintly; at 2.5 (the user's choice)
faint straight edges a few voxels out are what is left, and Sponza's columns are a little lighter than at 1.5. Against
the near field off, by mean difference of 765: Sponza 8.4 (6.4 at 1.5), Bistro's street 3.6 lit and 10.4 indirect (contact
shading at every moulding and frame), salle_de_bain and CornellBox 1 to 2. Costs (RTX 4070 Ti SUPER, 1920x1080): Bistro's
street 0.9 ms of rays and 0.4 of filter, Sponza's corridor 0.24 and 0.4; a ray every pixel took Bistro's 3.4 ms, and rays
2 voxels long 1.6 (the rays' count costs, not their length). `SPEEDO_GI_NEAR_SHOW`: 1 the rays' light alone, 2 the share
that met no contact, 3 and 4 the light of the rays that met something and of those that didn't.

Measured on KosmicKrisp by the frame's gpu time (2560x1440, 3 clips, 128 voxels, probes every 8, 128 rays, hysteresis
0.97; the gpu's state drifts between runs by up to a third, so compare runs interleaved): before relocation
CornellBox-Sphere 9.3 ms (4.5 without the indirect lighting; 7.5 with 32 rays, where its 3.0 ms are the voxels'
lighting 1.3, the resolve 0.9, the rays 0.6, the probes' update 0.2, the mips and flags little), Sponza's inside
(`0,2,0,0,90`) 12.0 (6.5), Bistro's street (`564,170,-2436,0,30`) 14.9 (9.9); with it CornellBox-Sphere 12.4 (4.5;
8.2 with 8 rays), Sponza's inside 13.0 (6.5); with the 16x16 depth maps and the rest of the leak fixes CornellBox-Sphere
11.8 (4.7), Sponza's inside 14.3 (6.5); with the ambient cube, the clips centered ahead and the emission clamp
(2026-10-22) CornellBox-Sphere 9.8, Sponza's inside 14.5 (7.4 without, that session), Bistro's street 16.6,
DamagedHelmet 10.4. Probes every 4 voxels cost 8 times the
probes (CornellBox 12.8 ms with 32 rays), with no visible gain at these voxel sizes. A still camera in Sponza's arcade
over 80 frames after settling: the frame's mean steps by 1.2% at most between frames and stays within 1.3% of its
average (64 rays 2.1%; hysteresis 0.9 with 128 rays 3.9% and 4%, with 64 rays 7% and 7%: the arches' light is a noisy
thing to sample with 64 rays). Renders: CornellBox's walls and ceiling smooth, with the lamp lighting the ceiling around
it, colour bleeding on the ceiling's edges and the spheres' contact shadows; Sponza's inside within a shade of its
surface reference (its arcades a little brighter), its sunlit upper floors lit by the sky; Bistro's street lit between
its walls. Against the old cascades: no splotches, no dark blobs at the walls' bases, no blocks behind the spheres, and
the clips' moves change nothing measurable. The frame's lighting skips the blocks nothing is near (their flags, from the
last frame; the static lighting clears the frame's radiance so the skipped voxels hold 0) and the restore copies only the blocks
something moving was in (`ComputeGiRestore`): CesiumMan 9.65 to 9.3 ms, CornellBox 12.4 to 12.0. What a small scene
still pays is its clips' probes and voxels near its surfaces, which fill them: DamagedHelmet 4.7 to 11.2 ms with the
ray budget (the resolve and lighting of a last clip that is all surface, the finer clips mostly empty). What's left
(TODO.md): that, the lighting's lag through slow changes, glossy reflections, hardware ray queries, and a path traced
reference.

## Emissive surfaces as lights

View > Emissive Lights (on by default; `SPEEDO_EMISSIVE_LIGHTS=0` to start without, `FrameInputs::emissiveLights`,
Copy Pose writes it): the model's emissive triangles light the scene as area lights, at the first hit and in the
indirect lighting's voxels, with soft shadows traced through the BVHs (`emitters.slang`, `gfx/emitters.h`). Before, an
emitter's light reached anything only through the indirect lighting's rays meeting it by chance: spread over its voxels
it lit as if it were a voxel big (Bistro's lanterns lit the walls around them yellow, a hundred times the bulbs' light),
met as a triangle by a ray in four pixels it was a white speck (the near field took no emission at all), and the
triangle's emission was read through a material index only the alpha masked BVH triangles carried, so EmissiveStrengthTest's
cubes lit nothing around them. The rays take only the light what they meet reflects now (`GiHitRadiance`; `GiMarch`
takes a voxel's emission out), and what the emitters give arrives through these:

- **The emitters** (`emitters::Build`, at upload, from the staged triangles: Bistro's 20972, 56 ms): the triangle lists'
  triangles that don't move whose material emits (its emissive factor times its strength, or an animation sets it),
  at each instance, in world space (`EmitterTriangle`: a corner and two edges whose cross product faces the front, a
  mirroring instance's swapped and flagged, `EMITTER_FLAG_MIRRORED`, so its texture coordinates' corners follow; its
  material and first index), and a tree over them (`EmitterNode`, `gEmitterTree`: a light BVH, pbrt's BVHLightSampler,
  built by its surface area orientation heuristic: box, cone of normals, power, the area times the luminance of the
  emission factor; the texture's isn't known there, and each point is weighed by its own). `EmitterPick` walks it from
  the root, each child by its share of `EmitterImportance` (pbrt's `LightBounds::Importance`: power over the distance
  to the box, by the least angles the emitters and the surface may face each other at; never 0 where something lights
  the point), its probability the product. A global alias table by power came first: Bistro's lanterns, a small share of
  its emitters' power, were rarely a candidate of a wall beside them, and the light was mottled in blotches
  of the filter's size (`mean |log2|` against the reference, blurred 4 pixels: 0.150 to 0.129, and smooth to the eye).
- **Per traced pixel** (every `SPEEDO_EMITTER_STRIDE`-th pixel each way, 4; a texel each of the emitters' images,
  `SHADER_TYPES_EMITTER_RAW_TEXTURE` on): Emitter Sample resamples `SPEEDO_EMITTER_CANDIDATE_COUNT` (8) candidates
  (RIS: a point of each picked triangle, uniformly, by the luminance of the irradiance it would give unshadowed, nearer
  than two pixels taken as two pixels away), and Emitter Shade traces the chosen point's shadow ray (`ShadowOccluder`:
  the static and moving BVHs, alpha masks cut out) and writes the irradiance over pi. Each frame's sample is its own:
  ReSTIR's temporal reuse (`SPEEDO_EMITTER_REUSE=1`: the reservoir of the traced pixel where its point was last frame,
  by the view's last view projection, where that point was the same surface, `EmitterTolerance` and normals within 25
  degrees, not where the stencil says something moves; its count capped at `SPEEDO_EMITTER_HISTORY_CAP`, 20, times the
  candidates; kept in `gEmitterReservoirs`: three parts, the frame's before the spatial reuse and after it the frame's
  and the last frame's, by parity; triangle, barycentrics as unorm16, W, and M and the visibility's age, 16 bits each)
  keeps a point for many frames, so Emitter Temporal's frames are of one sample, not independent ones, and gain little.
  It was the default until Bistro's string lights at night (hundreds of colored bulbs across its streets and its lit
  windows, the user's pose `-906.5073,218.3646,-40.5114,-7.916,60.803` at `SPEEDO_ENVIRONMENT_INTENSITY=0.002`,
  exposure 5) came out a grainy colored wash: against the reference (mean |log2|, still) 0.235 with it and 16 frames
  (cap 4 0.186, cap 1 0.164: the more history, the worse), 0.154 without it, 0.140 with 8 candidates (4 candidates were
  as good as 8 with it), 0.132 with 16; at the end of a turn there 0.334 against 0.239; Bistro's lantern pose 0.181 to
  0.089, EmissiveStrengthTest 0.064 to 0.033, CornellBox-Original 0.029 to 0.015, the means the same. With the reuse,
  a point its ray finds hidden is kept as the candidates it stood for, of no light (RTXDI's discard of invisible
  samples: kept as it was, a back wall's reservoir held the brighter cube behind the partition as often as its own, and
  half of EmissiveStrengthTest's pixels were black), and under a still camera a point found visible stays so for
  `SPEEDO_EMITTER_VISIBILITY_FRAME_COUNT` (8) frames without a ray (a quarter of that where something moves;
  `EmitterViewStill`). Below `SPEEDO_EMITTER_ROULETTE` (0.02) of the frame's white (the unshadowed estimate times the
  exposure) the ray is traced by chance, weighed up by it: Bistro's street by day under auto exposure, 2.5 ms (65.4 to
  67.9 with every ray).
- **Filter and frames**: Emitter X and Y at the traced pixels (`EmitterFilter`: the traced pixels within
  `SPEEDO_EMITTER_FILTER_REACH`, 16, pixels along rows and columns, on the same surface, by a tent, as the near field's);
  then Emitter Temporal at every pixel of the pre-pass's depth: the frame's filtered light upsampled (`EmitterUpsample`:
  bilinear over the 2x2 traced pixels around, each by how much it is on the pixel's surface, by the traced pixels'
  surfaces as Emitter Shade wrote them, `SHADER_TYPES_EMITTER_SURFACE_TEXTURE`, RGBA32F: the point and the normal packed
  exactly, `EmitterPackNormal`; else the 3x3 by that alone, else, thin geometry between the traced pixels, a frame's side
  or a ledge's edge, the 3x3 by how their normals agree, never 0: left without light, those pixels were dark dashes along
  every edge at night, magenta in the debug views, which the tonemapper made white streaks at exposure 5), blended by 1
  over the frames it holds, at least 1 over `SPEEDO_EMITTER_HISTORY_FRAME_COUNT` (32), into the history the frame's parity
  says (`SHADER_TYPES_EMITTER_HISTORY_TEXTURE`), bilinear from where the point was (the last view projection, both frames'
  jitters taken out, `PixelHistoryAt`), each pixel only where its last surface (`SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE`,
  RG32F: the distance from the eye and the packed normal) was the same (within a fifth of a percent and two pixels' depth
  along the surface, normals within 25 degrees; 16 frames, 0.154 against 0.188 at the string lights' pose), else the 3x3
  around that was (`PixelHistory`). It was accumulated at the
  traced pixels, which the main pass upsampled, until temporal upscaling's jitter: a traced pixel at an edge saw one
  surface in some frames and the other in others, the pixels upsampled from it changed as a block, and the upscaler's
  clamp to the 3x3 around passed that on as the frame's light: speckles on every edge, ledge and leaf the emitters light
  (the user's evening pose `-994.4661,340.8679,-53.6291,-8.370,78.594` at 19.817, balanced, the emitters' debug view at
  exposure 5: 6066 pixels a frame changed by more than 32 of 255, against 356 without upscaling; 738 per pixel, most of
  them the bulbs themselves, which the view draws and the jitter samples or not; at no measurable cost with the near
  field's frames per pixel too: 50.9 ms either way, 60.1 without upscaling). Checked by the distance alone, within 1%,
  the history blurred across window frames and railings (227). A frame far brighter than what a settled history (8
  frames or more) holds is taken as at most 8 times that, or that and a quarter of the frame's white more
  (`kFireflyRise`, `kFireflyFloor`: `HistoryFireflyClamp`, which the near field's history takes too): an emitter point the candidates favour (unshadowed and near) that
  seldom reaches the pixel, a lit window's interior beside its curtain, is the whole unshadowed light the once in tens
  of frames its ray gets through, a 32nd of which was a blob that faded over 32 frames (the user's night time lapse,
  `-827.3380,155.1481,33.8635,-9.897,72.297` from 00:19; the frames its random numbers pick, 63, 103 and 146, whatever
  the time). The curtain's pixels that jumped went from about 420 to 76 over 200 frames; the reference is 11% brighter
  there than the frames (8% before), the light those rare frames carry, and the frame as bright as it to 0.1% (4 times
  took 19% and 1%). While the view is still and the model has no animation (`EmitterData::stillBlend`;
  `ViewData::previousEyePosition`'s w 2, which `Views::Upload` sets by the view projections without the jitter), the
  history holds up to `SPEEDO_EMITTER_STILL_FRAME_COUNT` (256) frames, back to 32 once the view moves: a mean of the last
  32 frames' samples never settles, and what was left of their noise wandered at the pace the frames replaced it, light
  creeping over the walls (the user's night pose in Bistro's street, `-752.4570,186.6341,97.5802,-1.211,63.522` at 00:57,
  a near field stride of 4, performance: where the user pasted, a mean change of 1.74 of 255 over 20 frames, 1.65 of it
  the emitters'; theirs at the render size without upscaling 1.81, now 0.27; 1024 frames gain little). With the 3x3's
  history at edges (`PixelHistory`) and the jitters taken out (`PixelHistoryAt`), the pose's 1.74 is 0.83. What is left
  is FSR's: on a still frame with nothing traced it repeats with its jitter's cycle (32 frames at performance: 0.10
  between frames 32 apart, 0.53 between 16), and what noise is left in the emitters' light flips its locks and shading
  change tests at thin features, window frames' and pilasters' edges (0.77 with FSR at the render size, 0.27 without).
  `EmitterViewStill` compared the jittered view projections, which never found a view still under upscaling (it gates
  the visibility reuse, `SPEEDO_EMITTER_REUSE=1`). The near field's history takes the same 3x3, reprojection and still
  frames (`GiData::nearStillBlend`, `SPEEDO_GI_NEAR_STILL_FRAME_COUNT`, 256, where the model doesn't move and the lights
  haven't changed, nor are being followed: the volume it reads is lit again as the time of day advances), its count in
  its surfaces' third channel (RGBA32F; its alpha holds the contact share): the light in a lit window's opening, the
  user's pose over Bistro's facades `724.7479,659.6074,-2775.4233,-21.034,93.844` at 02:14, changed by 1.36 of 255 over
  20 frames, 0.22 with the near field off, now 0.28. Main's opaque fragments (`PREPASS_IMAGE_EMITTERS`) take
  it at their pixel (`EmitterIrradiance`) and add the base color times it to their direct light (Direct Light's debug
  view has it; blended and scattering fragments don't).
- **The voxels** (`EmitterVoxelIrradiance`, in `GiDirectRadiance`): `SPEEDO_EMITTER_VOXEL_SAMPLE_COUNT` (4) resampled
  points of `SPEEDO_EMITTER_VOXEL_CANDIDATE_COUNT` (8) candidates each, shadowed as the punctual lights' are, in the static
  voxels' kept light (made again when emitters start or stop lighting) and the moving ones' each frame. An animated
  emission is the frame's at the pixels, and the kept light's until the next voxelizing.
- **Debug views**: Emissive Light (white), its rays (unfiltered) and a reference (`DEBUG_VIEW_EMITTERS_REFERENCE`, 64
  independent estimates a traced pixel, accumulated while the camera is still: compare at stride 2, and only converged:
  the tonemapper compresses a noisy reference: a single frame of EmissiveStrengthTest's was 2% darker than 40 frames').

What was measured against the references (EmissiveStrengthTest at its frame, 40 frames; Bistro's lantern pose,
`3002.7041,404.1237,1410.4180,10.076,5.141` at exposure 6, 96 frames; mean, and `mean |log2|` blurred 4 and 24 pixels),
with the temporal reuse, before it was turned off (above): within 0.3% (EmissiveStrengthTest 0.089, Bistro 0.189 and
0.093; stride 2: 0.073, 0.138 and 0.069, a rod's shadow and thin trims a stride 4 loses). The reference uses the same
tree and candidates as the frames, so it checks what they converge to, not the tree's own densities. Spatial reuse (`SPEEDO_EMITTER_SPATIAL_COUNT`, 0; up to 8 traced pixels
within `spatialRadius`) by counts (1/M) came out 2.6% and 6% darker (a neighbor that couldn't have picked a point counted
as if it had tried); by the generalized balance heuristic (as merged now) 0.2% and still 6% with the discard (a
neighbor's point hidden there but not here), as bright as the reference without the discard but noisier than no spatial
reuse (Bistro, every other pixel traced: 0.180 against 0.129), so the discard applies only without it. Dropping a hidden point's count with it made
the light 40% too bright. Offsetting the traced pixels in turn over the stride's square made each frame's temporal reuse
a point beside the last, with the spatial reuse's bias (6% darker at stride 4). Costs (KosmicKrisp, 2560x1440, GI on, the
frame's gpu time), with the reuse: Bistro's street 58.4 to 63.8 ms, its lantern pose 70.2 to 75.4, EmissiveStrengthTest
15.3 to 16.3, CornellBox-Original 29.3 to 30.4; without it and 8 candidates, 4.1 ms more at the string lights' pose
(80.9 to 85.0, interleaved: the rays every frame 2.3, the candidates 1.8), the lantern pose 76.8 to 81.2, the others the
same; the first version (every other pixel, 8
candidates by power, the filter and frames at every pixel) took Bistro's street 49 ms more (GI off), 25 of them its shadow
rays, 1.7 a candidate, 4.4 the filter. Not yet: the emitters' specular (a glossy floor reflects no lantern), what moves emitting,
a textured emitter's power by its texture (DamagedHelmet's triangles all candidates by the factor), and the light on blended
surfaces.

## The sky: Earth's atmosphere and the time of day

The default environment (View > Environment > Physical sky; without `SPEEDO_AUTOLOAD_ENVIRONMENT`) is Earth's atmosphere
lit by the sun, after Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique") and his
reference implementation: air (Rayleigh), aerosols (Mie, Cornette and Shanks' phase) and ozone, with Bruneton's
coefficients in km (`SHADER_TYPES_ATMOSPHERE_*` in `capi.h`), in `sky.h`, which `environment.slang` and the main layout
share. It replaced a stylized gradient whose sun was fixed.

- **Tables** (`gfx::EnvironmentFilter`'s, storage images in the general layout): the transmittance to the top of the
  atmosphere (256x64, Bruneton's mapping by the distance to the top and to the horizon, 40 steps) and the light scattered
  twice or more (32x32, Hillaire's isotropic approximation: 64 directions of 20 steps, serially, since the tests run the
  kernels on the cpu without shared memory), made once; the sky's view of itself (256x128, `SHADER_TYPES_SKY_VIEW_TEXTURE`,
  slot 37: by the angle from the zenith, the rows closer together near the horizon, and the azimuth from the sun's, 0 to
  180 degrees, since the sky is symmetric about the sun's vertical plane; 32 steps, the ground below the horizon at
  albedo 0.3 lit by the sun), made at every redraw. `EnvironmentSky` draws the panorama (256 wide, without the sun) from
  it, and the prefilter runs on that as on a file's. The backdrop and mirror reflections sample the view (`SkyRadiance`)
  and add the sun's disk at the real radius (0.27 degrees) through the atmosphere (`EnvironmentData::skySunRadiance`, from
  the cpu).
- **The sun** is a directional light (`environment::SkySun`): `SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE` (2.6, white, in
  the renderer's lux, so that the default sun gives the 2.2 of the sky before) through the atmosphere
  (`environment::SunTransmittance`: the same optical depth in doubles, 256 steps, within half a percent of the table, which
  `assettest` checks), none below the horizon. Where the time of day puts it (`environment::SunDirection`): the
  declination by the day of the year, the hour angle by local solar time (15 degrees an hour from noon), +x east and -z
  north; the defaults Paris (Bistro's), the 21st of June, 10:30: 59 degrees up in the south east, where the fixed sun was.
  View > Environment (with the sky installed): Time of Day (shown as hours and minutes, typed as hours), Advance Time and
  Time Speed (a multiple of real time, 60 an hour a minute), Latitude, Day of Year; `SPEEDO_TIME_OF_DAY` (hours or
  hours:minutes), `SPEEDO_TIME_SPEED` (nonzero advances), `SPEEDO_LATITUDE`, `SPEEDO_DAY_OF_YEAR`; Copy Pose writes them.
- **Redraws**: the draw thread advances the time (`Scene::UpdateSky`, by the real time between frames) and draws the sky
  again where the sun has moved more than `kSkyRedrawAngle` (0.05 degrees: every 12 seconds at real time, every frame at
  600x): the sky fields of `gEnvironment` alone (the irradiance is the filter's), the view, the panorama and the
  prefiltering, into the installed environment, the filter's first barrier waiting for the frames still reading it, and
  its descriptor sets handed back once the gpu is done (`Pipeline::TakeDescriptorSets`, see "Descriptor sets": 3000
  frames at 3600x, a redraw each, where the pool would have run out of storage images within minutes). A redraw costs
  about 3 ms on KosmicKrisp (DamagedHelmet without indirect lighting, the time at 3600x: 9.1 ms frames
  against a 6.1 ms `Frame` scope); Bistro's street with the sun moving every frame 82.7 to 92.9 ms (2560x1440, GI on: the
  voxels lit again for the sun and the cache following it, see "Lighting the voxels").
- **What it looks like**: a deep blue zenith at noon ((0.021, 0.043, 0.096)), the sky 8% of a horizontal surface's light
  at noon (a very clear day's: Hillaire's aerosols are few; hazier skies give 10 to 30%), so shade is about 3 stops below
  sunlight, where the gradient gave half of it; a red sun near the horizon ((1.08, 0.36, 0.05) at 3 degrees up), a pink
  sky after sunset and the blue hour purple (the ozone), and at midnight in June at Paris (the sun 17.7 degrees down) a
  sky 4e-8 of noon's: night is black, lit by the emissive surfaces (see "Emissive surfaces as lights").
- **Tests**: `assettest`'s sky check draws it with the kernels on the cpu, checks its panorama with the sun added as any
  environment (the sun the one dominant light, where and as bright as put), the table against the cpu's transmittance at
  90, 30, 10 and 3 degrees up, the zenith blue at noon, the sky's share of the noon light (5 to 40%), the sun red at 3
  degrees up, and midnight dark.
- **Not yet** (TODO.md): aerial perspective (the haze between the eye and far surfaces), clouds, the moon and the stars,
  the aerosols as a setting (turbidity), and the time of day's redraw cost spread over frames.

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
recycled once the gpu is done with them. A layout whose bindings change at every dispatch hands its sets over instead
(`Pipeline::TakeDescriptorSets(set)`: the set's arrays so far, to destroy once the gpu is done with the commands that bound
them, which frees them back to the pool; the next bind takes a fresh one): the environment filter's, 20 sets a sky redraw,
every frame while the time of day advances fast, which would have emptied the pool's storage images within minutes. Push
descriptors would be the other way, but rhi's push path (`InternalPushDescriptorSet`, never used) doesn't work yet: with
the Environment layout's set 1 flagged, validation reported the template's set 1 not to be the flagged layout
(`VUID-VkDescriptorUpdateTemplateCreateInfo-templateType-00353`) and a set layout with a binding number twice
(`VUID-VkDescriptorSetLayoutCreateInfo-binding-00279`), and KosmicKrisp crashed in `vkCmdPushDescriptorSetWithTemplate`.
The reflection keys that layout's sets 1 and 16777216 (its push constants', declared in set 0), and the pipeline layout
lays its set layouts out in the order of those keys (`DescriptorSetLayoutFlatMap`, `GetDescriptorSetLayoutHandles`):
where to look first (TODO.md).

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

## Asset import and testing

Decoding is CPU only and lives in `gfx`: `gfx::obj::Import` (tinyobjloader), `gfx::gltf::Import` (cgltf) and
`gfx::image::Import` (stb_image, stb_image_resize2, stb_dxt). Both model importers produce a `gfx::mesh::Mesh`
(`gfx/meshimport.h`, which also dispatches on the extension: `mesh::Import`, `IsModelFile`, `Dependencies`).
`gfx::Model::Load` and `gfx::LoadTexture` cache the result and fill rhi staging buffers
(`Buffer::CreateStaging`, before taking a queue's lock), which the staging constructors of `Buffer`/`Image` upload; a
failed load prints why and returns null instead of trapping. When an importer changes what it produces, bump its
`objimport-vN`/`gltfimport-vN`/`imageimport-vN` tag in the loader's params hash, or stale caches keep the old output.
Assets outside `RootPath` are cached under `<user profile>/external/<absolute path>`.

glTF: a scene (`mesh::ImportOptions::scene`, else the default one, or the first; the file's scenes are
`ModelDesc::scenes`, and View > Scene, or `SPEEDO_AUTOLOAD_SCENE=<index>`, loads the file again with another, as a
cache entry of its own; so does a `KHR_materials_variants` variant, `mesh::ImportOptions::variant`, which gives each
primitive its variant's material when it maps one: `ModelDesc::variants`, View > Variant or
`SPEEDO_AUTOLOAD_VARIANT=<index>`, the default materials without) is flattened into one mesh with the node transforms applied (a mirroring one reverses the
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
texture, and for BLEND). BLEND materials (`blend`) are exact order independent transparency, by per pixel linked lists:
drawn after a view's opaque submeshes, unsorted, with the main layout's second fragment entry point
(`FragmentTransparent`, `[earlydepthstencil]`: its writes must only happen for fragments that pass the depth test, which
the opaque `FragmentMain` can't force, since alpha masked fragments must not write depth) and no color writes (every
`BlendMode` `kNone`, so no depth writes either). Each fragment takes a node (`OitNode`, 12 bytes: premultiplied color as
R11G11B10, alpha and 24 bits of the distance, the reversed depth's float bits, the next node) from `gOitNodes` by `gOitCounter` and pushes it on its pixel's list
(`gOitHeads`, `InterlockedExchange`); `ComputeMain` keeps the pixel's 16 nearest layers sorted (insertion) and blends
them back to front over the opaque color or the backdrop. The buffers are the frame graph's transients, sized with the
render target (4 nodes per pixel, past which fragments are dropped), and cleared before each main pass
(`CommandEncoder::FillBuffer`, in a pass of its own). Fragment
stores and atomics need the device's `fragmentStoresAndAtomics` (the device enables every feature it has). A
`GraphicsPipelineVariant` names its topology, a blend mode per color attachment (by the render target's color attachment
count; it writes depth only if one of them is `kOpaque`) and which of the layout's fragment entry points it runs
(`fragmentShader`, by their order in it: a layout's graphics entry points are all its stages otherwise). The pipeline variant is a parameter of `BindPipelineAuto`, part of the pipeline cache key. glTF texcoords already have v = 0 at the top, so unlike obj they aren't flipped, and
normal maps share the obj convention (with `normalTexture.scale` applied to their x and y, as the spec defines it).
Emissive (gltf `emissiveFactor` times `KHR_materials_emissive_strength` and the srgb `emissiveTexture`, obj `Ke` and
`map_Ke`) is added after the lighting, unclamped (CornellBox's lamp, `Ke 17 12 4`, saturates to white); an emissive
texture with a black factor isn't loaded, but an obj's `map_Ke` with `Ke 0` is the map alone, as exporters write it
(`objimport-v9`: Bistro's hanging lanterns, its shop signs and every lamp of its interior emitted nothing by the
factor). Occlusion maps (gltf `occlusionTexture`,
by its strength) darken only the ambient term, the stand-in for the indirect light gltf applies them to; they are
imported as `image::Usage::kOcclusion` (the red channel, linear, BC4), since gltf often packs occlusion, roughness and
metallic into one texture's r, g and b, which kMask's luminance would mix. Each texture is a `TextureRef` (`gfx/textureref.h`: path, texcoord set, `KHR_texture_transform` as a 2x3 matrix, and its
sampler as `rhi::SamplerDesc`), which the shader applies: vertices keep both texcoord sets as they are (`texCoord01.xy`,
`.zw`), materials name a `TextureView` per texture (`gTextureViews`: texture and sampler slot, set, transform; 0 is
material 0's, deduplicated per model), and `ViewTexCoord`/`SampleView` sample through them (the normal map's tangent
frame follows its own transformed texcoords). A model's distinct samplers get the 62 sampler slots other than the
default's and the clamping one's (`kModelSamplerSlots`; `ManySamplers.gltf` uses 36), and the previous model's go back to the default. Images embedded in buffers or data uris are named by their model file and
index (`TextureRef::embeddedImage`): `LoadTexture` caches them against the model file and, on a miss, reads their
bytes with `gltf::EmbeddedImage` and decodes them from memory (`image::Import`'s byte overload, which tells the formats
apart by their signature), so nothing is extracted to disk. Compressed meshes are decoded at import: meshopt (EXT_meshopt_compression, which cgltf
parses, and KHR_meshopt_compression, read from its json) into `cgltf_buffer_view::data` right after the buffers load
(`DecodeMeshopt`), so accessors read them as usual; draco per primitive (`DecodeDraco`: cgltf turns the extension's
draco attribute ids into accessor pointers by index), whose values `addPrimitive` reads in place of the accessors'.
Textures use their plain image if they have one, else their KHR_texture_basisu (KTX2) or EXT_texture_webp image, which
`image::Import` decodes: a KTX2 texture with a full mip chain keeps its own blocks where it can (normal maps: Basis transcoded to BC7, y as the
file has it, up the image, which the shader flips by `TEXTURE_VIEW_FLAG_NORMAL_Y_UP`, `Image::normalYUp`, since BC7
blocks can't be flipped and Basis's BC5 takes y from alpha; occlusion: Basis to BC4 from red; metallic-roughness: Basis
to BC7 with the view swizzled to roughness in r and metallic in g, `Image::channels` and rhi's
`ImageViewCreateDesc::components`; each at least as good as decoding and compressing again, by `assettest`'s psnr over
the channels the shader samples), and a color (or linear) one
(`ImportKtx2`: Basis Universal transcoded straight to BC7, or BC1, BC3 or BC7 data as it is, with the file's mips;
`image::Format::kBC7`, which only comes from there; `Image::ownBlocks`: assettest decodes BC7 with bcdec, a header-only
`ports/bcdec` overlay, and only warns about such images' compression and mips, which are the file's), other KTX2 textures (normal maps and the other swizzled usages) are transcoded to rgba8 and compressed
as any other, as are WebP images (libwebp, the first frame of an animation); the decoders' versions are part of those files' cache keys, and draco's and
meshoptimizer's of every gltf model's. KHR_node_visibility hides nodes; morph targets are applied at their default weights (the
node's, else the mesh's; position, normal and tangent deltas) unless an animation moves the node's weights. Skins and node animations (translation, rotation, scale; step,
linear and cubic spline) are drawn moving (`SceneAnimationData`, `ModelDesc::animation`, `gfx/sceneanimation.h`): the
nodes animations move, and their descendants, keep their meshes in node space, drawn with instances linked to the
node (`SceneInstanceLink`), and skinned meshes keep their vertices in mesh space with a `SkinVertex` each
(`gSkinVertices`: 4 joints and unorm16 weights), blended in the vertex shader by `gJointMatrices` from
`PushConstants::jointBase` (world(joint) * inverse bind). Everything else is flattened as before. A moving model has a
host visible instance and joint buffer per frame (`gModelInstances[frame]`, `gJointMatrices[frame]`), which
`Model::Animate` writes on the draw thread after the frame's fence wait, from `EvaluateNodes` on the cpu; a static one
binds its device local instance buffer in every slot, and the defaults (`gDefaultSkinVerticesUuid`, `gDefaultJointsUuid`, `gDefaultMorphDeltasUuid`, `gDefaultMorphWeightsUuid`)
stand in for what it doesn't have. Skinned bounds are at the rest pose (`RestJoints`,
`SkinPosition`): bind space can be far off (CesiumMan's root rotation). View > Animation picks the animation (the
first plays on load), crossfading from the previous one over 0.3 s (`EvaluateNodes` with two `ScenePose`s and a weight:
local transforms lerped and slerped, then composed), pauses and restarts it, and `SPEEDO_ANIMATION_TIME=<seconds>` freezes it (for screenshots). Animated
morph targets (a node with a weights channel) keep their deltas, per vertex a row of a `MorphDelta` per target transformed
like the vertex (`gMorphDeltas`, from `PushConstants::morphDeltaBase` for the vertices from `morphFirstVertex`), added in
the vertex shader before skinning by the frame's weights (`gMorphWeights[frame]` from `morphWeightBase`, a `SceneMorph`
per node), which `Model::Animate` writes from `EvaluateWeights` (crossfaded like the nodes); the bounds cover each
vertex's reach with the weights from 0 to 1. A weights channel's output accessor is scalars, weightCount per key (its
`count` is not the key count). KHR_animation_pointer: cgltf skips a channel target's extensions, so the importer reads
the pointers from the json with cgltf's jsmn (`AnimationPointers`) and parses them (`ParsePointer`). Node translation,
rotation, scale and weights pointers become ordinary channels; the rest get a `ScenePointerTarget` each, whose values (at
rest `pointerDefaults`) `EvaluatePointers` samples like morph weights (`Path::kPointer`, the channel's node is the target).
A node whose KHR_node_visibility animates is kept in its space (instance links, even if hidden at rest), and
`NodeVisibility` hides it and its descendants (`WriteInstances` collapses their instances to a point); lights under nodes
that move or hide follow them (`SceneLightLink`, `WriteLights`). Material values (`MaterialProperty`: the factors of the
core material and of the KHR_materials_* extensions) and texture transforms (a `MaterialTexture`'s offset, rotation and
scale, three targets from the offset's, which `TextureRef::animatedTransform` names: its view is its own, see
`TextureViewKey::animated`) are applied on the draw thread after `Model::Animate` (`ApplyPointerValues`), which patches
the uploaded `MaterialData` and `TextureView`s and uploads what changed. A material with an animated base color factor
keeps it in `MaterialData::color` rather than baked into the vertex colors. Lights' values (`LightProperty`, by the file's light, `SceneLight::source`: one light
can be on several nodes) and cameras' (`CameraProperty`, `SceneCamera::source`) are pointer targets too; a camera is
`animated` if its node moves or its values do, and `Model::Animate` updates the lights (`ApplyLightPointers`, then
`WriteLights`) and cameras (`ApplyCameraPointers`, `WriteCameras`); the draw thread uploads the lights and hands the
cameras to `Views::UpdateSceneCameras`, which re-applies the one the views look through if it animates (not their move
speed). The emissive factor and `KHR_materials_emissive_strength` are a pair of targets (the factor's, then the
strength's) whenever either animates, since the shader's emissive is their product; the anisotropy rotation becomes
its cosine and sine. A layer whose factor animates up from 0 is on (`LayersOf`: its textures load, its flags are set,
and transmission draws in the second phase). `AnimatedBounds` grows a model's bounds by its animations' poses (each
sampled 30 times a second, at most 600: the linked instances' vertex bounds and the per joint bounds of skinned
vertices), so the views frame all of it. `resources/test-models/gltf/AnimatedCameraLight.gltf` animates a camera's node
and yfov, a light's color and intensity, and an emissive strength. A set of files is drawn at rest (morph
targets at their default weights). Cameras are imported (`SceneCamera`, `ModelDesc::cameras`: world position and forward,
perspective field of view or orthographic height, near and far; none for a set of files) and the views use the first
(`Views::SetScene`); View > Camera picks another or frames the model (`Views::UseSceneCamera`). A camera's roll is
`cameraRotation.z` (applied before pitch and yaw), and its aspect ratio (gltf `aspectRatio`, or `xmag / ymag`)
letterboxes its view in its grid cell (`Views::InternalLayout`; the draw uses `Views::GetViewports`). The Khronos glTF-Sample-Assets `Models/` are the test set (see below; `assettest` takes
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
within a smoothing group, or within 60 degrees for group 0, from every face around the corner's position. So are all of
a mesh's where its file normals don't follow its faces (`stats.normalAlignment`, the area weighted cosine between a face
and its vertex normals after the flips, below a half, `kMinNormalAlignment`): erato's are turned 90 degrees about y from
its faces, exported in another frame than its positions (0.14; a sign test agreed on three quarters of its area), and
the indirect lighting looked along them into the statue, its robe dark and blotched, changing as the camera moved the
clips (the user's pose `-0.8296,17.5449,19.0786,0.000,0.000`). The rest of the McGuire set is at 0.76 or more, most at 1;
per part, foliage's normals bent up and out on purpose fell below (scrubPine, San Miguel's trees, 264 of Bistro's parts),
so it is the whole mesh's (`objimport-v8`). Some test scenes are z up (e.g. chestnut) and are loaded as
is. Faces are triangulated by the importer, not tinyobj (`LoadObj`'s triangulate off): by halving (`ForEachFaceTriangle`:
the triangle of a face's ends and its middle corner, then each side's), which leaves triangles and quads as a fan makes
them. A fan of a face of hundreds of corners is slivers that all meet at one corner, which no BVH separates (a tree too
deep for tinybvh's build and the traversal's stack), and tinyobj's own triangulation dropped most of such faces:
`geodesic_dual_classII_20_20.obj` came in with 825000 of its 4.58 M triangles (`objimport-v6`).

Rendering is linear: color textures are loaded with srgb formats (`gfx::image::Usage::kColor`, mips filtered in linear
space), the main render target is `R16G16B16A16_SFLOAT`, and `ComputeMain` scales it by the exposure (View > Exposure,
in stops), tonemaps it (`PushConstants::tonemapper`, View > Tonemapper or `SPEEDO_TONEMAPPER`: Khronos PBR Neutral, the
default, which leaves colors up to about 0.76 as they are; ACES, Hill's fit; AgX, Wrensch's fit of the base look;
Reinhard on luminance; linear) and applies the srgb curve when it copies to the swapchain, which stays unorm since it is a
storage image (and imgui's colors are srgb already). Auto exposure (View > Auto Exposure, on by default since 2026-10-26, the user's call with the sky's time of
day; `SPEEDO_AUTO_EXPOSURE=0` without, which captures compared at a fixed exposure need) needs no pass of its own: ComputeMain counts the scene's pixels by their log2 luminance, before exposure,
into 64 bins (`gExposureHistogram`, per frame index, host visible: shared memory per group, then atomics; emptied with
`FillBuffer` before, and a barrier to the host stage after), and `UpdateAutoExposure` reads a frame's histogram after
its fence (where `Model::Animate` writes the frame's buffers), averages the log luminance between the 50th and 95th
percentile, and moves the exposure toward `log2(0.2) - mean`, less up to 2 stops as the mean falls from daylight's
(2^-4 in the renderer's units: 2900 cd/m^2 scaled as the sky's sun is, EV100 14, Bistro's street at noon -4.2) to a lit
street's at night (2^-11, 22 cd/m^2, EV100 7; Bistro's -10.5): a night darker than a day, as the eye and a photographer
have it (one key of 0.3 lifted Bistro's night 8.7 stops, to look as bright as noon at 2.5), by an exponential
decay of 2 per second, of the time elapsed whatever it is, so it settles in the same time at any frame rate (fifo at 120
Hz and mailbox at 220 followed the decay from the same targets to 2%; a frame over a second used to take the target at
once). What it measures needn't: the indirect lighting settles by frames (its hysteresis), so a scene's first fraction
of a second differs between frame rates. The Exposure slider is added on top.
Shading is the glTF metallic-roughness brdf (`Shade` in the shaders: GGX, height correlated Smith, Schlick) over the
lights in `gLights` (`PushConstants::lightCount`; a model's KHR_lights_punctual lights, `ModelDesc::lights`, in lux and
candela, then the environment's dominant lights), plus image based lighting that occlusion darkens. The environment
(`gfx/environment.h`, cpu only) is an equirectangular panorama (+y up, -z at its center), a Radiance `.hdr` file
(`environment::Import`, scaled to 2048 wide) or, by default and in automated runs, the sky (see "The sky", below: Earth's
atmosphere, its sun where the time of day puts it). A sun disk drawn into the sky's panorama (`environment::AddSun`, 4x4
samples per texel, scaled to exactly its irradiance) tests the dominant light extraction. A panorama's strongest compact light sources are its dominant lights (not
necessarily a sun: a softbox in neutral.hdr, windows in ennis.hdr and papermill.hdr), which
`environment::ExtractDominantLights` takes out, up to `kMaxDominantLights` (4), each found in what the ones before left
(its brightest texel, if 50 times the mean of what is left, and the texels around it within 15 degrees brighter than 2%
of it, flood filled; they get the mean radiance of the texels bordering them, and the rest is the light's irradiance,
kept if at least 5% of pi times the whole panorama's mean), then sorted by irradiance (the brightest texel isn't always
the most light: ennis's second is its window, 2.5 lux against the first's 0.7). They light the scene as directional
lights instead (`Environment::dominantLights`, `SyncLights`: after the model's lights, turned by the environment's
rotation and scaled by its intensity, uploaded when they change; before any environment is installed, the procedural
sky's sun), so they cast shadows (by the shadow budget: a directional light takes a quarter of the atlas) and aren't
counted twice. View > Environment lists them. Where one of the model's directional lights shines from within 5 degrees of
one (checked each frame, after the rotation), they are the same light, merged: the model's alone is kept (as authored, maybe animated), and View > Environment says so (and
turns merging off and on, `gMergeLights`; `SPEEDO_MERGE_LIGHTS=0` starts without). The
Khronos panoramas' bright sources are clipped, so most come out at 3 to 5% of the mean (field, footprint_court, doge2
and pisa have none; papermill has 4, all windows); the sky's sun is 57%, and `assettest` checks it is the one
dominant light found, where and as bright as it was put (and the lighting levels against the panorama's mean less the
dominant lights' share). The client prefilters on the gpu (`gfx::EnvironmentFilter`, `environment.slang`, a layout of its
own, "Environment", kernels selected by `ComputePipelineVariant`): `LoadEnvironment` decodes a file and takes its dominant
lights out on the cpu (cached, `environment-vN`: the original and lighting panoramas as half floats, and the lights), and
uploads both panoramas; the sky needs nothing (the filter draws its source, `EnvironmentSky`, and its sun is known).
`InstallEnvironment` then records the filter on the draw thread (after the uploads are acquired, writing the rest of
`gEnvironment` first): the source pyramid (`EnvironmentDownsample`, by solid angle, down to 8 wide), level 0 (the original,
or the sky's source), the GGX and Charlie levels, and the irradiance (one group, into `gEnvironment[0]`), each kernel over
a mip as a storage image (`ImageViewCreateDesc::baseLevel`) and dispatched by threads (`CommandEncoder::DispatchThreads`,
by the reflected group size). The resources of a layout of its own start at set 1: set 0 is the push constants'
(`[[vk::binding(0, 0)]]` on them, as in `Main`), else they share binding 0 with a resource. `assettest` checks the same
kernels, run on the cpu: slang compiles `environment.slang` to c++ at build time (`slangc -target cpp`, a custom command
of the assettest target), and `src/tools/environmentkernels*` run it in `EnvironmentFilter::Record`'s order, with cpu
textures behind the prelude's `ITexture` (trilinear, around in u and clamped in v, as the gpu's sampler) and rows of
groups on threads. A group's threads run one after another there, so the kernels can't use shared memory or group
barriers (the irradiance is a thread per coefficient). The prelude's global names (`half`, `Vector`, ...) stay behind
`environmentkernelsbridge.cpp` (plain types), away from the gfx headers. It included `<stdfloat>` for c++23 and up,
which libc++ lacks in every language mode: `ports/shader-slang/0005-*` includes it only if `__has_include` finds it (to
go to the fork, as 0001 to 0004 did). libc++ runs
`std::execution::par` serially, hence the threads (the old cpu prefilter took 4 s a panorama, the kernels 1 s). The
kernels turn the panorama without its dominant lights into 6 levels of GGX prefiltered radiance (roughness `i / 5`, the split
sum with n = v = r, filtered importance sampling from a mip pyramid; level 0 is the panorama, dominant lights included:
the backdrop, and mirrors), the mips of one `R16G16B16A16_SFLOAT` texture
(`SHADER_TYPES_ENVIRONMENT_TEXTURE`), 6 levels prefiltered with the sheen's Charlie lobe (`Environment::sheenLevels`, from
an eighth of the width, `SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE`, `EnvironmentData::sheenLevelCount`), and the irradiance as 9 spherical harmonics coefficients (`EnvironmentData` in
`gEnvironment`). The shader takes the specular's second term from Karis' environment brdf fit, and looks the base and clearcoat
specular up along Frostbite's dominant direction (`DominantDirection`: leaned from the reflection toward the normal by
roughness, which keeps rough grazing reflections from skimming the horizon). The Charlie lobe can't be importance
sampled the usual way (Estevez and Kulla's half vectors): at low roughness its weight is near the horizon, where most
reflected samples land below it, and the levels' means drifted by up to 50%; `PrefilterSheenTexel` samples l over the
hemisphere with `cos = u^k` instead, k growing as the roughness drops, weighted by the lobe over that pdf. Its intensity and rotation (about
+y) are push constants (`PushConstants::environmentIntensity`/`environmentRotation`, from View > Environment, or
`SPEEDO_ENVIRONMENT_INTENSITY`/`SPEEDO_ENVIRONMENT_ROTATION` in degrees), applied to every lookup
(`EnvironmentDirection`). It is the backdrop too: the main color target is cleared to alpha 0 and opaque draws write
alpha 1, so `ComputeMain` knows where nothing is, finds the view the pixel is in (`ViewData::viewport`, `viewCount`)
and samples the panorama along the ray through it (`ViewData::inverseViewProjection`, unprojected at depths 1, the near plane,
and 0.5, which works for infinite far planes and orthographic views too). Outside the views (letterboxing) the clear color
stays. Equirectangular rows near the
poles cover less of the sphere: the pyramid's mips weigh rows by their sine, and a sample's mip goes by the texel's real
solid angle, otherwise the rough levels drift from the panorama's mean (papermill's by 15%), which `assettest` checks
(each level's mean within 3%, the irradiance's within 2%, and each level smoother than the one before). Image views
cover all of their image's mips unless `ImageViewCreateDesc::levelCount` says otherwise (they used to cover only level
0, so no texture was ever sampled below it). Metallic-roughness textures are `Usage::kMetallicRoughness` (BC5: roughness, the file's
green, in r, and metallic, its blue, in g); obj materials are dielectrics whose `Ks` scales the specular
(`MaterialData::specular`, also KHR_materials_specular's factor; 0 is matte, without even a fresnel rim) and whose `Ns`
gives the roughness (`sqrt(2 / (Ns + 2))`), and `MaterialData` starts from `DefaultMaterialData()` (roughness, specular
and specular color 1, ior 1.5: zero roughness is a mirror, and a zero specular color no fresnel at all). `Shade` lights a
`Surface` (diffuse color, f0 color, f90, roughness, occlusion, normal), which `ShadeFragment` builds as the spec does: the
dielectric's f0 is `((ior - 1) / (ior + 1))^2` (KHR_materials_ior) times KHR_materials_specular's color (factor times its
srgb texture), at most 1, times the strength (factor times its texture's alpha, `Usage::kAlpha`: BC4), and its f90 the
strength, mixed with the metal's (f0 the base color, f90 1) by metallic; the environment term is `f0 * A + f90 * B`.
Specular-glossiness materials (`MATERIAL_FLAG_SPECULAR_GLOSSINESS`) take their specular color as f0 (f90 1), their
diffuse scaled by 1 minus its largest component, and roughness `1 - glossiness * a` of the specular-glossiness texture
(in the specular color's slot, `kColor`: srgb rgb, linear alpha). KHR_materials_sheen adds a Charlie lobe (Estevez
and Kulla's visibility, as the Khronos sample viewer has it) over that, which scales the layers below by
`1 - max(sheen color) * E`, E its directional albedo (`kSheenAlbedo`: a 12x12 table over cos and roughness, integrated
numerically and clamped to 1; lit by the environment's sheen levels along the reflection, times E). KHR_materials_clearcoat adds a dielectric GGX layer
(f0 0.04) of its strength (`kOcclusion`: the texture's red), roughness (`kMetallicRoughness`: its green) and own normal
map (`NormalFromMap`, else the geometry's normal) over everything, emission included, which keeps `1 - strength *
F(n_c . v)`. KHR_materials_transmission replaces that much of the dielectric's diffuse with the light from behind
(`TransmittedLight`, as the Khronos sample viewer): the main pass draws in two phases when the model has transmissive
(not blended) materials (`MainPassPhase`): the other opaque submeshes, then, after the Transmission pass blits the color to the
transmission texture (`SHADER_TYPES_TRANSMISSION_TEXTURE`, a full mip chain: `Image::BlitFrom` and
`Image::GenerateMips`, which blits each level from the one above with per level barriers), the transmissive and
blended ones, loading the attachments. The refracted ray (by the ior) leaves the volume after the thickness
(KHR_materials_volume: factor times the texture's green, times the instance's scale; 0 is thin walled. the thickness is in the mesh's units, so a scaled node with a volume material is drawn as an instance of it, not flattened, for its instance to carry the scale: DragonAttenuation's dragon, under a node scaled by a quarter, came out deep red, four times as thick), is projected into
the view (fading to the environment along the ray over the view's last 5% and beyond it, where nothing was drawn:
clamping streaked the edge's texels, stretching as glass neared a corner; offsets shortened to stay inside, or mirrored
in, looked no better, and a guard band costs too much), and samples the texture at
`log2(width) * roughness * saturate(2 ior - 2)` through the clamping sampler
(`SHADER_TYPES_CLAMP_SAMPLER`, a reserved slot, so models get 62); where nothing opaque was drawn the environment along
the ray fills in by the texture's alpha, which is why the color target is cleared to transparent black (ComputeMain
draws the gray clear color itself where neither the views nor the backdrop are). The volume attenuates it by
`attenuationColor^(thickness / attenuationDistance)`, and KHR_materials_dispersion samples each color with its own ior
(spread by `(ior - 1) * 0.025 * dispersion`). KHR_materials_anisotropy stretches the GGX lobe along a direction in the
tangent frame (`SurfaceFrame`: the vertex tangents', else from derivatives; the texture's rg along +u and up the image,
glTF's bitangent, i.e. -v as sampled; `kLinear`), turned by the rotation: alpha along it `lerp(alpha, 1, strength^2)`,
and the environment is reflected about a bent normal (as the Khronos sample viewer). KHR_materials_iridescence's thin
film Fresnel (`IridescenceFresnel`, Belcour and Barla) is evaluated once per pixel at the view's angle over the
surface's f0 and mixed into the Fresnel of the environment and the lights by its strength. KHR_materials_diffuse_transmission
lights that much of the diffuse from the back (lights behind the surface, and the irradiance around -n), tinted by its
color alone, as the spec's `diffuse_btdf(diffuseTransmissionColor)` has it (it was multiplied by the base color too,
which turned DiffuseTransmissionTest's white and red quads at factor 1 dark teal), and weighed as the dielectric's diffuse
is (`1 - metallic`, `1 - transmission`); its strength texture is the alpha channel (`Usage::kAlpha`). KHR_materials_retroreflection (the minimal retroreflective microfacet model) blends the base lobe (`BaseLobe`: diffuse and specular, not sheen, clearcoat or transmission) with the same lobe seen from the view mirrored about the normal, `2 (n.v) n - v`, by its weight (factor times the texture's red, `kOcclusion`), which turns the specular back towards the light; the environment's retroreflective lookup is along the view itself (the mirrored view's reflection), and n.v, so the split sum's terms, don't change. It is one of the extended layers (`SHADER_TYPES_LAYERS_EXTENDED`). KHR_materials_volume_scatter is screen space subsurface scattering, after the Khronos sample renderer's but by the
extension's definitions (the renderer's own approximation drew ScatteringSkull pale, flat and barely blurred). Three passes
over the scattering materials' triangles (`BuildScatterDrawList`): Scatter Back draws their back faces' depth
(`BuildScatterBackDrawList`: single sided ones, front faces culled, `FragmentShadow`;
`SHADER_TYPES_SCATTER_BACK_DEPTH_TEXTURE`); Scatter (`FragmentScatter`, the layout's fourth fragment entry point) draws
the light coming out of them (`ScatterLight`): what enters from the front in the multiscatter color (the albedo of all of
the scattering, which the path tracers' single scattering albedo, `1 - (4.09712 + ...)^2`, is made to produce: tinting by
that is what made it pale), the environment's by occlusion, plus what comes through from behind, attenuated over the
thickness along the view to the nearest back face (the thickness map where there is none: ScatteringSkull's says about 1 m
on a 0.34 m skull), times the diffuse transmission and its color, less the specular's fresnel
(`SHADER_TYPES_SCATTER_TEXTURE`, the material + 1 in alpha, and `SHADER_TYPES_SCATTER_DEPTH_TEXTURE`); Scatter Blur
(`ComputeScatter`, the layout's second compute entry point, once per pixel: in the main pass's fragments it cost every
overdrawn one, nearly doubling Main) blurs it by Christensen and Burley's profile, scaled per color by the mean free path
(the reciprocal of the extinction, `-ln(attenuationColor) / attenuationDistance`, at most 10 attenuation distances) and the
multiscatter albedo (`BurleyScale`, the searchlight fit; the renderer's attenuation distance times the multiscatter color
through Blender's radius convention was a tenth of that), from the 55 samples precomputed as the renderer does, samples of the
same material only, weighed by their world space distance. The pattern is turned by the pixel's place in a 4x4 tile
(fixed, it made copies of the image; turned by interleaved gradient noise, faint stripes), which Scatter Denoise
(`ComputeScatterDenoise`, the third compute entry point) cancels: interleaved sampling, each pixel the mean of its 4x4
tile's neighbors on the same surface (same material, within 8 times their screen distance in world space), as many
samples as the 16 pixels took; the profile's albedo is each pixel's multiscatter color, factor times texture, which the Scatter pass writes as its second
color attachment (`SHADER_TYPES_SCATTER_ALBEDO_TEXTURE`, srgb; `kScatterBlend`), since the blur has no texcoords: a darker
texel absorbs sooner, so it diffuses less (up to 4 times), which `ScatterTexture.gltf` shows (a slab of bright and dark
stripes between slabs of each color, whose stripes must look like them; the material's factor alone smeared the dark
stripes into the bright). It cost nothing measurable, so it isn't a specialization.
The main pass adds it (`SHADER_TYPES_SCATTER_LIGHT_TEXTURE`, else, where the scatter pass didn't draw the surface,
`ScatterLight` in place) times `(1 - metallic) (1 - iridescence) (1 - transmission) (1 - clearcoat fresnel)`; all of the
diffuse transmission's light is the scattering's. The volume (thickness, attenuation) is filled for diffuse transmission
and scattering materials too, and attenuates the diffuse transmission's back light. What it can't do: light crossing
more than the profile's reach, caustics, and the volume's own shadowing.
Every `KHR_materials_*` of the glTF-Sample-Assets is drawn, and `KHR_materials_variants` is chosen at import (see above). gltf primitives without a material get the spec's default (white, metallic 1, roughness 1), a
material of the model's own, while obj faces without one use material 0, which opening an image textures. KHR_materials_unlit draws the base color alone. A model's materials
(`ModelCreateDesc::materials`, drawn per `submeshes`) are materials 1 and up in `gMaterialData`, which starts at
`SHADER_TYPES_MATERIAL_COUNT` (1024) and grows to the next power of two that holds a model's (`GrowMaterials`: a new
buffer, the old one retired after the gpu's work; NodePerformanceTest has 10000 per encoding, and its materials past the
first 1023 used to be drawn with the default one, with a warning; the index used to share a push constant with the view,
10 bits). The scatter pass writes a scattering material's index + 1 to a half float, exact to 2048: one past that is a
hard failure. Their diffuse, alpha
(`map_d`, `kMask`: BC4) and normal (`norm`, `kNormal`, else `map_bump`/`bump`, `kBump`: both BC5) textures are loaded with the model and go in `gTextures` slots from 58
(0 the main color, 1 to 3 temporal upscaling's composite and color, 4 the main depth (the depth pyramid's source), 12 the environment's, 15 the texture of material 0 that opening an image loads, 16 and 17 the menu bar blur's, 18 the pre-pass's normals, 19 to 22 the traced shadows' images, 23 to 25 the indirect lighting near field's, 26 to 29 its history's, 30 to 36 the emitters' and 37 the sky's view of itself, 38 the pre-pass's motion, 39 to 55 temporal upscaling's other images, 56 and 57 the emitters' pixels' surfaces). Bump textures are height maps in
most mtl files, but some are normal maps: the importer tells them apart by color (normal maps are bluish), turns
heights into normals (scaled by `-bm`), and stores all of them with +y along +v as sampled, i.e. down the image (the
obj importer flips v). Vertices carry gltf's tangents (`VertexP3fN3fTa4fT014fC4f::tangent`: xyz along +u, w the
handedness, so the bitangent `cross(n, t) * w` points *up* the image, i.e. along -v in this convention; mirroring node
and instance transforms flip w). gltf triangles with a normal map but no `TANGENT`, and obj faces whose material has a normal or bump map, get
MikkTSpace's (`mesh::GenerateTangents` in `gfx/tangents.h`, the `mikktspace` port; per obj material, whose vertices no
other material shares, since the material is part of the obj vertex key: from the normal map's texcoord set, with v flipped back up the image as the
exporters' MikkTSpace sees it, so that the bitangent points the way gltf's does; a vertex is split where its corners'
tangents differ, and generated tangents are made perpendicular to their own vertex normal, since degenerate triangles
get a neighbor's). Without them (degenerate corners, and models without normal maps) w is 0, and the fragment shader builds the tangent
frame from screen space derivatives instead,
corrected by the sign of `dot(cross(ddx(p), ddy(p)), n)`, which is negative here since the framebuffer's y
points down: without it bumps come out inverted.
Either frame follows the normal map's texture transform. NormalTangentMirrorTest checks the vertex tangent path: it
renders right either way, but negating its tangents' w must break it. A quad with a known height map (a dome, which must be lit on the side
the light comes from) is the quickest way to see a sign error. `InstallModel` switches model, textures and materials in one draw
thread step, after the textures are transitioned: every change to `gTextures` takes a new descriptor set (see above),
and the pool only holds 128 copies of that 1024 slot array. Installing a model also sets the views to its first camera,
or frames them on its bounds (`Views::SetScene`, `Views::FrameBounds`).

File > "Open..." is the file browser, which loads models, zip archives, environments (`.hdr`) and images, by their type
(`LoadAndInstallFile`), and several models at once (side by side or as authored); View > Environment > Open
Environment... is the same browser for panoramas only (`BrowserPurpose::kEnvironment`, with listing options of its own); View > Environment picks the physical sky or opens
a panorama. `SPEEDO_AUTOLOAD_MODEL`, `SPEEDO_AUTOLOAD_IMAGE` and `SPEEDO_AUTOLOAD_ENVIRONMENT` take paths absolute or
relative to the resource directory (without an environment, the sky loads, as one of the autoloads). Zip archives (opened, or a `.zip` in `SPEEDO_AUTOLOAD_MODEL`, which loads all of its models) are extracted
once into `<user profile>/archives/<name>-<hash of path, size and time>` with `core::zip` (`core/zip.h`: our own
reader, libdeflate's inflate and crc-32), then loaded from there like any other files; with several models the user picks one. The extractor reads
each entry by its local header: some archives have stale central directory entries (cube.zip in the McGuire archive),
which unzip ignores too. An entry shorter than a page is cut to its size once written: mio makes a new file a page long, of
whatever its stack held, and only ever grows it (`core::file::Write` cut its files already), so geodesic.zip's small objs
had garbage tails that failed to parse; the `.extracted` marker's length says which extractor made a directory, and an
older one's extraction is done again in place (`ExtractArchive`). `assettest` takes zip archives as well, extracting them the same way.

`scripts/assettest.ps1 <zips or dirs>` (PowerShell, as the other scripts) runs the `assettest` tool (imports every model and image and checks the result:
index ranges, normals, winding, missing textures, mip chains, unwritten blocks, compression error), and with `-Client`
also loads each model in the client (`SPEEDO_AUTOLOAD_EXIT=<frames>` makes it exit after that many drawn frames once the
autoloads have installed and `SPEEDO_CAMERA` has placed the views: counted in the ui's frames, which run ahead of the
draw thread's, a big model's install and a capture of it came apart), and
`SPEEDO_BACKGROUND=1` opens its window behind the others without taking the focus, so the runs don't interrupt whoever
is working, and `SPEEDO_PRESENT_MODE=fifo`: with mailbox, the client's default where the surface has it, NVIDIA's Windows
driver hangs about 1 exit in 100 in `vkDestroySwapchainKHR`, which no change of ours fixed: handling the window's messages
meanwhile, or keeping the presents' semaphores until their images were acquired again (which validation then reported
destroyed in use, `VUID-vkDestroySemaphore-semaphore-05149`: without present fences it can't know a present is done);
0 in 858 with fifo), failing
on load errors, asserts and validation messages. Only debug enables validation, but it imports slowly: run the
profile preset first, then debug with `-ClientOnly` and the same `-Work` dir, whose caches it reuses. A debug client
compiling the shaders (a new profile, or a changed shader) took more than ten minutes a profile, with no window
meanwhile: SPIRV-Tools' optimizer, which slang runs over every shader (`SLANG_OPTIMIZATION_LEVEL_MAXIMAL`), linked
statically from the debug spirv-tools package at the toolchain's `-Og` without inlining. `ports/shader-slang` and the
`ports/spirv-tools` overlay (of the upstream port, for this alone) build their debug configs with `-O2 -g`
(`OPTIONS_DEBUG`): about 2 minutes a profile now. (Mapping the debug config to the release slang in CMake couldn't pick
it: both dylibs share a name on the executable's rpath, and the debug directory comes first.) The validation layer's
dylib is in `debug/lib`, so a debug client run by hand sets `DYLD_LIBRARY_PATH` to it as the script does;
`VK_LAYER_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED` adds gpu assisted validation. Both run in
parallel: `assettest` checks its models and images on every thread (`CheckInOrder`: results printed in the serial order;
`--jobs N`), and the script runs `-Jobs` clients at once (default 4), each worker with a user profile of its own
(`<work>/user-<n>`: clients write pipeline caches, shader intermediates and asset caches there), a model to the same
worker (FNV-1a of its label), so its caches stay warm, each worker's longest first (by what each took the last time with
the preset, `<work>/logs/<preset>/durations.tsv`), and a worker with none of its own left takes the shortest of the
busiest worker's (the most time left; short, so cheap to load without its caches): assigned once and for all, the
workers that ran out waited for the one with the most, and the end of a run was one worker's alone (the full debug run
took 909 s for 2613 s of clients, of which 4 workers needed 653; with the stealing, on its first run without durations, 843 s for 3333 s, a fourth of which is 833: the stolen runs import again in the other profile, which the durations make the short ones); on macos a new worker profile is an APFS clone (`cp -c`) of
`<work>/user`, the serial runs' profile. Results print in the serial order, and the logs are kept per preset (`<work>/logs/<preset>`: a debug run after a profile one overwrote its logs, so a profile failure couldn't be read). That took the full regression from about 30
minutes to 5 or 6 (130 to 175 s profile, 180 s debug, warm). (Before each client, the script also used to wait a second in `caffeinate -u -t 1` to wake the
display; it wakes it once now.) For iterating, `fetch-test-assets.ps1 -Quick` prints a quick tier instead
(`scripts/test-assets/quick.txt`: a model or two per feature, plus the hand-made ones; about a minute); the full sets run
before committing. `assettest.ps1` takes `.hdr` panoramas as inputs too, and runs a directory of one model's encodings
given directly (a glTF sample model: its files all named after it) as one set, as when its parent is given. The client's
main loop sleeps in `glfwWaitEvents()`, so anything that must end it from another thread goes through
`RequestExit()`, which posts an empty event.

The test sets come from their sources, not from local copies: `scripts/fetch-test-assets.ps1` downloads Morgan McGuire's
Computer Graphics Archive (obj, about 2.7 GB), the Khronos glTF-Sample-Assets models (at a pinned commit, about
2.3 GB) and the Khronos glTF-Sample-Environments panoramas (about 400 MB; Git LFS files, downloaded from GitHub's LFS
media server at a pinned commit and checked against `scripts/test-assets/environments.txt`, the sizes and sha256s of
their LFS pointers; `assettest` checks each, and `assettest.ps1 -Client` runs the client once per panorama) into `resources/test-assets` (gitignored; the client's file browser opens there; or `$SPEEDO_TEST_ASSETS`), and
prints the paths to test:
`scripts/assettest.ps1 -Client (scripts/fetch-test-assets.ps1)` (from pwsh; from another shell,
`pwsh scripts/assettest.ps1 -Client $(pwsh scripts/fetch-test-assets.ps1)`). The archive publishes no versions or checksums and
does change (several files differ from a 2019 copy), so `scripts/test-assets/mcguire.txt` pins each file's size and
sha256: a changed file is kept as `.unverified` and reported until the manifest is updated, after checking what changed.
`resources/test-models/gltf` holds hand-made models for what no downloaded one covers (`SparseIndices.gltf`: index
accessors that are sparse, with and without base values; cgltf's `cgltf_accessor_read_index` and
`cgltf_accessor_unpack_indices` refuse sparse accessors, so `gltf::ReadIndices` applies them;
`InstancingTransforms.gltf`: instances of a single sided, asymmetric triangle under a scaled and moved node, with
normalized short rotations and a mirroring instance, which must face the camera too; `BlendOrder.gltf`: blended quads
listed nearest first (metallic, so they render dark); `BlendInstances.gltf`: a
blended quad instanced in front of and behind another; `ManySamplers.gltf`: 36 quads with a sampler
each, of every filter and wrap mode, more than the model sampler slots once were; `ScatterTexture.gltf`: volume scatter's
multiscatter texture, see above; `ManyLights.gltf`: 24 point and 8 spot lights between pillars, more shadowed lights than
the atlas holds at full size, best seen with `SPEEDO_ENVIRONMENT_INTENSITY=0.05`; `ThousandsOfLights.gltf`: 3800 small
point lights and 200 spot lights, all ranged, between 625 pillars on a 200 m ground, for clustering and culling at scale,
generated by a seeded script, best seen the same way), and is always part
of the printed paths, as is `resources/test-models/obj` (`BumpDomes`: a quad with a bump map of four domes, as is and with
its texture mirrored, which must both come out raised and lit from above, i.e. from the default light's side). Two archive files are both called `sponza.zip` (Crytek's and Dabrovic's), so the latter is saved as `dabrovic_sponza.zip`,
and Bistro's five zips (the scenes and three texture packs, which the scenes reference as `..\BuildingTextures\...`) are
extracted side by side into `mcguire/bistro/`. Known asset problems that only warn: erato's normals don't follow its faces and are
generated (see the obj importer above), Bistro and bmw have normals that get replaced, and
`geodesic_dual_classIII_20_10.obj` has 1419 stray vertices (at radius 1140, the others at 1), which `assettest`'s stray
vertex check (more than 100 times the median distance from the median point; ground planes reach about 20) finds. It and
`geodesic_dual_classII_20_20.obj` (faces of up to 384 corners that overlap: most of its rays meet a triangle tied with
another) make trees so deep and overlapping that a ray visits most of them: with a stack of 24 a ray in 40 dropped what didn't
fit, and a dispatch of such rays ran past Metal's command queue timeout (`VK_ERROR_DEVICE_LOST`, 2 runs in 3 of the
geodesic set). The stacks are 48 and 128 now (`BVH_STACK_SIZE`, `BVH2_STACK_SIZE`: no measurable cost, Bistro 25.13 ms
either way), and a ray stops after `BVH_MAX_STEP_COUNT` nodes (4096; the binary layout 16384), with the nearest of what
it met (`BvhHit::capped`; a capped shadow ray is shadowed), counted (`GI_STATISTIC_CAPPED_BVH_RAYS`, printed): every ray
of the dual 20_20 is capped, no ray of the other test scenes. `CheckBvh` reports capped rays apart, as a warning.
tinybvh's own stack is raised for their builds (`TINYBVH_STACK_SIZE` in `bvh.cpp`: "CombineLeafs, tree too deep" at 128).

Archives of several models are sets of variants (geodesic's 86 polyhedra, sphere's tessellations and texture mappings,
the CornellBox variants, ...), not scenes: each file stands alone at the origin, and the files of a set needn't share a
scale. `Model::Load(filePaths)` loads them as one model, side by side in a grid facing the camera, each scaled to the
same size and centered in its cell by its instance transforms (premultiplied by the placement; vertices are copied as
they are), each file still through its own cache entry, merged as staging data before the one upload. Autoloading a zip or a directory loads all of its models that way, opening a zip offers it next to choosing one,
the browser's Open loads several selected models so (or as authored), and `assettest.ps1` runs the client once per such archive (with the archive's first
image on the default material: sphere.zip's models name materials its mtl file doesn't have), and once per
subdirectory with several models of a directory it is given, rather than once per file: 38 client runs instead of 145
for the McGuire set. The glTF sample models are such subdirectories, of encodings of the same model (glTF, glTF-Binary,
glTF-Embedded, glTF-Quantized, Draco, KTX2, ...): one run each (150 instead of 339), with the encodings the importers
don't support skipped (`mesh::Unsupported`, which only parses the gltf json) rather than failing the set. `assettest`
compares the encodings of each model (files named after the model's directory, in its subdirectories): the same
triangle, vertex, material, submesh and texture counts, and bounds within 1% (quantization rounds). It only warns, since
some differ in the source files: ABeautifulGame's glb is another export (1152 fewer triangles), and StainedGlassLamp's
JPG-PNG variant has 8 materials instead of 13.

## Sanitizers: the sanitize and tsan presets

`<triplet>-sanitize` (macOS and Linux, from `setup.ps1`) is the profile configuration with `SPEEDO_SANITIZE=address,undefined`
(`CMakeLists.txt`): our own targets compiled with `-fsanitize=` and `-fno-sanitize-recover=all` (the first report aborts),
the vcpkg packages as they are. The runtimes are compiler-rt's, which the host llvm builds (its `compiler-rt` feature,
off Windows; adding it rebuilt llvm, about 50 minutes). The sanitizers replace malloc, so a sanitize build has no
mimalloc: the executables don't link it (its dylib interposes malloc process wide) and `SPEEDO_USE_MIMALLOC` isn't
defined, which takes the code's fallbacks (the Vulkan host allocation callbacks in `instance.cpp`: `aligned_alloc`
there needs a size that is a multiple of the alignment, and on macOS an alignment of at least a pointer, or it returns
null, which failed `vkCreateInstance` and `vkCreateDevice` out of host memory). Run it on the quick tier:
`pwsh scripts/assettest.ps1 -Preset <triplet>-sanitize -Client ...`, which sets `UBSAN_OPTIONS=print_stacktrace=1`
and the host llvm's `llvm-symbolizer`, and fails a run on its reports (`ERROR: AddressSanitizer`, `runtime error:`).
The quick tier passes, assettest and clients.

`<triplet>-tsan` is the same with `SPEEDO_SANITIZE=thread` (tsan combines with neither asan nor ubsan). tsan reports and
goes on, exiting with 66 (an abort, on macOS) once done; `assettest.ps1` sets `TSAN_OPTIONS` (the symbolizer, and
`tsan-suppressions.txt`) and fails a run on `WARNING: ThreadSanitizer`. The quick tier passes. What it found: printing
with `std::cout`/`std::cerr` from several threads races in libc++ (`operator<<` reads and resets the stream's `width()`
unlocked, which the standard says it mustn't): the asset loaders' reimport messages, from parallel texture loads. Print
with `std::println` (`stderr` for errors), one locked `fwrite` a line, which also keeps lines whole: there's no
`std::cout` or `std::cerr` left. `tsan-suppressions.txt` holds what isn't a race: a function local static's guard is
released in `__cxa_guard_release`, which our binaries bind from a C++ dylib (cpptrace's, slang's: each exports the
static libc++abi linked into it) rather than the system's libc++abi, so tsan's interceptor never sees it, and the
threads reading a static first race the one that initialized it (mio's `page_size()`, the only one the quick tier
hits). Our frames aren't symbolized there (no line numbers), so a suppression names a function tsan prints, not one
inlined into it.

About one client run in 50 faulted inside the tsan runtime on macOS 27 (`MetaMap::AllocBlock`, `OnUserFree`: frees of
system frameworks' allocations, CoreUI's libcompression on a dispatch queue), not in our code. Our fatal signal handler
then hung the run (its stack trace allocates, which waited for the tsan lock the faulting thread held): a sanitized
build (`SPEEDO_SANITIZE`, also a compile definition) leaves SIGILL, SIGABRT, SIGFPE and SIGSEGV to the sanitizer, which
reports them itself and exits.

Third-party code compiled into ours (the header-only libraries) whose reports are its idioms goes in
`sanitize-ignorelist.txt`, by function name (`fun:`): FASTBuild compiles preprocessed sources (`-frewrite-includes`), so a
`src:` entry only ever sees the `.cpp`, never the header. FASTBuild doesn't track the ignorelist either: after changing
it, touch the sources it concerns (or rebuild). stb_image_resize2 is the one there: unaligned 8 byte stores, and srgb
tables indexed through a biased base pointer.

## Gotcha: `<windows.h>` macros

`core/file.h` includes `<windows.h>` (mio's), so everything reading files sees it, and its macros rename our code:
`LoadImage` becomes `LoadImageW` (and `CreateWindow` `CreateWindowW`: platform's is `CreateNativeWindow`) in the files that include it (a link error, `gfx::Scene::LoadImageW`, where the
definition didn't), and a function like macro named like one of its types rewrites the SDK (`capi.h`'s scalars are
`FLOAT1`/`UINT1`/`INT1` because `UINT(name)` turned `typedef UINT (CALLBACK *YIELDPROC)` into an error in
`mciapi.h`). `core/mio_extra.h` includes mio with `WIN32_LEAN_AND_MEAN`, `NOUSER` and `NOGDI` (mio needs only the
kernel api); other headers keep `<windows.h>` out (see `core/profiling.h`).

Narrow strings are UTF-8 on Windows too: the executables' manifest (`src/windows.manifest`) sets the process's ANSI code
page to UTF-8, which libc++'s `path::string()` and the narrow file APIs (mio, the importers) convert with. With the
system's (1252) it threw on the glTF sample `Unicode❤♻Test` when the file browser listed it. lld-link can't merge a
manifest without libxml2 or mt.exe, so it is a resource (`src/windows.rc`), and `CMakeLists.txt` turns CMake's
`/MANIFEST:EMBED` (in `Windows-Clang.cmake`'s link rule, after the target's flags) into `/MANIFEST:NO`. A new
executable target needs `windows.rc` in its sources.

## Parallel work is tasks; callables are templates or tasks, not `std::function`

- Run parallel and background work as tasks on the application's executor (`core::CreateTask` and
  `TaskExecutor::Submit`, `TaskExecutor::ForEach` for a loop over indices, `core::AddDependency` to run one task after
  another, `Join` or a future's `IsReady` to wait), not on threads of its own (`std::thread`, `std::jthread`,
  `std::async`): the executor's threads are named for the debugger and Tracy and joined at shutdown, and a waiting `Join`
  or `ForEach` helps run the queue instead of blocking a thread. The executor's are the only threads, the tools' included
  (`assettest` makes one executor for its checks, `CheckInOrder`, the environment kernels and zip extraction). Work for a
  particular thread is a task too, queued for it: `mainCalls` (the main thread's, run by
  `platform::WindowedApplication::Main`) and `RHI::drawCalls` (the draw thread's; `TransitionThenBind` makes its bind
  one). Code that runs work in the background takes the executor (`FrameInputs::executor`, which background pipeline
  creation, `Pipeline::RequestGraphicsPipeline`'s task, is submitted to).
- Take a callable as a template parameter (`template <typename Fn> void Foo(Fn&& fn)`); where it runs later, or on
  another thread, as a task (`core::TaskHandle`, as `mainCalls` and `drawCalls` do; a header template can make it,
  passing only the handle into the .cpp, see `TransitionThenBind`); where it is called during the call into a .cpp
  that a template can't reach, as a `core::std_extra::function_ref` (c++26's, else `tl::function_ref`: not owning,
  never empty, so an optional one has a default, `core::NeverCancelled`, `NoUi`, `NoMaterial`, or is a `std::optional`);
  and where it is stored and called again (the frame graph's passes) or may be dropped unrun (a task never is: the
  preview's `CaptureDone`), as a `core::std_extra::move_only_function` (c++23's, else a polyfill in `core/std_extra.h`
  that supports `R(Args...) const` too). Not as a `std::function`, which copies, type-erases every call and allocates
  for larger captures. A `function_ref` bound to a lambda dangles once the lambda's full expression ends: bind it in the
  call (or an aggregate built in the call, as `FrameInputs` is), or to a named lambda that outlives it.
- Sleep until a time with `core::SleepUntil` (`core/time.h`), not `std::this_thread::sleep_until`/`sleep_for`, where
  the time matters: on Windows those round up to the system timer's resolution, 15.6 ms unless some process raises it,
  and the client's tick (input and the ui), paced to a millisecond, ran at 64 Hz: the camera and the ui moved at 64 Hz on
  a 120 Hz display while frames were drawn at hundreds a second, a stutter Tracy showed nothing of (the time was a
  sleep). It waits on a high resolution waitable timer there (ticks 1.56 ms apart, 15.4 before).
- The executor's threads are `core::std_extra::jthread`s: `std::jthread` with P2019's thread attributes (the threads'
  name and stack size passed as `jthread::name_hint` and `jthread::stack_size_hint` before the callable), std's where a
  standard library has them (`__cpp_lib_thread_attributes`; none yet, P2019R9 is at LEWG), else a polyfill whose platform
  code is in `core/std_extra.cpp` (pthreads, `_beginthreadex`), so the scheduler has none (its own thread naming went).
  A `std::thread` can't be given a stack size, and macOS gives one 512 KB, which a debug client loading rungholt ran past
  (`SIGBUS` in a stack guard, a client run in the full debug regression): a thread waiting in `Join` or `ForEach` runs
  whatever is ready meanwhile, so nested waits stack task upon task (the BVH builder's spatial splits wait on a parallel
  loop in each partition, running another partition meanwhile, which waits in turn), and the ui's tick landed on top, its
  `PrepareDraw` frame big in a debug build. The task threads' stacks are a main thread's, 8 MB (`kThreadStackByteCount`).
- Tasks come from a fixed pool (`kTaskPoolSize`), and a task stays allocated until its future is gone and the executor
  purges it (`JoinAll`, or after another task runs). A waiting `ForEach` runs other tasks meanwhile, so `ForEach`es nest,
  each holding a task per thread: a model's texture loads, each compressing on every thread, took about threads squared
  (see `kTaskPoolSize`).

## Gotcha: `core::CreateTask` stores lvalue arguments by reference

`CreateTask(callable, args...)` keeps `args` in a `std::tuple<Args...>` with `Args` deduced as
forwarding references, so an **lvalue argument is stored as a reference**, not copied. The task
usually runs later, so passing a local or a function parameter as an lvalue leaves a dangling
reference (typically read back as mimalloc's freed-memory pattern `0xdfdf...`). Pass arguments that
must outlive the call as rvalues — `std::move(x)` or an explicit copy like `std::vector(x)` — or capture them by
value in the callable, as `core::LoadQueue::Enqueue` does. Only pass lvalues when the reference is
intended and the referee outlives the task (e.g. the `Rpc` tasks' socket/poller, which are members
of the client/server object).

This is easy to miss with lambdas: a captureless lambda passed by reference happens to work (there's
nothing to read), but it breaks as soon as a capture is added.

## Pipelines own their layouts, by name

A pipeline layout belongs to the `rhi::Pipeline` that created it: `CreateLayout(name, shaderSet)` registers it under its
name, and `GetLayoutHandle(name)` finds it (`"Main"`, `"VertexZPrepass"`, `"Environment"`); binding another pipeline's
layout trips `BindLayoutAuto`'s ENSURE. A pipeline's descriptor sets are its own too, and hold one value per binding, so
two scenes can't share one: each `gfx::Scene` is given the pipeline it draws with (`Scene(rhi, pipeline, views)`; the
window's scene uses `Device::GetPipeline()`), and creates its layouts in it. (The device used to keep a map of layout
handles by name hash, which handed out the first pipeline's layouts.)

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

The source files are listed explicitly in `CMakeLists.txt` (`CORE_SOURCE_FILES`, `GFX_SOURCE_FILES`, ...): add a new
`.cpp` there. That changes `CMakeLists.txt`, which reconfigures the build dir (VS Code's CMake Tools does it with the
preset's environment). They were GLOBs, which only a configure re-reads, so a build dir that wasn't reconfigured after a
file was added failed to link (`undefined symbol` for what the new file defines); `CONFIGURE_DEPENDS` didn't fix that with
the FASTBuild generator: it picked up one new file, but twice its glob check didn't run, or the regenerate it triggered
(`cmake --regenerate-during-build`) did nothing.

## Scripts are PowerShell

Write the repository's scripts in PowerShell (`.ps1`, run with `pwsh` on every platform), not bash, batch or
python. The only exceptions are the bootstrap scripts `setup_linux.sh`, `setup_osx.sh` and `setup_windows.bat`,
which have to run before `pwsh` is installed, and should do no more than install it and hand over to `setup.ps1`.

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
- pdbs: vcpkg's `vcpkg_copy_pdbs` needs `dumpbin`, which isn't on the path for a chainloaded toolchain, and many
  ports install pdbs only under CMake's `MSVC` check, so on Windows no package had any. `x64-windows-clang.cmake`
  replaces the function (llvm-readobj for the path, llvm-pdbutil to check the guid) and runs it after every portfile
  (`VCPKG_POST_PORTFILE_INCLUDES`, `scripts/cmake/copy-pdbs.cmake`). The guid check catches targets sharing a pdb
  path: tidy-html5's `tidy.exe` overwrites `tidy.dll`'s, so that one has none.
- Ports that check `MSVC` take their Linux paths under our GNU style clang on Windows too: slang split its debug
  info with objcopy and strip (`SLANG_ENABLE_SPLIT_DEBUG_INFO=OFF`), and zstd, pugixml and draco need static
  linkage. miniz is static too: only slang uses it, for archive formats we don't use.

A port FASTBuild can't build at all can use another generator from its portfile (`vcpkg_cmake_configure(... GENERATOR
Ninja)`, see `ports/draco`: a shared library linked from object libraries only, and header-only object libraries). Ports
that link executables with GNU link groups (`-Wl,--start-group`) need a patch on apple platforms: ld64.lld has none, and
our clang reports itself as `Clang`, not `AppleClang`, which the ports' checks expect (`ports/draco/0001-*`).

FASTBuild generator pitfalls in third-party CMake (see `ports/tracy/0007-*`): a directory as a custom command
`OUTPUT` fails with `File missing despite success`, and custom commands that `DEPENDS` on a *target* name are not
ordered after it, so depend on the produced file (e.g. an `ExternalProject` `INSTALL_BYPRODUCTS`). ld64.lld does not
add `libobjc` implicitly the way Apple's linker does (`undefined symbol: objc_msgSend`, see `ports/tracy/0008-*`).

`ports/ktx` exists because IT policy blocks msys2's `bash.exe` on Windows: upstream's portfile acquires msys2 bash to
run KTX's `mkversion` script, and `0007-version-without-bash.patch` writes the same `version.h` from CMake instead
(bash is still required for the tests or `KTX_GENERATE_VK_FILES`, which we don't build). Don't add ports that need an
msys2 shell (`vcpkg_acquire_msys` with `bash`, `vcpkg_configure_make`) without patching that out; vcpkg's own
`pkgconf.exe` (`vcpkg_fixup_pkgconfig`) runs fine.

`ports/mimalloc` exists for a macOS bug in mimalloc 3.x, so retire it only once upstream fixes it. dyld allocates
each image's thread-local-variable block from the system zone (`malloc_type_malloc`) and frees it at thread exit
through the pthread key destructor `free`, which is interposed to `mi_free`. mimalloc's own thread locals are
included, so every exiting thread frees a block mimalloc doesn't own. Debug builds print
`mi_free: invalid pointer`, and both builds leak 32 bytes per thread. The patch interposes `free` with a check
that forwards foreign pointers to `malloc_zone_from_ptr(p)`.
