# TODO

* todo: compute pipeline
* todo: generalize drawcall submission & move out of window class. use sorted draw call lists.
* todo: multi window/swapchain capability
* todo: resource loading / manager
* todo: proper GLTF support
* todo: (maybe) switch from ms-gltf to cgltf
* todo: frame graph
* todo: clustered forward shading
* todo: shader graph
* todo: implement interprocess distributed task system using cppzmq &| zpp::bits
* todo: split and clean up concurrency-utils
* todo: refactor GraphicsContext into separate class
* todo: (maybe) use Scatter/Gather I/O
* todo: graph based GUI. current solution (imnodes) is buggy and not currently working at all.
* todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
* todo: remove all use of preprocessor macros, and replace with constexpr functions so that we can migrate to using modules.

* in progress: clean up utils.h and split it into multiple files. a lot of the things in there can likely be removed once support emerges in std (flat containers etc)
* in progress: streamlined project setup process on all platforms.
* in progress: make GraphicsApplication & Window class graphics independent (if possible)

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

* cut: dynamic mesh layout, depending on input data structure. (use GLTF instead)
