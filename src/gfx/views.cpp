#include <gfx/views.h>

#include <gfx/shaders/capi.h>

#include <core/profiling.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
		myBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("ViewBuffer{}", frameIt)),
			SHADER_TYPES_VIEW_COUNT * sizeof(ViewData),
			rhi::BufferUsage::kStorage,
			rhi::MemoryProperty::kHostVisible});

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
			// the cell, or the largest rectangle of the camera's aspect ratio centered in it
			auto& camera = cameras.Get()[(j * myGrid.x) + i];
			auto& viewport = camera.GetDesc().viewport;
			auto aspectRatio = camera.GetDesc().aspectRatio;
			auto viewWidth = width;
			auto viewHeight = height;
			if (aspectRatio > 0.0F && height > 0)
			{
				if (static_cast<float>(width) / static_cast<float>(height) > aspectRatio)
					viewWidth = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(height) * aspectRatio)));
				else
					viewHeight = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(width) / aspectRatio)));
			}
			viewport.x = static_cast<uint16_t>((i * width) + ((width - viewWidth) / 2));
			viewport.y = static_cast<uint16_t>((j * height) + ((height - viewHeight) / 2));
			viewport.width = static_cast<uint16_t>(viewWidth);
			viewport.height = static_cast<uint16_t>(viewHeight);
			camera.UpdateAll();
		}
	}
}

std::vector<ViewportCreateDesc> Views::GetViewports() const
{
	auto cameras = myCameras.Read();
	std::vector<ViewportCreateDesc> viewports;
	viewports.reserve(cameras.Get().size());
	for (const auto& camera : cameras.Get())
		viewports.push_back(camera.GetDesc().viewport);
	return viewports;
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
			desc.viewport.type = ViewType::Perspective;
			desc.fovY = CameraCreateDesc{}.fovY;
			desc.aspectRatio = 0.0F;
			desc.farPlane = 4.0F * radius;
			desc.nearPlane = desc.farPlane * 1e-4F;
			camera.UpdateAll();
		}
	}

	// the viewports follow the cameras' aspect ratios
	InternalLayout();

	SetMoveSpeed(0.25F * radius);

	// the views are otherwise only uploaded when the input changes them
	UpdateBuffers();
}

void Views::SetScene(const Bounds3f& bounds, std::vector<SceneCamera> cameras)
{
	bool hasCameras = !cameras.empty();
	{
		auto scene = myScene.Write();
		scene.Get() = Scene{.bounds = bounds, .cameras = std::move(cameras)};
	}
	UseSceneCamera(hasCameras ? std::optional<size_t>(0) : std::nullopt);
}

void Views::UseSceneCamera(std::optional<size_t> cameraIndex)
{
	ZoneScopedN("Views::UseSceneCamera");

	Bounds3f bounds;
	std::optional<SceneCamera> sceneCamera;
	{
		auto scene = myScene.Write();
		if (cameraIndex && *cameraIndex >= scene.Get().cameras.size())
			cameraIndex.reset();
		scene.Get().current = cameraIndex;
		bounds = scene.Get().bounds;
		if (cameraIndex)
			sceneCamera = scene.Get().cameras[*cameraIndex];
	}

	if (!sceneCamera)
	{
		FrameBounds(bounds);
		return;
	}

	InternalApplySceneCamera(*sceneCamera, bounds);
	SetMoveSpeed(0.25F * std::max(bounds.Radius(), 1e-3F));
	UpdateBuffers();
}

void Views::UpdateSceneCameras(std::span<const SceneCamera> cameras)
{
	ZoneScopedN("Views::UpdateSceneCameras");

	Bounds3f bounds;
	std::optional<SceneCamera> sceneCamera;
	{
		auto scene = myScene.Write();
		if (scene.Get().cameras.size() != cameras.size())
			return;
		scene.Get().cameras.assign(cameras.begin(), cameras.end());
		if (auto current = scene.Get().current; current && cameras[*current].animated)
			sceneCamera = cameras[*current];
		bounds = scene.Get().bounds;
	}
	if (!sceneCamera)
		return;

	InternalApplySceneCamera(*sceneCamera, bounds);
	UpdateBuffers();
}

