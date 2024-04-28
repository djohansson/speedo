#include "../window.h"
#include "../rhiapplication.h"
#include "../shaders/capi.h"

#include "utils.h"

#include <GLFW/glfw3.h>

#include <imgui.h>

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
void Window<Vk>::internalUpdateViewBuffer() const
{
	ZoneScopedN("Window::internalUpdateViewBuffer");

	auto bufferMemory = myViewBuffers[getCurrentFrameIndex()].getBufferMemory();
	void* data;
	VK_CHECK(vmaMapMemory(getDevice()->getAllocator(), bufferMemory, &data));

	ViewData* viewDataPtr = static_cast<ViewData*>(data);
	auto viewCount = (myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);
	assert(viewCount <= ShaderTypes_ViewCount);
	for (uint32_t viewIt = 0ul; viewIt < viewCount; viewIt++)
	{
		viewDataPtr->viewProjectionTransform =
			myViews[viewIt].getProjectionMatrix() * glm::mat4(myViews[viewIt].getViewMatrix());
		viewDataPtr++;
	}

	// vmaFlushAllocation(
	// 	getDevice()->getAllocator(),
	// 	bufferMemory,
	// 	0,
	// 	viewCount * sizeof(ViewData));

	vmaUnmapMemory(getDevice()->getAllocator(), bufferMemory);
}

template <>
uint32_t Window<Vk>::internalDrawViews(
	Pipeline<Vk>& pipeline,
	Queue<Vk>& queue,
	RenderPassBeginInfo<Vk>&& renderPassInfo)
{
	// setup draw parameters
	uint32_t drawCount = myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height;
	uint32_t drawThreadCount = 0;

	std::atomic_uint32_t drawAtomic = 0ul;

	// draw views using secondary command buffers
	// todo: generalize this to other types of draws
	if (pipeline.resources().model)
	{
		ZoneScopedN("Window::drawViews");

		drawThreadCount = std::min<uint32_t>(drawCount, std::max(1u, queue.getPool().getDesc().levelCount - 1));

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
			 frameIndex = getCurrentFrameIndex(),
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

				CommandBufferInheritanceInfo<Vk> inheritInfo{
					VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
				inheritInfo.renderPass = renderPassInfo.renderPass;
				inheritInfo.framebuffer = renderPassInfo.framebuffer;

				CommandBufferAccessScopeDesc<Vk> beginInfo{};
				beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
								  VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				beginInfo.pInheritanceInfo = &inheritInfo;
				beginInfo.level = threadIt + 1;

				auto cmd = queue.getPool().commands(beginInfo);

				auto bindState = [&pipeline](VkCommandBuffer cmd)
				{
					ZoneScopedN("bindState");

					// bind descriptor sets
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalBuffers);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalTextures);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalSamplers);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_View);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_Material);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_ModelInstances);

					// bind pipeline and buffers
					pipeline.bindPipelineAuto(cmd);

					BufferHandle<Vk> vbs[] = {pipeline.resources().model->getVertexBuffer()};
					DeviceSize<Vk> offsets[] = {0};
					vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
					vkCmdBindIndexBuffer(cmd, pipeline.resources().model->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
				};

				bindState(cmd);

				PushConstants pushConstants{.frameIndex = frameIndex};

				uint32_t dx = renderPassInfo.renderArea.extent.width / desc.splitScreenGrid.width;
				uint32_t dy = renderPassInfo.renderArea.extent.height / desc.splitScreenGrid.height;

				assert(dx > 0);
				assert(dy > 0);

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
							viewport.minDepth = 0.0f;
							viewport.maxDepth = 1.0f;

							assert(width > 0);
							assert(height > 0);

							VkRect2D scissor{};
							scissor.offset = {x, y};
							scissor.extent = {width, height};

							vkCmdSetViewport(cmd, 0, 1, &viewport);
							vkCmdSetScissor(cmd, 0, 1, &scissor);
						};

						setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

						uint16_t viewIndex = viewIt;
						uint16_t materialIndex = 0ui16;

						pushConstants.viewAndMaterialId = (static_cast<uint32_t>(viewIndex) << ShaderTypes_MaterialIndexBits) | materialIndex;

						auto drawModel = [&pushConstants, &pipeline](VkCommandBuffer cmd)
						{
							ZoneScopedN("drawModel");

							{
								ZoneScopedN("drawModel::vkCmdPushConstants");

								vkCmdPushConstants(
									cmd,
									pipeline.getLayout(),
									VK_SHADER_STAGE_ALL, // todo: input active shader stages + ranges from pipeline
									0,
									sizeof(pushConstants),
									&pushConstants);
							}

							{
								ZoneScopedN("drawModel::vkCmdDrawIndexed");

								vkCmdDrawIndexed(
									cmd,
									pipeline.resources().model->getDesc().indexCount,
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

				cmd.end();
			});
	}

	return drawThreadCount;
}

