// todo: compute pipeline
// todo: generalize drawcall submission & move out of window class. use sorted draw call lists.
// todo: multi window/swapchain capability
// todo: resource loading / manager
// todo: proper GLTF support
// todo: (maybe) switch from ms-gltf to cgltf
// todo: frame graph
// todo: clustered forward shading
// todo: shader graph
// todo: implement interprocess distributed task system using cppzmq &| zpp::bits
// todo: split and clean up concurrency-utils
// todo: make Application & Window class graphics independent (if possible)
// todo: refactor GraphicsContext into separate class
// todo: (maybe) use Scatter/Gather I/O
// todo: untangle client dependencies
// todo: graph based GUI. current solution (imnodes) is buggy and not currently working at all.
// todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
// todo: remove all use of preprocessor macros, and replace with constexpr functions so that we can migrate to using modules.

// in progress: clean up utils.h and split it into multiple files. a lot of the things in there can likely be removed once support emerges in std (flat containers etc)
// in progress: streamlined project setup process on all platforms.

// done: separate IMGUI and client abstractions more clearly. avoid referencing IMGUI:s windowdata members where possible
// done: instrumentation and timing information
// done: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// done: move stuff from headers into compilation units
// done: remove "gfx" and specialize
// done: extract descriptor sets
// done: port slang into vcpkg package
// done: move volcano into own github repo and rename to something else (speedo)
// done: migrate out of slang into own root and clean up folder structure
// done: replace all external deps with vcpkg packages
// done: replace cereal with glaze & zpp::bits

// cut: dynamic mesh layout, depending on input data structure. (use GLTF instead)

#pragma once

#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "renderimageset.h"
#include "types.h"
#include "window.h"

#include <core/concurrency-utils.h>
#include <core/file.h>
#include <core/nodegraph.h>
#include <core/utils.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <algorithm>
#include <filesystem>
#include <functional>
#include <future>
#include <thread>
#include <tuple>

#include <nfd.h>

template <GraphicsBackend B>
class Application
{
public:
	Application(const WindowState& window);
	~Application();

	bool tick();

	void resizeWindow(const WindowState& window);
	void resizeFramebuffer(int width, int height);

	void onMouse(const MouseState& mouse);
	void onKeyboard(const KeyboardState& keyboard);

	const char* getName() const;

	void setGraphicsBackend(GraphicsBackend backend);

private:
	Application();

	void initIMGUI(
		const WindowState& window,
		const std::shared_ptr<Device<B>>& device,
		CommandBufferHandle<B> commands,
		RenderPassHandle<B> renderPass,
		SurfaceHandle<B> surface,
		const std::filesystem::path& userProfilePath) const;
	void shutdownIMGUI();

	void createWindowDependentObjects(Extent2d<B> frameBufferExtent);

	auto& gfx() noexcept { return myGraphicsContext; }
	const auto& gfx() const noexcept { return myGraphicsContext; }

	TaskExecutor myExecutor{std::max(1u, std::thread::hardware_concurrency() - 1)};

	InputState myInput{};

	struct GraphicsContext
	{
		std::shared_ptr<Instance<B>> myInstance;
		std::shared_ptr<Device<B>> myDevice;

		std::shared_ptr<Window<B>> myMainWindow;
		std::shared_ptr<Pipeline<B>> myPipeline;

		CircularContainer<Queue<B>> myGraphicsQueues;
		CircularContainer<Queue<B>> myComputeQueues;
		CircularContainer<Queue<B>> myTransferQueues;

		enum CommandType : uint8_t
		{
			CommandType_GeneralPrimary,
			CommandType_GeneralSecondary,
			CommandType_GeneralTransfer,
			CommandType_DedicatedCompute,
			CommandType_DedicatedTransfer,
			CommandType_Count
		};

		std::array<CircularContainer<CommandPoolContext<B>>, CommandType_Count> myCommands;

		//std::shared_ptr<ResourceContext<B>> myResources;

		std::shared_ptr<RenderImageSet<B>> myRenderImageSet;

		std::unique_ptr<Buffer<B>> myMaterials;
		std::unique_ptr<Buffer<B>> myObjects;

		template <
			typename Key,
			typename Handle,
			typename KeyHash = HandleHash<Key, Handle>,
			typename KeyEqualTo = SharedPtrEqualTo<>>
		using HandleSet = UnorderedSet<Key, KeyHash, KeyEqualTo>;
		HandleSet<std::shared_ptr<PipelineLayout<B>>, PipelineLayoutHandle<B>> myLayouts;
	};

	GraphicsContext myGraphicsContext{std::make_shared<Instance<B>>()};

	Future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>> myOpenFileFuture;

	std::function<void()> myIMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<B> cmd)> myIMGUIDrawFunction;

	Future<void> myPresentFuture;

	//AutoSaveJSONFileObject<NodeGraph> myNodeGraph; // temp - should be stored elsewhere

	bool myRequestExit = false;
};

#include "application.inl"
