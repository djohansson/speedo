#include "../window.h"
#include "../shaders/shadertypes.h"

#include "utils.h"

#include <client/client.h> // TODO: eliminate this dependency

#include <stb_sprintf.h>

#if defined(__WINDOWS__)
#	include <execution>
#endif
#include <filesystem>
#include <future>
#include <numeric>
#include <string>
#include <string_view>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

template <>
void Window<Vk>::internalCreateFrameObjects(Extent2d<Vk> framebufferExtent)
{
	ZoneScopedN("Window::internalCreateFrameObjects");

	myViewBuffer = std::make_unique<Buffer<Vk>>(
		getDevice(),
		BufferCreateDesc<Vk>{
			myConfig.swapchainConfig.imageCount * ShaderTypes_ViewBufferCount * sizeof(ViewData),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});

	myViews.resize(myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);

	for (auto& view : myViews)
	{
		if (!view.desc().viewport.width)
			view.desc().viewport.width = framebufferExtent.width / myConfig.splitScreenGrid.width;

		if (!view.desc().viewport.height)
			view.desc().viewport.height =
				framebufferExtent.height / myConfig.splitScreenGrid.height;

		view.updateAll();
	}
}

template <>
void Window<Vk>::internalUpdateViewBuffer() const
{
	ZoneScopedN("Window::internalUpdateViewBuffer");

	void* data;
	VK_CHECK(vmaMapMemory(getDevice()->getAllocator(), myViewBuffer->getBufferMemory(), &data));

	ViewData* viewDataPtr = &static_cast<ViewData*>(data)[internalGetFrameIndex() * ShaderTypes_ViewCount];
	auto viewCount = (myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height);
	assert(viewCount <= ShaderTypes_ViewCount);
	for (uint32_t viewIt = 0ul; viewIt < viewCount; viewIt++)
	{
		viewDataPtr->viewProjectionTransform =
			myViews[viewIt].getProjectionMatrix() * glm::mat4(myViews[viewIt].getViewMatrix());
		viewDataPtr++;
	}

	vmaFlushAllocation(
		getDevice()->getAllocator(),
		myViewBuffer->getBufferMemory(),
		internalGetFrameIndex() * ShaderTypes_ViewCount * sizeof(ViewData),
		viewCount * sizeof(ViewData));

	vmaUnmapMemory(getDevice()->getAllocator(), myViewBuffer->getBufferMemory());
}

