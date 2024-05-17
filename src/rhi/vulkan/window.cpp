#include "../window.h"
#include "../rhiapplication.h"
#include "../shaders/capi.h"

#include "utils.h"

#include <GLFW/glfw3.h>

#include <imgui.h>

#include <cmath>
#if defined(__WINDOWS__)
#	include <execution>
#endif
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

	auto* bufferMemory = myViewBuffers[GetCurrentFrameIndex()].GetBufferMemory();
	void* data;
	VK_CHECK(vmaMapMemory(InternalGetDevice()->GetAllocator(), bufferMemory, &data));

	auto* viewDataPtr = static_cast<ViewData*>(data);
	auto viewCount = (myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);
	ASSERT(viewCount <= ShaderTypes_ViewCount);
	for (uint32_t viewIt = 0UL; viewIt < viewCount; viewIt++)
	{
		auto mvp = myViews[viewIt].GetProjectionMatrix() * glm::mat4(myViews[viewIt].GetViewMatrix());
		std::copy_n(&mvp[0][0], 16, &viewDataPtr->viewProjection[0][0]);
		viewDataPtr++;
	}

	// vmaFlushAllocation(
	// 	InternalGetDevice()->GetAllocator(),
	// 	bufferMemory,
	// 	0,
	// 	viewCount * sizeof(ViewData));

	vmaUnmapMemory(InternalGetDevice()->GetAllocator(), bufferMemory);
}

