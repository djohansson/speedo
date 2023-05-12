// wip: graph based GUI
// todo: compute pipeline
// todo: generalize drawcall submission & move out of window class. use sorted draw call lists.
// todo: multi window/swapchain capability
// todo: resource loading / manager
// todo: proper GLTF support
// todo: frame graph
// todo: clustered forward shading
// todo: shader graph
// todo: migrate out of slang into own root and clean up folder structure
// todo: replace all external deps with vcpkg packages
// todo: port slang into vcpkg package
// todo: replace cereal with zpp::bits
// todo: implement interprocess distributed task system using cppzmq & zpp::bits
// todo: split and clean up concurrency-utils
// todo: (maybe) switch from ms-gltf to cgltf
// todo: (maybe) use Scatter/Gather I/O

// done: separate IMGUI and volcano abstractions more clearly. avoid referencing IMGUI:s windowdata
// 		 members where possible
// done: instrumentation and timing information
// done: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// done: move stuff from headers into compilation units
// done: remove "gfx" and specialize
// done: extract descriptor sets
// cut: dynamic mesh layout, depending on input data structure. (use GLTF instead)

#pragma once

#include "command.h"
#include "concurrency-utils.h"
#include "device.h"
#include "file.h"
#include "instance.h"
#include "nodegraph.h"
#include "pipeline.h"
#include "renderimageset.h"
#include "types.h"
#include "utils.h"
#include "window.h"

#include "state.h" // temp - remove & clean up

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <filesystem>
#include <functional>
#include <future>
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

	std::shared_ptr<Instance<B>> myInstance;
	std::shared_ptr<Device<B>> myDevice;

	TaskExecutor myExecutor{std::max(1u, std::thread::hardware_concurrency() - 1)};

	InputState myInput{};

	std::shared_ptr<Window<B>> myMainWindow;
	std::shared_ptr<Pipeline<B>> myPipeline;

	std::list<Queue<B>> myGraphicsQueues;
	std::list<Queue<B>> myComputeQueues;
	std::list<Queue<B>> myTransferQueues;

	enum CommandContextType : uint8_t
	{
		CommandContextType_GeneralPrimary,
		CommandContextType_GeneralSecondary,
		CommandContextType_GeneralTransfer,
		CommandContextType_DedicatedCompute,
		CommandContextType_DedicatedTransfer,
		CommandContextType_Count
	};

	std::array<WrapContainer<CommandPoolContext<B>>, CommandContextType_Count> myCommands;

	//std::shared_ptr<ResourceContext<B>> myResources;

	std::shared_ptr<RenderImageSet<B>> myRenderImageSet;

	std::unique_ptr<Buffer<B>> myMaterials;
	std::unique_ptr<Buffer<B>> myObjects;

	AutoSaveJSONFileObject<NodeGraph> myNodeGraph; // temp - should be stored elsewhere

	template <
		typename Key,
		typename Handle,
		typename KeyHash = HandleHash<Key, Handle>,
		typename KeyEqualTo = SharedPtrEqualTo<>>
	using HandleSet = UnorderedSet<Key, KeyHash, KeyEqualTo>;
	HandleSet<std::shared_ptr<PipelineLayout<B>>, PipelineLayoutHandle<B>> myLayouts;

	Future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>>
		myOpenFileFuture;

	std::function<void()> myIMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<B> cmd)> myIMGUIDrawFunction;

	Future<void> myPresentFuture;

	bool myRequestExit = false;
};

#include "application.inl"
