#pragma once

#include "buffer.h"
#include "device.h"
#include "frame.h"
#include "gfx.h"
#include "glm.h"
#include "state.h"
#include "swapchain.h"
#include "texture.h"
#include "view.h"

#include <chrono>
#include <optional>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct WindowCreateDesc
{
	std::shared_ptr<DeviceContext<B>> deviceContext;
	SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
	Extent2d<B> windowExtent = {};
	Extent2d<B> framebufferExtent = {};
	Extent2d<B> splitScreenGrid = {};
	bool clearEnable = true;
	ClearValue<B> clearValue = {};
	bool imguiEnable = false;
};

template <GraphicsBackend B>
class Window
{
public:

	struct ViewBufferData // todo: needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment. right now uses manual padding.
	{
		glm::mat4 viewProj = glm::mat4(1.0f);
		glm::mat4 pad0;
		glm::mat4 pad1;
		glm::mat4 pad2;
	};

	Window(WindowCreateDesc<B>&& desc, CommandContext<B>& commands);
	~Window();

	const auto& getDesc() const { return myDesc; }
	const auto& getSwapchainContext() const { return *mySwapchainContext; }
	const auto& getViews() const { return myViews; }
	const auto& getActiveView() const { return myActiveView; }
	const auto& getViewBuffer() const { return *myViewBuffer; }
	const auto& getRenderPass() const { return myRenderPass; }

	auto& depthTexture() { return *myDepthTexture; }
	auto& frames() { return myFrames; }

	void createFrameObjects(CommandContext<B>& commandContext);
	void destroyFrameObjects();
	
	void updateInput(const InputState& input, uint32_t frameIndex, uint32_t lastFrameIndex);

	template <typename T>
	void addIMGUIDrawCallback(T callback) { myIMGUIDrawCallbacks.emplace_back(callback); }

    std::tuple<bool, uint32_t> flipFrame(uint32_t lastFrameIndex) const;
	uint64_t submitFrame(
		uint32_t frameIndex,
		uint32_t lastFrameIndex,
		const PipelineConfiguration<B>& config,
		uint64_t waitTimelineValue);
	void presentFrame(uint32_t frameIndex) const;
	void waitFrame(uint32_t frameIndex) const;

private:

	void renderIMGUI();
	void updateViewBuffer(uint32_t frameIndex) const;

	WindowCreateDesc<B> myDesc = {};
	std::unique_ptr<SwapchainContext<B>> mySwapchainContext;
	std::unique_ptr<Texture<B>> myDepthTexture;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
	std::vector<std::shared_ptr<Frame<B>>> myFrames;
	std::vector<std::chrono::high_resolution_clock::time_point> myFrameTimestamps;
	RenderPassHandle<B> myRenderPass = 0;
	std::vector<std::function<void()>> myIMGUIDrawCallbacks;
};
