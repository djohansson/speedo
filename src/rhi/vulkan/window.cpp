#include <rhi/window.h>
#include <rhi/rhiapplication.h>
#include <rhi/shaders/capi.h>

#include <rhi/vulkan/utils.h>

#include <GLFW/glfw3.h>

#include <imgui.h>

#include <string_view>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Window<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Window<kVk>);

template <>
void Window<kVk>::InternalUpdateViewBuffer()
{
	ZoneScopedN("Window::InternalUpdateViewBuffer");

	for (const auto& frame : mySwapchain.GetFrames())
	{
		auto* bufferMemory = myViewBuffers[frame.GetDesc().index].GetMemory();
		void* data;
		VK_CHECK(vmaMapMemory(GetDevice().GetAllocator(), bufferMemory, &data));
		ENSURE(data != nullptr);

		auto* viewDataPtr = static_cast<ViewData*>(data);
		auto viewCount = (GetDesc().splitScreenGrid.width * GetDesc().splitScreenGrid.height);
		ENSURE(viewCount <= SHADER_TYPES_VIEW_COUNT);
		constexpr size_t kMatrixElementCount = 16;
		auto cameras = myCameras.Read();
		for (uint32_t viewIt = 0UL; viewIt < viewCount; viewIt++)
		{
			auto mvp = cameras.Get()[viewIt].GetProjectionMatrix() * cameras.Get()[viewIt].GetViewMatrix();
			auto* vpDst = viewDataPtr->viewProjection;
	#if (GLM_ARCH & GLM_ARCH_AVX_BIT) && (SPEEDO_GRAPHICS_VALIDATION_LEVEL <= 1)
			_mm256_stream_ps(&vpDst[0][0], _mm256_insertf128_ps(_mm256_castps128_ps256(mvp[0].data), mvp[1].data, 1));
			_mm256_stream_ps(&vpDst[2][0], _mm256_insertf128_ps(_mm256_castps128_ps256(mvp[2].data), mvp[3].data, 1));
	#elif (GLM_ARCH & GLM_ARCH_SSE2_BIT) && (SPEEDO_GRAPHICS_VALIDATION_LEVEL <= 1)
			_mm_stream_ps(&vpDst[0][0], mvp[0].data);
			_mm_stream_ps(&vpDst[1][0], mvp[1].data);
			_mm_stream_ps(&vpDst[2][0], mvp[2].data);
			_mm_stream_ps(&vpDst[3][0], mvp[3].data);
	#else
			std::copy_n(&mvp[0][0], kMatrixElementCount, &vpDst[0][0]);
	#endif
			viewDataPtr++;
		}

		vmaFlushAllocation(
			GetDevice().GetAllocator(),
			bufferMemory,
			0,
			viewCount * sizeof(ViewData));

		vmaUnmapMemory(GetDevice().GetAllocator(), bufferMemory);
	}
}

template <>
void Window<kVk>::InternalInitializeViews(std::optional<Extent2d<kVk>> splitScreenGrid)
{
	auto cameras = myCameras.Write();

	// under the cameras lock, since input handling reads the grid with it held (see InternalUpdateViews)
	if (splitScreenGrid)
		InternalGetDesc().splitScreenGrid = *splitScreenGrid;

	cameras.Get().resize(static_cast<size_t>(GetDesc().splitScreenGrid.width) * static_cast<size_t>(GetDesc().splitScreenGrid.height));

	unsigned width = GetSwapchain().GetDesc().extent.width / GetDesc().splitScreenGrid.width;
	unsigned height = GetSwapchain().GetDesc().extent.height / GetDesc().splitScreenGrid.height;

	for (unsigned j = 0; j < GetDesc().splitScreenGrid.height; j++)
		for (unsigned i = 0; i < GetDesc().splitScreenGrid.width; i++)
		{
			auto& cam = cameras.Get()[(j * GetDesc().splitScreenGrid.width) + i];
			cam.GetDesc().viewport.x = i * width;
			cam.GetDesc().viewport.y = j * height;
			cam.GetDesc().viewport.width = width;
			cam.GetDesc().viewport.height = height;
			cam.UpdateAll();
		}
}

template <>
void Window<kVk>::OnResizeFramebuffer(int width, int height)
{
	ASSERT(width > 0);
	ASSERT(height > 0);
	ASSERT(GetDesc().contentScale.x == myState.xscale);
	ASSERT(GetDesc().contentScale.y == myState.yscale);

	mySwapchain.CreateSwapchain();

	myState.width = static_cast<uint32_t>(static_cast<float>(mySwapchain.GetDesc().extent.width) / myState.xscale);
	myState.height = static_cast<uint32_t>(static_cast<float>(mySwapchain.GetDesc().extent.height) / myState.yscale);

	InternalInitializeViews();
}

template <>
void Window<kVk>::OnResizeSplitScreenGrid(uint32_t width, uint32_t height)
{
	InternalInitializeViews(Extent2d<kVk>{.width = width, .height = height});
}

