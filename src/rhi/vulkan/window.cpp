#include "../window.h"
#include "../rhiapplication.h"
#include "../shaders/capi.h"

#include "utils.h"

#include <GLFW/glfw3.h>

#include <imgui.h>

#include <cmath>
#include <execution>
#include <filesystem>
#include <format>
#include <future>
#include <numeric>
#include <string>
#include <string_view>

template <>
void Window<kVk>::InternalUpdateViewBuffer() const
{
	ZoneScopedN("Window::InternalUpdateViewBuffer");

	auto* bufferMemory = myViewBuffers[GetCurrentFrameIndex()].GetMemory();
	void* data;
	VK_CHECK(vmaMapMemory(InternalGetDevice()->GetAllocator(), bufferMemory, &data));

	auto* viewDataPtr = static_cast<ViewData*>(data);
	auto viewCount = (myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);
	ASSERT(viewCount <= SHADER_TYPES_VIEW_COUNT);
	for (uint32_t viewIt = 0UL; viewIt < viewCount; viewIt++)
	{
		auto mvp = myViews[viewIt].GetProjectionMatrix() * glm::mat4(myViews[viewIt].GetViewMatrix());
#if GLM_ARCH & GLM_ARCH_AVX_BIT
		_mm256_stream_ps(&viewDataPtr->viewProjection[0][0], _mm256_set_m128(mvp[0], mvp[1]));
		_mm256_stream_ps(&viewDataPtr->viewProjection[2][0], _mm256_set_m128(mvp[2], mvp[3]));
#elif GLM_ARCH & GLM_ARCH_SSE2_BIT
		_mm_stream_ps(&viewDataPtr->viewProjection[0][0], mvp[0]);
		_mm_stream_ps(&viewDataPtr->viewProjection[1][0], mvp[1]);
		_mm_stream_ps(&viewDataPtr->viewProjection[2][0], mvp[2]);
		_mm_stream_ps(&viewDataPtr->viewProjection[3][0], mvp[3]);
#else
		std::copy_n(&mvp[0][0], 16, &viewDataPtr->viewProjection[0][0]);
#endif
		viewDataPtr++;
	}

	vmaFlushAllocation(
		InternalGetDevice()->GetAllocator(),
		bufferMemory,
		0,
		viewCount * sizeof(ViewData));

	vmaUnmapMemory(InternalGetDevice()->GetAllocator(), bufferMemory);
}

template <>
void Window<kVk>::InternalInitializeViews()
{
	myViews.resize(myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);

	unsigned width = myConfig.swapchainConfig.extent.width / myConfig.splitScreenGrid.width;
	unsigned height = myConfig.swapchainConfig.extent.height / myConfig.splitScreenGrid.height;

	for (unsigned j = 0; j < myConfig.splitScreenGrid.height; j++)
		for (unsigned i = 0; i < myConfig.splitScreenGrid.width; i++)
		{
			auto& view = myViews[j * myConfig.splitScreenGrid.width + i];
			view.GetDesc().viewport.x = i * width;
			view.GetDesc().viewport.y = j * height;
			view.GetDesc().viewport.width = width;
			view.GetDesc().viewport.height = height;
			view.UpdateAll();
		}
}

template <>
void Window<kVk>::OnResizeFramebuffer(int width, int height)
{
	ASSERT(width > 0);
	ASSERT(height > 0);

	auto& device = *InternalGetDevice();
	auto& instance = *device.GetInstance();
	auto* surface = GetSurface();
	auto* physicalDevice = device.GetPhysicalDevice();

	instance.UpdateSurfaceCapabilities(physicalDevice, surface);

	myConfig.swapchainConfig.extent = instance.GetSwapchainInfo(physicalDevice, surface).capabilities.currentExtent;

	ASSERT(myConfig.contentScale.x == myState.xscale);
	ASSERT(myConfig.contentScale.y == myState.yscale);

	myState.width = myConfig.swapchainConfig.extent.width / myState.xscale;
	myState.height = myConfig.swapchainConfig.extent.height / myState.yscale;

	InternalCreateSwapchain(myConfig.swapchainConfig, *this);
	InternalInitializeViews();
}

template <>
void Window<kVk>::OnResizeSplitScreenGrid(uint32_t width, uint32_t height)
{
	myConfig.splitScreenGrid = Extent2d<kVk>{width, height};

	InternalInitializeViews();
}