template <>
uint32_t Window<kVk>::InternalDrawViews(
	Pipeline<kVk>& pipeline, Queue<kVk>& queue, RenderPassBeginInfo<kVk>&& renderPassInfo)
{
	// setup draw parameters
	uint32_t drawCount = myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height;
	uint32_t drawThreadCount = 0;

	std::atomic_uint32_t drawAtomic = 0UL;

	// draw views using secondary command buffers
	// todo: generalize this to other types of draws
	if (pipeline.GetResources().model)
	{
		ZoneScopedN("Window::drawViews");

		drawThreadCount =
			std::min<uint32_t>(drawCount, std::max(1U, queue.GetPool().GetDesc().levelCount - 1));

		std::array<uint32_t, 128> seq;
		std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
		std::for_each_n(
#if defined(__WINDOWS__)
			std::execution::par,
#endif
			seq.begin(),
			drawThreadCount,
			[&pipeline,
			 &queue,
			 &renderPassInfo,
			 frameIndex = GetCurrentFrameIndex(),
			 &drawAtomic,
			 &drawCount,
			 &desc = myConfig](uint32_t threadIt)
			{
				ZoneScoped;

				auto drawIt = drawAtomic++;
				if (drawIt >= drawCount)
					return;

				auto zoneNameStr = std::format("Window::drawPartition thread:{0}", threadIt);

				ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

				CommandBufferInheritanceInfo<kVk> inheritInfo{
					VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
				inheritInfo.renderPass = renderPassInfo.renderPass;
				inheritInfo.framebuffer = renderPassInfo.framebuffer;

				CommandBufferAccessScopeDesc<kVk> beginInfo{};
				beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
								  VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				beginInfo.pInheritanceInfo = &inheritInfo;
				beginInfo.level = threadIt + 1;

				auto cmd = queue.GetPool().Commands(beginInfo);

				auto bindState = [&pipeline](VkCommandBuffer cmd)
				{
					ZoneScopedN("bindState");

					// bind descriptor sets
					pipeline.BindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalBuffers);
					pipeline.BindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalTextures);
					pipeline.BindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalSamplers);
					pipeline.BindDescriptorSetAuto(cmd, DescriptorSetCategory_View);
					pipeline.BindDescriptorSetAuto(cmd, DescriptorSetCategory_Material);
					pipeline.BindDescriptorSetAuto(cmd, DescriptorSetCategory_ModelInstances);

					// bind pipeline and buffers
					pipeline.BindPipelineAuto(cmd);

					BufferHandle<kVk> vbs[] = {pipeline.GetResources().model->GetVertexBuffer()};
					DeviceSize<kVk> offsets[] = {0};
					vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
					vkCmdBindIndexBuffer(cmd, pipeline.GetResources().model->GetIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
				};

				bindState(cmd);

				PushConstants pushConstants{.frameIndex = frameIndex};

				uint32_t dx = renderPassInfo.renderArea.extent.width / desc.splitScreenGrid.width;
				uint32_t dy = renderPassInfo.renderArea.extent.height / desc.splitScreenGrid.height;

				ASSERT(dx > 0);
				ASSERT(dy > 0);

				while (drawIt < drawCount)
				{
					auto drawView = [&pushConstants, &pipeline, &cmd, &dx, &dy, &desc](uint16_t viewIt)
					{
						ZoneScopedN("drawView");

						uint32_t i = viewIt % desc.splitScreenGrid.width;
						uint32_t j = viewIt / desc.splitScreenGrid.width;

						auto setViewportAndScissor = [](VkCommandBuffer cmd,
														int32_t x,
														int32_t y,
														uint32_t width,
														uint32_t height)
						{
							ZoneScopedN("setViewportAndScissor");

							VkViewport viewport{};
							viewport.x = static_cast<float>(x);
							viewport.y = static_cast<float>(y);
							viewport.width = static_cast<float>(width);
							viewport.height = static_cast<float>(height);
							viewport.minDepth = 0.0F;
							viewport.maxDepth = 1.0F;

							ASSERT(width > 0);
							ASSERT(height > 0);

							VkRect2D scissor{};
							scissor.offset = {x, y};
							scissor.extent = {width, height};

							vkCmdSetViewport(cmd, 0, 1, &viewport);
							vkCmdSetScissor(cmd, 0, 1, &scissor);
						};

						setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

						uint16_t viewIndex = viewIt;
						uint16_t materialIndex = 0UI16;

						pushConstants.viewAndMaterialId = (static_cast<uint32_t>(viewIndex) << ShaderTypes_MaterialIndexBits) | materialIndex;

						auto drawModel = [&pushConstants, &pipeline](VkCommandBuffer cmd)
						{
							ZoneScopedN("drawModel");

							{
								ZoneScopedN("drawModel::vkCmdPushConstants");

								vkCmdPushConstants(
									cmd,
									pipeline.GetLayout(),
									VK_SHADER_STAGE_ALL, // todo: input active shader stages + ranges from pipeline
									0,
									sizeof(pushConstants),
									&pushConstants);
							}

							{
								ZoneScopedN("drawModel::vkCmdDrawIndexed");

								vkCmdDrawIndexed(
									cmd,
									pipeline.GetResources().model->GetDesc().indexCount,
									1,
									0,
									0,
									666);
							}
						};

						drawModel(cmd);
					};

					drawView(drawIt);

					drawIt = drawAtomic++;
				}

				cmd.End();
			});
	}

	return drawThreadCount;
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
	(void)width; (void)height;

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
	InternalUpdateViewBuffer();
}

template <>
uint32_t
Window<kVk>::Draw(Pipeline<kVk>& pipeline, Queue<kVk>& queue, RenderPassBeginInfo<kVk>&& renderPassInfo)
{
	ZoneScopedN("Window::draw");

	return InternalDrawViews(
		pipeline, queue, std::forward<RenderPassBeginInfo<kVk>>(renderPassInfo));
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
	, myViewBuffers(std::make_unique<Buffer<kVk>[]>(ShaderTypes_FrameCount))
{
	ZoneScopedN("Window()");

	for (uint8_t i = 0; i < ShaderTypes_FrameCount; i++)
	{
		myViewBuffers[i] = Buffer<kVk>(
			InternalGetDevice(),
			BufferCreateDesc<kVk>{
				ShaderTypes_ViewCount * sizeof(ViewData),
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