template <>
void Window<kVk>::InternalUpdateViews(const core::InputState& input)
{
	ZoneScopedN("Window::InternalUpdateViews");

	auto cameras = myCameras.Write();

	// mouse positions are in framebuffer pixels (converted by the client), like the swapchain extent and viewports
	if (input.mouse.insideWindow && !input.mouse.leftDown)
	{
		// todo: generic view index calculation
		auto viewIdx = static_cast<size_t>(static_cast<float>(GetDesc().splitScreenGrid.width) * input.mouse.position[0] /
			static_cast<float>(mySwapchain.GetDesc().extent.width));
		auto viewIdy = static_cast<size_t>(static_cast<float>(GetDesc().splitScreenGrid.height) * input.mouse.position[1] /
			static_cast<float>(mySwapchain.GetDesc().extent.height));
		myActiveCamera = std::min((viewIdy * GetDesc().splitScreenGrid.width) + viewIdx, cameras.Get().size() - 1);

		//std::cout << *myActiveCamera << ":[" << input.mouse.position[0] << ", " << input.mouse.position[1] << "]" << '\n';
	}
	else if (!input.mouse.leftDown)
	{
		myActiveCamera.reset();

		//std::cout << "myActiveCamera.reset()" << '\n';
	}

	if (myActiveCamera)
	{
		//std::cout << "window.myActiveCamera read/consume" << '\n';

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

			// std::cout << *myActiveCamera << ":pos:[" << view.GetDesc().position.x << ", " <<
			//     view.GetDesc().position.y << ", " << view.GetDesc().position.z << "]" << '\n';

			doUpdateViewMatrix = true;
		}

		if (input.mouse.leftDown && input.mouse.position != input.mouse.lastPosition)
		{
			constexpr auto kRotSpeed = 5.0F;

			const float windowWidth = static_cast<float>(view.GetDesc().viewport.width);
			const float windowHeight = static_cast<float>(view.GetDesc().viewport.height);
			// the raw cursor movement: wrapping the positions into the view (fmod) made the delta jump by a whole view
			// size whenever the cursor crossed the window edge (or a split screen view boundary) while dragging
			float deltaMouse[2] = {
				input.mouse.lastPosition[0] - input.mouse.position[0],
				input.mouse.lastPosition[1] - input.mouse.position[1],
			};

			//std::cout << "dM[0]:" << dM[0] << ", dM[1]:" << dM[1] << '\n';

			view.GetDesc().cameraRotation -= //input.dt *
				glm::vec3(deltaMouse[1] / windowHeight, deltaMouse[0] / windowWidth, 0.0F) * kRotSpeed;

			// std::cout << *myActiveCamera << ":rot:[" << view.GetDesc().cameraRotation.x << ", " <<
			//     view.GetDesc().cameraRotation.y << ", " << view.GetDesc().cameraRotation.z << "]" << '\n';

			doUpdateViewMatrix = true;
		}

		if (doUpdateViewMatrix)
		{
			cameras.Get()[*myActiveCamera].UpdateViewMatrix();
		}
	}

	if (auto app = static_pointer_cast<RHIApplication>(core::Application::Get()); app)
	{
		auto [updateViewBufferTask, updateViewBufferFuture] = core::CreateTask(
			[this]() { UpdateViewBuffer(); });
		app->GetRHI<kVk>().drawCalls.enqueue(updateViewBufferTask);
	}
}

template <>
void Window<kVk>::OnInputStateChanged(const core::InputState& input)
{
	InternalUpdateViews(input);
}

template <>
Window<kVk>::Window(
	WindowCreateDesc<kVk>&& desc,
	SwapchainCreateDesc<kVk>&& swapchainDesc,
	WindowState&& state)
	: DeviceObject(std::forward<WindowCreateDesc<kVk>>(desc))
	, myState(std::forward<WindowState>(state))
	, mySwapchain(std::forward<SwapchainCreateDesc<kVk>>(swapchainDesc))
	, myViewBuffers(SHADER_TYPES_FRAME_COUNT)
{
	ZoneScopedN("Window()");

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		myViewBuffers[i] = Buffer<kVk>(
			BufferCreateDesc<kVk>{
				SuperType::CreateDeviceObjectCreateDesc(std::format("Window{}ViewBuffer{}", GetDesc().window, i)),
				SHADER_TYPES_VIEW_COUNT * sizeof(ViewData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});
	}

	InternalInitializeViews();
}

template <>
Window<kVk>::Window(Window&& other) noexcept
	: DeviceObject(std::move(other))
	, myState(std::exchange(other.myState, {}))
	, mySwapchain(std::exchange(other.mySwapchain, {}))
	, myViewBuffers(std::exchange(other.myViewBuffers, {}))
	, myCameras(std::exchange(other.myCameras, {}))
	, myActiveCamera(std::exchange(other.myActiveCamera, {}))
{}

template <>
Window<kVk>::~Window()
{
	ZoneScopedN("~Window()");
}

template <>
void Window<kVk>::Swap(Window& other) noexcept
{
	DeviceObject::Swap(other);
	std::swap(myState, other.myState);
	std::swap(mySwapchain, other.mySwapchain);
	std::swap(myViewBuffers, other.myViewBuffers);
	std::swap(myCameras, other.myCameras);
	std::swap(myActiveCamera, other.myActiveCamera);
	std::swap(myMinimized, other.myMinimized);
}

template <>
Window<kVk>& Window<kVk>::operator=(Window&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
