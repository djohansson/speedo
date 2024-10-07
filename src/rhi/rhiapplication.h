#pragma once

#include "capi.h"

#include <core/application.h>
#include <core/capi.h>
#include <core/inputstate.h>
#include <core/taskexecutor.h>
#include <core/upgradablesharedmutex.h>

#include <rhi/rhi.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <algorithm>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>

class RHIApplication : public Application
{	
public:
	~RHIApplication() noexcept(false) override;

	void Tick() override;

	void OnResizeFramebuffer(WindowHandle window, int width, int height);

	void UpdateInput() { InternalUpdateInput(); };
	void Draw() { InternalDraw(); };

	[[nodiscard]] WindowState* GetWindowState(WindowHandle window);

	[[nodiscard]] RHIBase& GetRHI() noexcept { return *myRHI; }
	[[nodiscard]] const RHIBase& GetRHI() const noexcept { return *myRHI; }

	template <GraphicsApi G>
	[[nodiscard]] RHI<G>& GetRHI() noexcept;

	template <GraphicsApi G>
	[[nodiscard]] const RHI<G>& GetRHI() const noexcept;

	static UpgradableSharedMutex gDrawMutex;
	static std::atomic_uint8_t gProgress;
	static std::atomic_bool gShowProgress;
	static bool gShowAbout;
	static bool gShowDemoWindow;

protected:
	explicit RHIApplication() = default;
	RHIApplication(
		std::string_view name,
		Environment&& env,
		CreateWindowFunc createWindowFunc);

	void InternalUpdateInput() override;

private:
	void InternalDraw();

	std::unique_ptr<RHIBase> myRHI;
	InputState myInput{};
};

namespace rhiapplication
{

template <typename LoadOp>
void OpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList, LoadOp loadOp);

} // namespace rhiapplication

#include "rhiapplication.inl"
