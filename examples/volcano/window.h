#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "file.h"
#include "glm.h"
#include "image.h"
#include "pipeline.h"
#include "state.h"
#include "swapchain.h"
#include "utils.h"
#include "view.h"

#include <array>
#include <chrono>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

template <GraphicsBackend B>
struct WindowConfiguration : SwapchainConfiguration<B>
{
	Extent2d<B> windowExtent = {};
	Extent2d<B> splitScreenGrid = { 1, 1 };
};

template <GraphicsBackend B>
class WindowContext : public Swapchain<B>
{
public:

	constexpr WindowContext() noexcept = default;
	WindowContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		SurfaceHandle<B>&& surface, // swapchain base class takes ownership of surface
		WindowConfiguration<B>&& defaultConfig = {});
	WindowContext(WindowContext&& other) noexcept;
	~WindowContext();

	WindowContext& operator=(WindowContext&& other) noexcept;

    void swap(WindowContext& rhs) noexcept;
    friend void swap(WindowContext& lhs, WindowContext& rhs) noexcept { lhs.swap(rhs); }

	const auto& getConfig() const noexcept { return myConfig; }
	const auto& getViews() const noexcept { return myViews; }
	const auto& getActiveView() const noexcept { return myActiveView; }
	const auto& getViewBuffer() const noexcept { return *myViewBuffer; }

	void onResizeWindow(Extent2d<B> windowExtent) { myConfig.windowExtent = windowExtent;	}
	void onResizeFramebuffer(Extent2d<B> framebufferExtent);

	void updateInput(const InputState& input);

	// todo: generalize, move out of window. use sorted draw call lists.
	void draw(
		PipelineContext<B>& pipeline,
		CommandPoolContext<B>& primaryContext,
		CommandPoolContext<Vk>* secondaryContexts,
		uint32_t secondaryContextCount);
	//

private:

	void internalUpdateViewBuffer(uint8_t frameIndex) const;
	void internalCreateFrameObjects(Extent2d<B> frameBufferExtent);

	uint32_t internalDrawViews(
		PipelineContext<B>& pipeline,
		CommandPoolContext<Vk>* secondaryContexts,
		uint32_t secondaryContextCount,
		const RenderPassBeginInfo<B>& renderPassInfo,
		uint8_t frameIndex);

	AutoSaveJSONFileObject<WindowConfiguration<B>> myConfig = {};
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
};

#include "window.inl"
