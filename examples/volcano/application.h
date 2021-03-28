// wip: specialize on graphics backend
// wip: graph based GUI
// wip: extract descriptor sets
// todo: multi window/swapchain capability
// todo: resource loading / manager
// todo: frame graph
// todo: compute pipeline
// todo: clustered forward shading
// todo: shader graph

// done: separate IMGUI and volcano abstractions more clearly. avoid referencing IMGUI:s windowdata
// 		 members where possible
// done: instrumentation and timing information
// done: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// done: move stuff from headers into compilation units
// done: remove "gfx" and specialize
// cut: dynamic mesh layout, depending on input data structure. (use GLTF instead)

#pragma once

#include "command.h"
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
#include <future>
#include <functional>
#include <tuple>

#include <nfd.h>

template <GraphicsBackend B>
struct ApplicationConfiguration
{
	Extent2d<B> mainWindowSplitScreenGrid = { 1, 1 };
	uint8_t maxCommandContextPerFrameCount = 4;
};

template <GraphicsBackend B>
class Application
{
public:

	Application(
		void* windowHandle,
		int width,
		int height,
		const char* rootPath = nullptr,
		const char* resourcePath = nullptr,
		const char* userProfilePath = nullptr);
	~Application();

	bool draw();

	void resizeWindow(const WindowState& state);
	void resizeFramebuffer(int width, int height);

	void onMouse(const MouseState& state);
	void onKeyboard(const KeyboardState& state);

	const char* getName() const;

private:

	void initIMGUI(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		CommandBufferHandle<B> commands,
		RenderPassHandle<B> renderPass,
		const std::filesystem::path& userProfilePath) const;
	void shutdownIMGUI();

	void createWindowDependentObjects(Extent2d<B> frameBufferExtent);

	void processTimelineCallbacks(uint64_t frameLastSubmitTimelineValue);

	std::filesystem::path myRootPath;
	std::filesystem::path myResourcePath;
	std::filesystem::path myUserProfilePath;

	AutoSaveJSONFileObject<ApplicationConfiguration<B>> myConfig;
	AutoSaveJSONFileObject<NodeGraph> myNodeGraph;

	InputState myInput = {};

	std::shared_ptr<InstanceContext<B>> myInstance;
	std::shared_ptr<DeviceContext<B>> myDevice;
	std::shared_ptr<WindowContext<B>> myWindow;
	std::shared_ptr<PipelineContext<B>> myPipeline;
	//std::shared_ptr<ResourceContext<B>> myResources;

	std::vector<QueueContext<B>> myGraphicsQueues;
	std::vector<QueueContext<B>> myComputeQueues;
	std::vector<QueueContext<B>> myTransferQueues;
	
	enum class CommandContextType : uint8_t { GeneralPrimary, GeneralSecondary, DedicatedCompute, DedicatedTransfer };
	UnorderedMap<CommandContextType, std::vector<CommandPoolContext<B>>> myCommands;

	std::shared_ptr<RenderImageSet<B>> myRenderImageSet;
	
	std::unique_ptr<Buffer<B>> myMaterials;
	std::unique_ptr<Buffer<B>> myObjects;

	HandleSet<std::shared_ptr<PipelineLayout<B>>, PipelineLayoutHandle<B>> myLayouts;

	std::future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>> myOpenFileFuture;
	
	std::function<void()> myIMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<B> cmd)> myIMGUIDrawFunction;

	uint64_t myLastFrameTimelineValue = 0ull;
	std::optional<uint64_t> myLastTransferTimelineValue;

	bool myRequestExit = false;
};

#include "application.inl"
