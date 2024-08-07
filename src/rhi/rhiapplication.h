#pragma once

#include "capi.h"

#include <core/application.h>
#include <core/capi.h>
#include <core/inputstate.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <memory>
#include <string_view>

template <GraphicsApi G>
struct Rhi;

class RhiApplication : public Application
{	
public:
	~RhiApplication() noexcept(false) override;

	void Tick() override;

	void OnResizeFramebuffer(WindowHandle window, int width, int height);

	void UpdateInput() { InternalUpdateInput(); };
	void Draw() { InternalDraw(); };

	[[nodiscard]] WindowState* GetWindowState(WindowHandle window);

protected:
	explicit RhiApplication() = default;
	RhiApplication(
		std::string_view name,
		Environment&& env,
		CreateWindowFunc createWindowFunc);

	template <GraphicsApi G>
	[[nodiscard]] Rhi<G>& InternalRhi();

	template <GraphicsApi G>
	[[nodiscard]] const Rhi<G>& InternalRhi() const;

	void InternalUpdateInput() override;

private:
	void InternalDraw();

	std::shared_ptr<void> myRhi;
	InputState myInput{};
};