template <>
void Window<Vk>::internalInitializeViews()
{
	myViews.resize(myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);

	unsigned width = myConfig.swapchainConfig.extent.width / myConfig.splitScreenGrid.width;
	unsigned height = myConfig.swapchainConfig.extent.height / myConfig.splitScreenGrid.height;

	for (unsigned j = 0; j < myConfig.splitScreenGrid.height; j++)
		for (unsigned i = 0; i < myConfig.splitScreenGrid.width; i++)
		{
			auto& view = myViews[j * myConfig.splitScreenGrid.width + i];
			view.desc().viewport.x = i * width;
			view.desc().viewport.y = j * height;
			view.desc().viewport.width = width;
			view.desc().viewport.height = height;
			view.updateAll();
		}
}

template <>
void Window<Vk>::onResizeFramebuffer(int w, int h)
{
	assert(w > 0);
	assert(h > 0);
	(void)w; (void)h;

	auto& device = *getDevice();
	auto& instance = *device.getInstance();
	auto surface = getSurface();
	auto physicalDevice = device.getPhysicalDevice();

	instance.updateSurfaceCapabilities(physicalDevice, surface);

	myConfig.swapchainConfig.extent = instance.getSwapchainInfo(physicalDevice, surface).capabilities.currentExtent;

	assert(myConfig.contentScale.x == myState.xscale);
	assert(myConfig.contentScale.y == myState.yscale);

	myState.width = myConfig.swapchainConfig.extent.width / myState.xscale;
	myState.height = myConfig.swapchainConfig.extent.height / myState.yscale;

	internalCreateSwapchain(myConfig.swapchainConfig, *this);
	internalInitializeViews();
}

template <>
void Window<Vk>::onResizeSplitScreenGrid(uint32_t width, uint32_t height)
{
	myConfig.splitScreenGrid = Extent2d<Vk>{width, height};
	
	internalInitializeViews();
}

