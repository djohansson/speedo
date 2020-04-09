// wip: create generic gfx structures and functions. long term goal is to move all functions from vk-utils.h into gfx.h.
// wip: dynamic mesh layout, depending on input data structure.
// wip: create constructors/destructors for composite structs, and use shared_ptrs or unique_ptrs when referencing them. done: Buffer, Texture, Model, DeviceContext.
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

#include "device.h"
#include "gfx.h" // replace with "gfx-types.h" once all types have been encapsulated
#include "glm.h"
#include "instance.h"
#include "utils.h"
#include "window.h"

#include "state.h" // temp - remove & clean up

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdlib>
#if defined(__WINDOWS__)
#	include <execution>
#endif
#include <filesystem>
#include <future>
#include <iostream>
#include <locale>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

#include <nfd.h>

#include <Tracy.hpp>
#include <TracyVulkan.hpp>

#include <imgui.h>
#include <examples/imgui_impl_vulkan.h>


template <GraphicsBackend B>
class Application : Noncopyable
{
public:

	Application(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath);
	~Application();

	void draw();

	void resizeWindow(const window_state& state);
	void resizeFramebuffer(int width, int height);

	void onMouse(const mouse_state& state);
	void onKeyboard(const keyboard_state& state);

private:

	// todo: encapsulate in Window
	void createState(Window<B>& window);
	void cleanupState(Window<B>& window);
	void updateInput(Window<B>& window) const;
	void updateViewBuffer(Window<B>& window) const;
	void initIMGUI(Window<B>& window, CommandBufferHandle<B> commandBuffer, float dpiScaleX, float dpiScaleY) const;
	void drawIMGUI(Window<B>& window) const;
	void checkFlipOrPresentResult(Window<B>& window, Result<B> result) const;
	void submitFrame(
		const DeviceContext<B>& deviceContext,
		const PipelineConfiguration<B>& config,
		Window<B>& window) const;
	void presentFrame(Window<B>& window) const;
	//

	// todo: encapsulate in PipelineConfiguration
	auto createPipelineConfig(DeviceHandle<B> device, RenderPassHandle<B> renderPass,
		DescriptorPoolHandle<B> descriptorPool, PipelineCacheHandle<B> pipelineCache,
        std::shared_ptr<PipelineLayoutContext<B>> layoutContext, std::shared_ptr<GraphicsPipelineResourceView<B>> resources) const;
	void updateDescriptorSets(const Window<B>& window, const PipelineConfiguration<B>& pipelineConfig) const;
	//

	std::unique_ptr<InstanceContext<B>> myInstance;
	std::unique_ptr<DeviceContext<B>> myGraphicsDevice;

	// todo: figure out best way of organizing these
	PipelineCacheHandle<B> myPipelineCache = 0;
	std::shared_ptr<GraphicsPipelineResourceView<B>> myDefaultResources;
	std::shared_ptr<PipelineLayoutContext<B>> myGraphicsPipelineLayout;
	std::shared_ptr<PipelineConfiguration<B>> myGraphicsPipelineConfig;
	//

	std::filesystem::path myResourcePath;

	std::map<int, bool> myKeysPressed;
	std::array<bool, 2> myMouseButtonsPressed;
	std::array<glm::vec2, 2> myMousePosition;

	static constexpr uint32_t NX = 2;
	static constexpr uint32_t NY = 2;
};

#include "application.inl"
