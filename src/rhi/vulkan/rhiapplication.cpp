#include <core/task.h>
#include <rhi/capi.h>
#include <rhi/rhiapplication.h>
#include <rhi/model.h>
#include <rhi/shaders/capi.h>
#include <rhi/vulkan/utils.h>
#include <gfx/scene.h>

#include <uuid.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

#include <gfx/imgui_extra.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <array>
#include <memory>

//#include <imnodes.h>

namespace rhi
{

template <>
RHI<kVk>& RHIApplication::GetRHI<kVk>() noexcept
{
	return *static_cast<RHI<kVk>*>(myRHI.get());
}

template <>
const RHI<kVk>& RHIApplication::GetRHI<kVk>() const noexcept
{
	return *static_cast<const RHI<kVk>*>(myRHI.get());
}

IMPLEMENT_OBJECT_GETINSTANCE(RenderImageSet<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(RenderImageSet<kVk>);

namespace rhiapplication
{

static core::ConcurrentQueue<ImDrawData> gIMGUIDrawData;
static imgui_extra::ImDrawDataSnapshot gIMGUIDrawDataSnapshot; // owns the draw lists referenced by gIMGUIDrawData
static std::array<uuids::uuid, 3> gRenderImageSetUuids;
static uuids::uuid gModelUuid;
static uuids::uuid gBlackTextureUuid;
static uuids::uuid gBlackTextureViewUuid;
static uuids::uuid gSamplersUuid;
static uuids::uuid gMaterialsUuid;
static uuids::uuid gModelInstancesUuid;

void IMGUIDrawFunction(
	CommandBufferHandle<kVk> cmd,
	PipelineHandle<kVk> pipeline = nullptr)
{
	ZoneScopedN("RHIApplication::IMGUIDraw");

	using namespace ImGui;

	static ImDrawData gDrawData{};
	while (gIMGUIDrawData.try_dequeue(gDrawData));

	ImGui_ImplVulkan_RenderDrawData(&gDrawData, cmd, pipeline);
}

static void IMGUIInit(
	Window<kVk>& window,
	RHI<kVk>& rhi,
	Pipeline<kVk>& pipeline,
	Queue<kVk>& graphicsQueue,
	uint32_t graphicsQueueCount,
	std::string_view imguiIniSettings)
{
	ZoneScopedN("RHIApplication::IMGUIInit");

	using namespace ImGui;
	using namespace rhiapplication;

	IMGUI_CHECKVERSION();
	CreateContext();
	auto& imguiIO = GetIO();
	imguiIO.IniFilename = nullptr;

	//imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	//imguiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	//imguiIO.FontGlobalScale = 1.0f;
	//imguiIO.FontAllowUserScaling = true;
	// auto& platformIo = ImGui::GetPlatformIO();
	// platformIo.Platform_CreateVkSurface =
	// 	(decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(*rhi.GetInstance(), "vkCreateWin32SurfaceKHR");

	LoadIniSettingsFromMemory(imguiIniSettings.data(), imguiIniSettings.size());

	const auto& surfaceCapabilities =
		rhi.GetInstance().GetSwapchainInfo(
			rhi.GetPrimaryDevice().GetPhysicalDevice(),
			window.GetSwapchain().GetDesc().surface).capabilities;

#if defined(__OSX__)
	float dpiScaleX = 1.0F;
	float dpiScaleY = 1.0F;
#else
	float dpiScaleX = window.GetDesc().contentScale.x;
	float dpiScaleY = window.GetDesc().contentScale.y;
#endif

	imguiIO.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.RasterizerDensity = std::max(window.GetDesc().contentScale.x, window.GetDesc().contentScale.y);
	config.PixelSnapH = false;

	imguiIO.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]));
	fontPath /= "fonts";
	fontPath /= "foo";

	constexpr std::array<const char*, 6> kFonts{{
		"Cousine-Regular.ttf",
		"DroidSans.ttf",
		"Karla-Regular.ttf",
		"ProggyClean.ttf",
		"ProggyTiny.ttf",
		"Roboto-Medium.ttf",
	}};

	constexpr float kDefaultFontSize = 16.0F;
	ImFont* defaultFont = nullptr;
	for (const auto* font : kFonts)
	{
		fontPath.replace_filename(font);
		defaultFont = imguiIO.Fonts->AddFontFromFileTTF(
			fontPath.generic_string().c_str(), kDefaultFontSize * std::max(dpiScaleX, dpiScaleY), &config);
	}

	// Setup style
	StyleColorsClassic();
	imguiIO.FontDefault = defaultFont;

	// Setup Vulkan binding
	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.Instance = rhi.GetInstance();
	initInfo.PhysicalDevice = rhi.GetPrimaryDevice().GetPhysicalDevice();
	initInfo.Device = rhi.GetPrimaryDevice();
	initInfo.QueueFamily = graphicsQueue.GetDesc().queueFamilyIndex;
	initInfo.Queue = graphicsQueue;
	initInfo.PipelineCache = pipeline.GetCache();
	initInfo.DescriptorPool = pipeline.GetDescriptorPool();
	initInfo.MinImageCount = surfaceCapabilities.minImageCount;
	// initInfo.ImageCount is used to determine the number of buffers in flight inside imgui, 
	// and since we allow up to queuecount in-flight renders per frame due to triple buffering,
	// this needs to be set to the queue count accordingly.
	// (passed in rather than read here, since the caller already holds the graphics queue write lock)
	// imgui also requires ImageCount >= MinImageCount, and devices may expose a single graphics queue.
	initInfo.ImageCount = std::max<uint32_t>(
		{graphicsQueueCount,
		 static_cast<uint32_t>(window.GetSwapchain().GetDesc().images.size()),
		 initInfo.MinImageCount});
	initInfo.Allocator = &rhi.GetInstance().GetHostAllocationCallbacks();
	initInfo.CheckVkResultFn = [](VkResult result) { VK_CHECK(result); };
	initInfo.MinAllocationSize = 64 * 1024;
	initInfo.UseDynamicRendering = window.GetSwapchain().GetDesc().useDynamicRendering;
	initInfo.PipelineInfoMain.RenderPass = initInfo.UseDynamicRendering ? VK_NULL_HANDLE : window.GetSwapchain().GetFrames()[0].GetHandle().first;
	initInfo.PipelineInfoMain.Subpass = 0;
	initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfoKHR{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
		.pNext = nullptr,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &window.GetSwapchain().GetDesc().surfaceFormat.format,
	};
	ImGui_ImplVulkan_Init(&initInfo);
	ImGui_ImplGlfw_InitForVulkan(reinterpret_cast<GLFWwindow*>(GetCurrentWindow()), true);

