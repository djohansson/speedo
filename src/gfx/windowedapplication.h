#pragma once

#include <gfx/filedialog.h>
#include <gfx/gpu.h>
#include <gfx/views.h>

#include <rhi/capi.h>

#include <core/application.h>
#include <core/loadqueue.h>
#include <core/inputstate.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace gfx
{

// an application drawing graphics in a window, on the build's graphics api (see gfx/gpu.h): it owns the rhi, draws
// the loaded model with its materials through the window's views, and runs the user interface
class WindowedApplication : public core::Application
{	
public:
	~WindowedApplication() override;
	
	[[nodiscard]] virtual bool Main();

	// also wakes the main loop, which sleeps in glfwWaitEvents() until the next window event
	void RequestExit() noexcept override;

	void OnResizeFramebuffer(WindowHandle window, int width, int height);
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

	[[nodiscard]] WindowState* GetWindowState(WindowHandle window);
	[[nodiscard]] uint32_t GetWindowCount() const noexcept;
	[[nodiscard]] WindowHandle GetWindow(uint32_t index) const noexcept;

	[[nodiscard]] RHI& GetRHI() noexcept { return *myRHI; }
	[[nodiscard]] const RHI& GetRHI() const noexcept { return *myRHI; }
	[[nodiscard]] Views& GetViews() noexcept { return *myViews; }

protected:
	WindowedApplication(
		std::string_view name,
		core::Environment&& env,
		CreateWindowFunc createWindowFunc);

private:
	template <typename LoadOp>
	void InternalOpenFileDialogueAsync(
		std::string&& resourcePathString,
		const std::vector<FileFilter>& filterList,
		LoadOp loadOp);
	template <typename LoadOp>
	void InternalOpenFolderDialogueAsync(std::string&& startPathString, LoadOp loadOp);
	// shows dialogue() (returning whether something was chosen, and its path) on the main thread, and queues
	// loadOp(path, progress) as a load (see gLoads) if something was
	template <typename Dialogue, typename LoadOp>
	void InternalDialogueAsync(Dialogue dialogue, LoadOp loadOp);
	
	std::unique_ptr<RHI> myRHI;
	std::unique_ptr<Views> myViews; // of the window
	std::string myImGuiIniSettings;
	static std::mutex gDrawMutex; //NOLINT(readability-identifier-naming) only ever locked exclusively
	static core::LoadQueue gLoads; //NOLINT(readability-identifier-naming) asset loads, shown with their progress
	static bool gShowAbout; //NOLINT(readability-identifier-naming)
	static bool gShowDemoWindow; //NOLINT(readability-identifier-naming)
	static bool gShowFps; //NOLINT(readability-identifier-naming)
	static bool gShowTps; //NOLINT(readability-identifier-naming)
};

} // namespace gfx

#include "windowedapplication.inl"
