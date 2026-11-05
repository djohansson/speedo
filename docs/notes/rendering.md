# Rendering

A note of CLAUDE.md (its index names each): the frame graph, the camera, upscaling, culling, shadows, the traced effects, the visibility buffer and the lights' clusters.

## The frame graph: passes declare, the graph synchronizes

A frame is recorded by `gfx::Renderer` (`gfx/renderer.h`) as a `gfx::FrameGraph` (`gfx/framegraph.h`): passes, added
once in the order they run, each declaring what it does with which resource (`ResourceUse`: an image's layout, and the
stages and accesses), with an execute callback that records only its own commands. The passes now: Cull Clear and Cull (where the gpu culls; Depth Pyramid and Cull Late after the Prepass, and Prepass Late and Main Late, with occlusion culling; GI Clear, GI Shift, GI Voxelize, GI Keep, GI Light Static, GI Restore, GI Voxelize Moving and GI before Main with indirect lighting), OIT clear, Shadows, Shadow Transmittance (with transmissive materials; Caustics Clear and Caustics where it redraws views and the device has ray queries), Scatter Back, Scatter, Scatter Blur, Scatter Denoise, Prepass, Shadow Trace Clear, Shadow Classify, Shadow Trace, Shadow Denoise X and Y (with ray traced shadows), GI Near, GI Near X and Y and GI Near Temporal (the indirect lighting's near field), Emitter Sample, Emitter Shade, Emitter X and Y and Emitter Temporal (emissive surfaces as lights), Main Visibility Clear, Main Visibility Classify and Main Visibility (the visibility buffer), Reflection Trace, Reflection X and Y and Reflection Temporal (the traced glossy reflections), Main,
Transmission and Transmission Mips, Main Transmissive, Depth Save, Aerial Perspective (the sky's air, see "The sky"), Exposure Clear, ComputeMain (or, with temporal upscaling or a lens, Composite,
the Upscale passes, Lens (the camera's lens, see "The physical camera") and Sharpen), Exposure Readback, UI Blur X and Y, UI Uploads, UI, Present. Don't add barriers or transitions in a pass's callback for what it declared; declare the use instead.

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
`ImGuiRenderer::AddTexture(view)` (an `ImTextureID` for `ImGui::Image`, a slot of the renderer's texture array; the view
must be in `kShaderReadOnly` whenever a frame drawing it runs, as an `ImageOutput`'s is) and `RemoveTexture(id)`, both on
the ui thread: the slot is released as imgui's own textures' are, once the draw thread has drawn a ui frame published
after the removal and the gpu is done with it.

The ui's frosted panels, as macOS's: every top level window shown with a background (the main menu bar's, the dialogs
such as the file browser and Lights, menus, popups, tooltips; not child windows, which are inside theirs, nor windows
without a background, which would show the blur alone; a collapsed one by its title bar) is translucent (`WindowBg`,
`TitleBg*`, `MenuBarBg` and `PopupBg` with alpha) and rounded, over its backdrop blurred, with the controls in a macOS
dark palette: faint white buttons, fields and scrollbars, macOS's blue for what is hovered, open, selected or pressed,
rounded frames (all `ImGuiLayer`'s style). `ImGuiLayer::EndFrame` collects their windows' rectangles and corners (in
framebuffer pixels: `ImGuiPanel`) with the frame's draw data, so a panel and its blur are of the same ui frame; `Draw`
takes the frame (`TakeFrame`) before recording and hands them to the scene as `UiBlurRect`s (`FrameTarget::uiBlurRects`). `ComputeMain`
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
so that two runs have the sun where it was at the same frame whatever their loads and frames took;
`SPEEDO_ANIMATION_FRAME_SECONDS=<s>` advances the model's animation so, so that two runs have what moves where it was at
the same frame (CommercialRefrigerator's door: the upscaler's trailing, before against after and against the same frames
unupscaled).

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
across runs; single runs are noisy, one pair showed almost no difference). `SHADER_TYPES_LAYERS_GLASS` (what the traced
reflections do at glass) is the Reflection Trace pass's kernel's alone, skipped where the model has no glass,
whatever `SPEEDO_SHADER_TIERS` says. `SPEEDO_SHADER_TIERS=0` draws with the
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

## The physical camera

`gfx/physicalcamera.h` (the user's call, 2026-11-05: a camera that behaves like a real one, and lenses whose artifacts
arise from the lens; its first stage, the sensor and the exposure, then the lenses (their vignetting, aberration and
distortion, see "The lens" below), and the glare, the depth of field, the motion blur and the grain follow, each a
commit). The renderer's units: a unit of radiance is `camera::kUnitLuminance`, 49230 cd/m^2 (the sky's sun gives
`SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE` units for 128000 lux, and a white Lambertian surface under a unit of illuminance
radiates a unit over pi), and a sensor saturates at 1.2 * 2^EV100 cd/m^2 (Lagarde and de Rousiers' physically based
camera), so the exposure that shows a unit as white, the renderer's 0 stops, is EV100 `kSaturationEv100`, 15.32: sunny
sixteen's, f/16 at 1/160 s and ISO 100, the defaults (`camera::Exposure`); the stops an aperture, shutter and ISO give are
`kSaturationEv100 - EV100(N, t, S)` (`ExposureStops`), on which View > Exposure (`Settings::exposureStops`) is a
compensation. The **sensor** (`camera::Sensor`: full frame, 36x24 mm, by default; APS-C, Super 35, 1 inch, a phone's
1/2.3 inch; View > Camera > Sensor, `SPEEDO_SENSOR=full-frame|aps-c|super35|1-inch|phone`) and the **focal length**
(View > Camera > Focal Length, `SPEEDO_FOCAL_LENGTH=<mm>`) give the vertical field of view (`camera::FieldOfView`,
`Views::SetFocalLength`, `SetSensorHeight`, which keeps the focal length, so a smaller sensor narrows the view as a
lens's does): the cameras' `fovY` stays the truth, the default 75 degrees reading as 15.6 mm on full frame, and a scene
camera's yfov as its focal length; the variables are applied with `SPEEDO_CAMERA`, once the model is in (the install
frames the views, which sets the field of view back: `ui::AutoLens`), and Copy Pose writes them where they aren't the
defaults. The **exposure** (`Settings::exposureMode`, `aperture`, `shutter`, `iso`; View > Camera > Exposure;
`SPEEDO_EXPOSURE_MODE=program|aperture|shutter|manual`, `SPEEDO_APERTURE=<f-number>`, `SPEEDO_SHUTTER=<seconds or
1/n>`, `SPEEDO_ISO`; Copy Pose writes the mode where it isn't program, as `SPEEDO_AUTO_EXPOSURE=0` for manual, and what
is set in it): auto exposure asks for stops as before (see "Auto exposure" under "Asset import and testing"), and the
camera chooses what to expose with (`camera::Choose`, per view, every frame: `ChosenExposure` in `scene.cpp`, whose
stops `ViewExposure` takes): in **program** mode the aperture and the shutter along the line through sunny sixteen, a
stop to the shutter for every stop to the aperture (narrower and faster where the scene is brighter, wider and slower
where darker), each as far as its limit (`camera::ExposureLimits`: f/0.7 to f/64 until a lens narrows it, 1/8000 to
30 s, 1/30 the slowest held by hand, which the program keeps to, ISO 50 to 409600), and the ISO for the rest; in
**aperture or shutter priority** the one set and the other along the line, then the ISO; **manual** exposes with what
is set (View > Auto Exposure off; turned off, it takes what the camera chose, as a lock does, and a mode change keeps the
chosen as the set, so a priority starts from the exposure it had). Beyond the limits the image is under or over exposed,
as a real camera's: moonlight at +20 stops is f/0.7, 1/30 s and ISO 38000 with the pinhole's aperture, f/1.4 and ISO
153000 with a 50 mm f/1.4; starlight past ISO 409600 stays dark. Within them the choice reaches auto exposure's stops
exactly, so the renders are as before (Bistro's street by day +2.76 at f/9.9, 1/62 s and ISO 100; the night pose over its
square, +6.5, f/3.9 at 1/30 s, the shutter at its handheld limit and the aperture the rest; a day's shutter priority at
1/1000 s f/7.9), and manual with the defaults is the 0 stops `SPEEDO_AUTO_EXPOSURE=0` always was (ThousandsOfLights at
a fixed exposure 0.37 of 255 from before, the traced effects' noise). The client prints the
choice at exit with the stops (`auto exposure: +x EV (f/n, 1/t s, ISO s, EV100 e)`), and the View menu shows it under
Auto Exposure.

**The lens** (`gfx/lens.h`, the second stage; View > Camera > Lens, `SPEEDO_LENS=<key>`, `Scene::Settings::lens`, which
Copy Pose writes where it isn't the pinhole): a prescription (`lens::Prescription`: its glass surfaces from the front as a
patent lists them, a radius, the distance to the next, the glass after it by its index at the d line and its Abbe number,
its clear radius; one of them the aperture stop, with its blade count and whether its glass-air surfaces are coated; its
nominal focal length, which `lens::Scale` scales it to, its aperture range, its third order distortion, an anamorphic
squeeze and what it lets through on axis), and from it, by paraxial ray transfer matrices (height and angle through
refractions and translations, Lee and Eisemann 2013 after Hullin et al. 2011, `lens::Trace`: the effective focal length
is -1 over the entrance to sensor matrix's c, the back focal distance -a over it, the entrance pupil the stop seen
through the elements before it), what the camera shows (`lens::Appear`, `lens::Appearance`): the vignetting over the
field (`SHADER_TYPES_LENS_VIGNETTING_COUNT` samples from the axis to the sensor's corner: cos^4 of the field angle, and
the mechanical one, the share of the stop's disk whose rays, traced back to the entrance and through, pass every clear
radius, the cat's eye, strongest wide open and at the corner), the lateral chromatic aberration (the red and the blue
images' heights over the green's, from the chief ray at 650, 550 and 450 nm through the glasses' dispersion, `IndexAt`,
linear in the inverse square of the wavelength; a design's correctors' glasses are chosen for it by bisection,
`Achromatize`), and each ghost (`lens::Ghosts`: every pair of glass-air surfaces, the ray reflected back at the later and
forward again at the earlier, as an affine map of the entrance pupil onto the sensor per wavelength, with the two
Fresnel reflectances' product through a quarter wave of magnesium fluoride where coated, `Reflectance`; for the glare,
not drawn yet). The lenses (`lens::Lenses()`, their keys): the pinhole (`pinhole`, the default: no surfaces, the frame
as it is, at any focal length), a 50 mm f/1.8 double Gauss (`gauss-50`, Kolb's 1995 prescription), a 28 mm f/2.8 wide
angle (`wide-28`, barrel distortion of 6%), an 85 mm f/1.8 portrait (`portrait-85`), a 50 mm f/2.8 vintage Tessar
(`vintage-50`, uncoated: its ghosts 25 times a coated lens's, and 85% transmittance) and a 50 mm 2x anamorphic
(`anamorphic-50`, the Gauss squeezed). `assettest` checks each (`CheckLenses`: the focal length it scales to, a positive
back focus, the vignetting within 0 and 1, the aberration under 2%, ghosts and their brightness): the Gauss's corner
0.265 of the axis wide open at 23.4 degrees, the wide angle's 0.356 with red 1.0027 and blue 0.9953 of green, the
portrait's 0.865, the vintage's 0.608. A prime pins the views' focal length (`Views::SetLens`: the framing and the
scene cameras take its field of view, the Focal Length drag is disabled, and `SPEEDO_FOCAL_LENGTH` is ignored) and the
aperture's range (`Scene::GetExposureLimits`, which `ChosenExposure` and the Exposure menu take: a manual f/0.7 is
clamped to its widest); the anamorphic lens's image, squeezed across onto the sensor, is shown unsqueezed, the views
letterboxed the squeeze wider than the sensor (as to a scene camera's aspect ratio) with the field across the squeeze
times a spherical lens's. The scene appears the lens each frame it draws (`FrameLens` in `scene.cpp`, before the views
upload: made again when the lens, the aperture to a sixth of a stop, the sensor's height or the first view's aspect
changes, since the mechanical vignetting goes by the aperture; `LensData` in `gLensData[frame]`), and the frame is
rendered wider by a margin (`LensData::margin`, `Views::SetLens`'s: the ideal image's radius at the lens's corner under
barrel distortion, 1 - k, over the least of the red's and the blue's scales, so that nothing the lens's image reads is
outside the frame; the cameras' `fovY` is the field rendered, `GetFieldOfView` the nominal). The **Lens pass**
(`ComputeLens`, `lens.slang`, `kLensEntryPoint`; where `LensData::enabled`, `PushConstants::lens`' `LENS_ACTIVE`),
between the frame's reconstruction and the final pass: a thread per output pixel finds its place in the lens's field
(its offset from the view's center over the view's half diagonal, on the sensor: an anamorphic view's offset across
over the squeeze), the point of the ideal image the distortion maps it to (r (1 - k r^2), the third order inverted
once), reads the red and the blue there at their images' scales and the green at its own, each over the margin, within
the view's viewport, and scales the three by the vignetting at its field angle times the transmittance, into the
lensed image (`SHADER_TYPES_LENSED_TEXTURE`, 73, and its storage view, output sized), which Sharpen exposes and tonemaps
in place of the reconstruction. The chain is one whatever the upscaling: with it off and a lens, the frame is composited
at the output's size (the Composite pass, as with upscaling) and Sharpen takes the lensed image without its sharpening
(`UPSCALE_SHARPEN`, the Upscale passes' alone), where ComputeMain did everything; the pinhole's frames are as before on
both paths (the Lens pass disabled, Sharpen reading the reconstruction, ComputeMain without upscaling).

**The glare** (the third stage, 2026-11-05; View > Camera > Lens Glare, `Settings::lensGlare`, `SPEEDO_LENS_GLARE=0`
without, which Copy Pose writes; `LensData::glare`, `flares`, `ghostCount`): three passes after the Lens pass, in
`lens.slang` and `flare.slang`, in the renderer's units of radiance as the lens's image is. The **Bloom** pass veils the
lens's image over itself: a chain of `SHADER_TYPES_BLOOM_LEVEL_COUNT` (6) levels at half the output's size and down
(`myBloom`, an output sized transient with a mip chain, a storage view per level, `gBloomLevels`, and sampled whole as
`SHADER_TYPES_BLOOM_TEXTURE`), `ComputeBloomDown` from the lens's image by Jimenez's 13 taps (the weights sum to 1: the
partial Karis average, which weighs a tap down by its brightness, dropped the sun's disk, thousands of times white, from
the chain), the levels after by a 4x4 tent over the 2x2 they hold, then `ComputeBloomUp` summing each level back into
the one above by a 3x3 tent, a dispatch a level with a barrier between as the depth pyramid's; the final pass adds the
first level over the level count times the lens's share (`lens::Appearance::glare`: half a percent coated, 2% uncoated,
a veiling glare index's order; the surfaces' reflections are the ghosts). Within the pass the levels are read through
their storage views: a sampled view of the chain, written by the dispatch before, read what the memory held before the
pass on KosmicKrisp. And a transient read through `gTextures` must be declared `Sampled` by some pass, which gives its
image the usage: declared storage alone, its sampled descriptor was invalid
(`VUID-VkWriteDescriptorSet-descriptorType-00337`), and KosmicKrisp read black through it with no other sign, which
took an afternoon of tints to find; the validation layer finds it at once. The **Flare Sources** pass
(`ComputeFlareSources`, a thread per texel of the chain's third level; `gLensSourceCount`, emptied by the Flare Clear
pass, and `gLensSources`, `LensSource`, at most `SHADER_TYPES_LENS_SOURCE_COUNT`, 8) lists the frame's bright blobs: a
texel over `kFlareSourceLeastLuminance` (8 times white at the view's exposure) that is the brightest of its 5x5 (ties
by place), where it is on the sensor (the 5x5's luminance weighted centroid, mm from the view's center; an anamorphic
view's across over the squeeze), and its light at the lens (the texels' radiance times their solid angle, over the
vignetting the lens's image was darkened by there), kept where its brightest ghost or its starburst would show
(`LensData::ghostGain`, `starGain`, from the cpu). The sun's disk in view is such a blob, so is a lamp; an extended
source (a lit window) is taken as a point at its brightest. The **Flare** pass draws their ghosts and starbursts over
black into `myFlare` (`SHADER_TYPES_FLARE_TEXTURE`, output sized), added: the Flare layout's (`flare.slang`, created
beside the Main layout, its set 1 the lens, its ghosts, the sources; `CommandEncoder::Draw`, rhi's plain draw, 6
vertices an instance), a quad an instance, each source's ghosts then its starburst, the unused degenerate
(`VertexFlare`), what each fragment is worked out from the pixel's place on the sensor (`FragmentFlare`). A **ghost**
(`lens::Ghosts`, `LensGhost` in `gLensGhosts[frame]`: per channel a, b to the sensor and as, bs to the stop, the two
reflectances' product): a ray entering at the height that lands on the pixel, (p - b t) / a with t the source's angle,
passes the first surface's clear radius and, where it first crosses the stop's plane, the blades' polygon
(`FlarePolygonDistance`, antialiased over a pixel), and its irradiance is the source's times the reflectances over a
squared, read as radiance through the pupil (f^2 over pi times the pupil's radius squared: stopped down a ghost is
smaller and sharper but no dimmer per area, as a real one); the quad is the box of the entrance's and the stop's images'
overlap (the whole entrance's image, mostly cut away, cost 2.5 ms of fill at 720p for the sun's 45 ghosts; 0.41 now).
The **starburst** (`FlareStarburst`): each edge of the blades' polygon diffracts a streak either way along its normal,
the source's irradiance times the edge's length squared over 4 pi^2 times the distance squared (an edge's far field),
as wide across as the wavelength times the focal length over the edge (its first zero, a pixel or two: red wider),
drawn as a gaussian of that integral at least a pixel wide, so that the streaks are brighter per pixel stopped down (the
edges longer wide open, the streaks thinner than a pixel); an anamorphic lens's cylindrical front element adds the
streak across the lenses are known for, from an edge of its whole clear aperture, tinted blue. In the renderer's units
the sun (2.6 units of illuminance) gives streaks across the frame at f/14 and faint ones at f/1.8, as sunstars go. The
coatings tint the Gauss's ghosts purple (magnesium fluoride reflects the red and the blue ends most); the vintage lens,
uncoated, cascades hexagons over the frame against the sun, 25 times brighter. A still pose changes by 1 of 255 a frame
(the sources' centroids don't jump with the jitter). Cost on KosmicKrisp at 720p, the Flare pass: the Gauss against
the sun 0.41 ms, the vintage 1.15, Bistro's street at night 0.16 (its lamps' starbursts, no ghost bright enough); the
glare isn't drawn over a letterbox. Not yet: ghosts of sources out of the frame (the sun just outside it flares a real
lens), extended sources' shapes, the ghosts at half resolution where their fill costs.

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

A view of a model on the ground plane has no far plane (the ground reaches the horizon: `Views::SetScene`'s `horizon`;
`CameraCreateDesc::farPlane` 0, glm's perspective as its far plane goes to infinity, `Camera::UpdateProjectionMatrix`), the
near plane the framing's as before; a scene camera's too, whatever its `zfar` (`Views::InternalApplySceneCamera`:
ThousandsOfLights' camera has one of 500 m, which the views took and `SPEEDO_CAMERA` kept, so the plane's rings were cut
4 degrees below the horizon from 32 m up and the sky model's own sun-lit ground showed between the cut and the horizon, a
gray band, the user's pose `70.0000,32.0000,70.0000,17.913,-45.000`, 2026-11-05). Not infinity: the release and profile builds are `-ffast-math`, which assumes there is
none, so `std::isinf` was compiled to false and the ground past the old far plane was clipped (the sky's own ground showed
there, below its horizon). What took a far plane takes none: the cascades slice along the frustum's edges from the eye
(`shadows.cpp`'s `unbounded`), the clusters and the frustum culling already did (Gribb and Hartmann's far plane comes out
as (0, 0, 0, near)). Unprojections stay sound at the ground's tiny depths (its depth row is (0, 0, 0, near) exactly, so
the inverse's w row has the zeros it should, and w is proportional to the depth): the haze at the horizon came out as the
aerial perspective's slices have it.

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
the accumulation, `Upscale Accumulation` RGBA16F, where the point was; started over without a history, where revealed,
or where something moving covers the pixel and moves, the main depth's stencil and the pre-pass's motion over a tenth of
a pixel, `kUpscaleSweptLeastMotion`: a thin moving thing passing over a pixel takes its luma away and back, a swing, and
the history kept there held what passed, CommercialRefrigerator's door frame leaving dark pixels over the sky as it swung,
its frames near the top edge 8.9 of 255 from the same frames unupscaled, now 5.5; by the stencil alone, which marks what
may move, the door standing closed lost its frost's swings, and its glass flickered three times as much, 0.57 of 255
between frames against 0.21 at the user's near pose),
and the shading change counts only as far as the frame's luma strays past 3 of its deviations
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
A stray is a change only where the last frame's luma, where the point was, strayed the same way too, and a drift only
where both are on its side: a feature the jitter's samples seldom hit strays once in many frames, and each time the
history was dropped for that frame's samples, a blob of light for a frame. CommercialRefrigerator's frost and the
bottles' highlights seen through it (the user's pose `-0.0138,1.0920,1.3258,0.000,0.000`, balanced) flashed in 10391
pixels over 43 frames of the door held a little open, now 994 (native 446, unupscaled none), held closed 6656, now 863,
and in the animation's closed hold 13012 over 48 frames, now 1456 (between frames 0.46 of 255, now 0.28); a single
stray had also pulled the drift its way for frames after, which then passed it. A change of the light is still one, a
frame later; the filament (5 flashing pixels over 38 still frames against 6) and Bistro's moving shadows (3.94 of 255
from unupscaled against 3.94) are as they were, and the door frame's trailing over the sky 5.0 against 5.5 (its pixels
off by more than 48, though, 678000 against 602000 over 40 frames).

Glass has no motion of its own here: through thin clear glass what is seen moves with what is behind it, and only its
frost with the glass, which one motion vector a pixel can't follow both of. Giving the transmissive phase its own motion
(a second color attachment over the pre-pass's motion) made the refrigerator's door suppress the highlights seen through
it as it swung, and measured no better walking or strafing at it: its pixels keep the motion of what is behind them.

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
disabled, ComputeMain's as it was; unless a lens is chosen, whose Lens pass takes the Composite and Sharpen passes without the Upscale passes, see "The physical camera").

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
1 m in DiffuseTransmissionPlant, where the model stands on no ground plane: on one the views have no far plane, see
"Depth: reversed"); `SPEEDO_CAMERA=frame` frames the model instead, as View > Camera > Frame model.
`SPEEDO_LAYOUT=1x1|1x2|2x1|2x2` starts with that grid of views, as View > Layout.

## Shadows

The punctual lights cast shadows from one depth atlas (`gfx/shadows.h`: 4096 texels, a grid of 128 texel cells; a frame
graph transient, `SHADER_TYPES_SHADOW_ATLAS_TEXTURE`), planned on the cpu each frame (`shadows::Plan`, in
`Renderer::Record`, after the frame's fence, from every view's camera, `FrameInputs::cameras`): directional lights first (4
cascades of 1024 texels per view, 512 from three views on, a view of a 2x2 grid having a quarter of the pixels; Zhang's
practical splits over the view's frustum up to the scene's bounds, each a bounding sphere seen along the light, depth from
the scene's nearest caster, snapped to whole texels; all of a light's views' cascades finest first, by texel size, so
that the first `PlaceInShadowMap` finds covering a point, which it takes, is the finest any view has: by cascade then
view, a far view's big nearest cascade covered a close-up view's points before its own, and lost an awning's contact
shadow in Bistro; testing every cascade in the shader for the finest cost San Miguel's main pass 2.2 ms with one view),
then point lights (6 cube faces, a little wider than 90 degrees so the filter near a face's edge stays in it) and spot
lights (the outer cone plus a margin), by brightness at the nearest eye. A point or spot light whose range is outside
every view's frustum lights nothing seen and gets none. The others' tiles are sized by how big their range looks from the
view it looks biggest in (`CellsFor`: its share of the view's height; at most 512 texels a face, 1024 a spot), then budgeted: while they need more than the atlas, the lowest priority light of those with the
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
isn't drawn again, unless something that moves is in its frustum and moved (not where the model's pose is the last
frame's, `FrameInputs::modelStill`: an animation paused or frozen) (`Renderer::InternalPlanShadowCache`); the others'
tiles are cleared (`IRenderTarget::ClearRect`) and drawn, and the pass is skipped when every view is kept.
`SPEEDO_SHADOW_CACHE=0` draws them all every frame. Point and spot lights keep theirs while the camera moves (their
tiles change size by octaves), the sun's cascades while it is still (they follow the view, snapped to texels: the far
ones, of big texels, change less often). San Miguel's still pose 86.9 to 68.4 ms (its two sided foliage four times over;
more than the 9.9 ms the baseline's left out Shadows pass freed, not yet understood), Sponza's 30.35 to 29.37,
Bistro's evening the same (its sun is down); renders identical, still and along a path.
`resources/test-models/gltf/ShadowScene.gltf` has a sun, a point and a spot light over boxes on a ground plane.

Transmissive surfaces aren't casters (nor in the rays' structures), but tint the light through them: the Shadow
Transmittance pass draws each shadow view's transmissive draws again (`BuildShadowTransmittanceDrawList`, the transmissive
phase's, records and all) into its tile of a second atlas at half the size (`SHADER_TYPES_SHADOW_TRANSMITTANCE_TEXTURE`,
RGBA16F, persistent and kept with the atlas's tiles; 32 MB), no depth, rgb multiplied over the layers and alpha the
nearest's depth (`rhi::BlendMode::kMultiplyMax`): what each lets through (`FragmentShadowTransmittance`: its transmission
of the dielectric, the base color, `VolumeAttenuation` over its thickness, less what its two faces reflect at normal
incidence), a volume's faces toward the light alone (each for the whole thickness, as the view through it takes it at its
front face), a thin wall's both. `ShadowFactor` returns a color: the traced shadow or the atlas's, times what the 4x4
texels around the point's place give by a cubic B-spline, each where the point is behind its nearest layer
(`ShadowTransmittance`; the normal offset keeps a layer's own lit face in front of it), wherever the frame drew any
(`PushConstants::shadows`' `SHADER_TYPES_SHADOWS_TRANSMITTANCE`, which also has a traced light's pixels place themselves in
its views); bilinear over 2x2, the edges were steps a texel wide, as long again on a surface at a slant. AttenuationTest's
blocks (the user's pose `0.0000,0.0000,19.9960,0.000,0.000`, 2 m in front of the board) cast nothing before; now each a
blue band as deep as its thickness and attenuation make its view, overlapping bands deeper; TransmissionTest's spheres
cast their colors on the floor, the metallic row's darker; the other glass sample models change by 0.4 of 255 at most
(the shadows behind TransmissionRoughnessTest's panels, DragonAttenuation's at its base). The traced reflections' and
refractions' hits take it too (see "Traced reflections and refractions"). Not yet: the indirect lighting's voxels
(sunlight through a window into a room).

**Caustics** (View > Caustics, under Ray Traced Refractions; on where the device has ray queries, `SPEEDO_CAUSTICS=0`
without, which Copy Pose writes; `caustics.slang`): where the transmittance is drawn, the light the transmissive surfaces
refract lands where it goes rather than straight behind them. The Caustics pass traces photons from every shadow view of
a light (`ShadowData::lightViews`: a directional light's cascades, a point light's faces) of which the frame redraws one
and one's frustum holds a transmissive draw (`Renderer::myCausticViews`; the atlas's cache keeps the others', and the
passes are disabled where none is redrawn), `SHADER_TYPES_CAUSTIC_PHOTONS_PER_AXIS` squared (16) a texel of the
transmittance where a transmissive surface is (its alpha) and no finer view of the light holds the glass
(`CausticViewHolds`, at the texel's nearest layer: each point of it is traced once, by the finest), each at a point of
its part of the texel jittered by its place (`CausticJitter`: on a regular grid a sphere drew rings of moire), from the
view's near plane along the light
(`ShadowData::inverseViewProjection`) through a structure of the transmissive surfaces that don't move (the top level
structure's third instance, `BVH_RAY_QUERY_MASK_GLASS`, built once of `bvh::RayQueryGlassGeometries`, which only the
photons and the traced refractions query; with the opaque scene's in one query, `BvhTraceSceneOrGlass`, which says
which it met). At each surface (at most `SHADER_TYPES_CAUSTIC_SURFACE_COUNT`, 8; `GlassPath`, `GlassPathStep`, which the
traced refractions walk too) a photon keeps what it
lets through (the transmission of the dielectric, the base color at the point, less Schlick's Fresnel by its ior), is
refracted about the normal as interpolated (the facets drew polygons), and in a volume attenuated over its thickness as
the view through it takes it (`VolumeAttenuation` at the entry, the factor by the texture and the instance's scale: along
the path inside, DragonAttenuation's dragon passed almost nothing), reflected back in where it can't leave; a thin wall
lets it through as it was going. It lands on the first opaque surface (alpha masks cut out) if what it crossed was
transmissive, inside a volume too (a core in a shell, a mosquito in amber, the ground behind glass modelled as one
surface of a thickness, which no photon leaves: dropped there, as at first, what is inside the glass took no light,
which the traced refractions show through it), added in fixed point to the 2x2 texels of the surface's place in
each of its light's views whose tile holds it (`CausticSplat`, by the views' texel sizes squared; `gCausticPhotons`, a
uint per channel and texel of the transmittance, emptied by Caustics Clear) where the atlas has that surface nearest,
within its receiver gradient's slack. A lens throws its light sideways: by the views redrawn alone, each tracing the
glass in its tile and landing in its tile, CausticSpheres' clear sphere, across the finest cascade's edge and focusing
past the ground (the image inverted), lit the ground inside the cascade with the half of it outside, which only the
next cascade traced: a straight cut through its caustic, dark on the finer side. The side lit through the coarser
cascade's photons (a quarter as many a texel there) is grainier. A photon meeting an opaque surface
before its texel's transmissive one is the light straight to it, which the lookup gives, and is dropped, and one meeting
none (its point past the surface's edge) lands where the light straight on goes, which the lookup takes away. The resolve
(`ComputeCausticsResolve`) writes the light into the transmittance's rgb (alpha kept), in units of what the light gives an
unshadowed surface; with `SHADER_TYPES_SHADOWS_CAUSTICS` the lookup gives a texel 1 in front of its nearest layer and 0
behind, plus its rgb for what receives light (not the transmissive surfaces themselves, `gShadingTransmissive`, set by
`ShadeFragment`: a depth test of the point against the layer failed on surfaces at a slant, the normal offset putting
them texels in front of it). CausticSpheres' spheres focus the sun into a spot in their dark shadows by their bases,
the clear one's white and the amber one's orange: seen from a camera between them the clear one hides most of its own,
and shows the ground behind it upside down, its shadow at its top, and the amber one leaves its own in view;
AttenuationTest's blocks throw their light aside of their shadows, as thick
glass does; DragonAttenuation's body spreads what it lets through over the floor and the cloth behind it, its shadow
dark (the floor's light around it the same as with the tint alone, red 78.7 of 255 against 79.5). The other glass
sample models change by 0.4 of 255 at most. The glass that moves is in a structure of its own, refit each frame (see
"Hardware ray queries"): before, a model with a transmissive draw that moves took the tint for all of its glass, and
IridescentDishWithOlives' dish had none under its lifting cover. Cost with the shadow cache off (every view redrawn
every frame; RTX 4070 Ti SUPER, 2560x1440): Caustics 0.19 to 0.55 ms, Caustics Clear 0.06, the frame 0.26 to 0.53 ms
more (CausticSpheres, AttenuationTest, DragonAttenuation; each view tracing the glass in its tile, the cascades over
again, 0.28 to 0.66 and 0.35 to 0.55); with it, nothing while the views are kept. A view something moving is in is
drawn every frame, and its light's photons traced with it, while the animation plays: IridescentDishWithOlives framed
1.27 ms of Caustics a frame (its cover a dome over most of the tiles), CommercialRefrigerator 0.56. Paused or frozen
(`FrameInputs::modelStill`, the model's pose the last frame's) its views are kept: by the draws' may move alone they were
drawn every frame, the dish's frame 5.67 ms against 4.48 now, its render the same to 0.001 of 255. Not yet: dispersion,
rough glass's spread, the photons' light in the indirect lighting's voxels, and the caustics' noise where few photons
land (a texel's 16).

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
and frames are faster for the overdraw no longer shaded (Bistro's street, GI on, 25.15 to 22.9 ms). With the visibility
buffer (the default, see "The visibility buffer") the opaque triangles aren't drawn again at all: Main Visibility shades
them from what the pre-pass wrote, and Main draws only the rest.

**The traced shadows** (View > Ray Traced Shadows, on by default; `SPEEDO_TRACED_SHADOWS=0` for the atlas alone, `all`
to trace every pixel): for up to `SHADER_TYPES_SHADOW_TRACE_SLOT_COUNT` (4) lights a frame (the directional ones first,
brightest first, then the point and spot lights the atlas has views of, by their light at the nearest eye; none of zero
light), where the model has a BVH (see "Tracing the triangles"; `Animate` places the moving one for them too), a ray per
pixel of the pre-pass's depth and traced light toward a point of the light's disk, through the static BVH (alpha masks
cut out, `GiAlphaMask`) and the moving one (`ShadowOccluder`), filtered over the penumbra's width
(`tracedshadows.slang`, `gShadowTrace[frame]`: `ShadowTraceData`, the slots' lights; the images are the frame graph's
transients, a slot a channel). Main's opaque fragments take them for the traced lights (`ShadowFactor`, by
`PushConstants::prepassImages`' `PREPASS_IMAGE_SHADOW_MASK`); everything else is the atlas's.

- **Classify** (Shadow Classify, `ComputeShadowClassify`, compute entry point 18): where the atlas is sure, no ray
  (`ShadowMapClassify`): a point no shadow view holds lit, unless its way to the light passes the casters' bounds
  (`ShadowPathMeetsCasters`, `ShadowTraceData::casterMin` and `casterMax`: the ground plane past the cascades traced a ray
  a pixel, see "The ground plane"); its 3x3 filter all lit or all shadowed, and none of 24 taps up to 24 texels away says otherwise
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

## Traced reflections and refractions

View > Ray Traced Reflections and Ray Traced Refractions (on by default where the device has ray queries, the user's call,
2026-11-01; greyed out elsewhere, `Scene::HasRayQueries`; `SPEEDO_TRACED_REFLECTIONS=0` and `SPEEDO_TRACED_REFRACTIONS=0`,
which Copy Pose writes): `traced.slang`, by `PushConstants::traced` (`TRACED_*`, set where the model has its acceleration
structures: the file browser's preview hasn't, and its reflections would all be the environment's). `Shade` takes what a
traced ray meets in place of the probes' and the environment's radiance along the reflection (`environmentRadiance`, which
the split sum's terms then weigh as before), two ways. A fragment's (Main's blended surfaces, the transmissive ones, the
forward path): sharp, a ray along the reflection's dominant direction (`SharpReflections`, traced before it is shaded, see
below), all of it below a roughness of 0.15, none from 0.4 (`TracedShare`); the clearcoat's reflection so either way. The visibility buffer's (compute, the opaque
surfaces): glossy, a ray a pixel along the view reflected about a normal of the GGX lobe (`TracedGlossyDirection`:
Heitz's visible normals, `TracedVisibleNormal`, by Roberts' R2 over the frames offset per pixel by interleaved gradient
noise, `TracedRandom`; the cone widened by the lobe, so its hit's textures blur as the reflection does), all of it up to a
roughness of 0.45, none from 0.6 (`TracedGlossyShare`: rougher, the probes' glossy reflection is as good). Its sample is
a ratio (Heitz et al.'s ratio estimator, as for shadows): the ray's radiance over what the probes and the environment
predict along the ray (as `environmentRadiance` along the reflection, the environment prefiltered for the roughness and the
probes' terms, along the direction traced: below the geometry, where a ray is turned to skim it, the prediction saw the
ground), and its weight the specular's weight of this pixel's own prediction, plus a fiftieth of white at the view's
exposure in both (`kReflectionPredictionLeast`: below it the prediction is too dark to matter, and the sample is the
radiance itself, at night). Where a ray sees what is predicted (the sky) the ratio is 1 and the pixel is shaded as
untraced, its normal map's detail and all; what the ray met that isn't predicted (a surface, a shadow) comes in at the
scale the filter keeps. Filtering the radiance itself, as at first, blurred what each pixel's own reflection direction
gives over the pixels around: ChronographWatch's carbon fiber (a gold metal of roughness 0.4 under a fine weave normal map,
the user's pose `-0.3403,1.1883,-2.8639,17.242,2.387`) came out as blurred blotches without its fibers, and Bistro's
scooter and lamp posts lost the rims along their curves; the ratio's renders are the untraced ones where the rays meet
the sky (MetalRoughSpheresNoTextures 0.17 of 255 from before, ABeautifulGame 0.08; Bistro's benchmark pose 3.8 from
untraced, 4.1 before). Predicting with the environment unblurred, and misses so, so that the lobe's rays are the blur,
cast the low sun's glow over Bistro in pink (6.0 from untraced). The shading records the ray (`TracedReflectionRecord`, in
compute only: a fragment shader's storage writes turn its early depth test off): what is predicted along it
(`SHADER_TYPES_REFLECTION_RAW_TEXTURE`), the weight with the roughness (`..._WEIGHT_TEXTURE`; `kReflectionUnknown` in both
where none, which `ShadeVisibility` writes first) and its direction and the roughness (`..._RAY_TEXTURE`); its share is
left out of the shading, and four passes after Main Visibility take it. Reflection Trace (`ComputeReflectionTrace`) traces
it, and writes the ratio and the hit distance over the prediction (the distance as -2 less it where what the ray met moves,
`ReflectionPackDistance`: what moves' structure has no model instance; the prediction alone where what it met isn't
known), Reflection X and Y filter it along rows and columns (`ReflectionFilter`: a gaussian as wide
as the blur the lobe makes where the ray met something, `ReflectionBlurPixels`, the hit distance by alpha over the pixel's
width on the surface, at most 16 pixels, weighed by the surface, as the near field's, and by the roughness; moving where
any tap met what moves), and Reflection Temporal blends it into the last frame's history where the point was
(`PixelHistory`, as the near field's), clamped to the box of the frame's filtered 3x3 (a reflection moves across the
surface as the eye does, which reprojecting the surface doesn't follow: kept, it trails), by 1 over the frames it holds,
from 2 at a mirror to 24 at the roughest traced, then adds the weight times it to the main color, before Main's blended
surfaces and the transmission texture read it. The clamp goes by how far what the surface reflects slid across it since the
last frame, over the blur its lobe makes there: the eye travelling `dt`, the image of what a ray met at hit distance `t`
behind a surface at distance `d` slides by `dt t / (d + t)`, in pixels over the pixel's width there; a turn of the view
about the eye slides nothing (a reflection is the eye's place's, not its direction's). Where it slid by less than its
blur, and nothing but what moves may change what the reflections see (`TRACED_SETTLED`: the lights neither changed nor are
being followed) and no tap met what moves, the history is unclamped and holds 64 frames (`kReflectionFramesStill`); by
more, the history is taken where the reflection was seen last frame instead (virtual motion, as NVIDIA's ReBLUR:
`ReflectionVirtualHistory`, at the image of what the ray met through the view's last view projection, as deep behind the
surface as a curved mirror puts it, `1 / v = 1 / t + 2 k` with `k` the surface's curvature from the pixels beside it,
`ReflectionCurvature`; each tap only where its surface faced as this one's does and its reflection met something about as
far), unclamped as well; else it is clamped as above. Not where a normal map turns the normal from the geometry's by more
than about 6 degrees in the 3x3 around (`ReflectionPackRoughness`: the weight's alpha, 2 more): each cell of the watch's
woven strap reflects along its own direction, the image of a mirror was another cell's, and moving sideways at the
user's pose `-0.5305,-2.4926,-1.6792,46.951,3.879` it trailed in streaks along the move (its last frame 3.68 of 255 from
a still render of its pose, 8.5% of the pixels off by more than 16; clamped 2.09 and 0.9%; now 2.16 and 1.2%). Close to the watch's case (the user's pose
`-1.0900,-0.8416,-0.1727,1.857,162.935`) a step aside slid the strap's reflection across it by tens of pixels a frame:
moving sideways there, a mean second difference of 0.92 clamped, untraced 0.09, now 0.24, and the move's last frame 0.46
of 255 from a still render of its pose (1.26 clamped). As a flat mirror's image, MetalRoughSpheresNoTextures' glossy
spheres smeared their reflections as the eye walked (0.83 from a still render, 0.78 clamped and with the curvature: a
convex mirror's image of what is far is half its radius deep). The frame's filtered 3x3 is smooth, so its box held the
history to the frame's own noise:
ChronographWatch's strap (its rays meeting the sky in some weave cells and the case in others) changed between still
frames by 0.258 of 255, untraced 0.021, now 0.024; under small turns of the view (the user's pose
`-0.6502,-2.5011,-1.5277,43.502,1.044`, 0.05 degrees a frame) the mean second difference over the frames (what changes but
the image's steady motion) was 4.49, untraced 0.60, now 0.74, and the turn's last frame is 0.88 from a still render of its
pose (2.56 clamped); walking sideways 5.88, untraced 1.83, 2.48 by the slide alone, 2.37 with the virtual motion. A mirror's reflection still clamps as the eye walks:
MetalRoughSpheresNoTextures' first column walked a sphere's spacing is as far from a still render of the last pose as
before (0.78 of 255, 0.77). The whole model's moving would have kept it clamped wherever anything moves (the watch's
seconds hand). Their history is valid once they ran the frame
before at this size (`TRACED_REFLECTION_HISTORY`); the frames they ran are above `TRACED_FRAME_SHIFT` (the rays' sampling,
the history's parity).
A ray that meets nothing takes the environment prefiltered for the roughness of the surface it leaves, as `Shade` looks it
up: at the panorama's level, a glossy ray meeting the sun's disk took it twice (the sun is a light, its highlight the
lobe's), a sample thousands of times the sky's, which the filter spread into a white square on MetalRoughSpheres; and a
sample is taken as at most 8 times white at the view's exposure (`kReflectionMostWhite`: a lamp's bulb met by one ray among
its neighbors'). Only where the reflection can show: the specular's share of its light (the split sum's
terms by the occlusion, as luminance) from 0.005, all of it from 0.015 (`kTracedStrengthNone`, `kTracedStrengthFull`;
a dielectric's is about 0.04 at normal incidence). San Miguel is an obj whose materials' `Ns` make walls, floors and trees
smooth (`sqrt(2 / (Ns + 2))`, 0.14 at 96) and their `Ks` their specular faint: traced everywhere they cost 1.85 ms a frame
and changed nothing to see. Gated at 0.01 to 0.03, a faint lacquered door and pilasters in Bistro lost the dusk sky their
reflections traced, and took back the probes' light along them, the lit interior's leaking through (orange). The ray starts
off the geometry on the viewer's side by a pixel and a half, as the traced shadows' (a normal map turning it below the
geometry has it skim the surface). `TransmittedLight` takes the light traced through the surface (`TracedFragmentRays`' refraction: into
its volume along the refracted ray, or through a thin wall as the view goes, as the screen space lookup has it, then
through the transmissive surfaces that don't move as the caustics' photons go, `GlassPath`: out of the volume where it
meets its back, refracted, less its Fresnel reflection, or reflected back in where it can't leave, through or into the
glass beyond, to the first opaque surface or the environment, at most `SHADER_TYPES_CAUSTIC_SURFACE_COUNT` surfaces, past
which the screen space lookup's is kept; the opaque scene's structures hold no glass, and the ray passed through its
own volume without leaving it: a ball showed what is behind it shifted rather than upside down, and a thin wall was
refracted as a volume's face; the glass that moves is in a structure of its own: passed through, IridescentDishWithOlives'
cover, a shell, magnified the olives under it as a solid lens would, which it now shows as they are), sharp
where the transmission texture's mips wouldn't blur it much (`roughness * saturate(2 ior - 2)`), what is
behind out of the view as well; and, whatever the roughness, where the screen space lookup leaves the view (its last 5%,
where untraced glass fades to the environment along the ray) the trace stands in for the environment: rough glass, partly
the transmission texture's, faded to the sky toward the view's edges as untraced glass does. Nothing else of the traced
reflections or refractions fades toward the view's edges (with traced refractions off, ABeautifulGame's glass pieces do,
which the user took for the reflections'). It used to start where the screen space lookup has the ray leave the volume, at the
material's thickness: USDShaderBallForGltf's glass shell has a thickness of up to 8.9 around a core a tenth of a unit
across, so its rays started inside the core (its back met, shaded black) or past everything (the sky), a wavy border
between the two (the user's pose `0.0069,0.0526,0.0899,13.528,-7.460`); from the surface they meet the core through the
shell, and DragonAttenuation, with nothing inside its volume, is the same to 0.15 of 255 (what that left out, a thick
slab's sideways shift, its exit gives now). A ray that meets a single sided triangle's back (`TracedRadiance`'s `valid`: it started inside
something, or looks where the triangle isn't drawn from) is dropped, and the shading keeps what it would have taken
without it. Past the glass (a refraction's ray, or a reflection's once it crossed glass: `TracedShade`'s `fromGlass`)
such a back is shaded as its front instead: it is of what the glass is set into, whose inside isn't modelled.
ABeautifulGame's glass balls are sunk a quarter of their diameter into their pawns and brass collars, modelled by their
outsides alone: dropped, what the bottoms of the balls refract took the screen space lookup, which faded to the
environment toward the view's edges (the user's pose `-0.1076,0.0909,-0.1234,14.436,67.711`, turned right); passed, as
the rasterizer culls them, the pawns' tops showed through them in jagged holes (the user's pose
`-0.0363,0.0840,0.0848,47.406,151.416`). The user's choice, 2026-11-04, knowing its cost: MosquitoInAmber's body, some
of whose triangles face inward, comes out in lit facets (passed, it was clean). A hit is shaded in `TracedHitRadiance`: its material at its texcoords
(base color, metallic and roughness, emission; not the extensions' layers nor its normal map) at the mip level a ray cone
picks (Akenine-Moller et al.: the pixel's width and angle where the ray starts, `TracedPixelCone`, against the triangle's
texels per area; without one a reflected texture shimmers), the vertices' normal placed by its instance
(`BvhHit::modelInstance`; what moves keeps the triangle's), lit as the voxels are (the lights of its cell of the finest
clip holding it, each with a shadow ray; every light without the indirect lighting), each light by what the
transmissive surfaces let through to the hit and the caustics' light too, the shadow transmittance's where the light's
views hold it (`ShadowTransmittance`, as the main pass takes it: the shadow rays meet no glass, and the ground seen
through CausticSpheres' clear sphere was lit as if the sphere weren't there, neither its shadow nor its caustic), and by
the indirect lighting there
(the probes' diffuse, and the environment's specular as much as they see of it; where its lookup barely sees its probes,
`GiLookupTrust`, the light of near field rays of its own, `TracedNearLight`, one in the glossy reflections and four in
the sharp, `GiNearTrace` as the near field's, rather than the lookup's second try, as the near field does for the
pixels: by that try the gold strip at the foot of Sponza's curtains reflected the floor in front of them lit from the
courtyard, in orange flames; they cost Main Visibility 0.09 ms at the user's pose there, 0.10 in Sponza's corridor and
0.31 in Bistro's street, 2560x1440, RTX 4070 Ti SUPER, half of it their code, traced where the lookup sees no probe
alone; fading such hits' light out instead cost nothing, but left golden patches along the corridor's hems), plus its
emission; where the ray meets
nothing, the environment along it (the sky with its sun). Debug view Traced Reflections (`traced-reflections`): what the
reflections see where they are traced (the glossy ones filtered and over frames), magenta elsewhere. Through the glass
(framed, against the refraction that didn't leave its volume and hits lit past the glass): the 16 glass sample models
by 0.06 to 2.8 of 255, USDShaderBallForGltf most (its shell reflects all of the light near its rim, and shows its
bubbles and its core as the path traced screenshot has them), then MosquitoInAmber 1.7 (the amber's surface focuses the
sun onto the mosquito), GlassVaseFlowers 1.4 (the sky upside down in the vases' feet) and DispersionTest 1.0, the rest
under 0.75. Main Transmissive (framed, 2560x1440, RTX 4070 Ti SUPER): DragonAttenuation 0.36 to 1.15 ms,
USDShaderBallForGltf 0.71 to 1.54, GlassVaseFlowers 0.40 to 0.58, ABeautifulGame 0.13 to 0.21, CausticSpheres 0.10 to
0.18, the hits' lookups 0.10 to 0.17 of it and the rest the path (a query for glass and one for the scene at every
surface crossed, as at first, took the dragon's 1.31); Main Visibility, the glossy reflections' hits' lookups, 0.004 to
0.03 more. A reflection's ray meets the glass too, that moves or not (`TracedRadiance`, with `BvhTraceSceneOrGlass`): its
reflection of what is around it, as a hit's specular is taken (`TracedIndirect`: the environment as much as the probes
see of it; not its own traced reflections), and the light through it (`TracedPath`, as the refractions take it),
from inside its volume out of it (a reflection of what is in the glass, a mosquito in amber); the rays passed through
it, and glass was missing from every reflection (CausticSpheres' amber sphere from its clear sphere's, a glass object
from a mirror floor's). A reflection follows `kTracedReflectionGlassSurfaceCount` (2) surfaces through the glass, past
which the shading keeps what it would have taken untraced: all 8, DragonAttenuation reflecting itself and its glossy
cloth reflecting it took its Main Visibility from 0.41 ms to 0.95 and its Main Transmissive from 1.14 to 2.16, 0.65
and 1.68 with 2 (its render within 0.09 of 255 of all 8's). Where the model has no glass, the kernel tracing the visibility
buffer's reflections is specialized without it (`SHADER_TYPES_LAYERS_GLASS`, `Renderer::myVisibilityNoGlass`; then Main
Visibility's): its code alone, never run, took MetalRoughSpheresNoTextures' Main Visibility from 0.382 ms to 0.416;
specialized, it and DamagedHelmet's are as before. With glass (framed, 2560x1440, RTX 4070 Ti SUPER), Main Visibility and Main Transmissive: CausticSpheres
over a mirror floor 0.60 to 0.65 and 0.36 to 0.39 ms, ABeautifulGame 0.36 to 0.44 and 0.21 to 0.24, DragonAttenuation as
above; the 16 glass sample models change by 0.02 to 0.13 of 255 (glass reflecting glass: TransmissionTest's spheres
their neighbors, DispersionTest's blocks each other). Not yet (TODO.md):
glossy refractions (the transmissive surfaces are drawn forward, after the visibility buffer: they keep the sharp trace,
and the transmission texture's mips where rough), the emissive surfaces' light at a hit where they are lights, a hit's own
reflections, and the glossy rays at a lower resolution (their cost, below).

Each kernel traces from one call site, in a loop the compiler doesn't unroll (`[loop]`): every call is a copy of all that
shades a hit (the glass's path, the hit's lights and their shadow rays, the probes, the near field's rays), since NIR
inlines every function and KosmicKrisp gives every ray query state of its own. The visibility buffer's in the Reflection
Trace pass (its glossy ray and the clearcoat's, which the shading records: `TracedCoatRecord`, the direction, the
roughness and the weight of the difference the trace makes, which the pass adds to the main color); a fragment's in
`TracedFragmentRays` before it is shaded (the base's and the clearcoat's reflections, `SharpReflections`, and the
refraction, a ray for each color under dispersion, `TransmittedTraced`, whose light `Shade` and `TransmittedLight` take);
and the way through the glass in one loop with what it ends on shaded once after it (`TracedPath`). Traced where each was
taken, the visibility buffer's generic kernel was 31.5 MB of Metal source on KosmicKrisp and took 127 s to compile cold
(19.1 MB and 58 s without glass), and the main pass's fragment shader 22.5 MB and 30 to 37 s (a copy of `Shade` in each of
`ShadeFragment`'s two returns, and of the refraction in each of dispersion's channels); now 6.0 MB and 6 s, 6.0 MB and 9 s,
and the Reflection Trace pass's 1.9 MB and 2 s, and a client's cold start with ray queries (IridescenceLamp) takes 40 s.
Renders are as before to the runs' noise (11 scenes of traced reflections, refractions, glass and clearcoat, the visibility
buffer's and the forward path's: 0.015 of 255 at most, two runs of the same client 0.022), and frames no slower: settled
on KosmicKrisp at the benchmark poses (2560x1440, interleaved), Sponza 27.6 to 28.0 ms before and 26.3 to 26.4 after,
Bistro's evening 48.5 to 51.3 and 48.1 to 50.7, ABeautifulGame 19.1 to 20.0 either way. Not measured on NVIDIA yet.
Pipelines that take a second or more to create are printed (`pipeline: <kind> <entry point> (specialization <n>) took
<s> s to create`; `SPEEDO_LOG_PIPELINES=1` prints every one).

Measured on NVIDIA (RTX 4070 Ti SUPER; the benchmark poses at 2560x1440, balanced, interleaved runs), on against off. Sharp
alone (the first version): Bistro 6.2 and 6.4 ms against 5.9 and 6.0, San Miguel 7.4 and 7.6 against 6.8 and 7.1, Sponza the
same (4.3); the gated captures differ from the ungated within the runs' own noise. Glossy: Bistro 7.1 to 7.4 ms against 5.9,
San Miguel 8.1 to 8.6 against 6.9 to 7.2, Sponza 4.8 against 4.3: the rays in Main Visibility (Bistro's 1.36 ms against 0.66),
the three passes 0.10 to 0.17 ms each (two images read a pixel, traced or not). A still pose changes between frames about as
much as untraced (the mean change of consecutive frames, of 255: ABeautifulGame 0.081 against 0.075, Bistro's benchmark pose
0.093 against 0.080). What changes: MetalRoughSpheresNoTextures' smooth metal spheres reflect their neighbours and the grid
behind (only the sky and the ground before), sharp in the first column and blurred by the roughness to the fourth (the
probes' light alone before); DragonAttenuation's glass shows its cloth refracted and reflected sharp; Bistro's scooter,
lamp posts, railings and lacquered panels reflect the street; San Miguel's fountain the sky over the courtyard; Sponza's
floor the sky through the gap under its curtains. Clean under the validation layer (DragonAttenuation, BrainStem, Bistro,
Sponza, MetalRoughSpheresNoTextures); the forward path (`SPEEDO_VISIBILITY_BUFFER=0`, sharp, traced in the fragment shader)
renders as the visibility buffer's sharp version did (0.16 of 255 apart), and without ray queries (`SPEEDO_RAY_QUERY=0`) the
traced code compiles out (0.14 of 255 from the untraced).

## The visibility buffer

The main list's opaque triangles are shaded from what the pre-pass wrote rather than drawn a second time (on by default
since 2026-11-01, the user's call; `SPEEDO_VISIBILITY_BUFFER=0`, read when the renderer's graph is made, draws them
again over the pre-pass's depth): the pre-pass writes each pixel's draw record, and its triangle (`SV_PrimitiveID`) and
instance in one word, the instance above the record's `primitiveBits` (`WriteDrawRecords`: the instance count's bits, at
least one; a draw whose primitives don't fit the rest is a hard failure), to a third color attachment
(`Prepass Visibility`, RG32 uint, `gVisibility`; `DrawRecord::firstIndex` finds the triangle's indices; RGBA32, 16
bytes a pixel, measured the same), and the Main Visibility pass (`ComputeMainVisibility`, compute entry point 42,
`ShadeVisibility` a pixel) places the triangle's corners as the vertex stage places them (`PlaceVertex`: morphed,
skinned, then by the instance, read once for the three), interpolates them at the pixel's center by perspective correct
barycentrics (`PixelBarycentrics`: Schied and Dachsbacher's, in The Forge's form), and shades the pixel as `FragmentMain`
does (`MainColor`) into the main color as a storage image (`gMainColor`), every view's pixels in one dispatch
(`ViewIndexAt`, by the viewports alone: `ViewAt`'s whole ViewData, and the instance read per corner, made the
reconstruction 2.2 ms at Sponza rather than 1.25). What the fragment stage has, the kernel makes: the barycentrics'
screen space derivatives stand in for its implicit ones (`gPositionDx`, `gTexCoordDx`: `SampleView`'s `SampleGrad`, the
view's lod bias as a factor, and `CotangentFrame`'s, `SurfaceDerivativesAt`; `__stage_switch` picks, with `case
compute:`, since slang's `default:` conflicted with the fragment stage's capabilities), a cut out fragment returns
rather than discards (`CutOutFragment`: the pre-pass kept only what passed), the view shaded is `gShadingViewId` rather
than the push constant, and the front face is the triangle's geometric normal toward the eye, reversed for a mirroring
instance, in world space (by the winding of its corners projected, a triangle with a corner behind the eye, the floor
beside the camera, came out back facing: its normals flipped). Main then draws what is left (`renderer::VisibilityUnshaded`:
blended triangles, points and lines) over it, loading the color, and it and Main Late are disabled where nothing is left
(a render pass loading and storing the color and depth for nothing). Shading with the view and the material broadcast
per SIMD group (a `WaveReadLaneFirst` loop, so that what is read of them is uniform, as a draw's fragments have them)
changed nothing. A fragment shader reading `SV_PrimitiveID` declares the Geometry capability (slang's choice), which
needs `geometryShader`, which KosmicKrisp lacks; Vulkan takes the Tessellation capability for it too, which KosmicKrisp
has, so rhi declares that instead there (`shader::DeviceCode`).

**Tiers.** The kernel is specialized without the extended layers where none of the frame's triangles needs them
(`ComputePipelineVariant::specialization`, a compute pipeline's specialization constant 0, as a graphics pipeline's):
generic, it cost Sponza 2.4 ms more. Where some do, the tiles (`SHADER_TYPES_VISIBILITY_TILE_SIZE`, 8 pixels a side, a
thread group's) can be sorted (Main Visibility Classify, `ComputeMainVisibilityClassify`, entry point 43: the pre-pass
marks a record whose material has one, `VISIBILITY_EXTENDED`, its top bit; a tile with a pixel so marked goes to one
list of `gVisibilityTiles`, the others to the other, each counted in its list's indirect dispatch,
`CommandEncoder::DispatchIndirect`, emptied by Main Visibility Clear), and Main Visibility shades each list with the
same entry point (`PushConstants::visibilityTiles`), the specialized kernel the plain tiles and the generic kernel the
others, in one pass (as passes of their own, the graph put a barrier between them, both writing the color: about 0.13 ms
more).
Sorting costs 0.21 to 0.25 ms (the passes, whatever each pixel reads: four dependent loads or one), so it is done where
it pays: where the last count of the tiles with something drawn without an extended layer (`gVisibilityStatistics`, read
back per frame index) was an eighth of the frame or more (`kVisibilityTieredShare`), and every 16th frame to count again
(`kVisibilityRecountFrameCount`); else the generic kernel shades every tile. On a plain quad filling the view with a
small clearcoat quad in front (`VisibilityTiers.gltf`, nearer than it frames), 13.50 ms against 15.48 with the generic
kernel everywhere; SheenChair and CarConcept, mostly extended, 13.83 and 13.79 against 13.77 and 13.71 (sorted every
frame, 14.02 and 13.98). The check in the shading kernel itself, a group's barriers around it, cost 0.45 ms even in
frames that skipped it at Sponza and San Miguel (the compiler's doing), hence the pass of its own.

Measured by the draw thread's frame time (balanced, 2560x1440, GI on, interleaved): San Miguel 40.95 to 33.56 ms, Bistro
33.99 to 31.19, Sponza 18.80 to 19.57. What it saves is the second draw of the triangles (about half of San Miguel's
Main, see docs/performance-plan.md); what it costs is the reconstruction, 1.25 ms at Sponza, where the second draw is
cheap. Renders are forward's to about twice the run to run noise (still captures at the benchmark poses, and models of
every feature: skins, morph targets, mirroring and instances, alpha masks, clearcoat's generic kernel and sorted tiles,
points and lines, split screen): texture filtering differs a little along a texture's sharp edges, the barycentrics'
derivatives being exact where the quads' are finite differences. Not yet: the transmissive phase (drawn as before).

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

