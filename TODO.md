# TODO
* todo: GLTF: the gaps of gfx::gltf::Import (see CLAUDE.md) and the renderer behind it
	* KTX2: color textures keep their blocks (BC7 for Basis Universal), but normal, mask, occlusion and metallic-roughness ones are transcoded to rgba8 and compressed again (a second lossy step), since their channels are rearranged for the shader.
	* animation: node (translation, rotation, scale) and morph weight animations and skins move, and KHR_animation_pointer's node transforms and weights, KHR_node_visibility, material values and texture transforms (not cameras, lights' own values, emissive strength or the anisotropy rotation; a layer animated from 0 is drawn without its textures, and transmission animated from 0 isn't drawn). a set of files with an animated base color loses it (it isn't baked into the vertex colors). joint normals use the joint matrices directly (non-uniform scale on joints skews them). the model's bounds (which frame the views) are the rest pose's. one animation plays at a time (switching crossfades over 0.3 s), without layering or additive blending. a set of files is drawn at rest
	* lights: punctual lights only, every light shades every pixel (no culling or clustering), no shadows. image based lighting's specular is the split sum with n = v = r (no stretched reflections at grazing angles), and the backdrop is the 2048 wide panorama, unfiltered (no depth of field or blur control)
	* shading models: metallic-roughness, specular-glossiness, unlit, and every KHR_materials_* extension of the glTF-Sample-Assets (specular, ior, clearcoat, sheen, transmission, volume, dispersion, anisotropy, iridescence, diffuse_transmission, emissive_strength), as the Khronos sample viewer approximates them. the sheen's environment light is the irradiance (not a Charlie prefiltered environment). transmission sees only the opaque scene (not other transmissive or blended surfaces), punctual lights don't shine through it, and a flattened mesh's volume thickness ignores its node's scale (only instance transforms scale it) obj materials map Ks and Ns to specular and roughness (their Ks color, Ka, Tf, Ni and illum are ignored)
	* alpha blending: exact per pixel linked lists, but with fixed budgets: 4 nodes per pixel on average (fragments past them are dropped, e.g. in full screen glass several layers deep), and the 16 nearest layers of a pixel blended. no adaptive growth of the node pool, and no MSAA
	* texcoords: sets above 1 fall back to set 0 (no sample model needs more: MosquitoInAmber has a TEXCOORD_2 that no material reads)
	* everything else is flattened into one Model with one draw per submesh: no per node transforms or culling at draw time
* todo: generalize drawcall submission & move out of windowedapplication class. frame graph implementation?
* todo: implement shadow rendering: cascaded shadow maps for directional lights, cube maps for point and spot lights, PCF filtering (PCSS optional), a budget of shadow casting lights
	* directional lights
	* point lights
* todo: optimize shaders. investigate if we can use vulkan specialization constants to simplify the shader for a specific material set for example.
* todo: investigate and implement optimization techniques such as clustered forward shading
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
