#pragma once

#include "capi.h"

#include <core/application.h>
#include <core/capi.h>
#include <core/file.h>
#include <core/future.h>
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

	void onResizeFramebuffer(const WindowState& window);

	void onMouse(const MouseEvent& mouse);
	void onKeyboard(const KeyboardEvent& keyboard);

	void updateInput() { internalUpdateInput(); };
	void draw() { internalDraw(); };

protected:
	explicit RhiApplication() = default;
	RhiApplication(
		std::string_view name,
		Environment&& env,
		CreateWindowFunc createWindowFunc,
		WindowState& window);

	template <GraphicsApi G>
	Rhi<G>& internalRhi();

	template <GraphicsApi G>
	const Rhi<G>& internalRhi() const;

private:
	void internalUpdateInput();
	void internalDraw();

	std::shared_ptr<void> myRhi;
	ConcurrentQueue<MouseEvent> myMouseQueue;
	ConcurrentQueue<KeyboardEvent> myKeyboardQueue;
};
