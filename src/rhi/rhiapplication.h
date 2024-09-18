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

protected:
	explicit RHIApplication() = default;
	RHIApplication(
		std::string_view name,
		Environment&& env,
		CreateWindowFunc createWindowFunc);

	template <GraphicsApi G>
	[[nodiscard]] RHI<G>& InternalRHI();

	template <GraphicsApi G>
	[[nodiscard]] const RHI<G>& InternalRHI() const;

	void InternalUpdateInput() override;

private:
	void InternalDraw();

	std::unique_ptr<RHIBase> myRHI;
	InputState myInput{};
};
