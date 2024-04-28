#pragma once

#include "capi.h"

#include <core/application.h>
#include <core/capi.h>
#include <core/file.h>
#include <core/future.h>
#include <core/inputstate.h>
#include <core/utils.h>

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

	void onResizeFramebuffer(WindowHandle window, int w, int h);

	void updateInput() { internalUpdateInput(); };
	void draw() { internalDraw(); };

	WindowState* getWindowState(WindowHandle window);

protected:
	explicit RhiApplication() = default;
	RhiApplication(
		std::string_view name,
		Environment&& env,
		CreateWindowFunc createWindowFunc);

	template <GraphicsApi G>
	Rhi<G>& internalRhi();

	template <GraphicsApi G>
	const Rhi<G>& internalRhi() const;

	virtual void internalUpdateInput() override;

private:
	void internalDraw();

	std::shared_ptr<void> myRhi;
	InputState myInput{};
};