	// IMNODES_NAMESPACE::CreateContext();
	// IMNODES_NAMESPACE::LoadCurrentEditorStateFromIniString(
	//	myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

static void ShutdownImgui()
{
	// size_t count;
	// myNodeGraph.layout.assign(IMNODES_NAMESPACE::SaveCurrentEditorStateToIniString(&count));
	// IMNODES_NAMESPACE::DestroyContext();

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();

	// snapshot draw lists are registered with the context's shared data, and must be gone before it is destroyed
	ImDrawData drawData;
	while (gIMGUIDrawData.try_dequeue(drawData));
	gIMGUIDrawDataSnapshot.Clear();

	ImGui::DestroyContext();
}

// material 0 samples this slot of gTextures, which "Open Image..." replaces
static constexpr uint32_t kMaterialTextureId = 15;

// hands `resource` to a graphics queue submission that waits for all graphics work submitted so far, and releases it
// from that submission's timeline callback, i.e. once the gpu can no longer be using it. call on the draw thread.
static void RetireAfterGraphicsWork(QueueTimelineContextData<kVk>& graphics, std::shared_ptr<void> resource)
{
	if (!resource)
		return;

	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	cmd.End();

	std::vector<core::TaskHandle> callbacks;
	callbacks.emplace_back(core::CreateTask([resource = std::move(resource)] {}).handle);

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline},
		.callbacks = std::move(callbacks)});

	graphicsSubmits |= graphicsQueue.Submit();
}

// makes an uploaded model the one being drawn, retiring the previous one. call on the draw thread.
static void InstallModel(RHI<kVk>& rhi, QueueTimelineContextData<kVk>& graphics, const std::shared_ptr<Model<kVk>>& model)
{
	ZoneScopedN("RHIApplication::InstallModel");

	auto& device = rhi.GetPrimaryDevice();
	auto& pipeline = device.GetPipeline();

	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), VK_PIPELINE_BIND_POINT_GRAPHICS);
	pipeline.SetVertexInputState(*model);
	pipeline.SetDescriptorData(
		"gVertexBuffer",
		DescriptorBufferInfo<kVk>{.buffer = model->GetVertexBuffer(), .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

	gModelUuid = uuids::uuid_name_generator{uuids::uuid_namespace_oid}("LoadedModel");
	RetireAfterGraphicsWork(graphics, device.ReplaceResource(gModelUuid, model));
}

// makes an uploaded image the texture sampled by material 0, retiring the previous one. call on the draw thread.
static void InstallImage(
	RHI<kVk>& rhi,
	QueueTimelineContextData<kVk>& graphics,
	const std::shared_ptr<Image<kVk>>& image,
	const std::shared_ptr<ImageView<kVk>>& imageView)
{
	ZoneScopedN("RHIApplication::InstallImage");

	// transition to a shader readable layout on the graphics queue first, and only point gTextures at the new view
	// once that has executed: the transition's timeline callback hands the rest back to the draw thread.
	auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();

	auto cmd = graphicsQueue.GetPool().Commands();
	{
		GPU_SCOPE(cmd, graphicsQueue, Transition); //NOLINT(bugprone-suspicious-stringview-data-usage)

		image->Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	cmd.End();

	auto [transitionDoneTask, transitionDoneFuture] = core::CreateTask([&rhi, image, imageView]
	{
		auto [bindTask, bindFuture] = core::CreateTask<QueueTimelineContextData<kVk>*>(
			[&rhi, image, imageView](QueueTimelineContextData<kVk>* graphics)
			{
				auto& device = rhi.GetPrimaryDevice();
				auto& pipeline = device.GetPipeline();

				pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), VK_PIPELINE_BIND_POINT_GRAPHICS);
				pipeline.SetDescriptorData(
					"gTextures",
					DescriptorImageInfo<kVk>{.sampler = {}, .imageView = *imageView, .imageLayout = image->GetDesc().layout},
					DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
					kMaterialTextureId);

				auto nameUuid = [](std::string_view name) { return uuids::uuid_name_generator{uuids::uuid_namespace_oid}(name); };
				RetireAfterGraphicsWork(*graphics, device.ReplaceResource(nameUuid("LoadedImage"), image));
				RetireAfterGraphicsWork(*graphics, device.ReplaceResource(nameUuid("LoadedImageView"), imageView));
			});
		rhi.drawCalls.enqueue(bindTask);
	});

	std::vector<core::TaskHandle> callbacks;
	callbacks.emplace_back(transitionDoneTask);

	graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
		.waitSemaphores = {graphics.semaphore},
		.waitDstStageMasks = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
		.waitSemaphoreValues = {graphics.timeline},
		.signalSemaphores = {graphics.semaphore},
		.signalSemaphoreValues = {++graphics.timeline},
		.callbacks = std::move(callbacks)});

	graphicsSubmits |= graphicsQueue.Submit();
}

