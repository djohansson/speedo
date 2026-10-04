#pragma once

#include <rhi/buffer.h>
#include <rhi/pipeline.h>
#include <rhi/swapchain.h>

#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace rhi
{

template <GraphicsApi G>
class Window;

template <GraphicsApi G>
struct WindowCreateDesc final : DeviceObjectCreateDesc<G>
{
	WindowHandle window{};
	glm::vec2 contentScale = glm::vec2(1.F, 1.F);
	bool fullscreen{false};
};

template <GraphicsApi G>
struct ObjectTraits<Window<G>>
{
	using CreateDescType = WindowCreateDesc<G>;
};

template <GraphicsApi G>
class Window final : public DeviceObject<Window<G>>
{
public:
	using SuperType = DeviceObject<Window<G>>;
	using CreateDescType = ObjectTraits<Window<G>>::CreateDescType;

	constexpr Window() noexcept = default;
	Window(WindowCreateDesc<G>&& desc, SwapchainCreateDesc<G>&& swapchainDesc, WindowState&& state);
	Window(Window&& other) noexcept;
	~Window();

	[[maybe_unused]] Window& operator=(Window&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return this->GetDesc().window; }//NOLINT(google-explicit-constructor)

	void Swap(Window& rhs) noexcept;
	friend void Swap(Window& lhs, Window& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] auto& GetSwapchain() noexcept { return mySwapchain; }
	[[nodiscard]] const auto& GetSwapchain() const noexcept { return mySwapchain; }
	[[nodiscard]] auto& GetState() noexcept { return myState; }
	[[nodiscard]] const auto& GetState() const noexcept { return myState; }
	// true while the framebuffer has no area (minimized): there is nothing to draw to
	[[nodiscard]] bool IsMinimized() const noexcept { return myMinimized; }
	void SetMinimized(bool minimized) noexcept { myMinimized = minimized; }

	// recreates the swapchain at the framebuffer's current size
	void OnResizeFramebuffer(int width, int height);

private:
	WindowState myState{};
	Swapchain<G> mySwapchain{};
	bool myMinimized{};
};

namespace window
{

// mirrors nfdu8filteritem_t, so the nfd header stays in the implementation
struct FileFilter
{
	const char* name; // shown in the dialogue
	const char* spec; // comma separated extensions, e.g. "jpg,png"
};

[[nodiscard]] std::tuple<bool, std::string> OpenFileDialogue(std::string&& resourcePathString, const std::vector<FileFilter>& filterList);

} // namespace window

} // namespace rhi
