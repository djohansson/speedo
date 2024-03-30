#pragma once

#include "capi.h"

#include <core/application.h>
#include <core/capi.h>
#include <core/file.h>
#include <core/future.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <filesystem>
#include <functional>
#include <memory>
#include <tuple>

template <GraphicsApi G>
struct Rhi;

class RhiApplication : public Application
{	
public:
	~RhiApplication() override;

	void tick() override;

	void onResizeWindow(const WindowState& window);
	void onResizeFramebuffer(uint32_t width, uint32_t height);

	void onMouse(const MouseState& mouse);
	void onKeyboard(const KeyboardState& keyboard);

protected:
	explicit RhiApplication() = default;
	RhiApplication(std::string_view name, Environment&& env, const WindowState& window);

	template <GraphicsApi G>
	Rhi<G>& internalRhi();

	template <GraphicsApi G>
	const Rhi<G>& internalRhi() const;

private:
	void createDevice(const WindowState& window);

	std::shared_ptr<void> myRhi;
};