static void DrawMainPass(
	RHI<kVk>& rhi,
	Window<kVk>& window,
	Pipeline<kVk>& pipeline,
	Queue<kVk>& graphicsQueue,
	CommandBufferHandle<kVk> cmd,
	uint16_t newFrameIndex,
	uint64_t graphicsTimeline)
{
	GPU_SCOPE(cmd, graphicsQueue, draw);

	auto& device = rhi.GetPrimaryDevice();
	auto& renderImageSet = *device.GetResource<RenderImageSet<kVk>>(gRenderImageSetUuids[newFrameIndex]);

	renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
	renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
	renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
	renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);

	pipeline.SetRenderTarget(renderImageSet);

	auto renderTargetInfo = renderImageSet.Begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), VK_PIPELINE_BIND_POINT_GRAPHICS);

	// setup draw parameters
	uint32_t drawCount = window.GetDesc().splitScreenGrid.width * window.GetDesc().splitScreenGrid.height;
	uint32_t drawThreadCount = 0;

	std::atomic_uint32_t drawAtomic = 0UL;

	// draw views using secondary command buffers
	// todo: generalize this to other types of draws
	if (device.HasResource(gModelUuid))
	{
		auto& model = *device.GetResource<Model<kVk>>(gModelUuid);

		ZoneScopedN("RHIApplication::Draw::drawViews");

		drawThreadCount = std::min<uint32_t>(drawCount, graphicsQueue.GetPool().GetDesc().levelCount);
		const auto& windowDesc = window.GetDesc();

		constexpr uint32_t kMaxDrawThreads = 128;
		std::array<uint32_t, kMaxDrawThreads> seq;
		std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
		std::for_each_n(
			seq.begin(),
			drawThreadCount,
			[&pipeline,
			&graphicsQueue,
			&renderTargetInfo,
			&renderImageSet,
			&newFrameIndex,
			&drawAtomic,
			&drawCount,
			&model,
			&windowDesc](uint32_t threadIt)
			{
				ZoneScoped;

				auto drawIt = drawAtomic++;
				if (drawIt >= drawCount)
					return;

				auto zoneNameStr = std::format("Window::drawPartition thread:{}", threadIt);

				ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

				CommandBufferInheritanceInfo<kVk> inheritInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
				CommandBufferAccessScopeDesc<kVk> beginInfo{};
				beginInfo.pInheritanceInfo = &inheritInfo;
				beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				beginInfo.level = threadIt + 1;
				// for dynamic rendering, setting this here is just to silence vvl warnings
				beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				//

				uint32_t deltaX = 0;
				uint32_t deltaY = 0;

				if (const auto* dynamicRenderingInfo = std::get_if<DynamicRenderingInfo<kVk>>(&renderTargetInfo))
				{
					inheritInfo.pNext = &dynamicRenderingInfo->inheritanceInfo;

					deltaX = dynamicRenderingInfo->renderInfo.renderArea.extent.width / windowDesc.splitScreenGrid.width;
					deltaY = dynamicRenderingInfo->renderInfo.renderArea.extent.height / windowDesc.splitScreenGrid.height;
				}
				else if (const auto* renderPassBeginInfo = std::get_if<VkRenderPassBeginInfo>(&renderTargetInfo))
				{
					inheritInfo.renderPass = renderPassBeginInfo->renderPass;
					inheritInfo.framebuffer = renderPassBeginInfo->framebuffer;

					deltaX = renderPassBeginInfo->renderArea.extent.width / windowDesc.splitScreenGrid.width;
					deltaY = renderPassBeginInfo->renderArea.extent.height / windowDesc.splitScreenGrid.height;
				}

				auto cmd = graphicsQueue.GetPool().Commands(beginInfo);

				auto bindState = [&pipeline, &model](VkCommandBuffer cmd)
				{
					ZoneScopedN("bindState");

					// bind vertex inputs
					std::array<BufferHandle<kVk>, 1> vbs = {model.GetVertexBuffer()};
					std::array<DeviceSize<kVk>, 1> offsets = {0};
					vkCmdBindVertexBuffers(cmd, 0, 1, vbs.data(), offsets.data());
					vkCmdBindIndexBuffer(cmd, model.GetIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);

					// bind descriptor sets
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MATERIAL);
					pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

					// bind pipeline and buffers
					pipeline.BindPipelineAuto(cmd);
				};

				bindState(cmd);

				PushConstants pushConstants{.frameIndex = newFrameIndex};

				ASSERT(deltaX > 0);
				ASSERT(deltaY > 0);

				while (drawIt < drawCount)
				{
					auto drawView = [&pushConstants, &pipeline, &model, &cmd, &deltaX, &deltaY, &windowDesc](uint16_t viewIt)
					{
						ZoneScopedN("drawView");

						uint32_t col = viewIt % windowDesc.splitScreenGrid.width;
						uint32_t row = viewIt / windowDesc.splitScreenGrid.width;

						auto setViewportAndScissor = [](VkCommandBuffer cmd,
														int32_t posX,
														int32_t posY,
														uint32_t width,
														uint32_t height)
						{
							ZoneScopedN("setViewportAndScissor");

							VkViewport viewport{};
							viewport.x = static_cast<float>(posX);
							viewport.y = static_cast<float>(posY);
							viewport.width = static_cast<float>(width);
							viewport.height = static_cast<float>(height);
							viewport.minDepth = 0.0F;
							viewport.maxDepth = 1.0F;

							ASSERT(width > 0);
							ASSERT(height > 0);

							VkRect2D scissor{};
							scissor.offset = {.x = posX, .y = posY};
							scissor.extent = {.width = width, .height = height};

							vkCmdSetViewport(cmd, 0, 1, &viewport);
							vkCmdSetScissor(cmd, 0, 1, &scissor);
						};

						setViewportAndScissor(
							cmd,
							static_cast<int32_t>(col * deltaX),
							static_cast<int32_t>(row * deltaY),
							deltaX,
							deltaY);

						uint16_t viewIndex = viewIt;
						constexpr uint32_t kMaterialIndex = 0U;
						constexpr uint32_t kDefaultModelInstanceId = 666;

						pushConstants.viewAndMaterialId = (static_cast<uint32_t>(viewIndex) << SHADER_TYPES_MATERIAL_INDEX_BITS) | kMaterialIndex;
						pushConstants.modelInstanceId = kDefaultModelInstanceId;

						auto drawModel = [&pushConstants, &pipeline, &model](VkCommandBuffer cmd)
						{
							ZoneScopedN("drawModel");

							{
								ZoneScopedN("drawModel::vkCmdPushConstants");

								pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));
							}

							{
								ZoneScopedN("drawModel::vkCmdDrawIndexed");

								vkCmdDrawIndexed(
									cmd,
									model.GetDesc().indexCount,
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

				cmd.End();
			});
	}

	for (uint32_t threadIt = 1UL; threadIt <= drawThreadCount; threadIt++)
		graphicsQueue.Execute(threadIt, graphicsTimeline);

	renderImageSet.End(cmd);
}

// auto loadGlTF = [](nfdchar_t* openFilePath)
// {
// 	try
// 	{
// 		std::filesystem::path path(openFilePath);

// 		if (path.is_relative())
// 			throw std::runtime_error("Command line argument path is not absolute");

// 		if (!path.has_filename())
// 			throw std::runtime_error("Command line argument path has no filename");

// 		if (!path.has_extension())
// 			throw std::runtime_error("Command line argument path has no filename extension");

// 		gltfstream::PrintInfo(path);
// 	}
// 	catch (const std::runtime_error& ex)
// 	{
// 		std::cerr << "Error! - ";
// 		std::cerr << ex.what() << "\n";

// 		throw;
// 	}

// 	return 0;
// };

void CreateWindowDependentObjects(RHI<kVk>& rhi)
{
	ZoneScopedN("CreateWindowDependentObjects");
	
	auto& device = rhi.GetPrimaryDevice();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto frameCount = window.GetSwapchain().GetFrames().size();
	ENSURE(frameCount <= gRenderImageSetUuids.size());
	
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto colorImage = Image<kVk>(
			ImageCreateDesc<kVk>{
				rhi.CreatePrimaryDeviceObjectCreateDesc(std::format("Main RT Color Image {}", frameIt)),
				{{.extent = window.GetSwapchain().GetDesc().extent}},
				window.GetSwapchain().GetDesc().surfaceFormat.format,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT/* | VK_IMAGE_USAGE_TRANSFER_DST_BIT*/ | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED});

		auto depthStencilImage = Image<kVk>(
			ImageCreateDesc<kVk>{
				rhi.CreatePrimaryDeviceObjectCreateDesc(std::format("Main RT DepthStencil Image {}", frameIt)),
				{{.extent = window.GetSwapchain().GetDesc().extent}},
				FindSupportedFormat(
					device.GetPhysicalDevice(),
					std::array{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
					VK_IMAGE_TILING_OPTIMAL,
					VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
						VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT/* | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT*/ | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED});

		// one render target per frame; replace any previous one (e.g. on resize), since CreateResource won't overwrite
		auto renderImageSetName = std::format("Main RT {}", frameIt);
		if (device.HasResource(renderImageSetName))
			device.EraseResource(uuids::uuid_name_generator{uuids::uuid_namespace_oid}(renderImageSetName));

		auto [renderImageSetUuid, renderImageSet, renderImageSetInserted] = device.CreateResource<RenderImageSet<kVk>>(renderImageSetName, std::move(colorImage), std::move(depthStencilImage));
		ENSURE(renderImageSetInserted);
		gRenderImageSetUuids[frameIt] = renderImageSetUuid;
	}

	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		
		auto cmd = graphicsQueue.GetPool().Commands();

		for (auto& frame : window.GetSwapchain().GetFrames())
		{
			frame.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			frame.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			frame.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		}

		for (const auto& renderImageSetGuid : std::span(gRenderImageSetUuids).first(frameCount))
		{
			auto& renderImageSet = *device.GetResource<RenderImageSet<kVk>>(renderImageSetGuid);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);
		}

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline}});

		graphicsSubmits |= graphicsQueue.Submit();
	}

	// Draw only writes the current frame's element of these arrays, but a dirty set is updated as a whole: after a
	// resize the other frames' elements would still reference the destroyed views. so write all of them up front,
	// with the layouts Draw uses (so Draw's own writes are skipped as unchanged).
	auto& pipeline = device.GetPipeline();
	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto& renderImageSet = *device.GetResource<RenderImageSet<kVk>>(gRenderImageSetUuids[frameIt]);
		auto& frame = window.GetSwapchain().GetFrames()[frameIt];

		pipeline.SetDescriptorData(
			"gTextures",
			DescriptorImageInfo<kVk>{
				.sampler={},
				.imageView=renderImageSet.GetAttachments()[0],
				.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			frameIt);

		pipeline.SetDescriptorData(
			"gRWTextures",
			DescriptorImageInfo<kVk>{
				.sampler={},
				.imageView=frame.GetAttachments()[0],
				.imageLayout=VK_IMAGE_LAYOUT_GENERAL},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
			frameIt);
	}
}