template <>
void Window<kVk>::InternalUpdateViews(const InputState& input)
{
	ZoneScopedN("Window::InternalUpdateViews");

	myTimestamps[1] = myTimestamps[0];
	myTimestamps[0] = std::chrono::high_resolution_clock::now();

	float dt = (myTimestamps[1] - myTimestamps[0]).count();

	if (input.mouse.insideWindow && !input.mouse.leftDown)
	{
		// todo: generic view index calculation
		size_t viewIdx = myConfig.splitScreenGrid.width * input.mouse.position[0] / (myConfig.swapchainConfig.extent.width / myConfig.contentScale.x);
		size_t viewIdy = myConfig.splitScreenGrid.height * input.mouse.position[1] / (myConfig.swapchainConfig.extent.height / myConfig.contentScale.y);
		myActiveView = std::min((viewIdy * myConfig.splitScreenGrid.width) + viewIdx, myViews.size() - 1);

		//std::cout << *myActiveView << ":[" << input.mouse.position[0] << ", " << input.mouse.position[1] << "]" << '\n';
	}
	else if (!input.mouse.leftDown)
	{
		myActiveView.reset();

		//std::cout << "myActiveView.reset()" << '\n';
	}

	if (myActiveView)
	{
		//std::cout << "window.myActiveView read/consume" << '\n';

		float dx = 0.F;
		float dz = 0.F;

		// todo: make a bitset iterator, and use a range based for loop here, use <bit> for __cpp_lib_bitops
		for (unsigned key = 0; key < input.keyboard.keysDown.size(); key++)
		{
			if (input.keyboard.keysDown[key])
			{
				switch (key)
				{
				case GLFW_KEY_W:
					dz = -1;
					break;
				case GLFW_KEY_S:
					dz = 1;
					break;
				case GLFW_KEY_A:
					dx = -1;
					break;
				case GLFW_KEY_D:
					dx = 1;
					break;
				default:
					break;
				}
			}
		}

		auto& view = myViews[*myActiveView];

		bool doUpdateViewMatrix = false;

		if (dx != 0 || dz != 0)
		{
			const auto& viewMatrix = view.GetViewMatrix();
			auto forward = glm::vec3(viewMatrix[0][2], viewMatrix[1][2], viewMatrix[2][2]);
			auto strafe = glm::vec3(viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]);

			constexpr auto kMoveSpeed = 0.000000002F;

			view.GetDesc().cameraPosition += dt * (dz * forward + dx * strafe) * kMoveSpeed;

			// std::cout << *myActiveView << ":pos:[" << view.GetDesc().cameraPosition.x << ", " <<
			//     view.GetDesc().cameraPosition.y << ", " << view.GetDesc().cameraPosition.z << "]" << '\n';

			doUpdateViewMatrix = true;
		}

		if (input.mouse.leftDown)
		{
			constexpr auto kRotSpeed = 0.00000001F;

			const float windowWidth = view.GetDesc().viewport.width / myConfig.contentScale.x;
			const float windowHeight = view.GetDesc().viewport.height / myConfig.contentScale.y;
			const float cx = std::fmod(input.mouse.leftLastEventPosition[0], windowWidth);
			const float cy = std::fmod(input.mouse.leftLastEventPosition[1], windowHeight);
			const float px = std::fmod(input.mouse.position[0], windowWidth);
			const float py = std::fmod(input.mouse.position[1], windowHeight);

			//std::cout << "cx:" << cx << ", cy:" << cy << '\n';

			float dM[2] = {cx - px, cy - py};

			//std::cout << "dM[0]:" << dM[0] << ", dM[1]:" << dM[1] << '\n';

			view.GetDesc().cameraRotation +=
				dt * glm::vec3(dM[1] / windowHeight, dM[0] / windowWidth, 0.0F) * kRotSpeed;

			// std::cout << *myActiveView << ":rot:[" << view.GetDesc().cameraRotation.x << ", " <<
			//     view.GetDesc().cameraRotation.y << ", " << view.GetDesc().cameraRotation.z << "]" << '\n';

			doUpdateViewMatrix = true;
		}

		if (doUpdateViewMatrix)
		{
			myViews[*myActiveView].UpdateViewMatrix();
		}
	}
}

template <>
void Window<kVk>::OnInputStateChanged(const InputState& input)
{
	InternalUpdateViews(input);
}

template <>
Window<kVk>::Window(
	const std::shared_ptr<Device<kVk>>& device,
	SurfaceHandle<kVk>&& surface,
	ConfigFile&& config,
	WindowState&& state)
	: Swapchain(
		device,
		config.swapchainConfig,
		std::forward<SurfaceHandle<kVk>>(surface), VK_NULL_HANDLE)
	, myConfig(std::forward<ConfigFile>(config))
	, myState(std::forward<WindowState>(state))
	, myViewBuffers(std::make_unique<Buffer<kVk>[]>(SHADER_TYPES_FRAME_COUNT))
{
	ZoneScopedN("Window()");

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		myViewBuffers[i] = Buffer<kVk>(
			InternalGetDevice(),
			BufferCreateDesc<kVk>{
				SHADER_TYPES_VIEW_COUNT * sizeof(ViewData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});
	}

	InternalInitializeViews();

	myTimestamps[0] = std::chrono::high_resolution_clock::now();
}

template <>
Window<kVk>::Window(Window&& other) noexcept
	: Swapchain(std::forward<Window>(other))
	, myConfig(std::exchange(other.myConfig, {}))
	, myState(std::exchange(other.myState, {}))
	, myViewBuffers(std::exchange(other.myViewBuffers, {}))
	, myTimestamps(std::exchange(other.myTimestamps, {}))
	, myViews(std::exchange(other.myViews, {}))
	, myActiveView(std::exchange(other.myActiveView, {}))
{}

template <>
Window<kVk>::~Window()
{
	ZoneScopedN("~Window()");
}

template <>
Window<kVk>& Window<kVk>::operator=(Window&& other) noexcept
{
	Swapchain::operator=(std::forward<Window>(other));
	myConfig = std::exchange(other.myConfig, {});
	myState = std::exchange(other.myState, {});
	myViewBuffers = std::exchange(other.myViewBuffers, {});
	myTimestamps = std::exchange(other.myTimestamps, {});
	myViews = std::exchange(other.myViews, {});
	myActiveView = std::exchange(other.myActiveView, {});
	return *this;
}

template <>
void Window<kVk>::Swap(Window& other) noexcept
{
	Swapchain::Swap(other);
	std::swap(myConfig, other.myConfig);
	std::swap(myState, other.myState);
	std::swap(myViewBuffers, other.myViewBuffers);
	std::swap(myTimestamps, other.myTimestamps);
	std::swap(myViews, other.myViews);
	std::swap(myActiveView, other.myActiveView);
}
