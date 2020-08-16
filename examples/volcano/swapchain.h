#pragma once

#include "device.h"
#include "frame.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
struct SwapchainCreateDesc : RenderTargetCreateDesc<B>
{
	SwapchainHandle<B> previous = 0;
};

template <GraphicsBackend B>
class SwapchainContext : public IRenderTarget<B>, public DeviceResource<B>
{
public:

	SwapchainContext(SwapchainContext&& other) = default;
	SwapchainContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		SwapchainCreateDesc<B>&& desc);
    ~SwapchainContext();

	SwapchainContext& operator=(SwapchainContext&& other) = default;

	virtual const RenderTargetCreateDesc<B>& getRenderTargetDesc() const final;

	virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

	virtual void blit(
        CommandBufferHandle<B> cmd,
        const std::shared_ptr<IRenderTarget<Vk>>& srcRenderTarget,
        const ImageSubresourceLayers<B>& srcSubresource,
        uint32_t srcIndex,
        const ImageSubresourceLayers<B>& dstSubresource,
        uint32_t dstIndex,
        Filter<B> filter = {}) final;

	virtual void clearSingleAttachment(
        CommandBufferHandle<B> cmd,
        const ClearAttachment<B>& clearAttachment) const final;
    virtual void clearAllAttachments(
        CommandBufferHandle<B> cmd,
        const ClearColorValue<B>& color = {},
        const ClearDepthStencilValue<B>& depthStencil = {}) const final;

	virtual void clearColor(CommandBufferHandle<B> cmd, const ClearColorValue<B>& color, uint32_t index) final;
    virtual void clearDepthStencil(CommandBufferHandle<B> cmd, const ClearDepthStencilValue<B>& depthStencil) final;

    virtual void transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index) final;
    virtual void transitionDepthStencil(CommandBufferHandle<B> cmd, ImageLayout<B> layout) final;

	virtual const std::optional<RenderPassBeginInfo<B>>& begin(CommandBufferHandle<B> cmd, SubpassContents<B> contents) final;
    virtual void end(CommandBufferHandle<B> cmd) final;

    const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return mySwapchain; }
	const auto& getFrames() const { return myFrames; }
	auto getFrameIndex() const { return myFrameIndex; }
	auto getLastFrameIndex() const { return myLastFrameIndex; }

	// todo: potentially remove this if the drivers will allow us to completely rely on the timeline in the future...
	std::tuple<SemaphoreHandle<B>, SemaphoreHandle<B>> getFrameSyncSemaphores() const;
	//

	std::tuple<bool, uint64_t> flip();
	void present(uint64_t timelineValue);

private:

	const SwapchainCreateDesc<B> myDesc = {};
	SwapchainHandle<B> mySwapchain = 0;
	std::vector<std::unique_ptr<Frame<B>>> myFrames;
	uint32_t myFrameIndex = 0;
	uint32_t myLastFrameIndex = 0;
};
