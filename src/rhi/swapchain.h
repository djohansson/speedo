#pragma once

#include <rhi/deviceobject.h>
#include <rhi/frame.h>
#include <rhi/queue.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <span>
#include <utility>
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
	// the most images the application asks for (0: the surface's limit): the counts asked for (Configure, and
	// GetImageCountLimits' second) stay within it
	uint32_t maxImageCount = 0;
	// the queue the frames are presented from (see GetPresentQueueType), and the families the images are drawn and
	// presented by: shared concurrently between the two when they differ, so that neither owns them
	QueueType presentQueueType = kQueueTypeGraphics;
	uint32_t graphicsQueueFamily = 0;
	uint32_t presentQueueFamily = 0;
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
	void SetClearValue(const ClearValue& value, uint32_t index) final;

	[[nodiscard]] auto GetSurface() const noexcept { return mySurface; }
	// queries the surface's current size. the swapchain's own extent if the surface leaves it to the swapchain.
	[[nodiscard]] Extent2d QuerySurfaceExtent();
	
	[[nodiscard]] FlipResult<G> Flip();
	[[nodiscard]] QueuePresentInfo<G> PreparePresent();
	[[maybe_unused]] bool WaitPresent(uint64_t presentId, uint64_t timeout = UINT64_MAX) const; //NOLINT(modernize-use-nodiscard)
	// waits (up to timeout, in nanoseconds) until the last frame presented has been shown: before the window changes how
	// it is shown (fullscreen), when fifo may have frames queued for the coming vertical blanks, which a display mode
	// change leaves unshown (see client::WindowedApplication::Draw). false if it timed out, or can't tell (no
	// VK_KHR_present_wait)
	[[maybe_unused]] bool WaitPresented(uint64_t timeout) const; //NOLINT(modernize-use-nodiscard)
	// whether it has presented since it was made (a released one hasn't)
	[[nodiscard]] bool HasPresented() const noexcept { return myHasPresented; }

	[[nodiscard]] auto& GetFrames() noexcept { return myFrames; }
	[[nodiscard]] const auto& GetFrames() const noexcept { return myFrames; }
	[[nodiscard]] auto GetCurrentFrameIndex() const noexcept { return myFrameIndex; }

	void CreateSwapchain();
	// destroys the swapchain and its frames, until the next CreateSwapchain (which it asks for, NeedsRecreate): with the
	// gpu idle, before the window changes how it is shown (fullscreen). NVIDIA's driver on Windows waited forever in
	// vkCreateSwapchainKHR or vkDestroySwapchainKHR for a swapchain that lived through a display mode change, even with
	// everything it presented shown; made after it, with none before, it doesn't
	void Release();
	// how the swapchain presents (its mode, if the surface has it, and how many images, within the surface's limits), for
	// the next CreateSwapchain, which it asks for (NeedsRecreate). on the thread that draws
	void Configure(PresentationMode mode, uint32_t imageCount);
	[[nodiscard]] PresentationMode GetPresentationMode() const;
	// which queue to present from: the compute queue where its family is another than the graphics queue's and can
	// present to the surface (a dedicated one, NVIDIA's), else the graphics queue
	[[nodiscard]] QueueType GetPresentQueueType() const noexcept { return this->GetDesc().presentQueueType; }
	// the modes the surface supports, as bits (1 << PresentationMode)
	[[nodiscard]] uint32_t GetPresentationModes() const;
	// how many images may be asked for: at least first, at most second (0: no limit), the surface's limits within the
	// application's (SwapchainCreateDesc::maxImageCount). the driver may make more than asked for (see GetFrames)
	[[nodiscard]] std::pair<uint32_t, uint32_t> GetImageCountLimits() const;
	[[nodiscard]] uint32_t GetImageCount() const noexcept { return static_cast<uint32_t>(this->GetDesc().images.size()); }
	// the window when it is in exclusive fullscreen (kInvalidWindowHandle when not): the next CreateSwapchain, which it
	// asks for if it changed, takes the window's monitor exclusively (VK_EXT_full_screen_exclusive, application
	// controlled, on Windows; nothing elsewhere). without it, NVIDIA's driver composited the fullscreen window, and the
	// display's variable refresh rate never engaged. on the thread that draws
	void SetExclusiveFullscreen(WindowHandle window) noexcept;

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
	uint64_t myLastPresentId{}; // 0: none (see PreparePresent)
	bool myHasPresented{};
	WindowHandle myExclusiveWindow{}; // see SetExclusiveFullscreen
	bool myExclusiveAcquired{}; // vkAcquireFullScreenExclusiveModeEXT succeeded on mySwapchain
	void InternalDestroySwapchain(SwapchainHandle<G> swapchain);
};

} // namespace rhi
