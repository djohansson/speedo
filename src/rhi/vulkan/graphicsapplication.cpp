#include "../graphicsapplication.h"
#include "../shaders/shadertypes.h"

#include "utils.h"

#include <core/application.h>
#include <core/gltfstream.h>
// #include <core/nodes/inputoutputnode.h>
// #include <core/nodes/slangshadernode.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

//#include <imnodes.h>

template <>
void GraphicsApplication<Vk>::initIMGUI(
	const WindowState& window,
	const std::shared_ptr<Device<Vk>>& device,
	CommandBufferHandle<Vk> commands,
	RenderPassHandle<Vk> renderPass,
	SurfaceHandle<Vk> surface,
	const std::filesystem::path& userProfilePath) const
{
	ZoneScopedN("GraphicsApplication::initIMGUI");

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
			*gfx().myInstance, "vkCreateWin32SurfaceKHR");

	const auto& surfaceCapabilities =
		gfx().myInstance->getSwapchainInfo(gfx().myDevice->getPhysicalDevice(), surface).capabilities;

	float dpiScaleX = static_cast<float>(surfaceCapabilities.currentExtent.width) /
					  gfx().myMainWindow->getConfig().windowExtent.width;
	float dpiScaleY = static_cast<float>(surfaceCapabilities.currentExtent.height) /
					  gfx().myMainWindow->getConfig().windowExtent.height;

	io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.PixelSnapH = false;

	io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(state().resourcePath);
	fontPath /= "fonts";
	fontPath /= "foo";

	constexpr const char* fonts[]{
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
	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.Instance = *gfx().myInstance;
	initInfo.PhysicalDevice = gfx().myDevice->getPhysicalDevice();
	initInfo.Device = *gfx().myDevice;
	initInfo.QueueFamily = gfx().myGraphicsQueues.get().getDesc().queueFamilyIndex;
	initInfo.Queue = gfx().myGraphicsQueues.get();
	initInfo.PipelineCache = gfx().myPipeline->getCache();
	initInfo.DescriptorPool = gfx().myPipeline->getDescriptorPool();
	initInfo.MinImageCount = gfx().myMainWindow->getConfig().swapchainConfig.imageCount;
	initInfo.ImageCount = gfx().myMainWindow->getConfig().swapchainConfig.imageCount;
	initInfo.Allocator = &gfx().myDevice->getInstance()->getHostAllocationCallbacks();
	initInfo.CheckVkResultFn = checkResult;
	// initInfo.DeleteBufferFn = [](void* user_data,
	// 							 VkBuffer buffer,
	// 							 VkDeviceMemory buffer_memory,
	// 							 const VkAllocationCallbacks* allocator)
	// {
	// 	auto& device = *static_cast<Device<Vk>*>(user_data);
	// 	device.addTimelineCallback(
	// 		[&device, buffer, buffer_memory, allocator](uint64_t)
	// 		{
	// 			if (buffer != VK_NULL_HANDLE)
	// 				vkDestroyBuffer(device, buffer, allocator);
	// 			if (buffer_memory != VK_NULL_HANDLE)
	// 				vkFreeMemory(device, buffer_memory, allocator);
	// 		});
	// };
	// initInfo.UserData = device.get();
	ImGui_ImplVulkan_Init(&initInfo, renderPass);
	ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window.handle), true);

	// Upload Fonts
	ImGui_ImplVulkan_CreateFontsTexture(commands);

	device->addTimelineCallback(std::make_tuple(
		1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed),
		[](uint64_t) { ImGui_ImplVulkan_DestroyFontUploadObjects(); }));

	// IMNODES_NAMESPACE::CreateContext();
	// IMNODES_NAMESPACE::LoadCurrentEditorStateFromIniString(
	//	myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

template <>
void GraphicsApplication<Vk>::shutdownIMGUI()
{
	// size_t count;
	// myNodeGraph.layout.assign(IMNODES_NAMESPACE::SaveCurrentEditorStateToIniString(&count));
	// IMNODES_NAMESPACE::DestroyContext();

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();

	ImGui::DestroyContext();
}

