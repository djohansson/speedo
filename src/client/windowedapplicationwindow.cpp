// the window's side of the client's windowed application (see WindowedApplication): its changes (a fullscreen switch, a
// size, a monitor), its presentation (View > Presentation), and the swapchain (or headless, the image) sized after it,
// before each frame (InternalPrepareWindow)
#include "windowedapplicationdetail.h"

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

namespace windowedapplication
{

// a presentation mode's name, for crash reports' events
constexpr std::string_view PresentationModeName(rhi::PresentationMode mode) noexcept
{
	switch (mode)
	{
	case rhi::PresentationMode::kFifo: return "fifo";
	case rhi::PresentationMode::kFifoRelaxed: return "fifo relaxed";
	case rhi::PresentationMode::kMailbox: return "mailbox";
	case rhi::PresentationMode::kImmediate: return "immediate";
	case rhi::PresentationMode::kCount: break;
	}
	return "?";
}

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI& rhi, platform::Window& window)
{
	ZoneScopedN("RecreateWindowDependentObjects");

	auto& device = rhi.GetPrimaryDevice();
	auto& swapchain = rhi.GetSwapchain(window);

	auto extent = swapchain.QuerySurfaceExtent();
	if (extent.width == 0 || extent.height == 0)
		return false;

	// from the previous one (oldSwapchain), unless a window change released it first (see Draw). none if the window was
	// minimized since the check above (the device's wait takes a while): its size is the one that counts
	device.WaitIdle();
	if (!swapchain.CreateSwapchain())
		return false;
	extent = swapchain.GetDesc().extent;
	rhi::CrashEvent(std::format(
		"swapchain made: {}x{}, {}, {} images", extent.width, extent.height, PresentationModeName(swapchain.GetPresentationMode()),
		swapchain.GetImageCount()));

	// the window's size in screen coordinates, as its framebuffer's (the swapchain's) over its content scale
	auto& state = window.GetState();
	state.width = static_cast<uint32_t>(static_cast<float>(extent.width) / state.xscale);
	state.height = static_cast<uint32_t>(static_cast<float>(extent.height) / state.yscale);

	App().GetViews().OnResizeFramebuffer({extent.width, extent.height});
	App().GetScene().Resize();

	return true;
}

// headless: the frames' image at extent (the window's framebuffer's), and everything sized after it. caller holds
// gDrawMutex
void ResizeImageOutput(RHI& rhi, gfx::ImageOutput& output, rhi::Extent2d extent)
{
	ZoneScopedN("ResizeImageOutput");

	rhi.GetPrimaryDevice().WaitIdle();
	output.Resize(extent);
	rhi::CrashEvent(std::format("image output made: {}x{}", extent.width, extent.height));

	App().GetViews().OnResizeFramebuffer({extent.width, extent.height});
	App().GetScene().Resize();
}

} // namespace windowedapplication

