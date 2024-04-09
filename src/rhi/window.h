#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "image.h"
#include "pipeline.h"
#include "swapchain.h"

#include <core/capi.h>
#include <core/file.h>
#include <core/utils.h>

#include <gfx/glm.h>
#include <gfx/view.h>

#include <array>
#include <bitset>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

template <GraphicsApi G>
struct WindowConfiguration
{
	SwapchainConfiguration<G> swapchainConfig;
	Extent2d<G> windowExtent;
	Extent2d<G> splitScreenGrid;
};

template <GraphicsApi G>
class Window final : public Swapchain<G>
{
public:
	constexpr Window() noexcept = default;
	Window(
		const std::shared_ptr<Device<G>>& device,
		SurfaceHandle<G>&& surface, // swapchain base class takes ownership of surface
		WindowConfiguration<G>&& defaultConfig = {});
	Window(Window&& other) noexcept;
	~Window();

	Window& operator=(Window&& other) noexcept;

	void swap(Window& rhs) noexcept;
	friend void swap(Window& lhs, Window& rhs) noexcept { lhs.swap(rhs); }

	auto& config() noexcept { return myConfig; }
	const auto& config() const noexcept { return myConfig; }

	const auto& getViews() const noexcept { return myViews; }
	const auto& getActiveView() const noexcept { return myActiveView; }
	const auto& getViewBuffer(uint8_t index) const noexcept { return myViewBuffers[index]; }

	void onResizeWindow(Extent2d<G> windowExtent) { myConfig.windowExtent = windowExtent; }
	void onResizeFramebuffer(uint32_t width, uint32_t height);

	void onMouse(const MouseState& mouse);
	void onKeyboard(const KeyboardState& keyboard);

	// todo: generalize, move out of window. use sorted draw call lists.
	uint32_t draw(
		Pipeline<Vk>& pipeline,
		Queue<Vk>& queue,
		RenderPassBeginInfo<Vk>&& renderPassInfo);
	//

private:
	void internalUpdateViewBuffer() const;
	void internalCreateFrameObjects(Extent2d<G> frameBufferExtent);
	void internalUpdateInput();

	uint32_t internalDrawViews(
		Pipeline<G>& pipeline,
		Queue<G>& queue,
		RenderPassBeginInfo<G>&& renderPassInfo);

	file::Object<WindowConfiguration<G>, file::AccessMode::ReadWrite, true> myConfig;
	struct InputState
	{
		std::bitset<512> keysPressed;
		struct MouseState
		{
			float position[2][2];
			uint8_t leftPressed : 1;
			uint8_t rightPressed : 1;
			uint8_t hoverScreen : 1;
		} mouse;
	} myInput{};
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<G>[]> myViewBuffers; // cbuffer data for all views
};