void Views::InternalApplySceneCamera(const SceneCamera& camera, const Bounds3f& bounds)
{
	const auto* sceneCamera = &camera;

	// the views' cameras turn by pitch (about x) and then yaw (about y), looking down -z: forward is
	// (cos(pitch) sin(yaw), -sin(pitch), -cos(pitch) cos(yaw))
	auto eye = glm::vec3(sceneCamera->position[0], sceneCamera->position[1], sceneCamera->position[2]);
	auto forward = glm::vec3(sceneCamera->forward[0], sceneCamera->forward[1], sceneCamera->forward[2]);
	auto pitch = std::asin(std::clamp(-forward.y, -1.0F, 1.0F));
	auto yaw = std::atan2(forward.x, -forward.z);
	// roll (about the view's z, before pitch and yaw, see Camera::UpdateViewMatrix) turns pitch and yaw's up, up0, to
	// cos(roll) * up0 - sin(roll) * right0, which is the file's up
	auto turn = glm::rotate(glm::mat4(1.0F), yaw, glm::vec3(0, -1, 0)) * glm::rotate(glm::mat4(1.0F), pitch, glm::vec3(-1, 0, 0));
	auto up0 = glm::vec3(turn * glm::vec4(0, 1, 0, 0));
	auto right0 = glm::vec3(turn * glm::vec4(1, 0, 0, 0));
	auto up = glm::vec3(sceneCamera->up[0], sceneCamera->up[1], sceneCamera->up[2]);
	auto roll = std::atan2(-glm::dot(up, right0), glm::dot(up, up0));

	// a far plane the file leaves out (an infinite perspective) takes in all of the bounds
	auto radius = std::max(bounds.Radius(), 1e-3F);
	auto farPlane = sceneCamera->zfar > 0.0F ? sceneCamera->zfar : 2.0F * (glm::distance(eye, bounds.Center()) + radius);
	auto nearPlane = sceneCamera->znear > 0.0F ? sceneCamera->znear : farPlane * 1e-4F;

	{
		auto cameras = myCameras.Write();
		for (auto& camera : cameras.Get())
		{
			auto& desc = camera.GetDesc();
			desc.position = -eye;
			desc.cameraRotation = glm::vec3(pitch, yaw, roll);
			desc.aspectRatio = sceneCamera->aspectRatio;
			desc.viewport.type = sceneCamera->orthographic ? ViewType::Orthographic : ViewType::Perspective;
			desc.fovY = sceneCamera->yfov > 0.0F ? sceneCamera->yfov : CameraCreateDesc{}.fovY;
			desc.orthoHalfHeight = sceneCamera->ymag > 0.0F ? sceneCamera->ymag : 1.0F;
			desc.nearPlane = nearPlane;
			desc.farPlane = farPlane;
			camera.UpdateAll();
		}
	}

	// the viewports follow the cameras' aspect ratios
	InternalLayout();
}

std::vector<std::string> Views::GetSceneCameraNames() const
{
	auto scene = myScene.Read();
	std::vector<std::string> names;
	names.reserve(scene.Get().cameras.size());
	for (const auto& camera : scene.Get().cameras)
		names.push_back(camera.name);
	return names;
}

std::optional<size_t> Views::GetSceneCamera() const
{
	return myScene.Read().Get().current;
}

void Views::SetMoveSpeed(float speed) noexcept
{
	// within reach of any scale, and never zero or negative (which the scroll wheel couldn't get out of)
	constexpr float kMinSpeed = 1e-6F;
	constexpr float kMaxSpeed = 1e9F;
	myMoveSpeed.store(std::clamp(speed, kMinSpeed, kMaxSpeed), std::memory_order_relaxed);
}

std::optional<Camera> Views::GetCamera(size_t view) const
{
	auto cameras = myCameras.Read();
	if (view >= cameras.Get().size())
		return std::nullopt;
	return cameras.Get()[view];
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
			auto eye = glm::inverse(camera.GetViewMatrix())[3];
			std::copy_n(&eye[0], 4, views[viewIt].eyePosition);
			auto inverseViewProjection = glm::inverse(viewProjection);
			std::copy_n(&inverseViewProjection[0][0], 16, &views[viewIt].inverseViewProjection[0][0]); //NOLINT(readability-magic-numbers)
			const auto& viewport = camera.GetDesc().viewport;
			views[viewIt].viewport[0] = viewport.x;
			views[viewIt].viewport[1] = viewport.y;
			views[viewIt].viewport[2] = viewport.width;
			views[viewIt].viewport[3] = viewport.height;
		}
		buffer.Flush(0, viewCount * sizeof(ViewData));
		buffer.Unmap();
	}
}

void Views::OnInputStateChanged(const core::InputState& input)
{
	ZoneScopedN("Views::OnInputStateChanged");

	// each notch of the wheel up makes the cameras 20% faster, down slower. trackpads scroll in fractions of notches.
	if (input.mouse.insideWindow && input.mouse.scroll[1] != 0.0F)
	{
		constexpr float kSpeedStep = 1.2F;
		SetMoveSpeed(GetMoveSpeed() * std::pow(kSpeedStep, input.mouse.scroll[1]));
	}

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

		constexpr auto kSecondsPerTick = 1e-9F; // input.dt is in nanoseconds

		view.GetDesc().position += input.dt * kSecondsPerTick * (deltaZ * forward + deltaX * strafe) * GetMoveSpeed();

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
