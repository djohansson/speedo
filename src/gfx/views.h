#pragma once

#include <gfx/bounds.h>
#include <gfx/camera.h>
#include <gfx/gpu.h>

#include <core/concurrentaccess.h>
#include <core/inputstate.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace gfx
{

// the views of a window, split screen in a grid: a camera each, controlled with the mouse and keyboard over its view,
// and the buffers (one per frame) the shaders read the views' view projections from
class Views final
{
public:
	Views(Device& device, glm::uvec2 framebufferExtent);
	Views(const Views&) = delete;
	Views(Views&&) noexcept = delete;
	~Views();

	Views& operator=(const Views&) = delete;
	Views& operator=(Views&&) noexcept = delete;

	// the grid's width and height in views. read on the draw thread, which is the only one that changes it.
	[[nodiscard]] glm::uvec2 GetGrid() const noexcept { return myGrid; }
	[[nodiscard]] const Buffer& GetBuffer(size_t frameIndex) const noexcept { return myBuffers[frameIndex]; }

	// lays the views out over the framebuffer again, e.g. after a resize. call on the draw thread.
	void OnResizeFramebuffer(glm::uvec2 framebufferExtent);
	// changes the grid. call on the draw thread.
	void OnResizeGrid(glm::uvec2 grid);
	// moves the active view's camera (the one under the mouse, or being dragged) by the input, and scales the movement
	// speed with the scroll wheel. the views then need uploading (see UpdateBuffers).
	void OnInputStateChanged(const core::InputState& input);

	// moves every camera back from bounds (looking down -z) until all of it is in view, fits the near and far planes to
	// its size, and sets the movement speed after it (see GetMoveSpeed). call on the draw thread.
	void FrameBounds(const Bounds3f& bounds);

	// how fast the cameras move (with w, a, s, d), in units per second: a quarter of the framed bounds' radius, so
	// crossing them takes the same time whatever the scale of the model. any thread.
	[[nodiscard]] float GetMoveSpeed() const noexcept { return myMoveSpeed.load(std::memory_order_relaxed); }
	void SetMoveSpeed(float speed) noexcept;

	// uploads the views' view projections. call on the draw thread.
	void UpdateBuffers();

private:
	void InternalLayout();

	std::vector<Buffer> myBuffers;
	core::ConcurrentAccess<std::vector<gfx::Camera>> myCameras;
	glm::uvec2 myFramebufferExtent{};
	glm::uvec2 myGrid{1, 1};
	std::optional<size_t> myActiveCamera;
	static constexpr float kDefaultMoveSpeed = 5.0F; // until a model is framed

	std::atomic<float> myMoveSpeed{kDefaultMoveSpeed};
};

} // namespace gfx
