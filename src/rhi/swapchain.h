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
	Extent2d<G> extent{1280, 720};
	SurfaceFormat<G> surfaceFormat{};
	PresentMode<G> presentMode{};
	uint8_t imageCount{};
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

	void Swap(Swapchain& rhs) noexcept;
	friend void Swap(Swapchain& lhs, Swapchain& rhs) noexcept { lhs.Swap(rhs); }

	virtual const RenderTargetCreateDesc<G>& GetRenderTargetDesc() const final;

	virtual ImageLayout<G> GetColorImageLayout(uint32_t index) const final;
	virtual ImageLayout<G> GetDepthStencilImageLayout() const final;

	virtual void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<Vk>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter = {}) final;

	virtual void ClearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const final;
	virtual void ClearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const final;

	virtual void
	ClearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) final;
	virtual void ClearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) final;

	virtual void
	TransitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) final;
	virtual void TransitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout) final;

	virtual RenderPassBeginInfo<G> Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) final;
	virtual void End(CommandBufferHandle<G> cmd) final;

	auto GetSurface() const noexcept { return mySurface; }
	
	std::tuple<bool, uint32_t, uint32_t> Flip();
	QueuePresentInfo<G> PreparePresent(const QueueHostSyncInfo<G>& hostSyncInfo);

	auto& GetFrames() noexcept { return myFrames; }
	const auto& GetFrames() const noexcept { return myFrames; }
	auto GetCurrentFrameIndex() const noexcept { return myFrameIndex; }

protected:
	void InternalCreateSwapchain(const SwapchainConfiguration<G>& config, SwapchainHandle<G> previous);

private:
	RenderTargetCreateDesc<G> myDesc{};
	SurfaceHandle<G> mySurface{};
	SwapchainHandle<G> mySwapchain{};
	std::vector<Frame<G>> myFrames;
	uint32_t myFrameIndex = 0;
};
