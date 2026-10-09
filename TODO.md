# TODO
* todo: GLTF: the gaps of gfx::gltf::Import (see CLAUDE.md) and the renderer behind it
	* KTX2: mask textures (and non-Basis normal, occlusion and metallic-roughness ones) are transcoded to rgba8 and compressed again (a second lossy step); no sample model has them
	* animation: node (translation, rotation, scale) and morph weight animations and skins move, and every KHR_animation_pointer target of the core spec and the extensions drawn (node transforms, weights and visibility, material values, texture transforms, lights, cameras). a set of files with an animated base color loses it (it isn't baked into the vertex colors). joint normals use the joint matrices directly (non-uniform scale on joints skews them). the model's bounds (which frame the views) cover its animations' poses and its morph targets' reach. one animation plays at a time (switching crossfades over 0.3 s), without layering or additive blending. a set of files is drawn at rest
	* lights: punctual lights only, every light shades every pixel (no culling or clustering). shadows: no PCSS (a fixed 4x4 texel filter), fitted to the first view only (other views beyond its cascades are unshadowed), at most 4 shadowed point and spot lights, and no culling per shadow view (each draws the whole scene); the environment casts none beyond its dominant lights (its strongest light sources, up to 4, extracted as directional lights; the Khronos panoramas' are clipped, so weak). image based lighting's specular is the split sum with n = v = r, looked up along the dominant direction (no stretched reflections at grazing angles), and the backdrop is the 2048 wide panorama, unfiltered (no depth of field or blur control)
	* shading models: metallic-roughness, specular-glossiness, unlit, and every KHR_materials_* extension of the glTF-Sample-Assets (specular, ior, clearcoat, sheen, transmission, volume, dispersion, anisotropy, iridescence, diffuse_transmission, emissive_strength), as the Khronos sample viewer approximates them. transmission sees only the opaque scene (not other transmissive or blended surfaces), punctual lights don't shine through it, and a flattened mesh's volume thickness ignores its node's scale (only instance transforms scale it) obj materials map Ks and Ns to specular and roughness (their Ks color, Ka, Tf, Ni and illum are ignored)
	* alpha blending: exact per pixel linked lists, but with fixed budgets: 4 nodes per pixel on average (fragments past them are dropped, e.g. in full screen glass several layers deep), and the 16 nearest layers of a pixel blended. no adaptive growth of the node pool, and no MSAA
	* texcoords: sets above 1 fall back to set 0 (no sample model needs more: MosquitoInAmber has a TEXCOORD_2 that no material reads)
	* everything else is flattened into one Model with one draw per submesh: no per node transforms or culling at draw time
* todo: optimize shaders. investigate if we can use vulkan specialization constants to simplify the shader for a specific material set for example.
* todo: investigate and implement optimization techniques such as clustered forward shading
	* culling: none at draw time (main pass or shadow views): every view draws every submesh, which is also why at most 4 point and spot lights cast shadows
* todo: file loading leftovers (see CLAUDE.md, "File I/O"): Model::Load's source load op still lets mesh::Import map the file again (the obj and gltf importers take paths, for their mtl files and buffers); slang reads the shader sources itself (an ISlangFileSystem could hand it mapped files); core::file::Map has no read-ahead hint on windows (PrefetchVirtualMemory)
* todo: image::CompressLevel (texture compression at import) loops over block rows with std::execution::par, which libc++ runs serially: spread them over threads (as src/tools/environmentkernelsbridge.cpp does), imports would be several times faster
* todo: frame graph (gfx::FrameGraph) follow-ups
	* one queue: no async compute or transfer passes, and the passes are recorded in order into one command buffer (only the main pass's views go to secondary command buffers)
	* the OIT node pool is sized for 4 nodes per pixel of the render target (177 MB at 2560x1440) and lives the whole frame, so nothing aliases it
	* sync validation (SPEEDO_VALIDATE_SYNC=1) sees no hazards on the partially bound descriptors, so it can't check the graph's barriers
* todo: move window class from rhi into gfx or app support library. same for imguirenderer.
* todo: split some of the bulkier rhi files such as pipeline and command into separate files.
* todo: multi window/swapchain capability
* todo: (maybe) use Scatter/Gather I/O
* todo: graph based GUI. current solution (imnodes) is buggy and not currently working at all.
	* shader graph
* todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
* todo: remove all use of preprocessor macros, and replace with constexpr functions so that we can migrate to using modules.
* todo: add CI using github agent vm running on TrueNAS
* todo: vcpkg: set up binary cache using some sort of artifact store. possibly running on local TrueNAS with proper auth/exposure. Or on my Backblaze account.
* todo: enable ubsan, asan & tsan on supported platforms
* todo: add ConcurrencyGroup:s to cmake:s fastbuild integration to work around issues with lld-link eating way too much memory during LTO
* todo: area lights (e.g. rectangle and disk lights, LTC shading; glTF has none, so they need a source) and their shadows

* in progress: implement interprocess distributed task system using cppzmq &| zpp::bits
* in progress: compute pipeline
* in progress: resource loading / manager

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
