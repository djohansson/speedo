#pragma once

#include <rhi/deviceobject.h>
#include <rhi/frame.h>
#include <rhi/queue.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <span>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
class Swapchain;

template <GraphicsApi G>
struct FlipResult
{
	Semaphore<G> acquireNextImageSemaphore;
	uint32_t lastFrameIndex = 0;
	uint32_t newFrameIndex = 0;
	bool success = false;
};

template <GraphicsApi G>
struct SwapchainCreateDesc : RenderTargetCreateDesc<G>
{
	SurfaceHandle<G> surface{};
	SurfaceFormat<G> surfaceFormat{};
	PresentMode<G> presentMode{};
};

template <GraphicsApi G>
struct ObjectTraits<Swapchain<G>>
{
	using CreateDescType = SwapchainCreateDesc<G>;
};

template <GraphicsApi G>
class Swapchain : public IRenderTarget<G>, public DeviceObject<Swapchain<G>>
{
public:
	using SuperType = DeviceObject<Swapchain<G>>;
	using CreateDescType = ObjectTraits<Swapchain<G>>::CreateDescType;

	constexpr Swapchain() noexcept = default;
	Swapchain(Swapchain&& other) noexcept;
	explicit Swapchain(CreateDescType&& desc);
	~Swapchain();

	[[maybe_unused]] Swapchain& operator=(Swapchain&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return mySwapchain; }//NOLINT(google-explicit-constructor)

	void Swap(Swapchain& rhs) noexcept;
	friend void Swap(Swapchain& lhs, Swapchain& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] RenderTargetPassHandle<G> GetHandle() final;
	[[nodiscard]] Extent2d GetExtent() const final;
	[[nodiscard]] std::span<const ImageHandle<G>> GetImages() const final;
	[[nodiscard]] std::span<const ImageViewHandle<G>> GetAttachments() const final;
	[[nodiscard]] std::span<const AttachmentDescription<G>> GetAttachmentDescs() const final;
	[[nodiscard]] ImageLayout GetLayout(uint32_t index) const final;
	[[nodiscard]] const std::optional<PipelineRenderingCreateInfo<G>>& GetPipelineRenderingCreateInfo() const final;

	// TODO(djohansson): make these two a single scoped call
	[[maybe_unused]] const RenderTargetBeginInfo<G>& Begin(CommandBufferHandle<G> cmd, SubpassContents contents) final;
	void End(CommandBufferHandle<G> cmd) final;
	//

	void ClearAll(
		CommandBufferHandle<G> cmd,
		std::span<const ClearValue> values) const final;

	void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<kVk>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter filter) final;

	void Copy(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex) final;

	void Clear(CommandBufferHandle<G> cmd, const ClearValue& value, uint32_t index) final;

	void Transition(CommandBufferHandle<G> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index) final;

	void SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp = {}) final;
	void SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp = {}) final;

	[[nodiscard]] auto GetSurface() const noexcept { return mySurface; }
	// queries the surface's current size. the swapchain's own extent if the surface leaves it to the swapchain.
	[[nodiscard]] Extent2d QuerySurfaceExtent();
	
	[[nodiscard]] FlipResult<G> Flip();
	[[nodiscard]] QueuePresentInfo<G> PreparePresent();
	[[maybe_unused]] bool WaitPresent(uint64_t presentId, uint64_t timeout = UINT64_MAX) const; //NOLINT(modernize-use-nodiscard)

	[[nodiscard]] auto& GetFrames() noexcept { return myFrames; }
	[[nodiscard]] const auto& GetFrames() const noexcept { return myFrames; }
	[[nodiscard]] auto GetCurrentFrameIndex() const noexcept { return myFrameIndex; }

	void CreateSwapchain();

	// set when an acquire or present reports the swapchain out of date or suboptimal (e.g. after a fullscreen switch,
	// which doesn't always come with a framebuffer resize event of the final size). cleared by CreateSwapchain.
	void OnPresentResult(PresentResult result) noexcept;
	[[nodiscard]] bool NeedsRecreate() const noexcept { return myNeedsRecreate; }

private:
	SurfaceHandle<G> mySurface{};
	SwapchainHandle<G> mySwapchain{};
	std::vector<Frame<G>> myFrames;
	uint32_t myFrameIndex{};
	// fences of vkAcquireNextImageKHR calls on mySwapchain that may not have been waited on yet. owned here, not by
	// the frame, so they can be waited on before mySwapchain is destroyed (on resize): waiting on them afterwards is invalid.
	std::vector<Fence<G>> myAcquireFences;
	bool myNeedsRecreate{};
};

} // namespace rhi
