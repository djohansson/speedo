#pragma once

#include <gfx/gpu.h>
#include <gfx/imguilayer.h>
#include <gfx/scene.h>
#include <gfx/views.h>

#include "preview.h"

#include <platform/filedialog.h>
#include <platform/windowedapplication.h>

#include <core/loadqueue.h>
#include <core/inputstate.h>

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
		core::Environment&& env,
		CreateWindowFunc createWindowFunc);

private:
	// the file browser (File > Browse...): folders and the files the client loads as tiles, with multi-select, the
	// selection previewed off-screen (see Preview), and opened side by side or as authored. ui thread, in PrepareDraw
	void InternalDrawBrowser();

	template <typename LoadOp>
	void InternalOpenFileDialogueAsync(
		std::string&& resourcePathString,
		const std::vector<platform::FileFilter>& filterList,
		LoadOp loadOp);
	template <typename LoadOp>
	void InternalOpenFolderDialogueAsync(std::string&& startPathString, LoadOp loadOp);
	// shows dialogue() (returning whether something was chosen, and its path) on the main thread, and queues
	// loadOp(path, progress) as a load (see gLoads) if something was
	template <typename Dialogue, typename LoadOp>
	void InternalDialogueAsync(Dialogue dialogue, LoadOp loadOp);
	
	std::unique_ptr<gfx::RHI> myRHI;
	std::unique_ptr<gfx::Views> myViews; // of the window
	std::unique_ptr<gfx::SwapchainOutput> myOutput; // the window's swapchain, which the scene draws into
	std::unique_ptr<gfx::Scene> myScene; // drawn through the views
	std::unique_ptr<gfx::ImGuiLayer> myImGui; // the user interface
	std::unique_ptr<Preview> myPreview; // made on the draw thread once requested (see RequestPreview)
	std::atomic_bool myPreviewRequested = false;
	std::atomic_bool myPreviewReady = false;
	std::atomic_bool myPreviewVisible = false;
	std::string myImGuiIniSettings;
	static std::mutex gDrawMutex; //NOLINT(readability-identifier-naming) only ever locked exclusively
	static core::LoadQueue gLoads; //NOLINT(readability-identifier-naming) asset loads, shown with their progress
	static bool gShowAbout; //NOLINT(readability-identifier-naming)
	static bool gShowDemoWindow; //NOLINT(readability-identifier-naming)
	static bool gShowFps; //NOLINT(readability-identifier-naming)
	static bool gShowTps; //NOLINT(readability-identifier-naming)
};

} // namespace client

#include "windowedapplication.inl"