template <>
void Window<Vk>::internalUpdateViews(const InputState& input)
{
	ZoneScopedN("Window::internalUpdateViews");

	myTimestamps[1] = myTimestamps[0];
	myTimestamps[0] = std::chrono::high_resolution_clock::now();

	float dt = (myTimestamps[1] - myTimestamps[0]).count();

	if (input.mouse.insideWindow && !input.mouse.leftDown)
	{
		// todo: generic view index calculation
		size_t viewIdx = myConfig.splitScreenGrid.width * input.mouse.position[0] / myConfig.swapchainConfig.extent.width;
		size_t viewIdy = myConfig.splitScreenGrid.height * input.mouse.position[1] / myConfig.swapchainConfig.extent.height;
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

		float dx = 0.f;
		float dz = 0.f;

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
			const auto& viewMatrix = view.getViewMatrix();
			auto forward = glm::vec3(viewMatrix[0][2], viewMatrix[1][2], viewMatrix[2][2]);
			auto strafe = glm::vec3(viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]);

			constexpr auto moveSpeed = 0.000000002f;

			view.desc().cameraPosition += dt * (dz * forward + dx * strafe) * moveSpeed;

			// std::cout << *myActiveView << ":pos:[" << view.desc().cameraPosition.x << ", " <<
			//     view.desc().cameraPosition.y << ", " << view.desc().cameraPosition.z << "]" << '\n';

			doUpdateViewMatrix = true;
		}

		if (input.mouse.leftDown)
		{
			constexpr auto rotSpeed = 0.000000001f;

			float cx = view.desc().viewport.x + (view.desc().viewport.width / 2);
			float cy = view.desc().viewport.y + (view.desc().viewport.height / 2);

			float dM[2] = {cx - input.mouse.position[0], cy - input.mouse.position[1]};

			view.desc().cameraRotation +=
				dt *
				glm::vec3(
					dM[1] / view.desc().viewport.height, dM[0] / view.desc().viewport.width, 0.0f) *
				rotSpeed;

			// std::cout << *myActiveView << ":rot:[" << view.desc().cameraRotation.x << ", " <<
			//     view.desc().cameraRotation.y << ", " << view.desc().cameraRotation.z << "]" << '\n';

			doUpdateViewMatrix = true;
		}

		if (doUpdateViewMatrix)
		{
			myViews[*myActiveView].updateViewMatrix();
		}
	}
}

template <>
void Window<Vk>::onInputStateChanged(const InputState& input)
{
	internalUpdateViews(input);
	internalUpdateViewBuffer();
}

template <>
uint32_t Window<Vk>::draw(
	Pipeline<Vk>& pipeline,
	Queue<Vk>& queue,
	RenderPassBeginInfo<Vk>&& renderPassInfo)
{
	ZoneScopedN("Window::draw");

	return internalDrawViews(
		pipeline,
		queue,
		std::forward<RenderPassBeginInfo<Vk>>(renderPassInfo));
}

template <>
Window<Vk>::Window(
	const std::shared_ptr<Device<Vk>>& device,
	SurfaceHandle<Vk>&& surface,
	ConfigFile&& config,
	WindowState&& state)
	: Swapchain(
		device,
		config.swapchainConfig,
		std::forward<SurfaceHandle<Vk>>(surface), VK_NULL_HANDLE)
	, myConfig(std::forward<ConfigFile>(config))
	, myState(std::forward<WindowState>(state))
	, myViewBuffers(std::make_unique<Buffer<Vk>[]>(ShaderTypes_FrameCount))
{
	ZoneScopedN("Window()");

	for (uint8_t i = 0; i < ShaderTypes_FrameCount; i++)
	{
		myViewBuffers[i] = Buffer<Vk>(
			getDevice(),
			BufferCreateDesc<Vk>{
				ShaderTypes_ViewCount * sizeof(ViewData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});
	}

	internalInitializeViews();

	myTimestamps[0] = std::chrono::high_resolution_clock::now();
}

template <>
Window<Vk>::Window(Window&& other) noexcept
	: Swapchain(std::forward<Window>(other))
	, myConfig(std::exchange(other.myConfig, {}))
	, myState(std::exchange(other.myState, {}))
	, myViewBuffers(std::exchange(other.myViewBuffers, {}))
	, myTimestamps(std::exchange(other.myTimestamps, {}))
	, myViews(std::exchange(other.myViews, {}))
	, myActiveView(std::exchange(other.myActiveView, {}))
{}

template <>
Window<Vk>::~Window()
{
	ZoneScopedN("~Window()");
}

template <>
Window<Vk>& Window<Vk>::operator=(Window&& other) noexcept
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
void Window<Vk>::swap(Window& other) noexcept
{
	Swapchain::swap(other);
	std::swap(myConfig, other.myConfig);
	std::swap(myState, other.myState);
	std::swap(myViewBuffers, other.myViewBuffers);
	std::swap(myTimestamps, other.myTimestamps);
	std::swap(myViews, other.myViews);
	std::swap(myActiveView, other.myActiveView);
}
