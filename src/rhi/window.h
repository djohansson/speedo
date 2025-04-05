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
#include <core/concurrentaccess.h>
#include <core/utils.h>

#include <gfx/glm_extra.h>
#include <gfx/camera.h>

#include <array>
#include <bitset>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nfd.h> // todo: move to implementation

template <GraphicsApi G>
struct WindowConfiguration
{
	SwapchainConfiguration<G> swapchainConfig{};
	glm::vec2 contentScale = glm::vec2(1.0f, 1.0);
	Extent2d<G> splitScreenGrid{1, 1}; // todo: replace with view list
	std::string imguiIniSettings;
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
		WindowHandle&& window, // window class takes ownership of window handle
		SurfaceHandle<G>&& surface, // swapchain base class takes ownership of surface
		ConfigFile&& config,
		WindowState&& state);
	Window(Window&& other) noexcept;
	~Window();

	[[maybe_unused]] Window& operator=(Window&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myWindow; }

	void Swap(Window& rhs) noexcept;
	friend void Swap(Window& lhs, Window& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] auto& GetConfig() noexcept { return myConfig; }
	[[nodiscard]] const auto& GetConfig() const noexcept { return myConfig; }
	[[nodiscard]] auto GetCameras() const noexcept { return ConcurrentReadScope(myCameras); }
	[[nodiscard]] const auto& GetActiveViewIndex() const noexcept { return myActiveCamera; }
	[[nodiscard]] const auto& GetViewBuffer(uint8_t index) const noexcept { return myViewBuffers[index]; }
	[[nodiscard]] auto& GetState() noexcept { return myState; }
	[[nodiscard]] const auto& GetState() const noexcept { return myState; }

	void OnInputStateChanged(const InputState& input);
	void OnResizeFramebuffer(int w, int h);
	void OnResizeSplitScreenGrid(uint32_t width, uint32_t height);

	void UpdateViewBuffer() const { InternalUpdateViewBuffer(); }

private:
	void InternalUpdateViewBuffer() const;
	void InternalInitializeViews();
	void InternalUpdateViews(const InputState& input);

	[[nodiscard]] uint32_t InternalDrawViews(
		Pipeline<G>& pipeline,
		Queue<G>& queue,
		const RenderingInfo<G>& renderInfo);

	file::Object<WindowConfiguration<G>, file::AccessMode::kReadWrite, true> myConfig;
	WindowHandle myWindow{};
	WindowState myState{};
	std::vector<Buffer<G>> myViewBuffers; // cbuffer data for all views
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
	ConcurrentAccess<std::vector<Camera>> myCameras;
	std::optional<size_t> myActiveCamera;
};

namespace window
{

std::tuple<bool, std::string> OpenFileDialogue(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList);

} // namespace window