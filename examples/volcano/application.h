// wip: dynamic mesh layout, depending on input data structure.
// wip: create constructors/destructors for composite structs, and use shared_ptrs or unique_ptrs when referencing them.
// wip: specialize on graphics backend
// wip: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// wip: move stuff from headers into compilation units
// todo: simplify descriptor structs, consider passing contexts instead of handles
// todo: extract descriptor sets
// todo: resource loading / manager
// todo: graph based GUI
// todo: frame graph
// todo: compute pipeline
// todo: clustered forward shading
// todo: shader graph

// done: separate IMGUI and volcano abstractions more clearly. avoid referencing IMGUI:s windowdata
// 		 members where possible
// done: instrumentation and timing information

#pragma once

#include "command.h"
#include "device.h"
#include "gfx.h" // replace with "gfx-types.h" once all types have been encapsulated
#include "glm.h"
#include "instance.h"
#include "window.h"

#include "state.h" // temp - remove & clean up

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <array>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <functional>
#include <iostream>
#include <locale>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <nfd.h>


template <GraphicsBackend B>
class Application
{
public:

	Application(Application&& other) = default;
	Application(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath, const char* userProfilePath);
	~Application();

	void draw();

	void resizeWindow(const WindowState& state);
	void resizeFramebuffer(int width, int height);

	void onMouse(const MouseState& state);
	void onKeyboard(const KeyboardState& state);

	const char* getName() const;

private:

	void initIMGUI(CommandContext<B>& commandContext) const;

	void createFrameObjects(CommandContext<B>& commandContext);
	void destroyFrameObjects();

	// todo: encapsulate in PipelineConfiguration?
	auto createPipelineConfig(DeviceHandle<B> device,
		DescriptorPoolHandle<B> descriptorPool, PipelineCacheHandle<B> pipelineCache,
        std::shared_ptr<PipelineLayoutContext<B>> layoutContext, std::shared_ptr<GraphicsPipelineResourceView<B>> resources) const;
	void updateDescriptorSets(const Window<B>& window, const PipelineConfiguration<B>& pipelineConfig) const;
	//

	std::shared_ptr<InstanceContext<B>> myInstance;
	std::shared_ptr<DeviceContext<B>> myDevice;
	
	std::shared_ptr<Window<B>> myWindow;
	uint32_t myLastFrameIndex = 0;
	uint64_t myLastFrameTimelineValue = 0;

	// todo: figure out best way of organizing these
	PipelineCacheHandle<B> myPipelineCache = 0;
	std::shared_ptr<GraphicsPipelineResourceView<B>> myDefaultResources;
	std::shared_ptr<PipelineLayoutContext<B>> myGraphicsPipelineLayout;
	std::shared_ptr<PipelineConfiguration<B>> myGraphicsPipelineConfig;
	//

	std::filesystem::path myResourcePath;
	std::filesystem::path myUserProfilePath;

	InputState myInput = {};

	SemaphoreHandle<B> myTimelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> myTimelineValue;

	std::shared_ptr<CommandContext<B>> myTransferCommandContext;
	FenceHandle<B> myTransferFence = 0;
	uint64_t myLastTransferTimelineValue = 0;

	std::future<std::tuple<nfdresult_t, nfdchar_t*, std::function<void(nfdchar_t*)>>> myOpenFileFuture;
};

#include "application.inl"