bool WindowedApplication::InternalPrepareWindow(gfx::Swapchain* swapchain)
{
	using namespace windowedapplication;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = GetWindow(GetCurrentWindow());

	// a window change (RequestWindowChange: a fullscreen switch, a size, a monitor): the main thread changes the window
	// once nothing presented is still waiting to be shown, with no swapchain (see Swapchain::Release), and frames are
	// skipped until it has. with fifo, frames queued for the coming vertical blanks when exclusive fullscreen changed the
	// display mode were never shown, and NVIDIA's driver then waited for them in vkCreateSwapchainKHR (or
	// vkDestroySwapchainKHR), forever: every switch into exclusive fullscreen under fifo hung; and a swapchain made after
	// leaving fullscreen, under fifo, hung there when the window was resized with frames queued. while minimized too
	// (windows minimizes exclusive fullscreen when it loses the focus): no frames are drawn then, and the switch waited for
	// one
	if (auto state = myWindowChangeState.load(std::memory_order_acquire); state != kWindowChangeNone)
	{
		if (state == kWindowChangeRequested)
		{
			rhi::CrashEvent("window change: swapchain released");
			device.WaitIdle();
			if (swapchain != nullptr)
			{
				InternalRetirePresents(*swapchain);
				swapchain->Release();
			}
			std::vector<core::std_extra::move_only_function<void()>> changes;
			{
				std::scoped_lock changesLock(myWindowChangesMutex);
				changes = std::exchange(myWindowChanges, {});
			}
			myWindowChangeState.store(kWindowChangeChanging, std::memory_order_release);
			CallOnMain(
				[this, changes = std::move(changes)]() mutable
				{
					for (auto& change : changes)
						change();
					{
						const auto& state = GetWindow(GetCurrentWindow()).GetState();
						rhi::CrashEvent(std::format(
							"window changed: {}x{}{}", state.width, state.height,
							state.fullscreenEnabled != 0U ? (state.fullscreenMode == FULLSCREEN_MODE_EXCLUSIVE ? ", exclusive fullscreen" : ", borderless fullscreen") : ""));
					}
					// resized, or not (exclusive fullscreen at the window's size): recreated either way
					myResizeRequested.store(true, std::memory_order_release);
					std::scoped_lock changesLock(myWindowChangesMutex);
					myWindowChangeState.store(
						myWindowChanges.empty() ? kWindowChangeNone : kWindowChangeRequested, std::memory_order_release);
				});
		}
		return false;
	}

	if (window.IsMinimized())
		return false;

	// the presentation the ui asked for (View > Presentation), applied by recreating the swapchain below (headless,
	// nothing is presented)
	if (auto request = myPresentationRequest.exchange(0, std::memory_order_relaxed); request != 0 && swapchain != nullptr)
	{
		auto presentation = UnpackPresentation(request);
		rhi::CrashEvent(std::format("presentation asked for: {}, {} images", PresentationModeName(presentation.mode), presentation.imageCount));
		swapchain->Configure(presentation.mode, presentation.imageCount);
	}

	// exclusive fullscreen takes the monitor (see Swapchain::SetExclusiveFullscreen), recreating the swapchain below when
	// it changes, and where the system refused it (composited meanwhile), when the window comes to the foreground
	if (swapchain != nullptr)
	{
		const auto& state = window.GetState();
		swapchain->SetExclusiveFullscreen(
			state.fullscreenEnabled != 0U && state.fullscreenMode == FULLSCREEN_MODE_EXCLUSIVE ? GetCurrentWindow()
																							 : kInvalidWindowHandle);
	}

	// a resize (see OnResizeFramebuffer), or what the swapchain found out of date (e.g. a fullscreen switch can leave it
	// at the previous size without a matching resize event)
	if (myResizeRequested.exchange(false, std::memory_order_acquire) || (swapchain != nullptr && swapchain->NeedsRecreate()))
	{
		if (swapchain == nullptr)
		{
			// headless: the image at the window's framebuffer size
			auto size = myFramebufferSize.load(std::memory_order_relaxed);
			rhi::Extent2d extent{.width = static_cast<uint32_t>(size >> 32U), .height = static_cast<uint32_t>(size & 0xFFFFFFFFU)};
			if (extent.width > 0 && extent.height > 0 && extent != myImageOutput->GetExtent())
				ResizeImageOutput(rhi, *myImageOutput, extent);
		}
		// releasing it first, and waiting for its presents, made a swapchain under fifo hang in vkDestroySwapchainKHR when
		// the window was resized: recreated from it, as the drivers expect, it doesn't
		else if (!RecreateWindowDependentObjects(rhi, window))
		{
			// no area yet: again before the next frame
			myResizeRequested.store(true, std::memory_order_release);
			return false;
		}
	}
	// a change of the render size (View > Upscaling), with the graphics queues done with the render targets (theirs alone),
	// waited for under their lock: a device wait raced the loaders' submits on the queues aliased to them (KosmicKrisp's
	// transfer queue is its graphics queue; validation's threading errors, at a startup with SPEEDO_UPSCALING)
	else if (App().GetScene().NeedsResize())
	{
		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			for (auto& [queue, submits] : graphics->queues)
				queue.WaitIdle();
		}
		App().GetScene().Resize();
	}
	if (swapchain != nullptr)
	{
		auto [minImageCount, maxImageCount] = swapchain->GetImageCountLimits();
		myPresentation.store(
			PackPresentation(Presentation{
				.mode = swapchain->GetPresentationMode(),
				.imageCount = swapchain->GetImageCount(),
				.modes = swapchain->GetPresentationModes(),
				.minImageCount = minImageCount,
				.maxImageCount = maxImageCount}),
			std::memory_order_relaxed);
	}

	return true;
}

void WindowedApplication::RequestWindowChange(core::std_extra::move_only_function<void()> change)
{
	// made by Draw's next frame (see Draw), after the ones asked for before
	std::scoped_lock changesLock(myWindowChangesMutex);
	myWindowChanges.push_back(std::move(change));
	// unless requested already, or changing: the main call that ends that one asks again (see Draw)
	auto none = kWindowChangeNone;
	myWindowChangeState.compare_exchange_strong(none, kWindowChangeRequested, std::memory_order_acq_rel);
}

void WindowedApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace windowedapplication;

	ZoneScopedN("WindowedApplication::OnResizeFramebuffer");

	// minimizing reports 0x0: keep the swapchain, and have Draw skip frames until the window is restored
	platform::WindowedApplication::OnResizeFramebuffer(window, width, height);
	if (GetWindow(window).IsMinimized())
		return;

	// headless, no surface knows the size: the image takes this one
	myFramebufferSize.store(
		(static_cast<uint64_t>(std::max(width, 0)) << 32U) | static_cast<uint64_t>(std::max(height, 0)), std::memory_order_relaxed);

	// recreated by Draw, on the draw thread, before its next frame (sized after the surface, which already has this
	// size): never here. this runs on the main thread, inside the window system's calls (a fullscreen switch's
	// glfwSetWindowMonitor), and it used to take the draw lock and recreate the swapchain itself: while Draw held the lock
	// recreating it (out of date), which on Windows waits for the window's thread to handle its messages, the main thread
	// waited for the lock, and neither went on (a switch out of fullscreen hung 2 runs in 3)
	myResizeRequested.store(true, std::memory_order_release);
}

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE