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

#include <nfd.h>

template <GraphicsApi G>
struct Rhi;

class RhiApplication : public Application
{	
public:
	~RhiApplication() override;

	bool tick() override;

	void onResizeWindow(const WindowState& window);
	void onResizeFramebuffer(uint32_t width, uint32_t height);

	void onMouse(const MouseState& mouse);
	void onKeyboard(const KeyboardState& keyboard);

	//void setGraphicsApi(GraphicsApi api);
	void createDevice(const WindowState& window);
	const WindowState& createWindow();

protected:
	RhiApplication(std::string_view name, Environment&& env);

	template <GraphicsApi G>
	Rhi<G>& internalRhi();

	template <GraphicsApi G>
	const Rhi<G>& internalRhi() const;

private:
	std::shared_ptr<void> myRhi;

	Future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>> myOpenFileFuture;

	bool myRequestExit = false;
};