// recreates the swapchain at the current surface size, and everything sized after it. caller holds gDrawMutex.
// returns false if the surface has no area (minimized), in which case nothing is recreated.
bool RecreateWindowDependentObjects(RHI<kVk>& rhi, Window<kVk>& window)
{
	ZoneScopedN("RecreateWindowDependentObjects");

	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& swapchain = window.GetSwapchain();

	instance.UpdateSurfaceCapabilities(device.GetPhysicalDevice(), swapchain.GetSurface());
	auto extent = instance.GetSwapchainInfo(device.GetPhysicalDevice(), swapchain.GetSurface()).capabilities.currentExtent;
	if (extent.width == std::numeric_limits<uint32_t>::max()) // surface size is determined by the swapchain
		extent = swapchain.GetDesc().extent;
	if (extent.width == 0 || extent.height == 0)
		return false;

	device.WaitIdle();
	window.OnResizeFramebuffer(static_cast<int>(extent.width), static_cast<int>(extent.height));
	CreateWindowDependentObjects(rhi);

	return true;
}

} // namespace rhiapplication

void RHIApplication::PrepareDraw()
{
	ZoneScopedN("RHIApplication::PrepareDraw");

	using namespace rhiapplication;
	using namespace ImGui;

	auto& rhi = GetRHI<kVk>();
	auto& device = rhi.GetPrimaryDevice();

	ImGui_ImplGlfw_NewFrame(); // will poll glfw input events and update input state
	ImGui_ImplVulkan_NewFrame(); // calls ImGui_ImplVulkan_CreateFontsTexture
	NewFrame();

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	static bool gShowStatistics = false;
	{
		if (gShowStatistics)
		{
			if (Begin("Statistics", &gShowStatistics))
			{
				Text("Unknowns: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_UNKNOWN));
				Text("Instances: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_INSTANCE));
				Text("Physical Devices: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
				Text("Devices: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_DEVICE));
				Text("Queues: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_QUEUE));
				Text("Semaphores: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_SEMAPHORE));
				Text("Command Buffers: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_COMMAND_BUFFER));
				Text("Fences: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_FENCE));
				Text("Device Memory: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_DEVICE_MEMORY));
				Text("Buffers: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_BUFFER));
				Text("Images: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_IMAGE));
				Text("Events: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_EVENT));
				Text("Query Pools: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_QUERY_POOL));
				Text("Buffer Views: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_BUFFER_VIEW));
				Text("Image Views: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_IMAGE_VIEW));
				Text("Shader Modules: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_SHADER_MODULE));
				Text("Pipeline Caches: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_PIPELINE_CACHE));
				Text("Pipeline Layouts: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
				Text("Render Passes: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_RENDER_PASS));
				Text("Pipelines: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_PIPELINE));
				Text("Descriptor Set Layouts: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
				Text("Samplers: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_SAMPLER));
				Text("Descriptor Pools: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
				Text("Descriptor Sets: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_DESCRIPTOR_SET));
				Text("Framebuffers: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_FRAMEBUFFER));
				Text("Command Pools: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_COMMAND_POOL));
				Text("Surfaces: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_SURFACE_KHR));
				Text("Swapchains: %u", GetTypeCount<kVk>(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
			}
			End();
		}
	}
#endif

	if (gShowDemoWindow)
		ShowDemoWindow(&gShowDemoWindow);

	if (gShowAbout && Begin("About client", &gShowAbout))
	{
		End();
	}

	if (bool loading = gShowProgress.load(std::memory_order_relaxed) &&
				 Begin(
					 "Loading",
					 &loading,
					 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
						 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
	{
		constexpr uint8_t kProgressMax = 255;
		constexpr float kProgressWindowWidth = 160.0F;
		SetWindowSize(ImVec2(kProgressWindowWidth, 0));
		ProgressBar((1.F / kProgressMax) * static_cast<float>(gProgress));
		End();
	}

	auto resourcePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]);
	auto& window = rhi.GetWindow(GetCurrentWindow());

	// automation: SPEEDO_AUTOLOAD_MODEL / SPEEDO_AUTOLOAD_IMAGE name a file in resources/models / resources/images to
	// load at startup, through the same load + install path as the "File" menu.
	if (static bool gAutoLoadDone = false; !gAutoLoadDone)
	{
		gAutoLoadDone = true;

		const char* autoLoadModel = std::getenv("SPEEDO_AUTOLOAD_MODEL");
		const char* autoLoadImage = std::getenv("SPEEDO_AUTOLOAD_IMAGE");
		if (autoLoadModel || autoLoadImage)
		{
			auto [autoLoadTask, autoLoadFuture] = core::CreateTask(
				[&rhi, &device, resourcePath,
				 modelFile = std::string(autoLoadModel ? autoLoadModel : ""),
				 imageFile = std::string(autoLoadImage ? autoLoadImage : "")]
				{
					if (!modelFile.empty())
					{
						auto model = Model<kVk>::LoadModel((resourcePath / "models" / modelFile).string(), gProgress);
						auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData<kVk>*>(
							[&rhi, model](QueueTimelineContextData<kVk>* graphics) { InstallModel(rhi, *graphics, model); });
						rhi.drawCalls.enqueue(installTask);
					}
					if (!imageFile.empty())
					{
						auto [image, imageView] = Image<kVk>::LoadImage(device, (resourcePath / "images" / imageFile).string(), gProgress);
						auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData<kVk>*>(
							[&rhi, image, imageView](QueueTimelineContextData<kVk>* graphics) { InstallImage(rhi, *graphics, image, imageView); });
						rhi.drawCalls.enqueue(installTask);
					}
				});
			rhi.mainCalls.enqueue(autoLoadTask);
		}
	}


	if (BeginMainMenuBar())
	{
		if (BeginMenu("File"))
		{
			if (MenuItem("Open OBJ..."))
			{
				static const std::vector<nfdu8filteritem_t> kFilterList ={
					nfdu8filteritem_t{.name = "Wavefront OBJ", .spec = "obj"}
				};
				auto resourceUpdatedFuture = InternalOpenFileDialogueAsync((resourcePath / "models").string(), kFilterList,
					[&rhi](std::string_view filePath, std::atomic_uint8_t& progressOut){
						auto model = Model<kVk>::LoadModel(filePath, progressOut);
						auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData<kVk>*>(
							[&rhi, model](QueueTimelineContextData<kVk>* graphics) { InstallModel(rhi, *graphics, model); });
						rhi.drawCalls.enqueue(installTask);
						return installFuture;
					});
			}
			if (MenuItem("Open Image..."))
			{
				static const std::vector<nfdu8filteritem_t> kFilterList = {
					nfdu8filteritem_t{.name = "Image files", .spec = "jpg,jpeg,png,bmp,tga,gif,psd,hdr,pic,pnm"}
				};

				auto resourceUpdatedFuture = InternalOpenFileDialogueAsync((resourcePath / "images").string(), kFilterList, 
					[&rhi, &device](std::string_view filePath, std::atomic_uint8_t& progressOut){
						auto [image, imageView] = Image<kVk>::LoadImage(device, filePath, progressOut);
						auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData<kVk>*>(
							[&rhi, image, imageView](QueueTimelineContextData<kVk>* graphics) { InstallImage(rhi, *graphics, image, imageView); });
						rhi.drawCalls.enqueue(installTask);
						return installFuture;
					});
			}
			// if (MenuItem("Open Scene..."))
			// {
			// 	static const std::vector<nfdu8filteritem_t> filterList = {
			// 		nfdu8filteritem_t{.name = "Scene files", .spec = "gltf,glb"}
			// 	};

			// 	InternalOpenFileDialogueAsync((resourcePath / "scenes").string(), filterList, 
			// 		[&scene = Scene{}](std::string_view filePath, std::atomic_uint8_t& progress){ scene::LoadScene(scene, filePath, progress); });
			// }
			Separator();
			if (MenuItem("Exit", "CTRL+Q"))
				core::Application::Get()->RequestExit();

			ImGui::EndMenu();
		}
		if (BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (BeginMenu("Layout"))
			{
				Extent2d<kVk> splitScreenGrid = window.GetDesc().splitScreenGrid;

				//static bool hasChanged = 
				bool selected1x1 = splitScreenGrid.width == 1 && splitScreenGrid.height == 1;
				bool selected1x2 = splitScreenGrid.width == 1 && splitScreenGrid.height == 2;
				bool selected2x1 = splitScreenGrid.width == 2 && splitScreenGrid.height == 1;
				bool selected2x2 = splitScreenGrid.width == 2 && splitScreenGrid.height == 2;
				bool anyChanged = false;

				if (MenuItem("1x1", "Ctrl+1", &selected1x1) && selected1x1)
				{
					splitScreenGrid.width = 1;
					splitScreenGrid.height = 1;
					anyChanged = true;
				}
				else if (MenuItem("1x2", "Ctrl+2", &selected1x2) && selected1x2)
				{
					splitScreenGrid.width = 1;
					splitScreenGrid.height = 2;
					anyChanged = true;
				}
				else if (MenuItem("2x1", "Ctrl+3", &selected2x1) && selected2x1)
				{
					splitScreenGrid.width = 2;
					splitScreenGrid.height = 1;
					anyChanged = true;
				}
				else if (MenuItem("2x2", "Ctrl+4", &selected2x2) && selected2x2)
				{
					splitScreenGrid.width = 2;
					splitScreenGrid.height = 2;
					anyChanged = true;
				}

				ImGui::EndMenu();

				if (anyChanged)
					window.OnResizeSplitScreenGrid(splitScreenGrid.width, splitScreenGrid.height);
			}
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			{
				if (MenuItem("Statistics..."))
					gShowStatistics = !gShowStatistics;
			}
#endif
			ImGui::EndMenu();
		}
		if (BeginMenu("About"))
		{
			if (MenuItem("Show IMGUI Demo..."))
				gShowDemoWindow = !gShowDemoWindow;
			Separator();
			if (MenuItem("About client..."))
				gShowAbout = !gShowAbout;
			ImGui::EndMenu();
		}

		EndMainMenuBar();
	}

	// unfortunately, we need to lock here to prevent imgui from modifying internal data structures under our feet in the draw thread.
	// this was not needed before, but with the recent rewrites to imgui textures,
	// we get races with imgui setting completion codes for texture uploads in the render thread which are also read and modified here.
	// so we need to lock :(
	{
		std::unique_lock lock(gDrawMutex);
		Render();

		// process texture creates/updates/destroys here rather than in the render thread: the snapshot below
		// outlives this frame, and imgui frees ImTextureData (e.g. when the font atlas grows) on the next NewFrame().
		if (auto* data = GetDrawData(); data && data->Textures)
			for (ImTextureData* tex : *data->Textures)
				if (tex->Status != ImTextureStatus_OK)
					ImGui_ImplVulkan_UpdateTexture(tex);
	}

	static ImDrawData gDrawData{};
	if (auto *data = GetDrawData())
	{
		gIMGUIDrawDataSnapshot.SnapUsingSwap(data, &gDrawData, GetTime());

		// detach the snapshot from imgui-owned texture data: resolve texture refs to their (already uploaded)
		// backend ids, and drop the texture list so RenderDrawData in the render thread doesn't touch it.
		for (ImDrawList* drawList : gDrawData.CmdLists)
			for (ImDrawCmd& drawCmd : drawList->CmdBuffer)
				drawCmd.TexRef = ImTextureRef(drawCmd.GetTexID());
		gDrawData.Textures = nullptr;

		gIMGUIDrawData.enqueue(std::move(gDrawData));
	}
}

bool RHIApplication::Main()
{
	using namespace rhiapplication;
	
	ZoneScopedN("RHIApplication::Main");

	auto& rhi = GetRHI<kVk>();

	core::TaskHandle mainCall;
	while (rhi.mainCalls.try_dequeue(mainCall))
	{
		GetExecutor().Call(mainCall);
	}

	return !IsExitRequested();
}

void RHIApplication::OnInputStateChanged(const core::InputState& input)
{
	using namespace rhiapplication;
	
	ZoneScopedN("RHIApplication::OnInputStateChanged");

	auto& rhi = GetRHI<kVk>();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& imguiIO = ImGui::GetIO();

	if (imguiIO.WantSaveIniSettings)
	{
		size_t iniStringSize;
		const char* iniString = ImGui::SaveIniSettingsToMemory(&iniStringSize);
		myImGuiIniSettings.assign(iniString, iniStringSize);
		imguiIO.WantSaveIniSettings = false;
	}

	if (!imguiIO.WantCaptureMouse && !imguiIO.WantCaptureKeyboard)
		window.OnInputStateChanged(input);
}

bool RHIApplication::Draw()
{
	using namespace rhiapplication;

	FrameMark;
	ZoneScopedN("RHIApplication::Draw");

	std::unique_lock lock(gDrawMutex);
	std::vector<core::TaskHandle> frameTasks;

	auto& rhi = GetRHI<kVk>();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& swapchain = window.GetSwapchain();
	auto& pipeline = rhi.GetPrimaryDevice().GetPipeline();
	auto& executor = GetExecutor();

	if (window.IsMinimized())
		return false;

	// e.g. a fullscreen switch can leave the swapchain at the previous size without a matching resize event
	if (swapchain.NeedsRecreate() && !RecreateWindowDependentObjects(rhi, window))
		return false;

	auto [acquireNextImageSemaphore, lastFrameIndex, newFrameIndex, flipSuccess] = swapchain.Flip();

	// one lock at a time: queue types may alias the same context (and mutex), see Device::GetQueue
	auto queueFamilyIndex = [&device](QueueType type) { return device.GetQueue(type).Read()->queueFamilyIndex; };
	bool dedicatedTransfer = queueFamilyIndex(kQueueTypeTransfer) != queueFamilyIndex(kQueueTypeCompute);
	bool dedicatedCompute = queueFamilyIndex(kQueueTypeCompute) != queueFamilyIndex(kQueueTypeGraphics);

	if (flipSuccess)
	{
		auto& lastFrame = swapchain.GetFrames()[lastFrameIndex];
		auto& newFrame = swapchain.GetFrames()[newFrameIndex];

		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [lastGraphicsQueue, lastGraphicsSubmits] = graphics->queues.FetchAdd();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();

		frameTasks.emplace_back(
			core::CreateTask([&executor, &queue = graphicsQueue, &semaphore = graphics->semaphore]
			{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);

		for (auto& fence : graphicsSubmits.fences)
			fence.Wait();

		graphicsSubmits = {};
		graphicsQueue.SwapAndResetPool();
		
		core::TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("RHIApplication::Draw::drawCall");
			GetExecutor().Call(drawCall, graphics.Get().get());
		}
		
		auto& renderImageSet = *device.GetResource<RenderImageSet<kVk>>(gRenderImageSetUuids[newFrameIndex]);

		auto cmd = graphicsQueue.GetPool().Commands();

		//NOLINTBEGIN(bugprone-suspicious-stringview-data-usage)

		ZoneScopedN("RHIApplication::Draw::submit");

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);
		
		DrawMainPass(
			rhi,
			window,
			pipeline,
			graphicsQueue,
			cmd,
			newFrameIndex,
			graphics->timeline);
		{
			GPU_SCOPE(cmd, graphicsQueue, computeMain);

			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, 0);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);

			swapchain.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			swapchain.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			swapchain.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);

			pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);

			pipeline.SetDescriptorData(
				"gTextures",
				DescriptorImageInfo<kVk>{
					.sampler={},
					.imageView=renderImageSet.GetAttachments()[0],
					.imageLayout=renderImageSet.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				newFrameIndex);

			pipeline.SetDescriptorData(
				"gRWTextures",
				DescriptorImageInfo<kVk>{
					.sampler={},
					.imageView=swapchain.GetAttachments()[0],
					.imageLayout=swapchain.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				newFrameIndex);

			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES);
			pipeline.BindPipelineAuto(cmd);

			PushConstants pushConstants{.frameIndex = newFrameIndex};

			pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));

			// cover the whole swapchain image: ComputeMain has 16x16 threads per group, each copying a 16x16 pixel bucket
			constexpr uint32_t kComputePixelsPerGroup = 16U * 16U;
			auto dstExtent = swapchain.GetExtent();
			vkCmdDispatch(
				cmd,
				(dstExtent.width + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				(dstExtent.height + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				1U);
		}
		// {
		// 	GPU_SCOPE(cmd, graphicsQueue, copy);

		// 	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		// 	window.Copy(
		// 		cmd,
		// 		renderImageSet,
		// 		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		// 		0,
		// 		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		// 		0);
		// }
		// {
		// 	GPU_SCOPE(cmd, graphicsQueue, blit);

		// 	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		// 	window.Blit(
		// 		cmd,
		// 		renderImageSet,
		// 		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		// 		0,
		// 		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		// 		0,
		// 		VK_FILTER_NEAREST);
		// }
		std::vector<core::TaskHandle> graphicsCallbacks;
		{
			GPU_SCOPE(cmd, graphicsQueue, imgui);

			swapchain.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, 0);
			swapchain.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			swapchain.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			
			pipeline.SetRenderTarget(newFrame);
			
			swapchain.Begin(cmd, VK_SUBPASS_CONTENTS_INLINE);
			IMGUIDrawFunction(cmd);
			swapchain.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, Transition);
			
			swapchain.Transition(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		}

		cmd.End();
		//NOLINTEND(bugprone-suspicious-stringview-data-usage)

		auto presentInfo = swapchain.PreparePresent();

		SemaphoreHandle<kVk> acquireNextImageSemaphoreHandle = acquireNextImageSemaphore;
		auto graphicsDoneSemaphore = Semaphore<kVk>(
			SemaphoreCreateDesc<kVk>{
				rhi.CreatePrimaryDeviceObjectCreateDesc(std::format("graphicsDoneSemaphore{}", newFrameIndex)),
				VK_SEMAPHORE_TYPE_BINARY
			});
		SemaphoreHandle<kVk> graphicsDoneSemaphoreHandle = graphicsDoneSemaphore;
		// keeps the semaphore alive until the gpu has waited on it. the acquire fence is owned (and waited on) by the
		// swapchain instead, see Swapchain::myAcquireFences
		graphicsCallbacks.emplace_back(
			core::CreateTask(
				[acquireNextImageSemaphore = std::move(acquireNextImageSemaphore)] {}).handle);
		graphicsCallbacks.emplace_back(
			core::CreateTask(
				[&swapchain, presentIds = std::move(presentInfo.presentIds),
				 graphicsDoneSemaphore = std::move(graphicsDoneSemaphore)]
				 {
					for (auto presentId : presentIds)
						swapchain.WaitPresent(presentId);
				 }).handle);

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {graphics->semaphore, acquireNextImageSemaphoreHandle},
			.waitDstStageMasks = {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_NONE},
			.waitSemaphoreValues = {lastGraphicsSubmits.maxTimelineValue, 1},
			.signalSemaphores = {graphics->semaphore, graphicsDoneSemaphoreHandle},
			.signalSemaphoreValues = {++graphics->timeline, 1},
			.callbacks = std::move(graphicsCallbacks)});

		graphicsSubmits |= graphicsQueue.Submit();

		presentInfo.waitSemaphores.emplace_back(graphicsDoneSemaphoreHandle);

		if (dedicatedCompute)
		{
			auto compute = device.GetQueue(kQueueTypeCompute).Write();
			auto& [computeQueue, computeSubmits] = compute->queues.FetchAdd();

			for (auto& fence : computeSubmits.fences)
				fence.Wait();

			computeSubmits = {};
			computeQueue.SwapAndResetPool();

			computeQueue.EnqueuePresent(std::move(presentInfo));
			Result<kVk> presentResult = VK_SUCCESS;
			computeSubmits |= computeQueue.Present(&presentResult);
			swapchain.OnPresentResult(presentResult);
		}
		else
		{
			graphicsQueue.EnqueuePresent(std::move(presentInfo));
			Result<kVk> presentResult = VK_SUCCESS;
			graphicsSubmits |= graphicsQueue.Present(&presentResult);
			swapchain.OnPresentResult(presentResult);
		}
	}

	GetExecutor().Submit(frameTasks);

	return flipSuccess;
}

RHIApplication::RHIApplication(
	std::string_view appName, core::Environment&& env, CreateWindowFunc createWindowFunc)
	: Application(std::forward<std::string_view>(appName), std::forward<core::Environment>(env))
	, myRHI(static_cast<RHI<kVk>*>(::operator new(sizeof(RHI<kVk>)))) // storage only, constructed below
{
	using namespace rhiapplication;

	// matches the (non-aligned) operator delete used when myRHI is deleted through RHIBase
	static_assert(alignof(RHI<kVk>) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);

	// construct in place after myRHI is published, since objects created during RHI construction
	// (e.g. device queue semaphores) resolve their Instance via RHIApplication::GetRHI<kVk>().
	std::construct_at(
		static_cast<RHI<kVk>*>(myRHI.get()),
		RHIInitializationData{.name = appName, .createWindowFunc = createWindowFunc});

	auto& rhi = GetRHI<kVk>();
	auto& instance = rhi.GetInstance();
	auto& device = rhi.GetPrimaryDevice();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& pipeline = device.GetPipeline();

	std::vector<core::TaskHandle> timelineCallbacks;

	// todo: create some resource global storage
	constexpr uint32_t kBlackTextureWidth = 4;
	constexpr uint32_t kBlackTextureHeight = 4;
	constexpr uint32_t kBlackTextureSize = kBlackTextureWidth * kBlackTextureHeight * 4;
	
	auto [blackTextureUuid, blackTexture, blackTextureInserted] = device.CreateResource<Image<kVk>>(
		"Black Texture",
		ImageCreateDesc<kVk>{
			rhi.CreatePrimaryDeviceObjectCreateDesc(),
			{ImageMipLevelDesc<kVk>{.extent = Extent2d<kVk>{.width=kBlackTextureWidth, .height=kBlackTextureHeight}, .size = kBlackTextureSize, .offset = 0}},
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_TILING_LINEAR,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED
		});
	auto [blackTextureViewUuid, blackTextureView, blackTextureViewInserted] = device.CreateResource<ImageView<kVk>>(
		"Black Texture View",
		ImageViewCreateDesc<kVk>{
			rhi.CreatePrimaryDeviceObjectCreateDesc(),
			*blackTexture,
			blackTexture->GetDesc().format,
			VK_IMAGE_ASPECT_COLOR_BIT});
	gBlackTextureUuid = blackTextureUuid;
	gBlackTextureViewUuid = blackTextureViewUuid;

	std::vector<SamplerCreateInfo<kVk>> samplerCreateInfos;
	constexpr float kDefaultSamplerMaxAnisotropy = 16.0F;
	constexpr float kDefaultSamplerMaxLod = 1000.0F;
	samplerCreateInfos.emplace_back(SamplerCreateInfo<kVk>{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0U,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.mipLodBias = 0.0F,
		.anisotropyEnable = VK_TRUE,
		.maxAnisotropy = kDefaultSamplerMaxAnisotropy,
		.compareEnable = VK_FALSE,
		.compareOp = VK_COMPARE_OP_ALWAYS,
		.minLod = 0.0F,
		.maxLod = kDefaultSamplerMaxLod,
		.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_FALSE});
	auto [samplersUuid, samplers, samplersInserted] = device.CreateResource<SamplerVector<kVk>>(
		"Samplers",
		SamplerVectorCreateDesc<kVk>{
			rhi.CreatePrimaryDeviceObjectCreateDesc(),
			std::move(samplerCreateInfos)});
	gSamplersUuid = samplersUuid;

	// initialize stuff on graphics queue
	constexpr uint32_t kTextureId = kMaterialTextureId;
	constexpr uint32_t kSamplerId = 2;
	static_assert(kTextureId < SHADER_TYPES_GLOBAL_TEXTURE_COUNT);
	static_assert(kSamplerId < SHADER_TYPES_GLOBAL_SAMPLER_COUNT);
	{
		auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		
		IMGUIInit(window, rhi, pipeline, graphicsQueue, graphics->queues.Capacity(), myImGuiIniSettings);

		auto cmd = graphicsQueue.GetPool().Commands();

		blackTexture->Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		blackTexture->Clear(cmd, {.color = {{0.0F, 0.0F, 0.0F, 1.0F}}});
		blackTexture->Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		std::vector<MaterialData> materialData(SHADER_TYPES_MATERIAL_COUNT);
		materialData[0].color[0] = 1.0;
		materialData[0].color[1] = 0.0;
		materialData[0].color[2] = 0.0;
		materialData[0].color[3] = 1.0;
		materialData[0].textureAndSamplerId =
			(kTextureId << SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS) | kSamplerId;

		core::TaskCreateInfo<void> materialTransfersDone;
		auto [materialsUuid, materials, materialsInserted] = device.CreateResource<Buffer<kVk>>(
			"Materials",
			BufferCreateDesc<kVk>{
				rhi.CreatePrimaryDeviceObjectCreateDesc("Materials"),
				SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
			materialData.data(),
			cmd,
			materialTransfersDone);
		gMaterialsUuid = materialsUuid;
		timelineCallbacks.emplace_back(materialTransfersDone.handle);

		constexpr uint32_t kDefaultModelInstanceId = 666;
		constexpr uint32_t kMatrix4x4ElementCount = 16;
		std::vector<ModelInstance> modelInstances(SHADER_TYPES_MODEL_INSTANCE_COUNT);
		static const auto kIdentityMatrix = glm::mat4x4(1.0);
		std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[kDefaultModelInstanceId].modelTransform[0][0]);
		auto modelTransform = glm::make_mat4(&modelInstances[kDefaultModelInstanceId].modelTransform[0][0]);
		auto inverseTransposeModelTransform = glm::transpose(glm::inverse(modelTransform));
		std::copy_n(&inverseTransposeModelTransform[0][0], kMatrix4x4ElementCount, &modelInstances[kDefaultModelInstanceId].inverseTransposeModelTransform[0][0]);

		core::TaskCreateInfo<void> modelTransfersDone;
		auto [modelInstancesUuid, modelInstancesBuffer, modelInstancesInserted] = device.CreateResource<Buffer<kVk>>(
			"ModelInstances",
			BufferCreateDesc<kVk>{
				rhi.CreatePrimaryDeviceObjectCreateDesc("ModelInstances"),
				SHADER_TYPES_MODEL_INSTANCE_COUNT * sizeof(ModelInstance),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			},
			modelInstances.data(),
			cmd,
			modelTransfersDone);
		gModelInstancesUuid = modelInstancesUuid;
		timelineCallbacks.emplace_back(modelTransfersDone.handle);

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline},
			.callbacks = std::move(timelineCallbacks)});

		graphicsSubmits |= graphicsQueue.Submit();
	}

	auto shaderIncludePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["RootPath"]) / "src/rhi/shaders";
	auto shaderIntermediatePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / ".slang.intermediate";

	ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);

	auto shaderSourceFile = shaderIncludePath / "shaders.slang";

	const auto& [zPrepassShaderLayoutPairIt, zPrepassShaderLayoutWasInserted] = device.GetPipelineLayoutHandles().emplace(
		std::hash<std::string_view>{}("VertexZPrepass"),
		pipeline.CreateLayout(shaderLoader.Load<kVk>(
			shaderSourceFile,
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.target = SLANG_SPIRV,
				.targetProfile = "SPIRV_1_6",
				.entryPoints = {{"VertexZPrepass", SLANG_STAGE_VERTEX}},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			})));

	pipeline.BindLayoutAuto(zPrepassShaderLayoutPairIt->second, VK_PIPELINE_BIND_POINT_GRAPHICS);

	pipeline.SetDescriptorData(
		"gModelInstances",
		DescriptorBufferInfo<kVk>{.buffer = *device.GetResource<Buffer<kVk>>("ModelInstances"), .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		pipeline.SetDescriptorData(
			"gViewData",
			DescriptorBufferInfo<kVk>{.buffer = window.GetViewBuffer(i), .offset = 0, .range = VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	const auto& [mainShaderLayoutPairIt, mainShaderLayoutWasInserted] = device.GetPipelineLayoutHandles().emplace(
		std::hash<std::string_view>{}("Main"),
		pipeline.CreateLayout(shaderLoader.Load<kVk>(
			shaderSourceFile,
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.target = SLANG_SPIRV,
				.targetProfile = "SPIRV_1_6",
				.entryPoints = {
					{"VertexMain", SLANG_STAGE_VERTEX},
					{"FragmentMain", SLANG_STAGE_FRAGMENT},
					{"ComputeMain", SLANG_STAGE_COMPUTE},
				},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			})));

	pipeline.BindLayoutAuto(mainShaderLayoutPairIt->second, VK_PIPELINE_BIND_POINT_GRAPHICS);

	pipeline.SetDescriptorData(
		"gMaterialData",
		DescriptorBufferInfo<kVk>{.buffer = *device.GetResource<Buffer<kVk>>("Materials"), .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MATERIAL);

	pipeline.SetDescriptorData(
		"gModelInstances",
		DescriptorBufferInfo<kVk>{.buffer = *device.GetResource<Buffer<kVk>>("ModelInstances"), .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	pipeline.SetDescriptorData(
		"gSamplers",
		DescriptorImageInfo<kVk>{.sampler = (*device.GetResource<SamplerVector<kVk>>("Samplers"))[0]},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
		kSamplerId);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		pipeline.SetDescriptorData(
			"gViewData",
			DescriptorBufferInfo<kVk>{.buffer = window.GetViewBuffer(i), .offset = 0, .range = VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);

	CreateWindowDependentObjects(rhi);
}

RHIApplication::~RHIApplication()
{
	// can't tear down the rhi here, since Application::Get() already returns null (see Shutdown())
	ENSUREF(!myRHI, "RHIApplication::Shutdown() must be called before the application is released");
}

void RHIApplication::Shutdown()
{
	using namespace rhiapplication;

	ZoneScopedN("RHIApplication::Shutdown");

	if (!myRHI)
		return;

	auto& rhi = GetRHI<kVk>();
	auto& device = rhi.GetPrimaryDevice();
	auto& executor = GetExecutor();

	// let in-flight frame tasks (from the last Draw) finish before tearing anything down
	executor.JoinAll();

	device.WaitIdle();

	// all gpu work has completed, so every pending timeline callback is due (they own per-frame fences/semaphores etc).
	// queue locks are released before joining, since callbacks may access the queues themselves.
	// each distinct queue context once, locking one at a time: queue types may alias the same context (and mutex).
	{
		constexpr auto kAllTimelineValues = std::numeric_limits<uint64_t>::max();

		std::vector<QueueTimelineContext<kVk>*> visited;
		for (auto type : {kQueueTypeGraphics, kQueueTypeCompute, kQueueTypeTransfer})
		{
			auto* context = &device.GetQueue(type);
			if (std::ranges::contains(visited, context))
				continue;
			visited.emplace_back(context);

			auto queues = context->Write();
			for (auto& [queue, submits] : queues->queues)
				queue.SubmitCallbacks(executor, kAllTimelineValues);
		}
	}
	executor.JoinAll();

	ShutdownImgui();

	// not myRHI.reset(): that nulls myRHI before destroying the rhi, but objects destroyed along with it
	// (e.g. window view buffers) still resolve the (not yet destroyed) devices through GetRHI()
	delete myRHI.get();
	(void)myRHI.release();
}

void RHIApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhiapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("RHIApplication::OnResizeFramebuffer");

	auto& rhi = GetRHI<kVk>();
	auto& rhiWindow = rhi.GetWindow(window);

	// minimizing reports 0x0: keep the swapchain, and have Draw skip frames until the window is restored
	rhiWindow.SetMinimized(width <= 0 || height <= 0);
	if (rhiWindow.IsMinimized())
		return;

	// the swapchain is sized after the surface (which already has this size) rather than width/height
	RecreateWindowDependentObjects(rhi, rhiWindow);
}

WindowState* RHIApplication::GetWindowState(WindowHandle window)
{
	return &GetRHI<kVk>().GetWindow(window).GetState();
}

uint32_t RHIApplication::GetWindowCount() const noexcept
{
	return GetRHI<kVk>().GetWindows().size();
}

WindowHandle RHIApplication::GetWindow(uint32_t index) const noexcept
{
	return *std::next(GetRHI<kVk>().GetWindows().begin(), index);
}

} // namespace rhi
