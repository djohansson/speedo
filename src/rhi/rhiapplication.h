#pragma once

#include "capi.h"

#include <core/application.h>
#include <core/capi.h>
#include <core/inputstate.h>
#include <rhi/rhi.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <memory>
#include <string>
#include <string_view>

class RHIApplication : public Application
{	
public:
	~RHIApplication() noexcept(false) override;
	
	[[nodiscard]] virtual bool Main();

	void OnResizeFramebuffer(WindowHandle window, int width, int height);
	void OnInputStateChanged(const InputState& input);
	
	void Draw();

	[[nodiscard]] WindowState* GetWindowState(WindowHandle window);

	[[nodiscard]] RHIBase& GetRHI() noexcept { return *myRHI; }
	[[nodiscard]] const RHIBase& GetRHI() const noexcept { return *myRHI; }

	template <GraphicsApi G>
	[[nodiscard]] RHI<G>& GetRHI() noexcept;

	template <GraphicsApi G>
	[[nodiscard]] const RHI<G>& GetRHI() const noexcept;

	static UpgradableSharedMutex gDrawMutex; //NOLINT(readability-identifier-naming)
	static std::atomic_uint8_t gProgress; //NOLINT(readability-identifier-naming)
	static std::atomic_bool gShowProgress; //NOLINT(readability-identifier-naming)
	static bool gShowAbout; //NOLINT(readability-identifier-naming)
	static bool gShowDemoWindow; //NOLINT(readability-identifier-naming)

protected:
	explicit RHIApplication() = default;
	RHIApplication(
		std::string_view name,
		Environment&& env,
		CreateWindowFunc createWindowFunc);

private:
	std::unique_ptr<RHIBase> myRHI;
};

namespace rhiapplication
{

template <typename LoadOp>
void OpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList, LoadOp loadOp);

} // namespace rhiapplication

#include "rhiapplication.inl"
