#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "glm.h"
#include "image.h"
#include "pipeline.h"
#include "state.h"
#include "swapchain.h"
#include "utils.h"
#include "view.h"

#include <array>
#include <chrono>
#include <optional>
#include <memory>
#include <utility>
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
struct DrawContext
{
};

template <GraphicsBackend B>
class WindowContext : public Noncopyable
{
public:

	WindowContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		WindowCreateDesc<B>&& desc);

	const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return mySwapchain; }
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

	auto& commandContext(uint32_t frameIndex, uint32_t contextIndex = 0) { return myCommands[frameIndex][contextIndex]; }
	
	void updateInput(const InputState& input);

	template <typename T>
	void addDrawCallback(T callback, const CommandContextBeginInfo<Vk>& beginInfo = {})
	{
		myDrawCallbacks.emplace_back(std::make_pair(beginInfo, callback));
	}

	void draw(const std::shared_ptr<PipelineContext<B>>& pipeline);

	struct ViewBufferData // todo: needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment. right now uses manual padding.
	{
		glm::mat4 viewProj = glm::mat4(1.0f);
		glm::mat4 pad0;
		glm::mat4 pad1;
		glm::mat4 pad2;
	};

private:

	void updateViewBuffer(uint32_t frameIndex) const;
	void createFrameObjects(Extent2d<B> frameBufferExtent);
	void destroyFrameObjects();

	std::shared_ptr<InstanceContext<B>> myInstance;
	std::shared_ptr<DeviceContext<B>> myDevice;
	WindowCreateDesc<B> myDesc = {};
	std::shared_ptr<SwapchainContext<B>> mySwapchain;
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
	std::vector<std::vector<std::shared_ptr<CommandContext<B>>>> myCommands;
	std::vector<std::pair<CommandContextBeginInfo<B>, std::function<void(CommandBufferHandle<B> cmd)>>> myDrawCallbacks;
};
