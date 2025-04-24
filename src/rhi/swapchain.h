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
	bool useDynamicRendering = true;
};

template <GraphicsApi G>
struct FlipResult
{
	Fence<G> acquireNextImageFence;
	Semaphore<G> acquireNextImageSemaphore;
	uint32_t lastFrameIndex = 0;
	uint32_t newFrameIndex = 0;
	bool succeess = false;
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

	[[maybe_unused]] Swapchain& operator=(Swapchain&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return mySwapchain; }

	void Swap(Swapchain& rhs) noexcept;
	friend void Swap(Swapchain& lhs, Swapchain& rhs) noexcept { lhs.Swap(rhs); }

	const RenderTargetCreateDesc<G>& GetRenderTargetDesc() const final;
	std::span<const ImageViewHandle<G>> GetAttachments() const final;
	std::span<const AttachmentDescription<G>> GetAttachmentDescs() const final;
	ImageLayout<G> GetLayout(uint32_t index) const final;
	const std::optional<PipelineRenderingCreateInfo<G>>& GetPipelineRenderingCreateInfo() const final;

	const RenderTargetBeginInfo<G>& Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) final;
	void ClearAll(
		CommandBufferHandle<G> cmd,
		std::span<const ClearValue<G>> values) const final;
	void End(CommandBufferHandle<G> cmd) final;

	void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<kVk>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter) final;

	void Copy(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex) final;

	void Clear(CommandBufferHandle<G> cmd, const ClearValue<G>& value, uint32_t index) final;

	void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout, ImageAspectFlags<G> aspectFlags, uint32_t index) final;

	void SetLoadOp(AttachmentLoadOp<G> loadOp, uint32_t index, AttachmentLoadOp<G> stencilLoadOp = {}) final;
	void SetStoreOp(AttachmentStoreOp<G> storeOp, uint32_t index, AttachmentStoreOp<G> stencilStoreOp = {}) final;

	[[nodiscard]] auto GetSurface() const noexcept { return mySurface; }
	
	[[nodiscard]] FlipResult<G> Flip();
	[[nodiscard]] QueuePresentInfo<G> PreparePresent();
	bool WaitPresent(uint64_t presentId, uint64_t timeout = UINT64_MAX) const;

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
	uint32_t myFrameIndex{};
};
