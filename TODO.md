# TODO

* todo: generalize drawcall submission & move out of rhiapplication class. use sorted draw call lists.
* todo: move window class from rhi into gfx or app support library. same for imguirenderer.
* todo: split some of the bulkier rhi files such as pipeline and command into separate files.
* todo: multi window/swapchain capability
* todo: GLTF: the gaps of gfx::gltf::Import (see CLAUDE.md) and the renderer behind it
	* KTX2: color textures keep their blocks (BC7 for Basis Universal), but normal, mask, occlusion and metallic-roughness ones are transcoded to rgba8 and compressed again (a second lossy step), since their channels are rearranged for the shader.
	* animation: node (translation, rotation, scale) animations and skins move. morph weight and KHR_animation_pointer animations are ignored: morph targets are applied at their default weights at import, so animating them needs the deltas on the gpu. joint normals use the joint matrices directly (non-uniform scale on joints skews them). the model's bounds (which frame the views) are the rest pose's. one animation plays at a time (switching crossfades over 0.3 s), without layering or additive blending. a set of files is drawn at rest
	* lights: punctual lights only, every light shades every pixel (no culling or clustering), no shadows. ambient light is a constant (no image based lighting), so smooth metals reflect a uniform gray
	* shading models: metallic-roughness (and unlit) only. specular-glossiness is drawn as a dielectric of its glossiness (its specular color is ignored), and the KHR_materials_* extensions (clearcoat, transmission, volume, sheen, iridescence, anisotropy, specular, ior, ...) are read past. KHR_materials_specular's factor is applied, its color isn't. obj materials map Ks and Ns to specular and roughness (their Ks color, Ka, Tf, Ni and illum are ignored)
	* alpha blending: sorted per submesh and instance (by the center of its bounds where the animation puts it, per view), not per triangle, and not order independent: the triangles within a blended submesh, and intersecting or interleaved submeshes, can come out in the wrong order
	* texcoords: sets above 1 fall back to set 0 (no sample model needs more: MosquitoInAmber has a TEXCOORD_2 that no material reads)
	* missing tangents: not generated with MikkTSpace (a library), the shader builds the frame from screen space derivatives instead, which can differ slightly from what the normal maps were baked against
	* everything else is flattened into one Model with one draw per submesh: no per node transforms or culling at draw time
* todo: tonemapping: Khronos PBR Neutral with a manual exposure is in; auto exposure and a choice of tonemappers aren't
* todo: frame graph
* todo: clustered forward shading
* todo: shader graph
* todo: (maybe) use Scatter/Gather I/O
* todo: graph based GUI. current solution (imnodes) is buggy and not currently working at all.
* todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
* todo: remove all use of preprocessor macros, and replace with constexpr functions so that we can migrate to using modules.
* todo: add CI using github agent vm running on TrueNAS
* todo: set up binary cache using some sort of artifact store. possibly running on local TrueNAS with proper auth/exposure. Or on my Backblaze account.
* todo: FIX GPU HANG
* todo: move launch & tasks jsons under .vscode into setup.ps1
* todo: enable ubsan, asan & tsan on supported platforms
* todo: add ConcurrencyGroup:s to cmake:s fastbuild integration to work around issues with lld-link eating way too much memory during LTO

* in progress: streamlined project setup process on all platforms.
* in progress: make RHIApplication & Window class graphics independent (if possible)
* in progress: implement interprocess distributed task system using cppzmq &| zpp::bits
* in progress: compute pipeline
* in progress: bootstrapped clang & libc++ compiler toolchain used on all platforms. (windows is fragile and tricky to set up, mac and linux should work by now)
* in progress: resource loading / manager

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
* done: ditch Fastbuild and add CMakeLists.txt build method and reuse the same toolchain as vcpkg packages uses.
* done: removed glaze

* cut: dynamic mesh layout, depending on input data structure. (use GLTF instead)
* cut: refactor GraphicsContext into separate class
* cut: USD integration. Too much bloat (boost, python etc). Use GLTF instead.
