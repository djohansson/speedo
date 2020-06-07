// wip: dynamic mesh layout, depending on input data structure.
// wip: specialize on graphics backend
// wip: organize secondary command buffers into some sort of pool, and schedule them on a couple of worker threads
// wip: move stuff from headers into compilation units
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
#include "file.h"
#include "gfx.h" // replace with "gfx-types.h" once all types have been encapsulated
#include "glm.h"
#include "instance.h"
#include "nodegraph.h"
#include "rendertexture.h"
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

private:

	void initIMGUI(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		CommandBufferHandle<B> commands,
		const std::filesystem::path& userProfilePath) const;
	void shutdownIMGUI();

	void createFrameObjects(Extent2d<B> frameBufferExtent);
	void destroyFrameObjects();

	void collectGarbage(uint64_t frameLastSubmitTimelineValue);

	// todo: encapsulate in PipelineConfiguration?
	auto createPipelineConfig(DeviceHandle<B> device,
		DescriptorPoolHandle<B> descriptorPool, PipelineCacheHandle<B> pipelineCache,
        std::shared_ptr<PipelineLayoutContext<B>> layoutContext, std::shared_ptr<GraphicsPipelineResourceView<B>> resources) const;
	void updateDescriptorSets(const Window<B>& window, const PipelineConfiguration<B>& pipelineConfig) const;
	//

	std::filesystem::path myResourcePath;
	std::filesystem::path myUserProfilePath;

	ScopedFileObject<NodeGraph> myNodeGraph;

	InputState myInput = {};

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

	std::shared_ptr<CommandContext<B>> myTransferCommandContext;
	uint64_t myLastTransferTimelineValue = 0;

	std::shared_ptr<RenderTexture<B>> myRenderTexture;

	std::future<std::tuple<nfdresult_t, nfdchar_t*, std::function<void(nfdchar_t*)>>> myOpenFileFuture;

	bool myRequestExit = false;
};

#include "application.inl"
