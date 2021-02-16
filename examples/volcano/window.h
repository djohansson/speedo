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
	uint8_t splitScreenGrid[2] = {};
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
	~WindowContext();

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

	// todo: move out from window
	auto& commandContext(uint8_t frameIndex, uint8_t contextIndex = 0) { return myCommands[frameIndex][contextIndex]; }
	//
	
	void updateInput(const InputState& input);

	void draw(const std::shared_ptr<PipelineContext<B>>& pipeline);

private:

	void updateViewBuffer(uint8_t frameIndex) const;
	void createFrameObjects(Extent2d<B> frameBufferExtent);
	void destroyFrameObjects();

	uint32_t internalDrawViews(
		const std::shared_ptr<PipelineContext<Vk>>& pipeline,
		const RenderPassBeginInfo<Vk>& renderPassInfo,
		uint8_t frameIndex);

	std::shared_ptr<InstanceContext<B>> myInstance;
	std::shared_ptr<DeviceContext<B>> myDevice;
	WindowCreateDesc<B> myDesc = {};
	std::shared_ptr<Swapchain<B>> mySwapchain;
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
	std::vector<std::vector<std::shared_ptr<CommandContext<B>>>> myCommands; // todo: move out from window
};
