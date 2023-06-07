#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "image.h"
#include "pipeline.h"
#include "swapchain.h"

#include "../client/state.h" // temp - remove & clean up

#include <core/file.h>
#include <core/glm.h>
#include <core/utils.h>
#include <core/view.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

template <GraphicsBackend B>
struct WindowConfiguration : SwapchainConfiguration<B>
{
	Extent2d<B> windowExtent{};
	Extent2d<B> splitScreenGrid{1, 1};
};

template <GraphicsBackend B>
class Window : public Swapchain<B>
{
public:
	constexpr Window() noexcept = default;
	Window(
		const std::shared_ptr<Device<B>>& device,
		SurfaceHandle<B>&& surface, // swapchain base class takes ownership of surface
		WindowConfiguration<B>&& defaultConfig = {});
	Window(Window&& other) noexcept;
	~Window();

	Window& operator=(Window&& other) noexcept;

	void swap(Window& rhs) noexcept;
	friend void swap(Window& lhs, Window& rhs) noexcept { lhs.swap(rhs); }

	const auto& getConfig() const noexcept { return myConfig; }
	const auto& getViews() const noexcept { return myViews; }
	const auto& getActiveView() const noexcept { return myActiveView; }
	const auto& getViewBuffer() const noexcept { return *myViewBuffer; }

	void onResizeWindow(Extent2d<B> windowExtent) { myConfig.windowExtent = windowExtent; }
	void onResizeFramebuffer(Extent2d<B> framebufferExtent);

	void updateInput(const InputState& input);

	// todo: generalize, move out of window. use sorted draw call lists.
	void draw(
		TaskExecutor& executor,
		Pipeline<B>& pipeline,
		CommandPoolContext<B>& primaryContext,
		CommandPoolContext<Vk>* secondaryContexts,
		uint32_t secondaryContextCount);
	//

private:
	void internalUpdateViewBuffer() const;
	void internalCreateFrameObjects(Extent2d<B> frameBufferExtent);

	uint32_t internalDrawViews(
		Pipeline<B>& pipeline,
		CommandPoolContext<Vk>* secondaryContexts,
		uint32_t secondaryContextCount,
		const RenderPassBeginInfo<B>& renderPassInfo);

	AutoSaveJSONFileObject<WindowConfiguration<B>> myConfig;
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
};

#include "window.inl"
