#pragma once

#include <gfx/gpu.h>
#include <gfx/imguilayer.h>
#include <gfx/scene.h>
#include <gfx/views.h>

#include "preview.h"
#include "thumbnails.h"

#include <platform/windowedapplication.h>

#include <core/loadqueue.h>
#include <core/inputstate.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace client
{

// the client's windowed application (see platform::WindowedApplication): it owns the rhi, draws the scene (gfx::Scene)
// through the window's views, and runs the user interface (gfx::ImGuiLayer), the frame loop and the loads
class WindowedApplication : public platform::WindowedApplication
{	
public:
	~WindowedApplication() override;

	// recreates the swapchain and what is sized after it (unless minimized)
	void OnResizeFramebuffer(WindowHandle window, int width, int height) override;
	// the change waits for Draw to stop presenting, and for what it presented to be shown (see Draw)
	void RequestWindowChange(core::std_extra::move_only_function<void()> change) override;
	void OnInputStateChanged(const core::InputState& input);
	
	// call as soon as you want the application to redraw itself, e.g. after changing the scene or camera
	// typically called from the ticking function, but can be called from anywhere (e.g. from a file dialogue callback after loading a new scene)
	void PrepareDraw();

	// will redraw the application as soon as possible. typically this should not be called directly, 
	// but rather PrepareDraw should be called to schedule a draw, and then the application will call Draw at the appropriate time.
	// returns false if no frame was presented (e.g. minimized window), so the caller can back off
	[[nodiscard]] bool Draw();

	// waits for all in-flight work and destroys the rhi. must be called while the application is still registered
	// in core::gApplication (i.e. before the last shared_ptr to it is released), since device objects resolve their
	// instance/device through Application::Get(), which returns null once the application is being destroyed.
	void Shutdown();

	[[nodiscard]] gfx::RHI& GetRHI() noexcept { return *myRHI; }
	[[nodiscard]] const gfx::RHI& GetRHI() const noexcept { return *myRHI; }
	[[nodiscard]] gfx::Views& GetViews() noexcept { return *myViews; }
	[[nodiscard]] gfx::Scene& GetScene() noexcept { return *myScene; }

	// the file browser's preview (see Preview): requested by the ui, made by the next Draw, null until then. recorded in
	// the frames the ui marks it visible in. ui thread
	void RequestPreview() noexcept { myPreviewRequested.store(true, std::memory_order_relaxed); }
	[[nodiscard]] Preview* GetPreview() noexcept { return myPreviewReady.load(std::memory_order_acquire) ? myPreview.get() : nullptr; }
	void SetPreviewVisible(bool visible) noexcept { myPreviewVisible.store(visible, std::memory_order_relaxed); }

protected:
	WindowedApplication(
		std::string_view name,
		core::Environment&& env);

private:
	// the file browser (File > Open...): folders and the files the client loads as tiles, with multi-select, the
	// selection previewed off-screen (see Preview), and opened side by side or as authored. ui thread, in PrepareDraw
	void InternalDrawBrowser();
	// View > Camera > Copy Pose (ctrl/cmd+shift+c): the view as the variables that reproduce it, to the clipboard
	void InternalCopyPose();
	// View > Camera > Paste Pose (ctrl/cmd+shift+v): a pose of Copy Pose's from the clipboard, applied (see PrepareDraw)
	void InternalPastePose();

	
	std::unique_ptr<gfx::RHI> myRHI;
	std::unique_ptr<gfx::Views> myViews; // of the window
	std::unique_ptr<gfx::SwapchainOutput> myOutput; // the window's swapchain, which the scene draws into
	std::unique_ptr<gfx::Scene> myScene; // drawn through the views
	std::unique_ptr<gfx::ImGuiLayer> myImGui; // the user interface
	std::unique_ptr<Preview> myPreview; // made on the draw thread once requested (see RequestPreview)
	// the browser's thumbnails, and its image preview (one big slot): made with its first frame, ui thread. destroyed after
	// imgui's renderer, which destroys their textures
	std::unique_ptr<Thumbnails> myThumbnails;
	std::unique_ptr<Thumbnails> myImagePreview;
	std::atomic_bool myPreviewRequested = false;
	std::atomic_bool myPreviewReady = false;
	std::atomic_bool myPreviewVisible = false;
	// the frames' slots of the per frame buffers (see SHADER_TYPES_FRAME_COUNT), taken in turn: the next one, and per
	// slot the graphics timeline value the last frame in it signaled, which the next frame in it waits for (see Draw)
	// the window's framebuffer was resized (OnResizeFramebuffer, main thread): Draw recreates the swapchain
	std::atomic_bool myResizeRequested = false;
	// a window change (RequestWindowChange): asked for, then Draw stopped and its presents shown, the main thread changing
	// the window (Draw skips frames meanwhile), done. one at a time: the ones asked for meanwhile wait their turn
	enum WindowChangeState : uint8_t
	{
		kWindowChangeNone,
		kWindowChangeRequested,
		kWindowChangeChanging,
	};
	std::atomic<WindowChangeState> myWindowChangeState = kWindowChangeNone;
	std::mutex myWindowChangesMutex;
	std::vector<core::std_extra::move_only_function<void()>> myWindowChanges; // guarded by myWindowChangesMutex
	uint32_t myFrameSlot = 0;
	// per frame slot, what its frame presented with: the semaphore the present waited on, and its present fences, where the
	// device has them (VK_EXT_swapchain_maintenance1). released when the slot comes round again (InternalRetirePresents)
	std::array<std::vector<gfx::Semaphore>, SHADER_TYPES_FRAME_COUNT> myPresentSemaphores;
	std::array<std::vector<rhi::Fence<rhi::kGraphicsApi>>, SHADER_TYPES_FRAME_COUNT> myPresentFences;
	void InternalRetirePresents(uint32_t frameSlot);
	void InternalRetirePresents(gfx::Swapchain& swapchain);
	std::array<uint64_t, SHADER_TYPES_FRAME_COUNT> myFrameSlotTimelines{};
	// the window's presentation (View > Presentation): what the ui asked for, which Draw applies to the swapchain before
	// its next frame (Swapchain::Configure), and what the swapchain has, with what the surface supports (the modes' bits,
	// and the image counts). packed by PackPresentation, 8 bits each (none: 0)
	struct Presentation
	{
		rhi::PresentationMode mode = rhi::PresentationMode::kFifo;
		uint32_t imageCount = 0;
		uint32_t modes = 0;
		uint32_t minImageCount = 0;
		uint32_t maxImageCount = 0; // 0: no limit
	};
	std::atomic_uint64_t myPresentationRequest = 0;
	std::atomic_uint64_t myPresentation = 0;
	[[nodiscard]] static constexpr uint64_t PackPresentation(const Presentation& presentation) noexcept
	{
		return (1ULL << 63) | (static_cast<uint64_t>(presentation.modes & 0xffU) << 32) |
			   (static_cast<uint64_t>(presentation.mode) << 24) | ((presentation.maxImageCount & 0xffU) << 16) |
			   ((presentation.minImageCount & 0xffU) << 8) | (presentation.imageCount & 0xffU);
	}
	[[nodiscard]] static constexpr Presentation UnpackPresentation(uint64_t packed) noexcept
	{
		return {
			.mode = static_cast<rhi::PresentationMode>((packed >> 24) & 0xffU),
			.imageCount = static_cast<uint32_t>(packed & 0xffU),
			.modes = static_cast<uint32_t>((packed >> 32) & 0xffU),
			.minImageCount = static_cast<uint32_t>((packed >> 8) & 0xffU),
			.maxImageCount = static_cast<uint32_t>((packed >> 16) & 0xffU)};
	}
	std::string myImGuiIniSettings;
	static std::mutex gDrawMutex; //NOLINT(readability-identifier-naming) only ever locked exclusively
	static core::LoadQueue gLoads; //NOLINT(readability-identifier-naming) asset loads, shown with their progress
	static bool gShowAbout; //NOLINT(readability-identifier-naming)
	static bool gShowDemoWindow; //NOLINT(readability-identifier-naming)
	static bool gShowFps; //NOLINT(readability-identifier-naming)
	static bool gShowTps; //NOLINT(readability-identifier-naming)
};

} // namespace client