template <>
uint32_t Window<Vk>::internalDrawViews(
	Pipeline<Vk>& pipeline,
	CommandPoolContext<Vk>* secondaryContexts,
	uint32_t secondaryContextCount,
	const RenderPassBeginInfo<Vk>& renderPassInfo)
{
	// setup draw parameters
	uint32_t drawCount = myConfig.splitScreenGrid.width * myConfig.splitScreenGrid.height;
	uint32_t drawThreadCount = std::min<uint32_t>(drawCount, secondaryContextCount);

	std::atomic_uint32_t drawAtomic = 0ul;

	// draw views using secondary command buffers
	// todo: generalize this to other types of draws
	if (pipeline.resources().model)
	{
		ZoneScopedN("Window::drawViews");

		std::array<uint32_t, 128> seq;
		std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
		std::for_each_n(
#if defined(__WINDOWS__)
			std::execution::par,
#endif
			seq.begin(),
			drawThreadCount,
			[&pipeline,
			 &secondaryContexts,
			 &renderPassInfo,
			 frameIndex = internalGetFrameIndex(),
			 &drawAtomic,
			 &drawCount,
			 &desc = myConfig](uint32_t threadIt)
			{
				ZoneScoped;

				auto drawIt = drawAtomic++;
				if (drawIt >= drawCount)
					return;

				static constexpr std::string_view drawPartitionStr = "Window::drawPartition";
				char drawPartitionWithNumberStr[drawPartitionStr.size() + 3];
				stbsp_sprintf(
					drawPartitionWithNumberStr,
					"%.*s%u",
					static_cast<int>(drawPartitionStr.size()),
					drawPartitionStr.data(),
					threadIt);

				ZoneName(drawPartitionWithNumberStr, std::ssize(drawPartitionWithNumberStr));

				CommandBufferInheritanceInfo<Vk> inheritInfo{
					VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
				inheritInfo.renderPass = renderPassInfo.renderPass;
				inheritInfo.framebuffer = renderPassInfo.framebuffer;

				CommandBufferAccessScopeDesc<Vk> beginInfo{};
				beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
								  VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				beginInfo.pInheritanceInfo = &inheritInfo;
				beginInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;

				auto& commandContext = secondaryContexts[threadIt];
				auto cmd = commandContext.commands(beginInfo);

				auto bindState = [&pipeline](VkCommandBuffer cmd)
				{
					ZoneScopedN("bindState");

					// bind descriptor sets
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalTextures);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_GlobalSamplers);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_View);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_Material);
					pipeline.bindDescriptorSetAuto(cmd, DescriptorSetCategory_Object);

					// bind pipeline and vertex/index buffers
					pipeline.bindPipelineAuto(cmd);

					VkBuffer vertexBuffers[]{*pipeline.resources().model};
					VkDeviceSize vertexOffsets[]{pipeline.resources().model->getVertexOffset()};

					vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
					vkCmdBindIndexBuffer(
						cmd,
						*pipeline.resources().model,
						pipeline.resources().model->getIndexOffset(),
						VK_INDEX_TYPE_UINT32);
				};

				bindState(cmd);

				uint32_t dx = renderPassInfo.renderArea.extent.width / desc.splitScreenGrid.width;
				uint32_t dy = renderPassInfo.renderArea.extent.height / desc.splitScreenGrid.height;

				while (drawIt < drawCount)
				{
					auto drawView = [&frameIndex, &pipeline, &cmd, &dx, &dy, &desc](uint16_t viewIt)
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

							VkRect2D scissor{};
							scissor.offset = {x, y};
							scissor.extent = {width, height};

							vkCmdSetViewport(cmd, 0, 1, &viewport);
							vkCmdSetScissor(cmd, 0, 1, &scissor);
						};

						setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

						auto drawModel = [&pipeline, &frameIndex, &viewIt](VkCommandBuffer cmd)
						{
							ZoneScopedN("drawModel");

							{
								ZoneScopedN("drawModel::vkCmdPushConstants");

								uint16_t viewId = frameIndex * ShaderTypes_ViewCount + viewIt;
								uint16_t materialId = 0ui16;
								uint16_t objectBufferIndex = 42ui16;
								uint16_t objectArrayIndex = 666ui16;

								PushConstants pushConstants{
									(static_cast<uint32_t>(viewId) << 16ul) | materialId,
									(static_cast<uint32_t>(objectBufferIndex) << 16ul) |
										objectArrayIndex};

								vkCmdPushConstants(
									cmd,
									pipeline.getLayout(),
									VK_SHADER_STAGE_ALL, // todo: input active shader stages + ranges from pipeline
									0,
									sizeof(PushConstants),
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
									0);
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
void Window<Vk>::draw(
	TaskExecutor& executor,
	Pipeline<Vk>& pipeline,
	CommandPoolContext<Vk>& primaryContext,
	CommandPoolContext<Vk>* secondaryContexts,
	uint32_t secondaryContextCount)
{
	ZoneScopedN("Window::draw");

	TaskGraph graph;
	auto [task, updateViewBufferFuture] = graph.createTask([this]{ internalUpdateViewBuffer(); });
	executor.submit(std::move(graph));

	auto& renderTarget = pipeline.getRenderTarget();

	auto cmd = primaryContext.commands();
	auto renderPassInfo = renderTarget.begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

	uint32_t drawThreadCount = internalDrawViews(
		pipeline, secondaryContexts, secondaryContextCount, renderPassInfo.value());

	for (uint32_t contextIt = 0ul; contextIt < drawThreadCount; contextIt++)
		primaryContext.execute(secondaryContexts[contextIt]);

	//renderTarget.nextSubpass(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
	renderTarget.end(cmd);
	renderTarget.transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);

	blit(
		cmd,
		renderTarget,
		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		0,
		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		0);

	executor.join(std::move(updateViewBufferFuture));
}

template <>
void Window<Vk>::onResizeFramebuffer(Extent2d<Vk> framebufferExtent)
{
	myConfig.swapchainConfig.extent = framebufferExtent;

	internalCreateSwapchain(myConfig.swapchainConfig, *this);
	internalCreateFrameObjects(framebufferExtent);
}

template <>
void Window<Vk>::updateInput(const InputState& input)
{
	ZoneScopedN("Window::updateInput");

	myTimestamps[1] = myTimestamps[0];
	myTimestamps[0] = std::chrono::high_resolution_clock::now();

	float dt = (myTimestamps[1] - myTimestamps[0]).count();

	if (input.mouseButtonsPressed[2])
	{
		// todo: generic view index calculation
		size_t viewIdx = input.mousePosition[0][0] /
						 (myConfig.windowExtent.width / myConfig.splitScreenGrid.width);
		size_t viewIdy = input.mousePosition[0][1] /
						 (myConfig.windowExtent.height / myConfig.splitScreenGrid.height);
		myActiveView =
			std::min((viewIdy * myConfig.splitScreenGrid.width) + viewIdx, myViews.size() - 1);

		//std::cout << *myActiveView << ":[" << input.mousePosition[0][0] << ", " << input.mousePosition[0][1] << "]" << '\n';
	}
	else if (!input.mouseButtonsPressed[0])
	{
		myActiveView.reset();

		//std::cout << "myActiveView.reset()" << '\n';
	}

	if (myActiveView)
	{
		//std::cout << "window.myActiveView read/consume" << '\n';

		float dx = 0.f;
		float dz = 0.f;

		for (const auto& [key, pressed] : input.keysPressed)
		{
			if (pressed)
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

		if (input.mouseButtonsPressed[0])
		{
			constexpr auto rotSpeed = 0.00000001f;

			auto dM = input.mousePosition[1] - input.mousePosition[0];

			view.desc().cameraRotation +=
				dt *
				glm::vec3(
					dM.y / view.desc().viewport.height, dM.x / view.desc().viewport.width, 0.0f) *
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
Window<Vk>::Window(
	const std::shared_ptr<Device<Vk>>& device,
	SurfaceHandle<Vk>&& surface,
	WindowConfiguration<Vk>&& defaultConfig)
	: Swapchain(
		device,
		defaultConfig.swapchainConfig,
		std::forward<SurfaceHandle<Vk>>(surface), VK_NULL_HANDLE)
	, myConfig(AutoSaveFileObject<WindowConfiguration<Vk>>(
		  std::filesystem::path(client_getUserProfilePath()) / "window.json",
		  std::forward<WindowConfiguration<Vk>>(defaultConfig)))
{
	ZoneScopedN("Window()");

	internalCreateFrameObjects(myConfig.swapchainConfig.extent);

	myTimestamps[0] = std::chrono::high_resolution_clock::now();
}

template <>
Window<Vk>::Window(Window&& other) noexcept
	: Swapchain(std::forward<Window>(other))
	, myConfig(std::exchange(other.myConfig, {}))
	, myTimestamps(std::exchange(other.myTimestamps, {}))
	, myViews(std::exchange(other.myViews, {}))
	, myActiveView(std::exchange(other.myActiveView, {}))
	, myViewBuffer(std::exchange(other.myViewBuffer, {}))
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
	myTimestamps = std::exchange(other.myTimestamps, {});
	myViews = std::exchange(other.myViews, {});
	myActiveView = std::exchange(other.myActiveView, {});
	myViewBuffer = std::exchange(other.myViewBuffer, {});
	return *this;
}

template <>
void Window<Vk>::swap(Window& other) noexcept
{
	Swapchain::swap(other);
	std::swap(myConfig, other.myConfig);
	std::swap(myTimestamps, other.myTimestamps);
	std::swap(myViews, other.myViews);
	std::swap(myActiveView, other.myActiveView);
	std::swap(myViewBuffer, other.myViewBuffer);
}
