#pragma once

#include <rhi/capi.h>
#include <rhi/rhi.h>

#include <core/application.h>
#include <core/capi.h>
#include <core/inputstate.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rhi
{

class RHIApplication : public core::Application
{	
public:
	~RHIApplication() override;
	
	[[nodiscard]] virtual bool Main();

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

	[[nodiscard]] RHIBase& GetRHI() noexcept { return *myRHI; }
	[[nodiscard]] const RHIBase& GetRHI() const noexcept { return *myRHI; }

	template <GraphicsApi G>
	[[nodiscard]] RHI<G>& GetRHI() noexcept;

	template <GraphicsApi G>
	[[nodiscard]] const RHI<G>& GetRHI() const noexcept;

protected:
	RHIApplication(
		std::string_view name,
		core::Environment&& env,
		CreateWindowFunc createWindowFunc);

private:
	template <typename LoadOp>
	[[maybe_unused]] auto InternalOpenFileDialogueAsync(
		std::string&& resourcePathString,
		const std::vector<window::FileFilter>& filterList,
		LoadOp loadOp);
	
	std::unique_ptr<RHIBase> myRHI;
	std::string myImGuiIniSettings;
	static core::UpgradableSharedMutex gDrawMutex; //NOLINT(readability-identifier-naming)
	static std::atomic_uint8_t gProgress; //NOLINT(readability-identifier-naming)
	static std::atomic_bool gShowProgress; //NOLINT(readability-identifier-naming)
	static bool gShowAbout; //NOLINT(readability-identifier-naming)
	static bool gShowDemoWindow; //NOLINT(readability-identifier-naming)
};

} // namespace rhi

#include "rhiapplication.inl"
