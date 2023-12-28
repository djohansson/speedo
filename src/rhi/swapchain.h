#pragma once

#include "device.h"
#include "frame.h"
#include "queue.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsApi G>
struct SwapchainConfiguration
{
	Extent2d<G> extent;
	SurfaceFormat<G> surfaceFormat;
	PresentMode<G> presentMode;
	uint8_t imageCount;
};

template <GraphicsApi G>
class Swapchain : public IRenderTarget<G>, public DeviceObject<G>
{
public:
	constexpr Swapchain() noexcept = default;
	Swapchain(Swapchain&& other) noexcept;
	Swapchain(
		const std::shared_ptr<Device<G>>& device,
		const SwapchainConfiguration<G>& config,
		SurfaceHandle<G>&& surface, // takes ownership
		SwapchainHandle<G> previous);
	~Swapchain();

	Swapchain& operator=(Swapchain&& other) noexcept;
	operator auto() const noexcept { return mySwapchain; }

	void swap(Swapchain& rhs) noexcept;
	friend void swap(Swapchain& lhs, Swapchain& rhs) noexcept { lhs.swap(rhs); }

	virtual const RenderTargetCreateDesc<G>& getRenderTargetDesc() const final;

	virtual ImageLayout<G> getColorImageLayout(uint32_t index) const final;
	virtual ImageLayout<G> getDepthStencilImageLayout() const final;

	virtual void blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<Vk>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter = {}) final;

	virtual void clearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const final;
	virtual void clearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const final;

	virtual void
	clearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) final;
	virtual void clearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) final;

	virtual void
	transitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) final;
	virtual void transitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout) final;

	virtual const std::optional<RenderPassBeginInfo<G>>&
	begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) final;
	virtual void end(CommandBufferHandle<G> cmd) final;

	auto getSurface() const noexcept { return mySurface; }
	auto getRenderPass() { return std::get<0>(static_cast<RenderTargetHandle<G>>(myFrames[myFrameIndex])); }
	auto getFramebuffer() { return std::get<1>(static_cast<RenderTargetHandle<G>>(myFrames[myFrameIndex])); }

	// todo: potentially remove this if the drivers will allow us to completely rely on the timeline in the future...
	std::tuple<SemaphoreHandle<G>, SemaphoreHandle<G>> getFrameSyncSemaphores() const noexcept
	{
		const auto& lastFrame = myFrames[myLastFrameIndex];
		const auto& frame = myFrames[myFrameIndex];

		return std::make_tuple(
			lastFrame.getNewImageAcquiredSemaphore(), frame.getRenderCompleteSemaphore());
	}
	//

	std::tuple<bool, uint64_t> flip();
	QueuePresentInfo<G> preparePresent(uint64_t timelineValue);

protected:
	auto internalGetFrameIndex() const noexcept { return myFrameIndex; }

	void
	internalCreateSwapchain(const SwapchainConfiguration<G>& config, SwapchainHandle<G> previous);

private:
	RenderTargetCreateDesc<G> myDesc{};
	SurfaceHandle<G> mySurface{};
	SwapchainHandle<G> mySwapchain{};
	std::vector<Frame<G>> myFrames;
	uint32_t myFrameIndex = 0;
	uint32_t myLastFrameIndex = 0;
};