template <>
void GraphicsApplication<Vk>::createWindowDependentObjects(Extent2d<Vk> frameBufferExtent)
{
	ZoneScopedN("GraphicsApplication::createWindowDependentObjects");

	auto colorImage = std::make_shared<Image<Vk>>(
		gfx().myDevice,
		ImageCreateDesc<Vk>{
			{{frameBufferExtent}},
			gfx().myMainWindow->getConfig().swapchainConfig.surfaceFormat.format,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	auto depthStencilImage = std::make_shared<Image<Vk>>(
		gfx().myDevice,
		ImageCreateDesc<Vk>{
			{{frameBufferExtent}},
			findSupportedFormat(
				gfx().myDevice->getPhysicalDevice(),
				{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
				VK_IMAGE_TILING_OPTIMAL,
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
					VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	gfx().myRenderImageSet =
		std::make_shared<RenderImageSet<Vk>>(gfx().myDevice, make_vector(colorImage), depthStencilImage);

	gfx().myPipeline->setRenderTarget(gfx().myRenderImageSet);
}

template <>
GraphicsApplication<Vk>::GraphicsApplication(Application::State&& state)
: Application(std::forward<Application::State>(state))
, myGraphicsContext{std::make_shared<Instance<Vk>>(
	InstanceConfiguration<Vk>{
		this->state().name,
		"speedo",
		ApplicationInfo<Vk>{
			VK_STRUCTURE_TYPE_APPLICATION_INFO,
			nullptr,
			nullptr,
			VK_MAKE_VERSION(1, 0, 0),
			nullptr,
			VK_MAKE_VERSION(1, 0, 0),
			VK_API_VERSION_1_2}})}
{
	ZoneScopedN("GraphicsApplication()");

	auto rootPath = this->state().rootPath;
	auto resourcePath = this->state().resourcePath;
	auto userProfilePath = this->state().userProfilePath;

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
			gfx().myCommands[GraphicsContext::CommandType_DedicatedTransfer].fetchAdd();

		auto& transferQueue = gfx().myTransferQueues.fetchAdd();

		gfx().myDevice->wait(transferQueue.getLastSubmitTimelineValue().value_or(0));

		dedicatedTransferContext.reset();

		gfx().myPipeline->setModel(
			std::make_shared<Model<Vk>>(gfx().myDevice, dedicatedTransferContext, openFilePath));

		transferQueue.enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {gfx().myDevice->getTimelineSemaphore()},
			 {1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		return transferQueue.submit();
	};

	auto loadImage = [this](nfdchar_t* openFilePath)
	{
		auto& dedicatedTransferContext =
			gfx().myCommands[GraphicsContext::CommandType_DedicatedTransfer].fetchAdd();

		auto& transferQueue = gfx().myTransferQueues.fetchAdd();

		gfx().myDevice->wait(transferQueue.getLastSubmitTimelineValue().value_or(0));

		dedicatedTransferContext.reset();

		gfx().myPipeline->resources().image =
			std::make_shared<Image<Vk>>(gfx().myDevice, dedicatedTransferContext, openFilePath);
		gfx().myPipeline->resources().imageView = std::make_shared<ImageView<Vk>>(
			gfx().myDevice, *gfx().myPipeline->resources().image, VK_IMAGE_ASPECT_COLOR_BIT);

		transferQueue.enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {gfx().myDevice->getTimelineSemaphore()},
			 {1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		transferQueue.submit();

		///////////

		auto& generalTransferContext = gfx().myCommands[GraphicsContext::CommandType_GeneralTransfer].fetchAdd();
		auto cmd = generalTransferContext.commands();

		auto& graphicsQueue = gfx().myGraphicsQueues.get();

		gfx().myPipeline->resources().image->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		cmd.end();

		graphicsQueue.enqueueSubmit(generalTransferContext.prepareSubmit(
			{{gfx().myDevice->getTimelineSemaphore()},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
			 {transferQueue.getLastSubmitTimelineValue().value_or(0)},
			 {gfx().myDevice->getTimelineSemaphore()},
			 {1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		gfx().myPipeline->setDescriptorData(
			"g_textures",
			DescriptorImageInfo<Vk>{
				{},
				*gfx().myPipeline->resources().imageView,
				gfx().myPipeline->resources().image->getImageLayout()},
			DescriptorSetCategory_GlobalTextures,
			1);

		return graphicsQueue.submit();
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
		ZoneScopedN("GraphicsApplication::IMGUIPrepareDraw");

		using namespace ImGui;

		ImGui_ImplVulkan_NewFrame();
		NewFrame();

		// todo: move elsewhere
		/*auto editableTextField = [](int id,
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
		};
		*/

#if GRAPHICS_VALIDATION_ENABLED
		static bool showStatistics = false;
		{
			if (showStatistics)
			{
				if (Begin("Statistics", &showStatistics))
				{
					Text("Unknowns: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_UNKNOWN));
					Text("Instances: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_INSTANCE));
					Text(
						"Physical Devices: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
					Text("Devices: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_DEVICE));
					Text("Queues: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_QUEUE));
					Text("Semaphores: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
					Text(
						"Command Buffers: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
					Text("Fences: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_FENCE));
					Text("Device Memory: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
					Text("Buffers: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_BUFFER));
					Text("Images: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_IMAGE));
					Text("Events: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_EVENT));
					Text("Query Pools: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
					Text("Buffer Views: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
					Text("Image Views: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
					Text(
						"Shader Modules: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
					Text(
						"Pipeline Caches: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
					Text(
						"Pipeline Layouts: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
					Text("Render Passes: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
					Text("Pipelines: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE));
					Text(
						"Descriptor Set Layouts: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
					Text("Samplers: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_SAMPLER));
					Text(
						"Descriptor Pools: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
					Text(
						"Descriptor Sets: %u",
						gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
					Text("Framebuffers: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
					Text("Command Pools: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
					Text("Surfaces: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
					Text("Swapchains: %u", gfx().myDevice->getTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
				}
				End();
			}
		}
#endif

		static bool showDemoWindow = false;
		if (showDemoWindow)
			ShowDemoWindow(&showDemoWindow);

		static bool showAbout = false;
		if (showAbout)
		{
			if (Begin("About client", &showAbout)) {}
			End();
		}

		/*static bool showNodeEditor = false;
		if (showNodeEditor)
		{
			ImGui::SetNextWindowSize(ImVec2(800, 450), ImGuiCond_FirstUseEver);

			if (Begin("Node Editor Window", &showNodeEditor)) {}

			//PushAllowKeyboardFocus(false);

			IMNODES_NAMESPACE::BeginNodeEditor();

			for (const auto& node : myNodeGraph.nodes)
			{
				char buffer[64];

				IMNODES_NAMESPACE::BeginNode(node->id());

				// title bar
				stbsp_sprintf(buffer, "##node%.*u", 4, node->id());

				IMNODES_NAMESPACE::BeginNodeTitleBar();

				float titleBarTextWidth =
					editableTextField(node->id(), buffer, node->name(), 160.0f, node->selected());

				IMNODES_NAMESPACE::EndNodeTitleBar();

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

							IMNODES_NAMESPACE::BeginInputAttribute(inputAttribute.id);

							inputTextWidth = editableTextField(
								inputAttribute.id,
								buffer,
								inputAttribute.name,
								80.0f,
								node->selected());

							IMNODES_NAMESPACE::EndInputAttribute();

							if (IsItemClicked() && IsMouseDoubleClicked(0))
								node->selected() = std::make_optional(inputAttribute.id);
						}

						if (rowIt < inOutNode->outputAttributes().size())
						{
							auto& outputAttribute = inOutNode->outputAttributes()[rowIt];
							stbsp_sprintf(buffer, "##outputattribute%.*u", 4, outputAttribute.id);

							if (hasInputPin)
								SameLine();

							IMNODES_NAMESPACE::BeginOutputAttribute(outputAttribute.id);

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

							IMNODES_NAMESPACE::EndOutputAttribute();

							if (IsItemClicked() && IsMouseDoubleClicked(0))
								node->selected() = std::make_optional(outputAttribute.id);
						}
					}
				}

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

				IMNODES_NAMESPACE::EndNode();
			}

			for (int linkIt = 0; linkIt < myNodeGraph.links.size(); linkIt++)
				IMNODES_NAMESPACE::Link(
					linkIt, myNodeGraph.links[linkIt].fromId, myNodeGraph.links[linkIt].toId);

			IMNODES_NAMESPACE::EndNodeEditor();

			//if (ImGui::IsWindowHovered() || ImGui::IsWindowFocused())
			{
				//if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Tab)))
				{
					if (BeginPopupContextWindow("Node Editor Context Menu"))
					{
						ImVec2 clickPos = GetMousePosOnOpeningCurrentPopup();

						enum class NodeType
						{
							SlangShaderNode
						};
						constexpr std::tuple<NodeType, std::string_view> menuItems[]{
							{NodeType::SlangShaderNode, "Slang Shader"}};

						for (const auto& menuItem : menuItems)
						{
							const auto& [itemType, itemName] = menuItem;

							if (Selectable(itemName.data()))
							{
								int id = ++myNodeGraph.uniqueId;
								IMNODES_NAMESPACE::SetNodeScreenSpacePos(id, clickPos);
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
				}
			}

			//PopAllowKeyboardFocus();

			End(); //Node Editor Window

			{
				int startAttr, endAttr;
				if (IMNODES_NAMESPACE::IsLinkCreated(&startAttr, &endAttr))
					myNodeGraph.links.emplace_back(Link{startAttr, endAttr});
			}

			// {
			//     const int selectedNodeCount = IMNODES_NAMESPACE::NumSelectedNodes();
			//     if (selectedNodeCount > 0)
			//     {
			//         std::vector<int> selectedNodes;
			//         selectedNodes.resize(selectedNodeCount);
			//         IMNODES_NAMESPACE::GetSelectedNodes(selectedNodes.data());
			//     }
			// }
		}
		*/

		if (BeginMainMenuBar())
		{
			if (BeginMenu("File"))
			{
				if (MenuItem("Open OBJ...") && !myOpenFileFuture.valid())
				{
					TaskGraph graph;
					auto [task, openFileFuture] = graph.createTask(
						[&openFileDialogue, &resourcePath, &loadModel]
						{ return openFileDialogue(resourcePath, "obj", loadModel); });
					myExecutor.submit(std::move(graph));
					myOpenFileFuture = std::move(openFileFuture);
				}
				if (MenuItem("Open Image...") && !myOpenFileFuture.valid())
				{
					TaskGraph graph;
					auto [task, openFileFuture] = graph.createTask(
						[&openFileDialogue, &resourcePath, &loadImage]
						{ return openFileDialogue(resourcePath, "jpg,png", loadImage); });
					myExecutor.submit(std::move(graph));
					myOpenFileFuture = std::move(openFileFuture);
				}
				if (MenuItem("Open GLTF...") && !myOpenFileFuture.valid())
				{
					TaskGraph graph;
					auto [task, openFileFuture] = graph.createTask(
						[&openFileDialogue, &resourcePath, &loadGlTF]
						{ return openFileDialogue(resourcePath, "gltf,glb", loadGlTF); });
					myExecutor.submit(std::move(graph));
					myOpenFileFuture = std::move(openFileFuture);
				}
				Separator();
				if (MenuItem("Exit", "CTRL+Q"))
					myRequestExit = true;

				ImGui::EndMenu();
			}
			if (BeginMenu("View"))
			{
				// if (MenuItem("Node Editor..."))
				// 	showNodeEditor = !showNodeEditor;
#if GRAPHICS_VALIDATION_ENABLED
				{
					if (MenuItem("Statistics..."))
						showStatistics = !showStatistics;
				}
#endif
				ImGui::EndMenu();
			}
			if (BeginMenu("About"))
			{
				if (MenuItem("Show IMGUI Demo..."))
					showDemoWindow = !showDemoWindow;
				Separator();
				if (MenuItem("About client..."))
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
		ZoneScopedN("GraphicsApplication::IMGUIDraw");

		using namespace ImGui;

		ImGui_ImplVulkan_RenderDrawData(GetDrawData(), cmd);
	};

	//myNodeGraph = std::filesystem::path(client_getUserProfilePath()) / "nodegraph.json"; // temp - this should be stored in the resource path
}

template <>
void GraphicsApplication<Vk>::createDevice(const WindowState& window)
{
	auto rootPath = state().rootPath;
	auto resourcePath = state().resourcePath;
	auto userProfilePath = state().userProfilePath;

	auto shaderIncludePath = rootPath / "src/rhi/shaders";
	auto shaderIntermediatePath = userProfilePath / ".slang.intermediate";

	auto vkSDKPathEnv = std::getenv("VK_SDK_PATH");
	auto vkSDKPath = vkSDKPathEnv ? std::filesystem::path(vkSDKPathEnv) : rootPath;
	auto vkSDKBinPath = vkSDKPath / "bin";

	ShaderLoader shaderLoader(
		{shaderIncludePath},
		{std::make_tuple(SLANG_SOURCE_LANGUAGE_HLSL, SLANG_PASS_THROUGH_DXC, vkSDKBinPath)},
		shaderIntermediatePath);

	auto shaderReflection = shaderLoader.load<Vk>(shaderIncludePath / "shaders.slang");
	
	auto surface = createSurface(*gfx().myInstance, &gfx().myInstance->getHostAllocationCallbacks(), window.nativeHandle);

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
				constexpr uint32_t deviceTypePriority[]{
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

	gfx().myDevice = std::make_shared<Device<Vk>>(
		gfx().myInstance, DeviceConfiguration<Vk>{detectSuitableGraphicsDevice(gfx().myInstance, surface)});

	auto detectSuitableSwapchain = [](auto instance, auto device, auto surface)
	{
		const auto& swapchainInfo =
			instance->getSwapchainInfo(device->getPhysicalDevice(), surface);

		SwapchainConfiguration<Vk> config{swapchainInfo.capabilities.currentExtent};

		constexpr Format<Vk> requestSurfaceImageFormat[]{
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_FORMAT_B8G8R8_UNORM,
			VK_FORMAT_R8G8B8_UNORM};
		constexpr ColorSpace<Vk> requestSurfaceColorSpace =
			VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		constexpr PresentMode<Vk> requestPresentMode[]{
			VK_PRESENT_MODE_MAILBOX_KHR,
			VK_PRESENT_MODE_FIFO_RELAXED_KHR,
			VK_PRESENT_MODE_FIFO_KHR,
			VK_PRESENT_MODE_IMMEDIATE_KHR};

		// Request several formats, the first found will be used
		// If none of the requested image formats could be found, use the first available
		for (unsigned requestIt = 0; requestIt < std::size(requestSurfaceImageFormat);
			 requestIt++)
		{
			SurfaceFormat<Vk> requestedFormat{requestSurfaceImageFormat[requestIt], requestSurfaceColorSpace};

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
		for (unsigned requestIt = 0; requestIt < std::size(requestPresentMode); requestIt++)
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

	gfx().myPipeline = std::make_shared<Pipeline<Vk>>(
		gfx().myDevice, PipelineConfiguration<Vk>{(userProfilePath / "pipeline.cache").string()});

	gfx().myMainWindow = std::make_shared<Window<Vk>>(
		gfx().myDevice,
		std::move(surface),
		WindowConfiguration<Vk>{
			detectSuitableSwapchain(gfx().myInstance, gfx().myDevice, surface),
			{window.width, window.height},
			{1ul, 1ul}});

	{
		uint32_t frameCount = gfx().myMainWindow->getConfig().swapchainConfig.imageCount;

		auto& primaryContexts = gfx().myCommands[GraphicsContext::CommandType_GeneralPrimary];
		auto& secondaryContexts = gfx().myCommands[GraphicsContext::CommandType_GeneralSecondary];
		auto& generalTransferContexts = gfx().myCommands[GraphicsContext::CommandType_GeneralTransfer];
		auto& dedicatedComputeContexts = gfx().myCommands[GraphicsContext::CommandType_DedicatedCompute];
		auto& dedicatedTransferContexts = gfx().myCommands[GraphicsContext::CommandType_DedicatedTransfer];

		auto& graphicsQueues = gfx().myGraphicsQueues;
		auto& computeQueues = gfx().myComputeQueues;
		auto& transferQueues = gfx().myTransferQueues;

		VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

		constexpr bool useCommandBufferCreateReset = true;
		if (useCommandBufferCreateReset)
			cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		const auto& queueFamilies = gfx().myDevice->getQueueFamilies();
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
						gfx().myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

					for (uint32_t secondaryContextIt = 0ul; secondaryContextIt < 4;
						 secondaryContextIt++)
					{
						secondaryContexts.emplace_back(CommandPoolContext<Vk>(
							gfx().myDevice,
							CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));
					}
				}

				generalTransferContexts.emplace_back(CommandPoolContext<Vk>(
					gfx().myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				graphicsQueues.emplace_back(Queue<Vk>(
					gfx().myDevice,
					QueueCreateDesc<Vk>{
						0,
						queueFamilyIndex,
						primaryContexts.get().commands(
							CommandBufferAccessScopeDesc<Vk>(false))}));
			}
			else if (queueFamily.flags & QueueFamilyFlagBits_Compute)
			{
				// only need one for now..

				dedicatedComputeContexts.emplace_back(CommandPoolContext<Vk>(
					gfx().myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				computeQueues.emplace_back(
					Queue<Vk>(gfx().myDevice, QueueCreateDesc<Vk>{0, queueFamilyIndex}));
			}
			else if (queueFamily.flags & QueueFamilyFlagBits_Transfer)
			{
				// only need one for now..

				dedicatedTransferContexts.emplace_back(CommandPoolContext<Vk>(
					gfx().myDevice, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				transferQueues.emplace_back(
					Queue<Vk>(gfx().myDevice, QueueCreateDesc<Vk>{0, queueFamilyIndex}));
			}
		}
	}

	createWindowDependentObjects(gfx().myMainWindow->getConfig().swapchainConfig.extent);

	// todo: create some resource global storage
	gfx().myPipeline->resources().black = std::make_shared<Image<Vk>>(
		gfx().myDevice,
		ImageCreateDesc<Vk>{
			std::make_vector(ImageMipLevelDesc<Vk>{Extent2d<Vk>{4, 4}, 16 * 4, 0}),
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_TILING_LINEAR,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	gfx().myPipeline->resources().blackImageView = std::make_shared<ImageView<Vk>>(
		gfx().myDevice, *gfx().myPipeline->resources().black, VK_IMAGE_ASPECT_COLOR_BIT);

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
	gfx().myPipeline->resources().samplers =
		std::make_shared<SamplerVector<Vk>>(gfx().myDevice, std::move(samplerCreateInfos));
	//

	// initialize stuff on general transfer queue
	{
		auto& generalTransferContext = gfx().myCommands[GraphicsContext::CommandType_GeneralTransfer].fetchAdd();
		auto cmd = generalTransferContext.commands();

		gfx().myPipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		gfx().myPipeline->resources().black->clear(cmd, {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}});
		gfx().myPipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		initIMGUI(
			window,
			gfx().myDevice,
			cmd,
			gfx().myMainWindow->getRenderPass(),
			gfx().myMainWindow->getSurface(),
			userProfilePath);

		cmd.end();

		gfx().myGraphicsQueues.get().enqueueSubmit(generalTransferContext.prepareSubmit(
			{{gfx().myDevice->getTimelineSemaphore()},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
			 {gfx().myGraphicsQueues.get().getLastSubmitTimelineValue().value_or(0)},
			 {gfx().myDevice->getTimelineSemaphore()},
			 {1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		gfx().myGraphicsQueues.get().submit();
	}

	constexpr uint32_t textureId = 1;
	constexpr uint32_t samplerId = 2;
	static_assert(textureId < ShaderTypes_GlobalTextureCount);
	static_assert(samplerId < ShaderTypes_GlobalSamplerCount);
	{
		auto& dedicatedTransferContext =
			gfx().myCommands[GraphicsContext::CommandType_DedicatedTransfer].fetchAdd();

		auto& transferQueue = gfx().myTransferQueues.fetchAdd();

		auto materialData = std::make_unique<MaterialData[]>(ShaderTypes_MaterialCount);
		materialData[0].color = glm::vec4(1.0, 0.0, 0.0, 1.0);
		materialData[0].textureAndSamplerId =
			(textureId << ShaderTypes_GlobalTextureIndexBits) | samplerId;
		gfx().myMaterials = std::make_unique<Buffer<Vk>>(
			gfx().myDevice,
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
		gfx().myObjects = std::make_unique<Buffer<Vk>>(
			gfx().myDevice,
			dedicatedTransferContext,
			BufferCreateDesc<Vk>{
				ShaderTypes_ObjectBufferInstanceCount * sizeof(ObjectData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
			objectData.get());

		transferQueue.enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {gfx().myDevice->getTimelineSemaphore()},
			 {1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		transferQueue.submit();
	}

	// set global descriptor set data

	auto [layoutIt, insertResult] =
		gfx().myLayouts.emplace(std::make_shared<PipelineLayout<Vk>>(gfx().myDevice, shaderReflection));
	assert(insertResult);
	gfx().myPipeline->setLayout(*layoutIt, VK_PIPELINE_BIND_POINT_GRAPHICS);

	gfx().myPipeline->setDescriptorData(
		"g_viewData",
		DescriptorBufferInfo<Vk>{gfx().myMainWindow->getViewBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_View);

	gfx().myPipeline->setDescriptorData(
		"g_materialData",
		DescriptorBufferInfo<Vk>{*gfx().myMaterials, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Material);

	gfx().myPipeline->setDescriptorData(
		"g_objectData",
		DescriptorBufferInfo<Vk>{*gfx().myObjects, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Object,
		42);

	gfx().myPipeline->setDescriptorData(
		"g_samplers",
		DescriptorImageInfo<Vk>{(*gfx().myPipeline->resources().samplers)[0]},
		DescriptorSetCategory_GlobalSamplers,
		samplerId);
}

template <>
GraphicsApplication<Vk>::~GraphicsApplication()
{
	ZoneScopedN("~GraphicsApplication()");

	auto device = gfx().myDevice;
	auto instance = gfx().myInstance;

	{
		ZoneScopedN("~GraphicsApplication()::waitCPU");

		myExecutor.join(std::move(myPresentFuture));
	}

	{
		ZoneScopedN("~GraphicsApplication()::waitGPU");

		device->waitIdle();
	}

	shutdownIMGUI();

	myGraphicsContext = {};

	assert(device.use_count() == 1);
	assert(instance.use_count() == 2);
}

template <>
void GraphicsApplication<Vk>::onMouse(const MouseState& mouse)
{
	gfx().myMainWindow->onMouse(mouse);
}

template <>
void GraphicsApplication<Vk>::onKeyboard(const KeyboardState& keyboard)
{
	gfx().myMainWindow->onKeyboard(keyboard);
}

template <>
bool GraphicsApplication<Vk>::tick()
{
	FrameMark;

	{

		ZoneScopedN("ImGui_ImplGlfw_NewFrame");

		ImGui_ImplGlfw_NewFrame();
	}

	TaskGraph frameGraph;

	auto [drawTask, drawFuture] = frameGraph.createTask([this]
	{
		ZoneScopedN("GraphicsApplication::draw");

		auto [flipSuccess, lastPresentTimelineValue] = gfx().myMainWindow->flip();

		if (flipSuccess)
		{
			ZoneScopedN("GraphicsApplication::draw::submit");

			auto& primaryContext = gfx().myCommands[GraphicsContext::CommandType_GeneralPrimary].fetchAdd();
			constexpr uint32_t secondaryContextCount = 4;
			auto secondaryContexts = &gfx().myCommands[GraphicsContext::CommandType_GeneralSecondary].fetchAdd(
				secondaryContextCount);

			auto& graphicsQueue = gfx().myGraphicsQueues.fetchAdd();

			if (lastPresentTimelineValue)
			{
				{
					ZoneScopedN("GraphicsApplication::draw::waitFrame");

					gfx().myDevice->wait(lastPresentTimelineValue);
				}

				primaryContext.reset();

				for (uint32_t secIt = 0; secIt < secondaryContextCount; secIt++)
					secondaryContexts[secIt].reset();
			}

			auto cmd = primaryContext.commands();

			GPU_SCOPE_COLLECT(cmd, graphicsQueue);

			{
				GPU_SCOPE(cmd, graphicsQueue, clear);

				gfx().myRenderImageSet->clearDepthStencil(cmd, {1.0f, 0});
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, transition);
				
				gfx().myRenderImageSet->transitionColor(
					cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
				gfx().myRenderImageSet->transitionDepthStencil(
					cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, draw);
				
				gfx().myMainWindow->draw(
					myExecutor,
					*gfx().myPipeline,
					primaryContext,
					secondaryContexts,
					secondaryContextCount);
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, imgui);
				
				gfx().myMainWindow->begin(cmd, VK_SUBPASS_CONTENTS_INLINE);

				myIMGUIPrepareDrawFunction(); // todo: kick off earlier (but not before ImGui_ImplGlfw_NewFrame)
				myIMGUIDrawFunction(cmd);

				gfx().myMainWindow->end(cmd);
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, transitionColor);
				
				gfx().myMainWindow->transitionColor(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);
			}

			cmd.end();

			auto [imageAquired, renderComplete] = gfx().myMainWindow->getFrameSyncSemaphores();

			graphicsQueue.enqueueSubmit(primaryContext.prepareSubmit(
				{{gfx().myDevice->getTimelineSemaphore(), imageAquired},
					{VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
					{graphicsQueue.getLastSubmitTimelineValue().value_or(0), 1},
					{gfx().myDevice->getTimelineSemaphore(), renderComplete},
					{1 + gfx().myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed), 1}}));

			graphicsQueue.enqueuePresent(
				gfx().myMainWindow->preparePresent(graphicsQueue.submit()));
		}

		if (lastPresentTimelineValue)
		{
			ZoneScopedN("GraphicsApplication::draw::processTimelineCallbacks");

			// todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
			gfx().myDevice->processTimelineCallbacks(static_cast<uint64_t>(lastPresentTimelineValue));
		}

		{
			if (myOpenFileFuture.valid() && myOpenFileFuture.is_ready())
			{
				ZoneScopedN("GraphicsApplication::draw::openFileCallback");

				const auto& [openFileResult, openFilePath, onCompletionCallback] =
					myOpenFileFuture.get();
				if (openFileResult == NFD_OKAY)
				{
					onCompletionCallback(openFilePath);
					std::free(openFilePath);
				}
			}
		}
	});

	auto [presentTask, presentFuture] = frameGraph.createTask(
		[](Queue<Vk>* queue) { queue->present(); }, &gfx().myGraphicsQueues.get());

	frameGraph.addDependency(drawTask, presentTask);

	{
		ZoneScopedN("GraphicsApplication::tick::waitPresent");

		myExecutor.join(std::move(myPresentFuture));
	}

	myPresentFuture = std::move(presentFuture);

	myExecutor.submit(std::move(frameGraph));

	return !myRequestExit;
}

template <>
void GraphicsApplication<Vk>::onResizeFramebuffer(uint32_t, uint32_t)
{
	ZoneScopedN("GraphicsApplication::onResizeFramebuffer");

	{
		ZoneScopedN("GraphicsApplication::onResizeFramebuffer::waitGPU");

		gfx().myDevice->wait(gfx().myGraphicsQueues.get().getLastSubmitTimelineValue().value_or(0));
	}

	auto physicalDevice = gfx().myDevice->getPhysicalDevice();
	gfx().myInstance->updateSurfaceCapabilities(physicalDevice, gfx().myMainWindow->getSurface());
	auto framebufferExtent =
		gfx().myInstance->getSwapchainInfo(physicalDevice, gfx().myMainWindow->getSurface())
			.capabilities.currentExtent;

	gfx().myMainWindow->onResizeFramebuffer(framebufferExtent);

	gfx().myPipeline->setDescriptorData(
		"g_viewData",
		DescriptorBufferInfo<Vk>{gfx().myMainWindow->getViewBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_View);

	createWindowDependentObjects(framebufferExtent);
}
