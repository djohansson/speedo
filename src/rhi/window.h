#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "image.h"
#include "pipeline.h"
#include "swapchain.h"

#include <core/capi.h>
#include <core/file.h>
#include <core/inputstate.h>
#include <core/utils.h>

#include <gfx/glm_extra.h>
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
	SwapchainConfiguration<G> swapchainConfig{};
	glm::vec2 contentScale = glm::vec2(1.0f, 1.0);
	Extent2d<G> splitScreenGrid{1, 1}; // todo: replace with view list
	bool fullscreen{false};
};

template <GraphicsApi G>
class Window final : public Swapchain<G>
{
public:
	using ConfigFile = file::Object<WindowConfiguration<G>, file::AccessMode::kReadWrite, true>;

	constexpr Window() noexcept = default;
	Window(
		const std::shared_ptr<Device<G>>& device,
		SurfaceHandle<G>&& surface, // swapchain base class takes ownership of surface
		ConfigFile&& config,
		WindowState&& state);
	Window(Window&& other) noexcept;
	~Window();

	Window& operator=(Window&& other) noexcept;

	void Swap(Window& rhs) noexcept;
	friend void Swap(Window& lhs, Window& rhs) noexcept { lhs.Swap(rhs); }

	const auto& GetConfig() const noexcept { return myConfig; }
	const auto& GetViews() const noexcept { return myViews; }
	const auto& GetActiveView() const noexcept { return myActiveView; }
	const auto& GetViewBuffer(uint8_t index) const noexcept { return myViewBuffers[index]; }
	auto& GetState() noexcept { return myState; }
	const auto& GetState() const noexcept { return myState; }

	void OnInputStateChanged(const InputState& input);
	void OnResizeFramebuffer(int w, int h);
	void OnResizeSplitScreenGrid(uint32_t width, uint32_t height);

	// todo: generalize, move out of window. use sorted draw call lists.
	uint32_t Draw(
		Pipeline<G>& pipeline,
		Queue<G>& queue,
		RenderPassBeginInfo<G>&& renderPassInfo);
	//

private:
	
	void InternalUpdateViewBuffer() const;
	void InternalInitializeViews();
	void InternalUpdateViews(const InputState& input);

	uint32_t InternalDrawViews(
		Pipeline<G>& pipeline,
		Queue<G>& queue,
		RenderPassBeginInfo<G>&& renderPassInfo);

	file::Object<WindowConfiguration<G>, file::AccessMode::kReadWrite, true> myConfig;
	WindowState myState{};
	std::unique_ptr<Buffer<G>[]> myViewBuffers; // cbuffer data for all views
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
};
