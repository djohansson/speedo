# Layering: the platform, the file browser, the ImGui renderer and the shaders

A note of CLAUDE.md (its index names each): the layers in full, where CLAUDE.md keeps the map.

## Layering: core ← platform ← rhi ← gfx ← client

- `platform` is the window system, and the only code that calls glfw: its backend (`platform/glfw.cpp`, `glfw.h`:
  initializing and terminating glfw, the window's creation and destruction, its callbacks, the event loop's wait and
  wake up, and the Vulkan surface; on macOS, before `glfwInit`, AppKit's window restoration off in the process's
  registered defaults, `ApplePersistenceIgnoreState` and `NSQuitAlwaysKeepsWindows`: after a launch of the packaged app
  that crashed or was stopped, AppKit asked at the next whether to reopen its windows, holding that launch for minutes
  until answered, 2026-11-08), the windows (`platform::Window`: a native handle, its `WindowState` and whether it
  is minimized; `WindowHandle`/`WindowState` in `platform/capi.h`, with `GetCurrentWindow`/`SetCurrentWindow` and the
  C entry points `main.c` calls: `PlatformInit`, `PlatformWaitEvents`, `PlatformWakeMain`, `PlatformTerminate`, and
  `PlatformCreateVulkanSurface` for rhi, its types opaque, `void*`), `platform::WindowedApplication` (makes its window
  and destroys it, runs `mainCalls` on the main thread in `Main`, wakes the main loop on `RequestExit`, tracks
  minimizing in `OnResizeFramebuffer`, and the window changes: fullscreen, sizes, monitors) and imgui, all but its
  drawing: `platform::ImGuiLayer` (`platform/imguilayer.h`: the context, fonts and style, the frames built on the ui
  thread, `BeginFrame`, `Fade`, `EndFrame`, and the triple buffer of their draw data snapshots, `imgui_extra`, which the
  draw thread takes, `TakeFrame`, with the frosted panels' rectangles, `ImGuiPanel`; its renderer is gfx's), and its
  platform side (`platform::imgui`, imgui's glfw
  backend; its input callbacks are installed by `platform::imgui::Init` rather than the backend: they run on the main
  thread, in `glfwWaitEvents`, and fill imgui's input queue, an `ImVector`, which `ImGui::NewFrame` consumes on the ui
  thread, so each calls the window's own callback, then the backend's under a mutex that `NewFrame` returns held until
  `ImGui::NewFrame` is done; unlocked, the two once crashed in `FindLatestInputEvent` with an index out of range). It
  links glfw privately (and Vulkan's headers, for `glfwCreateWindowSurface`'s types), imgui publicly (the frames' draw
  data), and its interface knows
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
  as out of date; refused, `VK_ERROR_INITIALIZATION_FAILED`, it is made composited until the window comes to the
  foreground, `PlatformIsWin32Foreground`, which `SetExclusiveFullscreen` checks each frame meanwhile (glfw's focus
  events don't tell: a window made behind another has its thread's focus, and the click that brings it to the foreground
  sends none): an application controlled swapchain without it presents nothing, its presents failing as lost, and it
  was made again every frame, refused each time, until the window was minimized between
  `RecreateWindowDependentObjects`' check of its size and the swapchain's, a swapchain of 0x0 whose renderer's images
  NVIDIA's driver refused; `CreateSwapchain` makes none of no area now. On 2026-11-01, driver 610.88, RTX 4070 Ti SUPER,
  it was refused with the window in the foreground too, every try for 3 s after a switch, and the composited window's
  variable refresh rate engaged anyway under fifo, not under mailbox: switching to fifo, the display froze for a couple
  of seconds while it did, the draw thread's longest frame 136 ms), borderless (an
  undecorated window covering the monitor, on every platform; on macOS with the menu bar and dock hidden, and composited)
  or, on macOS, native (its own space). Exclusive and native
  let macOS scan the swapchain out directly, which flickered with KosmicKrisp from mesa 26.2 (frames out of order), so
  borderless was the only mode for a while; with mesa main (8fc4981) all three work. On macOS exclusive keeps the
  display's mode: glfw sets the mode nearest the one asked for among those it lists, which left out a Retina display's
  scaled modes, so a MacBook Pro's 1512x982 points at 2x became 1920x1200 at 1x (the ui and the window changed scale),
  and the `glfw3` overlay port lists them (see "vcpkg"). A window in a space of its own (native's, or the green button's,
  which glfw and the state don't know of) leaves it at Alt+Enter whatever the mode (`InMacSpace`): taken to exclusive
  inside the green button's space, leaving it crashed (glfw restored the display mode under the space, and AppKit threw
  reshaping it, in `_NSFullScreenContentController`). The same menu sets the swapchain's
  presentation (`Swapchain::Configure`: `rhi::PresentationMode`, FIFO, FIFO relaxed, mailbox or immediate, of those the
  surface has, and an image count, applied by `Draw`, which recreates the swapchain; KosmicKrisp has FIFO and immediate,
  and takes exactly 3 images; at startup the swapchain takes FIFO, with 3 images where the surface allows: mailbox, the
  default where the surface had it until 2026-11-01, never engaged the display's variable refresh rate), as
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
  `VK_LAYER_PATH=<install>/x64-windows-clang/bin` and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` validates too,
  into a file: `VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG` and
  `VK_KHRONOS_VALIDATION_LOG_FILENAME=<file>` (the layer prints to stdout only while there is no messenger, and the crash
  reports make one with a device that has address binding reports, NVIDIA's: from then on nothing reached stdout, a run's
  30 best practices warnings included, `VK_LAYER_VALIDATE_BEST_PRACTICES=1`, a check that the log works). For a lost
  device, the Crash Diagnostic Layer (the
  `vulkan-crash-diagnostic-layer` overlay port, installed beside the validation layer:
  `VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_crash_diagnostic`) dumps the submits and commands the gpu hadn't finished, with
  the shader and descriptor sets of the last one started, to `<home>/cdl/<time>/cdl_dump.yaml` (its `CDL_*` variables
  weren't taken from the environment here: the dump's settings show what was). With the layer loaded, the client's own
  crash report defers to it (see "Crash reports").
- `rhi` is the graphics backend abstraction (instance, devices, queues, buffers, images, pipelines, swapchains),
  templated on `GraphicsApi`. It must not include `gfx` or know about the application, nor hold code of a library above
  the graphics api (imgui's renderer is gfx's, see below): rhi objects find
  their instance and devices through `rhi::GetRHI<G>()`, which an `RHI<G>` serves for its whole lifetime (registered
  by its first member, so before its members are created and after they are destroyed).
  The RHI is made for a window the platform already made (`RHIInitializationData::window`): its surface picks the
  devices that can present to it, and it keeps a swapchain per window (`RHI::GetSwapchain(WindowHandle)`).
- `gfx` is everything above it: importers (`obj::Import`, `image::Import`), `Model`, `LoadTexture`, cameras and
  `Views`, the renderer and frame graph, `gfx::Scene` and `gfx::ImGuiRenderer`. `gfx::Scene` (`gfx/scene.h`) is what is
  drawn and how it is lit: the installed model, image and environment, their materials, textures, lights and animation,
  the default resources standing in for what they don't have, and the `Renderer`. Its loads (`LoadModels`,
  `LoadImage`, `LoadEnvironment`) run in load tasks and install on the draw thread; `Update` and `Record` are the draw
  thread's per frame; the ui changes its `Settings` and reads its animation, scenes and statistics. Its state is an
  `Impl` in `scene.cpp` (what were the application's file statics), whose lambdas capture `this`: the application
  destroys the scene in `Shutdown`, after the tasks and draw calls that may use it. `gfx::ImGuiRenderer` draws the ui's
  imgui frames (see "ImGui's renderer", below). `gfx/graphicsqueue.h` has
  the draw thread's graphics queue helpers (`UpdateBufferOnGraphics`, `TransitionThenBind`, `RetireAfterGraphicsWork`).
- `client` is the application: `client::WindowedApplication` (a `platform::WindowedApplication`) owns the rhi, the
  views, the scene, the imgui layer and its renderer, and keeps the frame loop (`client/windowedapplication.cpp`: `Draw`, fences,
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
  several always used, or as authored, each file where it is), the rest each by its type; the models and archives on a
  ground plane where its "Ground plane" box is ticked (on by default since 2026-11-04, the user's call; see "The
  ground plane", under "Asset import and testing"). What it opens is the newest of File > Open Recent's list
  (`WindowedApplication::InternalOpen`, which the menu opens through as well; `RecentFile`: the paths, how several are
  arranged and the ground, the last `kRecentFileCount`, 6, in `<user profile>/recent.txt`, a line each, read when the
  menu first asks and written at each change; those no longer there disabled, checked as the menu opens; the folder
  holding them and how they were opened in the menu's right column, since a gltf sample model's encodings share its
  name). `SPEEDO_BROWSE=<dir>`,
  `SPEEDO_BROWSE_SELECT=<path>[;<path>...]` (paths from the folder), `SPEEDO_BROWSE_ENVIRONMENT=1`, `SPEEDO_BROWSE_RECURSIVE=0`,
  `SPEEDO_BROWSE_KINDS=<models,archives,images,panoramas>`, `SPEEDO_BROWSE_GROUND=0` and `SPEEDO_BROWSE_OPEN=side-by-side|as-authored` drive it at
  startup, for tests and screenshots (`SPEEDO_BROWSE_OPEN_FRAME_COUNT=<n>` opens once the preview has loaded the selection
  that many frames: a double click, whose first click started it; a preview is given up when the browser closes, its
  loads cancelled). Tiles show thumbnails (`client::Thumbnails`, `client/thumbnails.h`): rgba8 in
  one imgui texture registered with `ImGui::RegisterUserTexture` (an atlas of 160 pixel slots, 24x24, reused least
  recently drawn first: the renderer's texture slots, `SHADER_TYPES_IMGUI_TEXTURE_COUNT`, wouldn't hold one each), made on the task executor (4 at a
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
- ImGui's renderer is gfx's `ImGuiRenderer` (`gfx/imguirenderer.h`), on rhi alone, as anything else gfx draws: rhi
  knows nothing of imgui (it used imgui's Vulkan backend, `ImGuiRenderer<G>`). A pipeline of its own (its descriptor
  sets are the ui's) and a layout, "ImGui" (`imgui.slang`): imgui's vertices pulled from a host visible buffer per frame
  slot (`gImGuiVertices[frame]`, 5 words a vertex: rhi's pipelines have no vertex input), its 16 bit indices bound, each
  draw's texture a slot of one array (`gImGuiTextures`, `SHADER_TYPES_IMGUI_TEXTURE_COUNT`, 256; slot 0 a white default
  for the slots nothing holds, so `ImTextureID_Invalid` is none) and its first vertex pushed as constants
  (`ImGuiConstants`): the draws' vertex offset is 0, since slang's `SV_VertexID` leaves the draw's base vertex out (as
  D3D's), and with imgui's offsets the draws after a frame's first read the wrong vertices (spikes and scrambled text,
  found comparing captures). The blending, sampler (linear, clamped) and clipping are imgui's Vulkan backend's, and its
  ui renders the same, to 0.007 of 255 (the menus and the Lights window, the old renderer's capture from a debug client
  built before, imgui being linked statically). imgui's textures (its font atlas: replaced as it grows, around the 8th
  frame of every run) are created on the ui thread as imgui asks (`UpdateTextures`, from `ImGuiLayer::EndFrame`), and the
  caller's (`AddTexture`: the browser's preview) take slots as well; the draw thread binds them, records the uploads
  (`CommandEncoder::CopyBufferToImage`) and releases them once no frame drawing them remains (`PrepareFrame`: the slot
  bound to the default, the texture destroyed and the slot freed once the gpu is done). A change of the slots takes a
  new descriptor set, and the ones before go once the gpu is done with them (`Pipeline::TakeDescriptorSets`). Its
  platform side is `platform::imgui` (`Init`, `NewFrame`, `Shutdown`), and the rest of imgui `platform::ImGuiLayer`.
  What it takes of imgui is checked where it can be, so that a change of imgui (a version, or its configuration in
  `imconfig.h`) or of the code around it fails rather than drawing garbage. At compile time (`imguirenderer.cpp`): the
  vertex's layout, as `capi.h` gives it to the shader (`SHADER_TYPES_IMGUI_VERTEX_*`: its size, each member's offset and
  type), the color's channels (`SHADER_TYPES_IMGUI_COLOR_*_SHIFT` against `IM_COL32_*_SHIFT`), the index (16 or 32 bits,
  bound as it is), the texture id (an integer of 64 bits, invalid 0), imgui 1.92's textures and the push constants'
  layout; checked by compiling it with BGRA colors, a reordered `ImDrawVert` and a 32 bit `ImTextureID` (each fails,
  naming what) and 32 bit indices (fine). At run time: a texture id is its slot and a generation of it (`TextureId`), so a
  draw of a texture released (`RemoveTexture`) or not the renderer's fails, naming it (a removed preview drawn on: "the ui
  draws texture 0x100000005, which isn't one of the renderer's"), where it drew what the slot held since, or past the
  array (a lost device); a release of an id not held fails too; `Draw` must be given the draw data `PrepareFrame` copied
  into the frame slot (the UI Uploads pass before the UI pass), each draw within its list's indices and vertices, and the
  lists' counts imgui's; imgui gets the device's largest texture (`DeviceLimits::maxImageDimension2d`) for its atlas, and
  a texture other than rgba fails, naming its format. The constants are pushed again after a user callback, which may
  change the render state. Not checked: what a render target's format is (an srgb one would encode imgui's srgb colors
  twice: the swapchain's is unorm, being a storage image), and the indices' values (read past their list's vertices,
  which the counts don't cover).
- Shaders are gfx's: the slang sources and the C header they share with the C++ code (`gfx/shaders/`), and
  `gfx::ShaderLoader`, which compiles them for `rhi::kShaderFormat` and reflects their bindings into a neutral
  `rhi::ShaderSet` (`rhi/shaderset.h`: binaries, entry points, and per set the bindings and push constants). rhi
  doesn't link slang. `Pipeline::CreateLayout` turns a `ShaderSet` into the backend's descriptor set layouts (every
  binding partially bound). What the app's descriptor sets need from the pool comes in through
  `RHIInitializationData::descriptorPoolSizes`, since only the app knows its array sizes. The main layout's module,
  `shaders.slang`, is parts by area (`bindings`, `common`, `debug`, `packing`, `vertex`, `texturing`, `ibl`, `brdf`, `shading`,
  `shadows`, `lights`, `transmission`, `scatter`, `fragment`, `tonemap`, `composite`, `uiblur`, `cull`, `bvh`, `gi`,
  `tracedshadows`, `traced`, `ginear`, `emitters`, `upscale`, `aerialperspective`), which it includes textually after
  `capi.h` and `sky.h`: one translation unit, so the header's declarations exist once (each part including it as a part
  of the module, `__include` and `implementing`, declared its structs again: ambiguous). A part may use what any other
  declares, in any order. Slang's dependency list names every part, so editing one recompiles. Entry points are chosen
  by name and stage (`ShaderLoader`), not by their `[shader(...)]` attributes; one had strayed onto `TonemapAces`.
- The shaders compile in the background (the user's ask, 2026-11-07: a first launch's compile behind a progress bar, the
  ui running meanwhile, as the test assets' download does). `Scene`'s constructor makes the default resources and starts
  a task compiling its four layouts (the z prepass's, the main one, the flares' and the environment filter's:
  `ShaderCompiles`, the configurations the layouts are then loaded with) in turn into the shader cache, with one
  `ShaderLoader`. `Scene::Prepare`, which `WindowedApplication::Draw` calls before each frame outside the graphics queue's
  lock, makes the rest once they are done (the layouts, from cache hits, their bindings, the environment filter and the
  renderer: 85 ms). Until then the scene isn't ready: `Record` draws a frame graph of its own (the output cleared, then the
  output's final passes: the ui over the swapchain's image, presented), `Update`, `Resize` and `NeedsResize` do nothing,
  the loads import meanwhile and only their installs wait (`WaitUntilReady`), the ui doesn't ask for the renderer (the
  statistics, the lights window), and the file browser's preview isn't made (its scene compiles once the window's is
  ready, reading the cache). The Loading window shows "Compiling shaders" above the loads waiting: the entry points of the
  layouts done, or the time taken over the last cold compile's (`.slang.intermediate/compile-seconds`, else 30 s),
  whichever is further, at most 95% until ready, since slang reports no progress.
  Slang is used by one thread at a time: it isn't thread safe, even across global sessions, which share state. The
  compile was parallel at first (the layouts at once, and the main layout's 78 entry points in 4 groups, each a session
  and compile request of its own: the cold first load 17.5 s rather than 28.8, its resident peak 2.0 GB rather than 0.85),
  and 2 clients of the quick tier's 59 crashed in `~Session`: every global session holds the embedded SPIR-V grammar
  (`SPIRVCoreGrammarInfo::getEmbeddedVersion`, a static `RefPtr`, slang's counts not atomic), sessions made and destroyed
  on several threads counted it wrong, and a later destroy freed it (8 threads making and destroying sessions crashed
  within 1000 rounds, 3 times in 3). A lock around that alone would have left whatever else they share; so a
  `ShaderLoader` is the only one alive (`ENSUREF` in its constructor), and its users take turns by design: the ImGui
  renderer compiles in the window's constructor before the scene starts its task, and the preview's scene after the
  window's is ready. Measured on the M4 Pro (KosmicKrisp, the helmet autoloaded on a fresh profile, the whole run): 28.9
  to 29.4 s (the compile 24.9 s, a resident peak of 1.0 GB; warm 3.3 s), the window drawn and the ui running throughout,
  where the constructor's compile took 27.4 s before any frame. The first frame after the switch takes 0.46 s (its
  pipelines, cold), and the frames until the sky is installed are white: auto exposure adapting from an empty scene, as at
  a warm startup.
  The client links no slang (the user's call, 2026-11-08: an install compiles nothing, so a package needn't ship slang,
  31 MB of the macOS bundle's 79). Everything that calls slang is the shader compiler library (`shadercompiler`,
  `gfx/shadercompiler.h`/`.cpp`), a shared library built beside the client, which `ShaderLoader` loads at its first
  compile (`core::DynamicLibrary`: dlopen, LoadLibraryW; `SPEEDO_SHADERCOMPILER_FILE` names it) and calls through two C
  entry points, a compile's request and response serialized with zpp::bits (each side has its own static libc++); it uses
  core's headers (assertions, containers) but none of its compiled code, which would be a second copy of core's globals.
  gfx keeps slang's headers alone (a configuration names its enums), and a compile's cache key names slang's version from
  its port (`SPEEDO_SLANG_VERSION`) rather than asking the library. `ShaderLoader::Load` reads an install's shaders
  precompiled, where their source would be (`<stem>-<parameters hash>.shaderset`, the hash of the configuration, slang's
  version and the format: `ShaderLoader::PrecompiledPath`), else the profile's cache, else compiles through the library,
  a hard failure where it can't be loaded (an install missing a variant). `tools/shaderprecompile` writes them, every
  layout the client may load (`gfx/shaderlayouts.h`: the ImGui renderer's and the scene's, with and without ray queries:
  7 sets, the flares' and environment filter's the same either way), and `scripts/package.ps1` runs it into the build's
  `precompiled-shaders`, which the Client component installs in place of the sources. The shaders' debug information
  follows the build (`kShaderDebugInfoLevel`): all of it where profiling is on (for gpu captures), none in release, the
  package's build, whose main layout took 81 MB a variant with it and 3.8 MB without (the 7 sets 7.2 MB, compiled in
  24 s). Measured (M4 Pro, the helmet): the packaged app's cold first launch (its Metal cache emptied, a fresh profile)
  33 s with no shader compile, its first frame 1.76 s after start (the UI's pipelines cold in Metal), where it took 69 s
  with the sources compiled; a checkout's release client on a fresh profile compiles through the library, its first
  frame 0.48 s after start (the ImGui shader compiled in release, without debug information), 37 s in all. slang dumps
  its intermediates only where given a directory (the profile's `.slang.intermediate`): without one it wrote them into
  the working directory, which left 344 numbered `.spv` files in the checkout's root when the tool ran there.
  Then the compute pipelines are made ahead, before the first frame (the user's ask, 2026-11-07: the driver's compile
  shown, the window never frozen in it): `Scene::Prepare` requests the environment layout's and the renderer's
  (`Renderer::ComputePipelineVariants`: every entry point at specialization 0, and the visibility buffer's extended tier
  and the glassless Reflection Trace) through `Pipeline::RequestComputePipelines`, a task each, which copy the layout and
  the entry point's stage when asked for (the bound ones change meanwhile), on the executor; the scene is ready once
  they are made, and the Loading window shows "Creating pipelines n of m". 83 of them, 0.5 s with Metal's cache warm; a
  cold first launch took minutes of driver compiles where the draw thread made them on first use (495 s for the packaged
  app's first run, the window frozen and the hang watch's 120 s for one pipeline tripped). They run in chains, each
  pipeline after the one before it in its chain (`core::AddDependency`), as many chains as half the executor's threads
  (`TaskExecutor::GetThreadCount`): submitted all at once, a cold first launch's compiles (1 to 10 s each) took 9 of the 12
  threads for tens of seconds, two more held by loads waiting to install and one by the Rpc chain, and the draw thread's
  frames (a task chain on the executor too) had no thread: no frame for 20 s, the hang watch tripped (2026-11-08, the
  packaged app with its Metal cache emptied, sampled). The graphics pipelines a pass binds first aren't made ahead (they
  depend on its render target's formats, set as it records), nor waited for: `RequestDrawListPipelines` (`gfx/drawlist.h`)
  asks for them from the state the pass set (its default variant and its items' generic ones, a run by its first item),
  each missing one made by a task on the executor (`Pipeline::RequestGraphicsPipeline`), and the pass (the views' prepass,
  main and other lists, the shadows and their transmittance) records its draws only once none is missing, its tiles
  cleared meanwhile. The renderer counts the missing ones a frame (`Renderer::GetGraphicsPipelineProgress`), the Loading
  window shows them as "Creating pipelines n of m" once the scene is ready (`Scene::GetGraphicsPipelineProgress`), and
  what counts frames from a model's install waits for them: the autoload exit and its capture (`SPEEDO_AUTOLOAD_EXIT`)
  restart their count while any is missing, and the browser's thumbnail capture counts its 20 frames from when the
  preview's are made. Before, the generic main pass pipeline (`FragmentMain`) was made where the pass bound it, 16.5 s
  cold with the window frozen. Measured on the packaged app (M4 Pro, the helmet, its Metal cache emptied, a fresh profile,
  the hang watch at 15 s): the first launch 69 s, the compute pipelines ahead in 20.7 s and `FragmentMain` made in 19.4 s
  in the background while frames were drawn, the capture with the helmet; a second launch 5 to 6 s, the 83 pipelines from
  the profile's cache in 0.01 s. Without an executor (`FrameInputs::executor` null) a pass's pipelines are made on first
  use, as before. It doesn't stop the release builds' gpu restarts (see indirect-lighting.md): 6 of 20 runs lost the
  device with them made ahead, about half without; the mesa port's workaround does.
- The backend's type aliases (`DescriptorType<G>`, `PushConstantRange<G>`, ...) are in the *global* namespace (see
  `rhi/vulkan/types.inl`), so a neutral rhi type of the same name hides them inside `namespace rhi`, and is ambiguous
  in code with `using namespace rhi` (gfx's application). Pick another name, or qualify: `::DescriptorType<G>` in
  rhi, `rhi::DescriptorType` in gfx.

