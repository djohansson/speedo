#pragma once

#include "device.h"
#include "frame.h"
#include "rendertarget.h"
#include "types.h"
#include "queue.h"

#include <memory>

template <GraphicsBackend B>
struct SwapchainConfiguration
{
	Extent2d<B> extent = {};
	SurfaceFormat<B> surfaceFormat = {};
	PresentMode<B> presentMode = {};
	uint8_t imageCount = 0;
};

template <GraphicsBackend B>
class Swapchain : public IRenderTarget<B>, public DeviceObject<B>
{
public:

	constexpr Swapchain() noexcept = default;
	Swapchain(Swapchain&& other) noexcept;
	Swapchain(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		const SwapchainConfiguration<B>& config,
		SurfaceHandle<B>&& surface, // takes ownership
		SwapchainHandle<B> previous);
	~Swapchain();

	Swapchain& operator=(Swapchain&& other) noexcept;
	operator auto() const noexcept { return mySwapchain; }

	void swap(Swapchain& rhs) noexcept;
	friend void swap(Swapchain& lhs, Swapchain& rhs) noexcept { lhs.swap(rhs); }

	virtual const RenderTargetCreateDesc<B>& getRenderTargetDesc() const final;

	virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

	virtual void blit(
        CommandBufferHandle<B> cmd,
        const IRenderTarget<Vk>& srcRenderTarget,
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

	auto getSurface() const noexcept { return mySurface; }
	const auto& getFrames() const noexcept { return myFrames; }
	auto getRenderPass() { return static_cast<RenderPassHandle<B>>(myFrames[myFrameIndex]); }
	auto getFrameIndex() const noexcept { return myFrameIndex; }
	auto getLastFrameIndex() const noexcept { return myLastFrameIndex; }

	// todo: potentially remove this if the drivers will allow us to completely rely on the timeline in the future...
	std::tuple<SemaphoreHandle<B>, SemaphoreHandle<B>> getFrameSyncSemaphores() const noexcept
	{
		const auto& lastFrame = myFrames[myLastFrameIndex];
		const auto& frame = myFrames[myFrameIndex];

		return std::make_tuple(lastFrame.getNewImageAcquiredSemaphore(), frame.getRenderCompleteSemaphore());
	}
	//

	std::tuple<bool, uint64_t> flip();
	QueuePresentInfo<B> preparePresent(uint64_t timelineValue);

protected:

	void internalCreateSwapchain(
		const SwapchainConfiguration<B>& config,
		SwapchainHandle<B> previous);

private:

	RenderTargetCreateDesc<B> myDesc = {};
	SurfaceHandle<B> mySurface = {};
	SwapchainHandle<B> mySwapchain = {};
	std::vector<Frame<B>> myFrames;
	uint32_t myFrameIndex = 0;
	uint32_t myLastFrameIndex = 0;
};

#include "swapchain.inl"
