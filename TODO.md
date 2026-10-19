# TODO
* plan (in this order): culling follow-ups, measured as before (the frame's gpu time, the `Frame` scope, and the cull statistics; SPEEDO_CAMERA, SPEEDO_CAMERA_TURN and SPEEDO_CAMERA_WALK for repeatable views)
	1. per instance culling: one draw's instances share its bounds now, so an instanced submesh is culled whole (bounds per instance, and the cull kernel writing the instances it keeps)
	2. the depth pyramid in one dispatch (shared memory, as AMD's single pass downsampler) instead of a dispatch per level with barriers between
	3. the occlusion modes and the gpu culling measured again on a desktop gpu (Windows or Linux): there the second render pass is cheap and the reprojection's atomics may not be, and the count is native
	4. revisit KosmicKrisp's DrawIndexedIndirectCount once mesa's kk_cmd_draw.c builds indirect command buffers on the gpu (its TODO, still there in main 44fd0fde, 2026-10-19): then drop DeviceLimits::drawIndirectCountEmulated's exception
	afterwards, small things when there's time: the file browser's leftovers (below), CMAKE_SUPPRESS_REGENERATION
* todo: indirect lighting (diffuse first, glossy later), fully dynamic every frame, noise free without temporal accumulation (no history, no reprojection of lighting, no denoiser: what temporal methods show as ghosting, lag and shimmer), tracing in compute on every platform and with hardware ray queries where the device has them
	* the method: world space radiance cascades (Sannikov, "Radiance Cascades", 2023; Path of Exile 2 ships the 2D screen space form). probes in cascades around the camera, each cascade 2x (or 4x) the probe spacing of the one before, its rays 4x more directions and covering the next interval of distance (near: few directions, far: many), merged far to near each frame into the irradiance at the nearest probes, which the main pass interpolates (with a normal offset, and the probes' visibility against leaks). noise free by construction: a fixed set of rays, no random sampling to converge. its cost doesn't grow with the light count, and emissive surfaces (CornellBox's lamp) and the sky light it like any other light. the 3D world space form is young (memory: cascades x probes x directions; cascade seams, ringing), so the plan starts with what checks it
	* what the rays hit: a radiance volume, not the triangles: a clipmap of voxels around the camera (albedo, normal, emission, occupancy), lit each frame by what the main pass lights with (the clustered lights, the shadow atlas, the environment's dominant lights and sky), and traced by a hierarchical march over the occupancy's mips. the static models' voxels are made once per model and cached as the assets are (keyed by the model's file and the importers' versions, loaded in the load tasks), placed by the instances; what moves (skinned, morphed, animated) is voxelized on the gpu each frame. with hardware ray queries, the rays hit the triangles instead and read the radiance volume at the hit (sharper contacts, less leaking), the same cascades either way
	* bounces: one bounce strictly per frame, or more by lighting the voxels with the last frame's irradiance too (a feedback of the light itself, not an accumulation of samples: no noise or ghosting, but light that moves takes a frame per bounce to settle). to be decided by what it looks like
	* steps, each measured (the frame's gpu time) and compared against a reference (a path traced ground truth of the hand made test scenes, CornellBox among them, offline, many samples):
		1. done (SPEEDO_GI=1, see CLAUDE.md "Indirect lighting"): one voxel volume over the scene, voxelized on the gpu at install (not cached yet: it takes a frame), lit by the direct light, debug views (SPEEDO_GI_DEBUG)
		2. done: the cascades traced and merged every frame, cascade 0's irradiance in the main pass's diffuse ambient term, the probes' visibility per voxel, two bounces in the frame. +0.6 to 2 ms on KosmicKrisp
		3. done: soft probe visibility (no blocks where voxels are large), the merge seeing only the probes above it can see, solid angle weights, grid traversed shadow rays, a reference in the same voxels (SPEEDO_GI_DEBUG=6): 80% of its light on Sponza's outer wall. +1.4 to 1.7 ms at 128 voxels
		next: a camera clipmap of volumes instead of one over the scene (Bistro's 128 voxels are 1.2 m wide, and 256 costs 6 ms), the merge's parallax (radiance cascades' bilinear fix: the last 20% of the light), the faint seams on CornellBox's boxes, then on by default
		4. what moves voxelized each frame (CesiumMan, the animated sample models)
		5. hardware ray queries where available (desktop Vulkan, Metal through a backend that exposes them): the same cascades, triangle hits
		6. glossy: the cascades' radiance (not only irradiance) for rough reflections, then something sharper for mirrors (screen space reflections falling back to the cascades)
	* considered and not chosen: DDGI probe volumes (updated with hysteresis: lag, and noise without it), ReSTIR GI and path tracing with denoisers (temporal reuse), Lumen like surface caches with screen probes (temporal), voxel cone tracing alone (cones leak and blur, but its voxels are the radiance volume here), lightmaps (static only), screen space GI alone (misses what is off screen or hidden)
* todo: GLTF: the gaps of gfx::gltf::Import (see CLAUDE.md) and the renderer behind it
	* KTX2: mask textures (and non-Basis normal, occlusion and metallic-roughness ones) are transcoded to rgba8 and compressed again (a second lossy step); no sample model has them
	* animation: node (translation, rotation, scale) and morph weight animations and skins move, and every KHR_animation_pointer target of the core spec and the extensions drawn (node transforms, weights and visibility, material values, texture transforms, lights, cameras). a set of files with an animated base color loses it (it isn't baked into the vertex colors). joint normals use the joint matrices directly (non-uniform scale on joints skews them). the model's bounds (which frame the views) cover its animations' poses and its morph targets' reach. one animation plays at a time (switching crossfades over 0.3 s), without layering or additive blending. a set of files is drawn at rest
	* lights: punctual lights only, binned into clusters per view (see CLAUDE.md, "Clustered lighting"). shadows: no PCSS (a fixed 4x4 texel filter), fitted to the first view only (other views beyond its cascades are unshadowed), point and spot light shadows sized by how big the light looks and budgeted over the atlas (sizes change by octaves as the camera moves; shadow views cull by their frustums, see CLAUDE.md "Culling"); the environment casts none beyond its dominant lights (its strongest light sources, up to 4, extracted as directional lights; the Khronos panoramas' are clipped, so weak). image based lighting's specular is the split sum with n = v = r, looked up along the dominant direction (no stretched reflections at grazing angles), and the backdrop is the 2048 wide panorama, unfiltered (no depth of field or blur control)
	* shading models: metallic-roughness, specular-glossiness, unlit, and the KHR_materials_* extensions of the glTF-Sample-Assets (specular, ior, clearcoat, sheen, transmission, volume, volume_scatter, dispersion, anisotropy, iridescence, diffuse_transmission, emissive_strength, retroreflection, variants), as the Khronos sample viewer approximates them. transmission sees only the opaque scene (not other transmissive or blended surfaces), and punctual lights don't shine through it. obj materials map Ks and Ns to specular and roughness (their Ks color, Ka, Tf, Ni and illum are ignored)
	* alpha blending: exact per pixel linked lists, but with fixed budgets: 4 nodes per pixel on average (fragments past them are dropped, e.g. in full screen glass several layers deep), and the 16 nearest layers of a pixel blended. no adaptive growth of the node pool, and no MSAA
	* texcoords: sets above 1 fall back to set 0 (no sample model needs more: MosquitoInAmber has a TEXCOORD_2 that no material reads)
	* everything else is flattened into one Model, drawn per submesh chunk (see CLAUDE.md, "Culling"): no per node transforms at draw time
* todo: the glTF-Sample-Assets materials not drawn yet (compared against the samples' reference screenshots)
	* what only ray or path tracing matches (TransmissionRoughnessTest, AttenuationTest and DispersionTest's screenshots are path traced, ScatteringSkull's from Substance Stager, TrafficCone's from a ray tracer): one refraction through an estimated thickness, so no internal or total internal reflection (DispersionTest's bright faces), and no transmissive surface seen through another
	* not settled: DiffuseTransmissionTeacup's cup looked more mirror-like than the reference's satin porcelain (no matching camera angle to compare)
* todo: optimize shaders further: the layer groups a material doesn't use are compiled away by a specialization constant (see CLAUDE.md, "Shader tiers"); the uber-shader's other branches (texture presence, alpha modes, the light loop) aren't
* todo: culling leftovers: nothing that moves is culled (skinned, morphed and animated draws are always drawn, and left out of the reprojected depth), nor are instanced submeshes by instance (see the plan)
* todo: file loading leftovers (see CLAUDE.md, "File I/O"): Model::Load's source load op still lets mesh::Import map the file again (the obj and gltf importers take paths, for their mtl files and buffers); slang reads the shader sources itself (an ISlangFileSystem could hand it mapped files); core::file::Map has no read-ahead hint on windows (PrefetchVirtualMemory)
* todo: frame graph (gfx::FrameGraph) follow-ups
	* one queue: no async compute or transfer passes, and the passes are recorded in order into one command buffer (only the main pass's views go to secondary command buffers)
	* the OIT node pool is sized for 4 nodes per pixel of the render target (177 MB at 2560x1440) and lives the whole frame, so nothing aliases it
	* sync validation (SPEEDO_VALIDATE_SYNC=1) sees no hazards on the partially bound descriptors, so it can't check the graph's barriers
* todo: imguirenderer: rhi::ImGuiRenderer is imgui's vulkan backend and its textures, which could live with gfx's imgui context instead (gfx would need the backend's vulkan calls, which it mustn't have, so likely a renderer api of rhi's instead)
* todo: split some of the bulkier rhi files such as pipeline and command into separate files.
* todo: (maybe) CMAKE_SUPPRESS_REGENERATION: the FASTBuild generator re-runs cmake from inside a build when CMakeLists.txt changed (its rebuild-bff step), without the preset's environment when the build wasn't started through a preset; that is the path that breaks the shared install/ tree (see CLAUDE.md), and once left a build dir without its CMakeCache.txt ("not a CMake build directory": fixed by cmake --preset). suppressing it removes that path, at the cost of running Configure by hand after CMakeLists.txt changes outside VS Code's editor (a git pull, scripted edits): until then a build uses the old file lists
* todo: the file browser: the preview can't be orbited (it frames the model, or uses its camera); long paths overflow the breadcrumb row; long names are clipped (the tooltip has them); the thumbnail cache is never pruned (a changed file or shader leaves the old entries behind)
* todo: multi window/swapchain capability
* todo: (maybe) use Scatter/Gather I/O
* todo: graph based GUI. current solution (imnodes) is buggy and not currently working at all.
	* shader graph
* todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
* todo: remove all use of preprocessor macros, and replace with constexpr functions so that we can migrate to using modules.
* todo: add CI using github agent vm running on TrueNAS
* todo: vcpkg: set up binary cache using some sort of artifact store. possibly running on local TrueNAS with proper auth/exposure. Or on my Backblaze account.
* todo: add ConcurrencyGroup:s to cmake:s fastbuild integration to work around issues with lld-link eating way too much memory during LTO
* todo: area lights (e.g. rectangle and disk lights, LTC shading; glTF has none, so they need a source) and their shadows

* in progress: implement interprocess distributed task system using cppzmq &| zpp::bits
* in progress: compute pipeline
* in progress: resource loading / manager

* done: the shadow views' occlusion culling (SPEEDO_SHADOW_OCCLUSION_CULLING=1: two phases in each light's view, one pyramid of the atlas for every tile), off by default: it hides almost nothing at the chunks' size and costs 0.5 to 1 ms (see CLAUDE.md); every list culled on the gpu everywhere (uncompacted where the count is emulated: free on KosmicKrisp); SPEEDO_CAPTURE saves a frame as a bmp for comparisons without the screen
* done: occlusion modes (SPEEDO_OCCLUSION_MODE): reprojected, one phase against the last frame's depth pushed to where it is seen now, and hybrid; two-phase stays the default (reprojected costs the same on KosmicKrisp and hides less while the camera moves, hybrid more). the reprojection never hid a visible draw, still, turning or walking. found on the way: depth stencil formats' stencil was never attached; host reads of device writes lacked an invalidate and a barrier
* done: two phase occlusion culling: the draws seen last frame, a depth pyramid of them (half size, a level per dispatch), then the rest it doesn't hide (Cull Late, Main Late); Sponza's frame 5.34 to 4.83 ms; the cull kernel's decisions in Statistics
* done: the gpu's frustum culling (the Cull pass): the draws in view compacted per run and counted, drawn with DrawIndexedIndirectCount, where the count is native; where it is emulated (KosmicKrisp: DeviceLimits::drawIndirectCountEmulated) the cpu culls, and occlusion culls the main pass uncompacted (empty commands in place)
* done: indirect draws: a run of draws with the same state one DrawIndexedIndirect of commands the cpu writes (DrawStream chunks), each draw naming its DrawRecord (material, instance, skin and morph bases) by its first instance
* done: the frame's whole gpu time (the Frame scope), where a backend doesn't time every pass, and the median frame time with SPEEDO_GPU_TIMINGS=1
* done: capacity limits fail hard (ENSUREF), and what real content hit grows: the draw records, the materials (NodePerformanceTest's 10000), the cull buffers
* done: clustered lighting at scale: unranged lights bounded by the scene, 4096 lights, ThousandsOfLights.gltf (4000 lights binned on the cpu in 80 us a frame, so no compute pass); SPEEDO_LAYOUT for split views
* done: the About window, and Statistics' submits and batches per frame
* done: the ui fades out when unused and back in over the menu bar or a window, menu or popup
* done: zip archives previewed and thumbnailed in the file browser (their models side by side), listed with the models by default; an entry without a preview clears it
* done: the README's screenshots (ChronographWatch, CarConcept, DamagedHelmet), and the README brought up to date
* done: the task executor's threads named in Tracy; RHIBase removed; Tracy's vk-query-count patch dropped
* done: a tsan preset, run by assettest.ps1: the quick tier passes. it found std::cout racing in libc++ (all printing is std::println now); the tsan runtime itself faults now and then on macOS 27, which our crash handler turned into a hang (sanitized builds leave fatal signals to the sanitizer)
* done: a sanitize preset (asan and ubsan on our own targets, compiler-rt from the host llvm, without mimalloc), run by assettest.ps1: the quick tier passes. it found the Vulkan host allocation fallback (without mimalloc) returning null
* done: every window frosted and rounded as macOS's (dialogs too), with a macOS dark palette for the controls
* done: the menu bar, menus, popups and tooltips as macOS's: translucent and rounded over their backdrop blurred (ImGuiLayer collects the panels with each frame, the UI Blur X and Y passes blur them back)
* done: borderless fullscreen on every platform
* done: exclusive fullscreen on macOS again (the default; native and borderless selectable in View > Presentation, with the swapchain's present mode): KosmicKrisp's flicker scanned out directly is gone with mesa main
* done: the main shader module in parts by area (shaders.slang includes bindings, vertex, ibl, brdf, lights, shading, ... textually)
* done: spot lights bounded by their cone (gfx::LightReach), not their range's sphere, in the clusters and the shadow budget (ManyLights 5.2 to 4.9 ms)
* done: command buffers are move-only values with one owner (rhi::CommandBuffer): begun from the pool, lent as handles, ended and handed back by EnqueueSubmit or CommandEncoder::ExecuteCommands; Commands(), OneShotCommands, the oneShot flag and CommandBufferArray are gone
* done: the views are uploaded per frame (Views::Upload, the recording frame's buffer only), so a frame culls by the camera it draws with
* done: clustered lighting: lights binned on the cpu into 16x9x24 clusters per view, each fragment looping over its cluster's (ManyLights with 255 lights: 20.7 to 5.2 ms)
* done: shadows for every point and spot light in view: the atlas in 128 texel cells, tiles sized by how big the light looks, halved by priority until they fit, placed biggest first (ManyLights.gltf: 32 shadowed lights)
* done: culling: submeshes cut into spatial chunks of 16384 triangles with their bounds (mesh::Chunk), skipped per view and per shadow view outside its frustum (gfx::Frustum); fewer redundant state changes per draw. Bistro's gpu time from the street 11.0 to 7.8 ms, framed 9.1 to 7.3. SPEEDO_CAMERA for repeatable viewpoints
* done: image::CompressLevel spreads its block rows over the task executor (std::execution::par ran serially under libc++): compressing a 2048 texture took 23 ms instead of 62 (filebench --import), the decode and mip resizes are serial still
* done: View > Lights: the lights listed and drawn over the views (markers, spot cones, point ranges, shadow frusta), SPEEDO_SHOW_LIGHTS=1
* done: volume scatter's profile scaled by each pixel's multiscatter color (texture included), with ScatterTexture.gltf to test it
* done: volume scatter's blur denoised: the sample pattern turned by a 4x4 tile, cancelled by an edge aware 4x4 mean (interleaved sampling)
* done: volume scatter by the extension's definitions: the multiscatter color as the albedo, occlusion, the profile from the mean free path, thickness from a back face depth pass, and the blur in a compute pass of its own
* done: KHR_materials_volume_scatter (ScatteringSkull): the sample renderer's screen space subsurface scattering, a Scatter pass of the light entering scattering materials, blurred over their surfaces by Burley's profile in the main pass; the volume now attenuates diffuse transmission too
* done: KHR_materials_variants: chosen at import, as a scene is (mesh::ImportOptions::variant, a cache entry of its own), from View > Variant or SPEEDO_AUTOLOAD_VARIANT
* done: KHR_materials_retroreflection (TrafficCone): the base lobe also evaluated from the view mirrored about the normal, blended in by the weight; the environment's retroreflective lookup is along the view
* done: KHR_materials_volume thickness scales with its node: a scaled node with a volume material is drawn as an instance of it instead of flattened (DragonAttenuation was deep red, DragonDispersion near black)
* done: the platform library (core ← platform ← rhi ← gfx): windows (platform::Window), the windowed application base (main thread work, exit wake-ups), the file dialogs and imgui's platform side; rhi keeps a swapchain per window instead of rhi::Window
* done: zip extraction moved to core (core::zip): inflating with libdeflate (2.6x stb_image's) straight into the mapped files, checked with libdeflate's crc-32 (130x the table it had), entries spread over the task executor's threads; timed by filebench --zip
* done: file loading through core's memory mapped files (core::file::Map, with a read-ahead hint, and Write): the zip extractor, the image, environment, obj and gltf importers and assettest, measured with src/tools/filebench
* done: a sun in the procedural sky, and every environment's strongest light sources (its dominant lights, up to 4) extracted as directional lights (matching direction, rotation, intensity and shadows), merged with the model's directional lights from the same direction (optional)
* done: IBL accuracy: the sheen lit by an environment prefiltered with its Charlie lobe, and the base and clearcoat specular looked up along the dominant direction
* done: GLTF animation leftovers: KHR_animation_pointer for cameras and lights, emissive strength and anisotropy rotation, the bounds over the animations, layers animated up from 0
* done: KTX2: normal, occlusion and metallic-roughness textures keep their own blocks (Basis transcoded to BC7/BC4)
* done: shadows: a depth atlas (4096, frame graph transient) of 4 cascades per directional light, a cube of 6 faces per point light and a view per spot light, filtered (3x3 bilinear PCF), lights by priority until the atlas is full
* done: generalize drawcall submission & move out of windowedapplication class: a frame graph (gfx::FrameGraph: barriers and layout transitions from what passes declare, culling, transients placed in shared memory by lifetime), gfx::Renderer (the frame's passes) and draw lists (gfx::DrawList)
* done: tonemapping: auto exposure (a histogram from ComputeMain, read back) and a choice of tonemappers (PBR Neutral, ACES, AgX, Reinhard, linear)
* done: separate IMGUI and client abstractions more clearly. avoid referencing IMGUI:s windowdata members where possible
* done: instrumentation and timing information
* done: glm for the math types (the importers' Vec3 and matrix helpers, and the imported colors, transforms, lights, cameras and environment), arrays only for shader layouts
* done: one path for gpu timings: rhi::GpuProfiler per queue feeds both Tracy's gpu zones (its C api, in place of TracyVkCtx) and the frame graph's pass timings
* done: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
* done: move stuff from headers into compilation units
* done: remove "gfx" and specialize
* done: extract descriptor sets
* done: port slang into vcpkg package
* done: move volcano into own github repo and rename to something else (speedo)
* done: migrate out of slang into own root and clean up folder structure
* done: replace all external deps with vcpkg packages
* done: replace cereal with glaze & zpp::bits
* done: untangle client dependencies
* done: switch from ms-gltf to cgltf
* done: clean up (concurrency-)utils.h and split it into multiple files. a lot of the things in there can likely be removed once support emerges in std (flat containers etc)
* done: ditch Fastbuild bffs and add CMakeLists.txt build method and reuse the same toolchain as vcpkg packages uses.
* done: removed glaze
* done: move launch & tasks jsons under .vscode into setup.ps1
* done: streamlined project setup process on all platforms.
* done: make RHIApplication & Window class graphics independent (if possible)
* done: bootstrapped clang & libc++ compiler toolchain used on all platforms. (windows is fragile and tricky to set up, mac and linux should work by now)

* cut: dynamic mesh layout, depending on input data structure. (use GLTF instead)
* cut: refactor GraphicsContext into separate class
* cut: USD integration. Too much bloat (boost, python etc). Use GLTF instead.
