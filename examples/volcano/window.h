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

#include <optional>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct WindowDesc
{
	std::shared_ptr<DeviceContext<B>> deviceContext;
    SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
	Extent2d<B> windowExtent;
	Extent2d<B> framebufferExtent;
	bool clearEnable = true;
	ClearValue<B> clearValue = {};
	bool imguiEnable = true;
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

	Window(WindowDesc<B>&& desc, CommandContext<B>& commands);
	~Window();

	const auto& getWindowDesc() const { return myWindowDesc; }
	const auto& getSwapchainContext() const { return *mySwapchainContext; }
	const auto& getDepthTexture() const { return *myDepthTexture; }
	const auto& getViews() const { return myViews; }
	const auto& getActiveView() const { return myActiveView; }
	const auto& getViewBuffer() const { return *myViewBuffer; }
	const auto& getFrameIndex() const { return myFrameIndex; }
	const auto& getLastFrameIndex() const { return myLastFrameIndex; }
	const auto& getFrames() const { return myFrames; }
	const auto& getRenderPass() const { return myRenderPass; }

	void createFrameObjects(CommandContext<B>& commandContext);
	void destroyFrameObjects();
	
	void updateInput(const InputState& input);

    std::tuple<bool, uint32_t> flipFrame();
	void submitFrame(const PipelineConfiguration<B>& config);
	void presentFrame() const;

private:

	void drawIMGUI();
	void updateViewBuffer() const;

	WindowDesc<B> myWindowDesc = {};
	std::unique_ptr<SwapchainContext<B>> mySwapchainContext;
	std::unique_ptr<Texture<B>> myDepthTexture;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
	uint32_t myFrameIndex = 0; // Current frame being rendered to (0 <= myFrameIndex < myFrames.size())
	uint32_t myLastFrameIndex = 0; // Last frame being rendered to (0 <= myLastFrameIndex < myFrames.size())
	std::vector<Frame<B>> myFrames;
	RenderPassHandle<B> myRenderPass = 0;

	static constexpr uint32_t NX = 4;
	static constexpr uint32_t NY = 4;
};
