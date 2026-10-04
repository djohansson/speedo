// vulkan specific: the view buffers' usage and memory flags (see gfx/gpu.h)
#include <gfx/views.h>

#include <rhi/shaders/capi.h>

#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <format>

#include <GLFW/glfw3.h>

namespace gfx
{

Views::Views(Device& device, glm::uvec2 framebufferExtent)
	: myFramebufferExtent(framebufferExtent)
{
	ZoneScopedN("Views()");

	myBuffers.reserve(SHADER_TYPES_FRAME_COUNT);
	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
		myBuffers.emplace_back(rhi::BufferCreateDesc<rhi::kGraphicsApi>{
			device.CreateDeviceObjectCreateDesc(std::format("ViewBuffer{}", frameIt)),
			SHADER_TYPES_VIEW_COUNT * sizeof(ViewData),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});

	InternalLayout();
}

Views::~Views() = default;

// lays out a camera per view, over the framebuffer
void Views::InternalLayout()
{
	auto cameras = myCameras.Write();

	cameras.Get().resize(static_cast<size_t>(myGrid.x) * myGrid.y);

	auto width = myFramebufferExtent.x / myGrid.x;
	auto height = myFramebufferExtent.y / myGrid.y;

	for (uint32_t j = 0; j < myGrid.y; j++)
	{
		for (uint32_t i = 0; i < myGrid.x; i++)
		{
			auto& camera = cameras.Get()[(j * myGrid.x) + i];
			camera.GetDesc().viewport.x = i * width;
			camera.GetDesc().viewport.y = j * height;
			camera.GetDesc().viewport.width = width;
			camera.GetDesc().viewport.height = height;
			camera.UpdateAll();
		}
	}
}

void Views::OnResizeFramebuffer(glm::uvec2 framebufferExtent)
{
	{
		// input handling reads the extent and grid with the cameras locked
		auto cameras = myCameras.Write();
		myFramebufferExtent = framebufferExtent;
	}
	InternalLayout();
}

void Views::OnResizeGrid(glm::uvec2 grid)
{
	{
		auto cameras = myCameras.Write();
		myGrid = glm::max(grid, glm::uvec2(1));
	}
	InternalLayout();
}

void Views::FrameBounds(const Bounds3f& bounds)
{
	// far enough back for a sphere around the bounds to fit the (75 degree) field of view
	auto radius = std::max(bounds.Radius(), 1e-3F);
	auto eye = bounds.Center() + glm::vec3(0.0F, 0.0F, 1.4F * radius);

	{
		auto cameras = myCameras.Write();
		for (auto& camera : cameras.Get())
		{
			auto& desc = camera.GetDesc();
			desc.position = -eye;
			desc.cameraRotation = glm::vec3(0.0F);
			desc.farPlane = 4.0F * radius;
			desc.nearPlane = desc.farPlane * 1e-4F;
			camera.UpdateAll();
		}
	}

	// the views are otherwise only uploaded when the input changes them
	UpdateBuffers();
}

void Views::UpdateBuffers()
{
	ZoneScopedN("Views::UpdateBuffers");

	auto cameras = myCameras.Read();
	auto viewCount = std::min<size_t>(cameras.Get().size(), SHADER_TYPES_VIEW_COUNT);

	for (auto& buffer : myBuffers)
	{
		auto memory = buffer.Map();
		auto* views = reinterpret_cast<ViewData*>(memory.data());
		for (size_t viewIt = 0; viewIt < viewCount; viewIt++)
		{
			const auto& camera = cameras.Get()[viewIt];
			auto viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
			std::copy_n(&viewProjection[0][0], 16, &views[viewIt].viewProjection[0][0]); //NOLINT(readability-magic-numbers)
		}
		buffer.Flush(0, viewCount * sizeof(ViewData));
		buffer.Unmap();
	}
}

void Views::OnInputStateChanged(const core::InputState& input)
{
	ZoneScopedN("Views::OnInputStateChanged");

	auto cameras = myCameras.Write();

	// mouse positions are in framebuffer pixels (converted by the client), like the framebuffer extent and viewports
	if (input.mouse.insideWindow && !input.mouse.leftDown && myFramebufferExtent.x > 0 && myFramebufferExtent.y > 0)
	{
		auto viewX = static_cast<size_t>(static_cast<float>(myGrid.x) * input.mouse.position[0] / static_cast<float>(myFramebufferExtent.x));
		auto viewY = static_cast<size_t>(static_cast<float>(myGrid.y) * input.mouse.position[1] / static_cast<float>(myFramebufferExtent.y));
		myActiveCamera = std::min((viewY * myGrid.x) + viewX, cameras.Get().size() - 1);
	}
	else if (!input.mouse.leftDown)
	{
		myActiveCamera.reset();
	}

	if (!myActiveCamera)
		return;

	// look up the movement keys directly, rather than scanning every key; opposite keys cancel out
	const auto& keysDown = input.keyboard.keysDown;
	auto axis = [&keysDown](int negativeKey, int positiveKey)
	{
		return static_cast<float>(keysDown[positiveKey]) - static_cast<float>(keysDown[negativeKey]);
	};
	const float deltaX = axis(GLFW_KEY_A, GLFW_KEY_D);
	const float deltaZ = axis(GLFW_KEY_W, GLFW_KEY_S);

	auto& view = cameras.Get()[*myActiveCamera];

	bool doUpdateViewMatrix = false;

	if (deltaX != 0 || deltaZ != 0)
	{
		const auto& viewMatrix = view.GetViewMatrix();
		auto forward = glm::vec3(viewMatrix[0][2], viewMatrix[1][2], viewMatrix[2][2]);
		auto strafe = glm::vec3(viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]);

		constexpr auto kMoveSpeed = 0.000000005F;

		view.GetDesc().position += input.dt * (deltaZ * forward + deltaX * strafe) * kMoveSpeed;

		doUpdateViewMatrix = true;
	}

	if (input.mouse.leftDown && input.mouse.position != input.mouse.lastPosition)
	{
		constexpr auto kRotSpeed = 5.0F;

		const auto windowWidth = static_cast<float>(view.GetDesc().viewport.width);
		const auto windowHeight = static_cast<float>(view.GetDesc().viewport.height);
		// the raw cursor movement: wrapping the positions into the view (fmod) made the delta jump by a whole view
		// size whenever the cursor crossed the window edge (or a split screen view boundary) while dragging
		const std::array deltaMouse{
			input.mouse.lastPosition[0] - input.mouse.position[0],
			input.mouse.lastPosition[1] - input.mouse.position[1],
		};

		view.GetDesc().cameraRotation -= glm::vec3(deltaMouse[1] / windowHeight, deltaMouse[0] / windowWidth, 0.0F) * kRotSpeed;

		doUpdateViewMatrix = true;
	}

	if (doUpdateViewMatrix)
		view.UpdateViewMatrix();
}

} // namespace gfx
