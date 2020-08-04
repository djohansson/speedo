#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "frame.h"
#include "glm.h"
#include "image.h"
#include "pipeline.h"
#include "state.h"
#include "swapchain.h"
#include "utils.h"
#include "view.h"

#include <chrono>
#include <optional>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct WindowCreateDesc
{
	Extent2d<B> windowExtent = {};
	Extent2d<B> framebufferExtent = {};
	Extent2d<B> splitScreenGrid = {};
	uint8_t maxCommandContextPerFrameCount = 4;
};

template <GraphicsBackend B>
class WindowContext : public Noncopyable
{
public:

	WindowContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		WindowCreateDesc<B>&& desc);

	const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return *mySwapchain; }
	const auto& getViews() const { return myViews; }
	const auto& getActiveView() const { return myActiveView; }
	const auto& getViewBuffer() const { return *myViewBuffer; }

	void onResizeWindow(Extent2d<B> windowExtent)
	{
		myDesc.windowExtent = windowExtent;
	}
	void onResizeFramebuffer(Extent2d<B> framebufferExtent)
	{
		myDesc.framebufferExtent = framebufferExtent;
		createFrameObjects(framebufferExtent);
	}

	auto& frames() { return myFrames; }
	auto& commandContext(uint32_t frameIndex, uint32_t contextIndex = 0) { return myCommands[frameIndex][contextIndex]; }
	
	void updateInput(const InputState& input, uint32_t frameIndex, uint32_t lastFrameIndex);

	template <typename T>
	void addIMGUIDrawCallback(T callback) { myIMGUIDrawCallbacks.emplace_back(callback); }

    std::tuple<bool, uint32_t, uint64_t> flipFrame(uint32_t lastFrameIndex) const;
	uint64_t submitFrame(
		uint32_t frameIndex,
		uint32_t lastFrameIndex,
		const std::shared_ptr<PipelineContext<B>>& pipeline,
		uint64_t waitTimelineValue);
	void presentFrame(uint32_t frameIndex) const;

	struct ViewBufferData // todo: needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment. right now uses manual padding.
	{
		glm::mat4 viewProj = glm::mat4(1.0f);
		glm::mat4 pad0;
		glm::mat4 pad1;
		glm::mat4 pad2;
	};

private:

	void renderIMGUI();
	void updateViewBuffer(uint32_t frameIndex) const;

	void createFrameObjects(Extent2d<B> frameBufferExtent);
	void destroyFrameObjects();

	std::shared_ptr<InstanceContext<B>> myInstance;
	std::shared_ptr<DeviceContext<B>> myDevice;
	WindowCreateDesc<B> myDesc = {};
	std::unique_ptr<SwapchainContext<B>> mySwapchain;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
	std::vector<std::shared_ptr<Frame<B>>> myFrames;
	std::vector<std::vector<std::shared_ptr<CommandContext<B>>>> myCommands;
	std::vector<std::chrono::high_resolution_clock::time_point> myFrameTimestamps;
	std::vector<std::function<void()>> myIMGUIDrawCallbacks;
};
