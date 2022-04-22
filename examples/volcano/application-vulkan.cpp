#include "application.h"
#include "gltfstream.h"
#include "nodes/inputoutputnode.h"
#include "nodes/slangshadernode.h"
#include "resources/shaders/shadertypes.h"
#include "vk-utils.h"
#include "volcano.h"

#include <stb_sprintf.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <imnodes.h>

template <>
void Application<Vk>::initIMGUI(
	const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
	CommandBufferHandle<Vk> commands,
	RenderPassHandle<Vk> renderPass,
	SurfaceHandle<Vk> surface,
	const std::filesystem::path& userProfilePath) const
{
	ZoneScopedN("Application::initIMGUI");

	using namespace ImGui;

	IMGUI_CHECKVERSION();
	CreateContext();
	auto& io = GetIO();
	static auto iniFilePath = (userProfilePath / "imgui.ini").generic_string();
	io.IniFilename = iniFilePath.c_str();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.FontGlobalScale = 1.0f;
	//io.FontAllowUserScaling = true;

	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	auto& platformIo = ImGui::GetPlatformIO();
	platformIo.Platform_CreateVkSurface =
		(decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(
			myInstance->getInstance(), "vkCreateWin32SurfaceKHR");

	const auto& surfaceCapabilities =
		myInstance->getSwapchainInfo(myDevice->getPhysicalDevice(), surface).capabilities;

	float dpiScaleX = static_cast<float>(surfaceCapabilities.currentExtent.width) /
					  myMainWindow->getConfig().windowExtent.width;
	float dpiScaleY = static_cast<float>(surfaceCapabilities.currentExtent.height) /
					  myMainWindow->getConfig().windowExtent.height;

	io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.PixelSnapH = false;

	io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(volcano_getResourcePath());
	fontPath /= "fonts";
	fontPath /= "foo";

	const char* fonts[] = {
		"Cousine-Regular.ttf",
		"DroidSans.ttf",
		"Karla-Regular.ttf",
		"ProggyClean.ttf",
		"ProggyTiny.ttf",
		"Roboto-Medium.ttf",
	};

	ImFont* defaultFont = nullptr;
	for (auto font : fonts)
	{
		fontPath.replace_filename(font);
		defaultFont =
			io.Fonts->AddFontFromFileTTF(fontPath.generic_string().c_str(), 16.0f, &config);
	}

	// Setup style
	StyleColorsClassic();
	io.FontDefault = defaultFont;

	// Setup Vulkan binding
	ImGui_ImplVulkan_InitInfo initInfo = {};
	initInfo.Instance = myInstance->getInstance();
	initInfo.PhysicalDevice = myDevice->getPhysicalDevice();
	initInfo.Device = myDevice->getDevice();
	initInfo.QueueFamily = myGraphicsQueues.front().getDesc().queueFamilyIndex;
	initInfo.Queue = myGraphicsQueues.front();
	initInfo.PipelineCache = myPipeline->getCache();
	initInfo.DescriptorPool = myPipeline->getDescriptorPool();
	initInfo.MinImageCount = myMainWindow->getConfig().imageCount;
	initInfo.ImageCount = myMainWindow->getConfig().imageCount;
	initInfo.Allocator = nullptr;
	// initInfo.HostAllocationCallbacks = nullptr;
	initInfo.CheckVkResultFn = checkResult;
	initInfo.DeleteBufferFn = [](void* user_data,
								 VkBuffer buffer,
								 VkDeviceMemory buffer_memory,
								 const VkAllocationCallbacks* allocator)
	{
		DeviceContext<Vk>* deviceContext = static_cast<DeviceContext<Vk>*>(user_data);
		deviceContext->addTimelineCallback(
			[device = deviceContext->getDevice(), buffer, buffer_memory, allocator](uint64_t)
			{
				if (buffer != VK_NULL_HANDLE)
					vkDestroyBuffer(device, buffer, allocator);
				if (buffer_memory != VK_NULL_HANDLE)
					vkFreeMemory(device, buffer_memory, allocator);
			});
	};
	initInfo.UserData = deviceContext.get();
	ImGui_ImplVulkan_Init(&initInfo, renderPass);

	// Upload Fonts
	ImGui_ImplVulkan_CreateFontsTexture(commands);

	deviceContext->addTimelineCallback(std::make_tuple(
		1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed),
		[](uint64_t) { ImGui_ImplVulkan_DestroyFontUploadObjects(); }));

	imnodes::Initialize();
	imnodes::LoadCurrentEditorStateFromIniString(
		myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

template <>
void Application<Vk>::shutdownIMGUI()
{
	size_t count;
	myNodeGraph.layout.assign(imnodes::SaveCurrentEditorStateToIniString(&count));
	imnodes::Shutdown();

	ImGui_ImplVulkan_Shutdown();
	ImGui::DestroyContext();
}

template <>
void Application<Vk>::createWindowDependentObjects(Extent2d<Vk> frameBufferExtent)
{
	ZoneScopedN("Application::createWindowDependentObjects");

	auto colorImage = std::make_shared<Image<Vk>>(
		myDevice,
		ImageCreateDesc<Vk>{
			{{frameBufferExtent}},
			myMainWindow->getConfig().surfaceFormat.format,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	auto depthStencilImage = std::make_shared<Image<Vk>>(
		myDevice,
		ImageCreateDesc<Vk>{
			{{frameBufferExtent}},
			findSupportedFormat(
				myDevice->getPhysicalDevice(),
				{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
				VK_IMAGE_TILING_OPTIMAL,
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
					VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	myRenderImageSet =
		std::make_shared<RenderImageSet<Vk>>(myDevice, make_vector(colorImage), depthStencilImage);

	myPipeline->setRenderTarget(myRenderImageSet);
}

template <>
Application<Vk>::Application(void* windowHandle, int width, int height)
	: myInstance(std::make_shared<InstanceContext<Vk>>())
	, myExecutor(std::max(1u, std::thread::hardware_concurrency() - 1))
{
	ZoneScopedN("Application()");

	auto rootPath = std::filesystem::path(volcano_getRootPath());
	auto resourcePath = std::filesystem::path(volcano_getResourcePath());
	auto userProfilePath = std::filesystem::path(volcano_getUserProfilePath());

	auto shaderIncludePath = resourcePath / "shaders";
	auto shaderIntermediatePath = userProfilePath / ".slang.intermediate";

	auto vkSDKPathEnv = std::getenv("VK_SDK_PATH");
	auto vkSDKPath = vkSDKPathEnv ? std::filesystem::path(vkSDKPathEnv) : rootPath;
	auto vkSDKBinPath = vkSDKPath / "bin";

	ShaderLoader shaderLoader(
		{shaderIncludePath},
		{/*std::make_tuple(SLANG_SOURCE_LANGUAGE_HLSL, SLANG_PASS_THROUGH_DXC, vkSDKBinPath)*/},
		shaderIntermediatePath);

	auto shaderReflection = shaderLoader.load<Vk>(resourcePath / "shaders" / "shaders.slang");

	auto surface = createSurface(myInstance->getInstance(), windowHandle);

	auto detectSuitableGraphicsDevice = [](auto instance, auto surface)
	{
		const auto& physicalDevices = instance->getPhysicalDevices();

		std::vector<std::tuple<uint32_t, uint32_t>> graphicsDeviceCandidates;
		graphicsDeviceCandidates.reserve(physicalDevices.size());

		std::cout << physicalDevices.size() << " vulkan physical device(s) found: " << std::endl;

		for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < physicalDevices.size();
			 physicalDeviceIt++)
		{
			auto physicalDevice = physicalDevices[physicalDeviceIt];

			const auto& physicalDeviceInfo = instance->getPhysicalDeviceInfo(physicalDevice);
			const auto& swapchainInfo = instance->getSwapchainInfo(physicalDevice, surface);

			std::cout << physicalDeviceInfo.deviceProperties.properties.deviceName << std::endl;

			for (uint32_t queueFamilyIt = 0;
				 queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size();
				 queueFamilyIt++)
			{
				const auto& queueFamilyProperties =
					physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
				const auto& queueFamilyPresentSupport =
					swapchainInfo.queueFamilyPresentSupport[queueFamilyIt];

				if (queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT &&
					queueFamilyPresentSupport)
					graphicsDeviceCandidates.emplace_back(
						std::make_tuple(physicalDeviceIt, queueFamilyIt));
			}
		}

		std::sort(
			graphicsDeviceCandidates.begin(),
			graphicsDeviceCandidates.end(),
			[&instance, &physicalDevices](const auto& lhs, const auto& rhs)
			{
				constexpr uint32_t deviceTypePriority[] = {
					4,		   //VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
					1,		   //VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
					0,		   //VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
					2,		   //VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
					3,		   //VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
					0x7FFFFFFF //VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM = 0x7FFFFFFF
				};
				const auto& [lhsPhysicalDeviceIndex, lhsQueueFamilyIndex] = lhs;
				const auto& [rhsPhysicalDeviceIndex, rhsQueueFamilyIndex] = rhs;

				auto lhsDeviceType =
					instance->getPhysicalDeviceInfo(physicalDevices[lhsPhysicalDeviceIndex])
						.deviceProperties.properties.deviceType;
				auto rhsDeviceType =
					instance->getPhysicalDeviceInfo(physicalDevices[rhsPhysicalDeviceIndex])
						.deviceProperties.properties.deviceType;

				return deviceTypePriority[lhsDeviceType] < deviceTypePriority[rhsDeviceType];
			});

		if (graphicsDeviceCandidates.empty())
			throw std::runtime_error("failed to find a suitable GPU!");

		return std::get<0>(graphicsDeviceCandidates.front());
	};

	myDevice = std::make_shared<DeviceContext<Vk>>(
		myInstance, DeviceConfiguration<Vk>{detectSuitableGraphicsDevice(myInstance, surface)});

	auto detectSuitableSwapchain = [](auto instance, auto device, auto surface)
	{
		const auto& swapchainInfo =
			instance->getSwapchainInfo(device->getPhysicalDevice(), surface);

		SwapchainConfiguration<Vk> config = {swapchainInfo.capabilities.currentExtent};

		static constexpr Format<Vk> requestSurfaceImageFormat[] = {
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_FORMAT_B8G8R8_UNORM,
			VK_FORMAT_R8G8B8_UNORM};
		static constexpr ColorSpace<Vk> requestSurfaceColorSpace =
			VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		static constexpr PresentMode<Vk> requestPresentMode[] = {
			VK_PRESENT_MODE_MAILBOX_KHR,
			VK_PRESENT_MODE_FIFO_RELAXED_KHR,
			VK_PRESENT_MODE_FIFO_KHR,
			VK_PRESENT_MODE_IMMEDIATE_KHR};

		// Request several formats, the first found will be used
		// If none of the requested image formats could be found, use the first available
		for (uint32_t requestIt = 0ul; requestIt < std::ssize(requestSurfaceImageFormat);
			 requestIt++)
		{
			SurfaceFormat<Vk> requestedFormat = {
				requestSurfaceImageFormat[requestIt], requestSurfaceColorSpace};

			auto formatIt = std::find_if(
				swapchainInfo.formats.begin(),
				swapchainInfo.formats.end(),
				[&requestedFormat](VkSurfaceFormatKHR format)
				{
					return requestedFormat.format == format.format &&
						   requestedFormat.colorSpace == format.colorSpace;
				});

			if (formatIt != swapchainInfo.formats.end())
			{
				config.surfaceFormat = *formatIt;
				break;
			}
		}

		// Request a certain mode and confirm that it is available. If not use
		// VK_PRESENT_MODE_FIFO_KHR which is mandatory
		for (uint32_t requestIt = 0ul; requestIt < std::ssize(requestPresentMode); requestIt++)
		{
			auto modeIt = std::find(
				swapchainInfo.presentModes.begin(),
				swapchainInfo.presentModes.end(),
				requestPresentMode[requestIt]);

			if (modeIt != swapchainInfo.presentModes.end())
			{
				config.presentMode = *modeIt;

				switch (config.presentMode)
				{
				case VK_PRESENT_MODE_MAILBOX_KHR:
					config.imageCount = 3;
					break;
				default:
					config.imageCount = 2;
					break;
				}

				break;
			}
		}

		return config;
	};

	myPipeline = std::make_shared<PipelineContext<Vk>>(
		myDevice, PipelineConfiguration<Vk>{userProfilePath / "pipeline.cache"});

	myMainWindow = std::make_shared<WindowContext<Vk>>(
		myDevice,
		std::move(surface),
		WindowConfiguration<Vk>{
			detectSuitableSwapchain(myInstance, myDevice, surface),
			{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
			{1ul, 1ul}});

	{
		uint32_t frameCount = myMainWindow->getConfig().imageCount;

		auto& primaryContexts = myCommands[CommandContextType_GeneralPrimary];
		auto& secondaryContexts = myCommands[CommandContextType_GeneralSecondary];
		auto& generalTransferContexts = myCommands[CommandContextType_GeneralTransfer];
		auto& dedicatedComputeContexts = myCommands[CommandContextType_DedicatedCompute];
		auto& dedicatedTransferContexts = myCommands[CommandContextType_DedicatedTransfer];

		VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

		static constexpr bool useCommandBufferCreateReset = true;
		if (useCommandBufferCreateReset)
			cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		const auto& queueFamilies = myDevice->getQueueFamilies();
		for (auto queueFamilyIt = queueFamilies.begin(); queueFamilyIt != queueFamilies.end();
			 queueFamilyIt++)
		{
			const auto& queueFamily = *queueFamilyIt;
			auto queueFamilyIndex =
				static_cast<uint32_t>(std::distance(queueFamilies.begin(), queueFamilyIt));

			if (queueFamily.flags & QueueFamilyFlagBits_Graphics)
			{
				for (uint32_t frameIt = 0ul; frameIt < frameCount; frameIt++)
				{
					primaryContexts.emplace_back(CommandPoolContext<Vk>(
						myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

					for (uint32_t secondaryContextIt = 0ul; secondaryContextIt < 4;
						 secondaryContextIt++)
					{
						secondaryContexts.emplace_back(CommandPoolContext<Vk>(
							myDevice,
							CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));
					}
				}

				generalTransferContexts.emplace_back(CommandPoolContext<Vk>(
					myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				myGraphicsQueues.emplace_back(QueueContext<Vk>(
					myDevice,
					QueueContextCreateDesc<Vk>{
						0,
						queueFamilyIndex,
						primaryContexts.front().commands(
							CommandBufferAccessScopeDesc<Vk>(false))}));
			}
			else if (queueFamily.flags & QueueFamilyFlagBits_Compute)
			{
				// only need one for now..

				dedicatedComputeContexts.emplace_back(CommandPoolContext<Vk>(
					myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				myComputeQueues.emplace_back(
					QueueContext<Vk>(myDevice, QueueContextCreateDesc<Vk>{0, queueFamilyIndex}));
			}
			else if (queueFamily.flags & QueueFamilyFlagBits_Transfer)
			{
				// only need one for now..

				dedicatedTransferContexts.emplace_back(CommandPoolContext<Vk>(
					myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				myTransferQueues.emplace_back(
					QueueContext<Vk>(myDevice, QueueContextCreateDesc<Vk>{0, queueFamilyIndex}));
			}
		}
	}

	createWindowDependentObjects(myMainWindow->getConfig().extent);

	// todo: create some resource global storage
	myPipeline->resources().black = std::make_shared<Image<Vk>>(
		myDevice,
		ImageCreateDesc<Vk>{
			std::make_vector(ImageMipLevelDesc<Vk>{Extent2d<Vk>{4, 4}, 16 * 4, 0}),
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_TILING_LINEAR,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	myPipeline->resources().blackImageView = std::make_shared<ImageView<Vk>>(
		myDevice, *myPipeline->resources().black, VK_IMAGE_ASPECT_COLOR_BIT);

	std::vector<SamplerCreateInfo<Vk>> samplerCreateInfos;
	samplerCreateInfos.emplace_back(SamplerCreateInfo<Vk>{
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		nullptr,
		0u,
		VK_FILTER_LINEAR,
		VK_FILTER_LINEAR,
		VK_SAMPLER_MIPMAP_MODE_LINEAR,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		0.0f,
		VK_TRUE,
		16.f,
		VK_FALSE,
		VK_COMPARE_OP_ALWAYS,
		0.0f,
		1000.0f,
		VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		VK_FALSE});
	myPipeline->resources().samplers =
		std::make_shared<SamplerVector<Vk>>(myDevice, std::move(samplerCreateInfos));
	//

	// initialize stuff on general transfer queue
	{
		auto& generalTransferContext = myCommands[CommandContextType_GeneralTransfer].fetchAdd();
		auto cmd = generalTransferContext.commands();

		myPipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		myPipeline->resources().black->clear(cmd, {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}});
		myPipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		initIMGUI(
			myDevice,
			cmd,
			myMainWindow->getRenderPass(),
			myMainWindow->getSurface(),
			userProfilePath);

		cmd.end();

		myGraphicsQueues.front().enqueueSubmit(generalTransferContext.prepareSubmit(
			{{myDevice->getTimelineSemaphore()},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
			 {myGraphicsQueues.front().getLastSubmitTimelineValue().value_or(0)},
			 {myDevice->getTimelineSemaphore()},
			 {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		myGraphicsQueues.front().submit();
	}

	constexpr uint32_t textureId = 1;
	constexpr uint32_t samplerId = 2;
	static_assert(textureId < ShaderTypes_GlobalTextureCount);
	static_assert(samplerId < ShaderTypes_GlobalSamplerCount);
	{
		auto& dedicatedTransferContext =
			myCommands[CommandContextType_DedicatedTransfer].fetchAdd();

		auto materialData = std::make_unique<MaterialData[]>(ShaderTypes_MaterialCount);
		materialData[0].color = glm::vec4(1.0, 0.0, 0.0, 1.0);
		materialData[0].textureAndSamplerId =
			(textureId << ShaderTypes_GlobalTextureIndexBits) | samplerId;
		myMaterials = std::make_unique<Buffer<Vk>>(
			myDevice,
			dedicatedTransferContext,
			BufferCreateDesc<Vk>{
				ShaderTypes_MaterialCount * sizeof(MaterialData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
			materialData.get());

		auto objectData = std::make_unique<ObjectData[]>(ShaderTypes_ObjectBufferInstanceCount);
		auto identityMatrix = glm::mat4x4(1.0f);
		objectData[666].modelTransform = identityMatrix;
		objectData[666].inverseTransposeModelTransform =
			glm::transpose(glm::inverse(identityMatrix));
		myObjects = std::make_unique<Buffer<Vk>>(
			myDevice,
			dedicatedTransferContext,
			BufferCreateDesc<Vk>{
				ShaderTypes_ObjectBufferInstanceCount * sizeof(ObjectData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
			objectData.get());

		myTransferQueues.front().enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {myDevice->getTimelineSemaphore()},
			 {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		myTransferQueues.front().submit();
	}

	// set global descriptor set data

	auto [layoutIt, insertResult] =
		myLayouts.emplace(std::make_shared<PipelineLayout<Vk>>(myDevice, shaderReflection));
	assert(insertResult);
	myPipeline->setLayout(*layoutIt, VK_PIPELINE_BIND_POINT_GRAPHICS);

	myPipeline->setDescriptorData(
		"g_viewData",
		DescriptorBufferInfo<Vk>{myMainWindow->getViewBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_View);

	myPipeline->setDescriptorData(
		"g_materialData",
		DescriptorBufferInfo<Vk>{*myMaterials, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Material);

	myPipeline->setDescriptorData(
		"g_objectData",
		DescriptorBufferInfo<Vk>{*myObjects, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Object,
		42);

	myPipeline->setDescriptorData(
		"g_samplers",
		DescriptorImageInfo<Vk>{(*myPipeline->resources().samplers)[0]},
		DescriptorSetCategory_GlobalSamplers,
		samplerId);

	// GUI + misc callbacks

	auto openFileDialogue = [](const std::filesystem::path& resourcePath,
							   const nfdchar_t* filterList,
							   std::function<uint32_t(nfdchar_t*)>&& onCompletionCallback)
	{
		auto resourcePathStr = resourcePath.string();
		nfdchar_t* openFilePath;
		return std::make_tuple(
			NFD_OpenDialog(filterList, resourcePathStr.c_str(), &openFilePath),
			openFilePath,
			std::forward<std::function<uint32_t(nfdchar_t*)>>(onCompletionCallback));
	};

	auto loadModel = [this](nfdchar_t* openFilePath)
	{
		auto& dedicatedTransferContext =
			myCommands[CommandContextType_DedicatedTransfer].fetchAdd();

		std::rotate(
			myTransferQueues.begin(), std::next(myTransferQueues.begin()), myTransferQueues.end());

		myDevice->wait(myTransferQueues.front().getLastSubmitTimelineValue().value_or(0));

		dedicatedTransferContext.reset();

		myPipeline->setModel(
			std::make_shared<Model<Vk>>(myDevice, dedicatedTransferContext, openFilePath));

		myTransferQueues.front().enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {myDevice->getTimelineSemaphore()},
			 {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		myTransferQueues.front().submit();

		return 1;
	};

	auto loadImage = [this](nfdchar_t* openFilePath)
	{
		auto& dedicatedTransferContext =
			myCommands[CommandContextType_DedicatedTransfer].fetchAdd();

		std::rotate(
			myTransferQueues.begin(), std::next(myTransferQueues.begin()), myTransferQueues.end());

		myDevice->wait(myTransferQueues.front().getLastSubmitTimelineValue().value_or(0));

		dedicatedTransferContext.reset();

		myPipeline->resources().image =
			std::make_shared<Image<Vk>>(myDevice, dedicatedTransferContext, openFilePath);
		myPipeline->resources().imageView = std::make_shared<ImageView<Vk>>(
			myDevice, *myPipeline->resources().image, VK_IMAGE_ASPECT_COLOR_BIT);

		myTransferQueues.front().enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {myDevice->getTimelineSemaphore()},
			 {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		myTransferQueues.front().submit();

		///////////

		auto& generalTransferContext = myCommands[CommandContextType_GeneralTransfer].fetchAdd();
		auto cmd = generalTransferContext.commands();

		myPipeline->resources().image->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		cmd.end();

		myGraphicsQueues.front().enqueueSubmit(generalTransferContext.prepareSubmit(
			{{myDevice->getTimelineSemaphore()},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
			 {myTransferQueues.front().getLastSubmitTimelineValue().value_or(0)},
			 {myDevice->getTimelineSemaphore()},
			 {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		myGraphicsQueues.front().submit();

		///////////

		myPipeline->setDescriptorData(
			"g_textures",
			DescriptorImageInfo<Vk>{
				{},
				*myPipeline->resources().imageView,
				myPipeline->resources().image->getImageLayout()},
			DescriptorSetCategory_GlobalTextures,
			1);

		return 1;
	};

	auto loadGlTF = [](nfdchar_t* openFilePath)
	{
		try
		{
			std::filesystem::path path(openFilePath);

			if (path.is_relative())
				throw std::runtime_error("Command line argument path is not absolute");

			if (!path.has_filename())
				throw std::runtime_error("Command line argument path has no filename");

			if (!path.has_extension())
				throw std::runtime_error("Command line argument path has no filename extension");

			gltfstream::PrintInfo(path);
		}
		catch (const std::runtime_error& ex)
		{
			std::cerr << "Error! - ";
			std::cerr << ex.what() << "\n";

			throw;
		}

		return 0;
	};

	myIMGUIPrepareDrawFunction =
		[this, openFileDialogue, loadModel, loadImage, loadGlTF, resourcePath]
	{
		ZoneScopedN("Application::IMGUIPrepareDraw");

		using namespace ImGui;

		ImGui_ImplVulkan_NewFrame();
		NewFrame();

		// todo: move elsewhere
		auto editableTextField = [](int id,
									const char* label,
									std::string& str,
									float maxTextWidth,
									std::optional<int>& selected)
		{
			auto textSize =
				std::max(maxTextWidth, CalcTextSize(str.c_str(), str.c_str() + str.size()).x);

			PushItemWidth(textSize);
			//PushClipRect(textSize);

			if (id == selected.value_or(0))
			{
				if (IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IsMouseClicked(0))
				{
					PushAllowKeyboardFocus(true);
					SetKeyboardFocusHere();

					if (InputText(
							label,
							&str,
							ImGuiInputTextFlags_EnterReturnsTrue |
								ImGuiInputTextFlags_CallbackAlways,
							[](ImGuiInputTextCallbackData* data)
							{
								//PopClipRect();
								PopItemWidth();

								auto textSize = std::max(
									*static_cast<float*>(data->UserData),
									CalcTextSize(data->Buf, data->Buf + data->BufTextLen).x);

								PushItemWidth(textSize);
								//PushClipRect(textSize);

								data->SelectionStart = data->SelectionEnd;

								return 0;
							},
							&maxTextWidth))
					{
						if (str.empty())
							str = "Empty";

						selected.reset();
					}

					PopAllowKeyboardFocus();
				}
				else
				{
					if (str.empty())
						str = "Empty";

					selected.reset();
				}
			}
			else
			{
				TextUnformatted(str.c_str(), str.c_str() + str.size());
			}

			//PopClipRect();
			PopItemWidth();

			return std::max(maxTextWidth, CalcTextSize(str.c_str(), str.c_str() + str.size()).x);
			;
		};

		static bool showStatistics = false;
		(void)showStatistics;
		if constexpr (PROFILING_ENABLED)
		{
			if (showStatistics)
			{
				if (Begin("Statistics", &showStatistics))
				{
					Text("Unknowns: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_UNKNOWN));
					Text("Instances: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_INSTANCE));
					Text(
						"Physical Devices: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
					Text("Devices: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DEVICE));
					Text("Queues: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_QUEUE));
					Text("Semaphores: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
					Text(
						"Command Buffers: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
					Text("Fences: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_FENCE));
					Text("Device Memory: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
					Text("Buffers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_BUFFER));
					Text("Images: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_IMAGE));
					Text("Events: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_EVENT));
					Text("Query Pools: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
					Text("Buffer Views: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
					Text("Image Views: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
					Text(
						"Shader Modules: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
					Text(
						"Pipeline Caches: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
					Text(
						"Pipeline Layouts: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
					Text("Render Passes: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
					Text("Pipelines: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE));
					Text(
						"Descriptor Set Layouts: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
					Text("Samplers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SAMPLER));
					Text(
						"Descriptor Pools: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
					Text(
						"Descriptor Sets: %u",
						myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
					Text("Framebuffers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
					Text("Command Pools: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
					Text("Surfaces: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
					Text("Swapchains: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
				}
				End();
			}
		}

		static bool showDemoWindow = false;
		if (showDemoWindow)
			ShowDemoWindow(&showDemoWindow);

		static bool showAbout = false;
		if (showAbout)
		{
			if (Begin("About volcano", &showAbout)) {}
			End();
		}

		static bool showNodeEditor = false;
		if (showNodeEditor)
		{
			ImGui::SetNextWindowSize(ImVec2(800, 450), ImGuiCond_FirstUseEver);

			Begin("Node Editor Window", &showNodeEditor);

			PushAllowKeyboardFocus(false);

			imnodes::BeginNodeEditor();

			for (const auto& node : myNodeGraph.nodes)
			{
				char buffer[64];

				imnodes::BeginNode(node->id());

				// title bar
				stbsp_sprintf(buffer, "##node%.*u", 4, node->id());

				imnodes::BeginNodeTitleBar();

				float titleBarTextWidth =
					editableTextField(node->id(), buffer, node->name(), 160.0f, node->selected());

				imnodes::EndNodeTitleBar();

				if (IsItemClicked() && IsMouseDoubleClicked(0))
					node->selected() = std::make_optional(node->id());

				// attributes
				if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
				{
					auto rowCount = std::max(
						inOutNode->inputAttributes().size(), inOutNode->outputAttributes().size());

					for (uint32_t rowIt = 0ul; rowIt < rowCount; rowIt++)
					{
						float inputTextWidth = 0.0f;
						bool hasInputPin = rowIt < inOutNode->inputAttributes().size();
						if (hasInputPin)
						{
							auto& inputAttribute = inOutNode->inputAttributes()[rowIt];
							stbsp_sprintf(buffer, "##inputattribute%.*u", 4, inputAttribute.id);

							imnodes::BeginInputAttribute(inputAttribute.id);

							inputTextWidth = editableTextField(
								inputAttribute.id,
								buffer,
								inputAttribute.name,
								80.0f,
								node->selected());

							imnodes::EndInputAttribute();

							if (IsItemClicked() && IsMouseDoubleClicked(0))
								node->selected() = std::make_optional(inputAttribute.id);
						}

						if (rowIt < inOutNode->outputAttributes().size())
						{
							auto& outputAttribute = inOutNode->outputAttributes()[rowIt];
							stbsp_sprintf(buffer, "##outputattribute%.*u", 4, outputAttribute.id);

							if (hasInputPin)
								SameLine();

							imnodes::BeginOutputAttribute(outputAttribute.id);

							float outputTextWidth =
								CalcTextSize(
									outputAttribute.name.c_str(),
									outputAttribute.name.c_str() + outputAttribute.name.size())
									.x;

							if (hasInputPin)
								Indent(
									std::max(titleBarTextWidth, inputTextWidth + outputTextWidth) -
									outputTextWidth);
							else
								Indent(
									std::max(titleBarTextWidth, outputTextWidth + 80.0f) -
									outputTextWidth);

							editableTextField(
								outputAttribute.id,
								buffer,
								outputAttribute.name,
								80.0f,
								node->selected());

							imnodes::EndOutputAttribute();

							if (IsItemClicked() && IsMouseDoubleClicked(0))
								node->selected() = std::make_optional(outputAttribute.id);
						}
					}
				}

				imnodes::EndNode();

				if (BeginPopupContextItem())
				{
					if (Selectable("Add Input"))
					{
						if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
						{
							stbsp_sprintf(
								buffer,
								"In %u",
								static_cast<unsigned>(inOutNode->inputAttributes().size()));
							inOutNode->inputAttributes().emplace_back(
								Attribute{++myNodeGraph.uniqueId, buffer});
						}
					}
					if (Selectable("Add Output"))
					{
						if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
						{
							stbsp_sprintf(
								buffer,
								"Out %u",
								static_cast<unsigned>(inOutNode->outputAttributes().size()));
							inOutNode->outputAttributes().emplace_back(
								Attribute{++myNodeGraph.uniqueId, buffer});
						}
					}
					EndPopup();
				}
			}

			for (int linkIt = 0; linkIt < myNodeGraph.links.size(); linkIt++)
				imnodes::Link(
					linkIt, myNodeGraph.links[linkIt].fromId, myNodeGraph.links[linkIt].toId);

			imnodes::EndNodeEditor();

			int hoveredNodeId;
			if (/*imnodes::IsEditorHovered() && */
				!imnodes::IsNodeHovered(&hoveredNodeId) &&
				BeginPopupContextItem("Node Editor Context Menu"))
			{
				ImVec2 clickPos = GetMousePosOnOpeningCurrentPopup();

				enum class NodeType
				{
					SlangShaderNode
				};
				static constexpr std::tuple<NodeType, std::string_view> menuItems[] = {
					{NodeType::SlangShaderNode, "Slang Shader"}};

				for (const auto& menuItem : menuItems)
				{
					const auto& [itemType, itemName] = menuItem;

					if (Selectable(itemName.data()))
					{
						int id = ++myNodeGraph.uniqueId;
						imnodes::SetNodeScreenSpacePos(id, clickPos);
						myNodeGraph.nodes.emplace_back(
							[&menuItem, &id]() -> std::shared_ptr<INode>
							{
								const auto& [itemType, itemName] = menuItem;

								switch (itemType)
								{
								case NodeType::SlangShaderNode:
									return std::make_shared<SlangShaderNode>(
										SlangShaderNode(id, std::string(itemName.data()), {}));
								default:
									assert(false);
									break;
								}
								return {};
							}());
					}
				}

				EndPopup();
			}

			PopAllowKeyboardFocus();

			End();

			{
				int startAttr, endAttr;
				if (imnodes::IsLinkCreated(&startAttr, &endAttr))
					myNodeGraph.links.emplace_back(Link{startAttr, endAttr});
			}

			// {
			//     const int selectedNodeCount = imnodes::NumSelectedNodes();
			//     if (selectedNodeCount > 0)
			//     {
			//         std::vector<int> selectedNodes;
			//         selectedNodes.resize(selectedNodeCount);
			//         imnodes::GetSelectedNodes(selectedNodes.data());
			//     }
			// }
		}

		if (BeginMainMenuBar())
		{
			if (BeginMenu("File"))
			{
				if (MenuItem("Open OBJ...") && !myOpenFileFuture.valid())
					myOpenFileFuture = myExecutor.fork(
						[openFileDialogue, resourcePath, loadModel]
						{ return openFileDialogue(resourcePath, "obj", loadModel); });
				if (MenuItem("Open Image...") && !myOpenFileFuture.valid())
					myOpenFileFuture = myExecutor.fork(
						[openFileDialogue, resourcePath, loadImage]
						{ return openFileDialogue(resourcePath, "jpg,png", loadImage); });
				if (MenuItem("Open GLTF...") && !myOpenFileFuture.valid())
					myOpenFileFuture = myExecutor.fork(
						[openFileDialogue, resourcePath, loadGlTF]
						{ return openFileDialogue(resourcePath, "gltf,glb", loadGlTF); });
				Separator();
				if (MenuItem("Exit", "CTRL+Q"))
					myRequestExit = true;

				ImGui::EndMenu();
			}
			if (BeginMenu("View"))
			{
				if (MenuItem("Node Editor..."))
					showNodeEditor = !showNodeEditor;
				if constexpr (PROFILING_ENABLED)
				{
					if (MenuItem("Statistics..."))
						showStatistics = !showStatistics;
				}
				ImGui::EndMenu();
			}
			if (BeginMenu("About"))
			{
				if (MenuItem("Show IMGUI Demo..."))
					showDemoWindow = !showDemoWindow;
				Separator();
				if (MenuItem("About volcano..."))
					showAbout = !showAbout;
				ImGui::EndMenu();
			}

			EndMainMenuBar();
		}

		EndFrame();
		UpdatePlatformWindows();
		Render();
	};

	myIMGUIDrawFunction = [](CommandBufferHandle<Vk> cmd)
	{
		ZoneScopedN("Application::IMGUIDraw");

		using namespace ImGui;

		ImGui_ImplVulkan_RenderDrawData(GetDrawData(), cmd);
	};

	myNodeGraph = std::filesystem::path(volcano_getUserProfilePath()) /
				  "nodegraph.json"; // temp - this should be stored in the resource path

	if constexpr (PROFILING_ENABLED)
	{
		char* allocatorStatsJSON = nullptr;
		vmaBuildStatsString(myDevice->getAllocator(), &allocatorStatsJSON, true);
		std::cout << allocatorStatsJSON << std::endl;
		vmaFreeStatsString(myDevice->getAllocator(), allocatorStatsJSON);
	}
}

template <>
Application<Vk>::~Application()
{
	ZoneScopedN("~Application()");

	{
		ZoneScopedN("Application::waitGPU");

		// wait in on all queues
		uint64_t timelineValue = 0ull;

		for (const auto& queue : myGraphicsQueues)
			timelineValue = std::max(timelineValue, queue.getLastSubmitTimelineValue().value_or(0));

		for (const auto& queue : myComputeQueues)
			timelineValue = std::max(timelineValue, queue.getLastSubmitTimelineValue().value_or(0));

		for (const auto& queue : myTransferQueues)
			timelineValue = std::max(timelineValue, queue.getLastSubmitTimelineValue().value_or(0));

		myDevice->wait(timelineValue);
	}

	shutdownIMGUI();
}

template <>
void Application<Vk>::onMouse(const MouseState& state)
{
	bool leftPressed = state.button == GLFW_MOUSE_BUTTON_LEFT && state.action == GLFW_PRESS;
	bool rightPressed = state.button == GLFW_MOUSE_BUTTON_RIGHT && state.action == GLFW_PRESS;

	auto screenPos = glm::vec2(state.xpos, state.ypos);

	myInput.mousePosition[0] =
		glm::vec2{static_cast<float>(screenPos.x), static_cast<float>(screenPos.y)};
	myInput.mousePosition[1] = leftPressed && !myInput.mouseButtonsPressed[0]
								   ? myInput.mousePosition[0]
								   : myInput.mousePosition[1];

	myInput.mouseButtonsPressed[0] = leftPressed;
	myInput.mouseButtonsPressed[1] = rightPressed;
	myInput.mouseButtonsPressed[2] = state.insideWindow && !myInput.mouseButtonsPressed[0];
}

template <>
void Application<Vk>::onKeyboard(const KeyboardState& state)
{
	if (state.action == GLFW_PRESS)
		myInput.keysPressed[state.key] = true;
	else if (state.action == GLFW_RELEASE)
		myInput.keysPressed[state.key] = false;
}

template <>
bool Application<Vk>::draw()
{
	ZoneScopedN("Application::draw");

	myExecutor.join(std::move(myPresentFuture));
	myExecutor.join(std::move(myProcessTimelineCallbacksFuture));

	auto [flipSuccess, lastPresentTimelineValue] = myMainWindow->flip();

	if (flipSuccess)
	{
		auto& primaryContext = myCommands[CommandContextType_GeneralPrimary].fetchAdd();
		constexpr uint32_t secondaryContextCount = 4;
		auto secondaryContexts =
			&myCommands[CommandContextType_GeneralSecondary].fetchAdd(secondaryContextCount);

		std::rotate(
			myGraphicsQueues.begin(), std::next(myGraphicsQueues.begin()), myGraphicsQueues.end());

		auto imguiPrepareDrawFuture = myExecutor.fork([this] { myIMGUIPrepareDrawFunction(); });

		if (lastPresentTimelineValue)
		{
			ZoneScopedN("Application::draw::waitFrame");

			// todo: don't wait on transfer here - use callback to publish updated resources instead

			myDevice->wait(std::max(
				lastPresentTimelineValue,
				myTransferQueues.front().getLastSubmitTimelineValue().value_or(0)));

			primaryContext.reset();

			for (uint32_t secIt = 0; secIt < secondaryContextCount; secIt++)
				secondaryContexts[secIt].reset();
		}

		auto cmd = primaryContext.commands();

		{
			GPU_SCOPE(cmd, myGraphicsQueues.front(), collect);
			myGraphicsQueues.front().traceCollect(cmd);
		}
		{
			GPU_SCOPE(cmd, myGraphicsQueues.front(), clear);
			myRenderImageSet->clearDepthStencil(cmd, {1.0f, 0});
		}
		{
			GPU_SCOPE(cmd, myGraphicsQueues.front(), transition);
			myRenderImageSet->transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
			myRenderImageSet->transitionDepthStencil(
				cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		}
		{
			GPU_SCOPE(cmd, myGraphicsQueues.front(), draw);
			myMainWindow->updateInput(myInput);
			myMainWindow->draw(
				myExecutor, *myPipeline, primaryContext, secondaryContexts, secondaryContextCount);
		}
		{
			GPU_SCOPE(cmd, myGraphicsQueues.front(), imgui);
			myMainWindow->begin(cmd, VK_SUBPASS_CONTENTS_INLINE);

			myExecutor.join(std::move(imguiPrepareDrawFuture));

			myIMGUIDrawFunction(cmd);

			myMainWindow->end(cmd);
		}
		{
			GPU_SCOPE(cmd, myGraphicsQueues.front(), transitionColor);
			myMainWindow->transitionColor(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);
		}

		cmd.end();

		auto [imageAquired, renderComplete] = myMainWindow->getFrameSyncSemaphores();

		myGraphicsQueues.front().enqueueSubmit(primaryContext.prepareSubmit(
			{{myDevice->getTimelineSemaphore(), imageAquired},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
			 {myGraphicsQueues.front().getLastSubmitTimelineValue().value_or(0), 1},
			 {myDevice->getTimelineSemaphore(), renderComplete},
			 {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed), 1}}));

		myGraphicsQueues.front().enqueuePresent(
			myMainWindow->preparePresent(myGraphicsQueues.front().submit()));

		myPresentFuture = myExecutor.fork(
			[](QueueContext<Vk>* queue) { queue->present(); }, 1, &myGraphicsQueues.front());
	}

	if (lastPresentTimelineValue)
	{
		ZoneScopedN("Application::draw::submitTimelineCallbacks");

		// todo: have the thread pool poll Host+Device visible memory heap for GPU completion instead
		myProcessTimelineCallbacksFuture = myExecutor.fork(
			[](uint64_t timelineValue, DeviceContext<Vk>* deviceContext)
			{ deviceContext->processTimelineCallbacks(timelineValue); },
			1,
			static_cast<uint64_t>(lastPresentTimelineValue),
			myDevice.get());
		//.then([] { std::cout << "continuation" << std::endl; });
	}

	{
		ZoneScopedN("Application::draw::handleCallbacks");

		if (myOpenFileFuture.valid() && myOpenFileFuture.is_ready())
		{
			ZoneScopedN("Application::draw::openFileCallback");

			const auto& [openFileResult, openFilePath, onCompletionCallback] =
				myOpenFileFuture.get();
			if (openFileResult == NFD_OKAY)
			{
				onCompletionCallback(openFilePath);
				std::free(openFilePath);
			}
		}
	}

	return myRequestExit;
}

template <>
void Application<Vk>::resizeFramebuffer(int, int)
{
	ZoneScopedN("Application::resizeFramebuffer");

	{
		ZoneScopedN("Application::resizeFramebuffer::waitGPU");

		myDevice->wait(myGraphicsQueues.front().getLastSubmitTimelineValue().value_or(0));
	}

	auto physicalDevice = myDevice->getPhysicalDevice();
	myInstance->updateSurfaceCapabilities(physicalDevice, myMainWindow->getSurface());
	auto framebufferExtent =
		myInstance->getSwapchainInfo(physicalDevice, myMainWindow->getSurface())
			.capabilities.currentExtent;

	myMainWindow->onResizeFramebuffer(framebufferExtent);

	myPipeline->setDescriptorData(
		"g_viewData",
		DescriptorBufferInfo<Vk>{myMainWindow->getViewBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_View);

	createWindowDependentObjects(framebufferExtent);
}
