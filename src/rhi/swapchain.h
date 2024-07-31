#pragma once

#include "device.h"
#include "frame.h"
#include "queue.h"
#include "rendertarget.h"
#include "types.h"

#include <core/copyableatomic.h>

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
	[[nodiscard]] operator auto() const noexcept { return mySwapchain; }

	void Swap(Swapchain& rhs) noexcept;
	friend void Swap(Swapchain& lhs, Swapchain& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const RenderTargetCreateDesc<G>& GetRenderTargetDesc() const final;

	ImageLayout<G> GetColorImageLayout(uint32_t index) const final;
	ImageLayout<G> GetDepthStencilImageLayout() const final;

	RenderPassBeginInfo<G> Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents, std::span<const VkClearValue> clearValues) final;
	void End(CommandBufferHandle<G> cmd) final;

	void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<kVk>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter) final;

	void ClearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const final;
	void ClearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const final;

	void ClearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) final;
	void ClearDepthStencil(CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) final;

	void TransitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) final;
	void TransitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout) final;

	void SetColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<G> loadOp) final;
	void SetColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<G> storeOp) final;
	void SetDepthStencilAttachmentLoadOp(AttachmentLoadOp<G> loadOp) final;
	void SetDepthStencilAttachmentStoreOp(AttachmentStoreOp<G> storeOp) final;

	[[nodiscard]] auto GetSurface() const noexcept { return mySurface; }
	
	[[nodiscard]] std::tuple<bool, uint32_t, uint32_t> Flip();
	[[nodiscard]] QueuePresentInfo<G> PreparePresent(const QueueHostSyncInfo<G>& hostSyncInfo);

	[[nodiscard]] auto& GetFrames() noexcept { return myFrames; }
	[[nodiscard]] const auto& GetFrames() const noexcept { return myFrames; }
	[[nodiscard]] auto GetCurrentFrameIndex() const noexcept { return myFrameIndex; }

protected:
	void InternalCreateSwapchain(const SwapchainConfiguration<G>& config, SwapchainHandle<G> previous);

private:
	RenderTargetCreateDesc<G> myDesc{};
	SurfaceHandle<G> mySurface{};
	SwapchainHandle<G> mySwapchain{};
	std::vector<Frame<G>> myFrames;
	CopyableAtomic<uint32_t> myFrameIndex{};
};
