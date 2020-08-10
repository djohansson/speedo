// wip: dynamic mesh layout, depending on input data structure.
// wip: specialize on graphics backend
// wip: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// wip: move stuff from headers into compilation units
// wip: graph based GUI
// todo: extract descriptor sets
// todo: resource loading / manager
// todo: frame graph
// todo: compute pipeline
// todo: clustered forward shading
// todo: shader graph
// todo: remove "gfx" and specialize

// done: separate IMGUI and volcano abstractions more clearly. avoid referencing IMGUI:s windowdata
// 		 members where possible
// done: instrumentation and timing information

#pragma once

#include "applicationcontext.h"
#include "device.h"
#include "file.h"
#include "instance.h"
#include "nodegraph.h"
#include "pipeline.h"
#include "renderimageset.h"
#include "types.h"
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
class Application
{
public:

	Application(Application&& other) = default;
	Application(
		void* view,
		int width,
		int height,
		const char* resourcePath,
		const char* userProfilePath);
	~Application();

	bool draw();

	void resizeWindow(const WindowState& state);
	void resizeFramebuffer(int width, int height);

	void onMouse(const MouseState& state);
	void onKeyboard(const KeyboardState& state);

	const char* getName() const;

	void* getGraphicsTracyContext() const { if constexpr (PROFILING_ENABLED) return myGraphicsTracyContext; return nullptr; };
    void* getComputeTracyContext() const { if constexpr (PROFILING_ENABLED) return myGraphicsTracyContext; return nullptr; }

private:

	void initIMGUI(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		CommandBufferHandle<B> commands,
		const std::filesystem::path& userProfilePath) const;
	void shutdownIMGUI();

	void createWindowDependentObjects(Extent2d<B> frameBufferExtent);

	void processTimelineCallbacks(uint64_t frameLastSubmitTimelineValue);

	std::filesystem::path myResourcePath;
	std::filesystem::path myUserProfilePath;

	AutoSaveJSONFileObject<NodeGraph> myNodeGraph;

	InputState myInput = {};

	std::shared_ptr<ApplicationContext<B>> myContext;

	std::shared_ptr<PipelineContext<B>> myGraphicsPipeline;
	std::shared_ptr<PipelineLayout<B>> myGraphicsPipelineLayout;
	std::shared_ptr<RenderImageSet<B>> myRenderImageSet;

	std::vector<std::shared_ptr<WindowContext<B>>> myWindows;
	
	std::future<std::tuple<nfdresult_t, nfdchar_t*, std::function<void(nfdchar_t*)>>> myOpenFileFuture;
	std::function<void(CommandBufferHandle<B> cmd)> myIMGUIDrawFunction;
	RenderPassHandle<B> myIMGUIRenderPass = 0;

	uint64_t myLastFrameTimelineValue = 0;
	uint64_t myLastTransferTimelineValue = 0;

	bool myRequestExit = false;

#if PROFILING_ENABLED
    void* myGraphicsTracyContext = nullptr;
    void* myComputeTracyContext = nullptr;
#endif
};

#include "application.inl"
