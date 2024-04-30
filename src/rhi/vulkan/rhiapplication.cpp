#include "../rhi.h"
#include "../rhiapplication.h"
#include "utils.h"

#include <core/application.h>
#include <core/upgradablesharedmutex.h>
// #include <core/gltfstream.h>
// #include <core/nodes/inputoutputnode.h>
// #include <core/nodes/slangshadernode.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

//#include <imnodes.h>

#include <nfd.h>

#include <shared_mutex>

namespace rhiapplication
{

static UpgradableSharedMutex g_drawMutex{};
static std::pair<TaskHandle, Future<void>> g_updateTask{};
static std::pair<TaskHandle, Future<void>> g_drawTask{};
// drawTask -> updateTask -> drawTask -> updateTask -> ...

static std::tuple<nfdresult_t, nfdchar_t*> openFileDialogue(const std::filesystem::path& resourcePath, const nfdchar_t* filterList)
{
	auto resourcePathStr = resourcePath.string();
	nfdchar_t* openFilePath;
	return std::make_tuple(NFD_OpenDialog(filterList, resourcePathStr.c_str(), &openFilePath), openFilePath);
}

static void loadModel(Rhi<Vk>& rhi, TaskExecutor& executor, nfdchar_t* openFilePath, std::atomic_uint8_t& progress)
{
	auto& [transferQueueInfos, transferSemaphore] = rhi.queues[QueueType_Transfer];
	auto& [transferQueue, transferSubmit] = transferQueueInfos.front();
	
	uint64_t transferSemaphoreValue = transferSubmit.maxTimelineValue;

	rhi.pipeline->setModel(
		std::make_shared<Model<Vk>>(
			rhi.device,
			transferQueue,
			++transferSemaphoreValue,
			openFilePath,
			progress));

	transferQueue.enqueueSubmit(QueueDeviceSyncInfo<Vk>{
		{transferSemaphore},
		{VK_PIPELINE_STAGE_TRANSFER_BIT},
		{transferSubmit.maxTimelineValue},
		{transferSemaphore},
		{++transferSemaphoreValue}});

	transferSubmit = transferQueue.submit();
}

static void loadImage(Rhi<Vk>& rhi, TaskExecutor& executor, nfdchar_t* openFilePath, std::atomic_uint8_t& progress)
{
	auto& [transferQueueInfos, transferSemaphore] = rhi.queues[QueueType_Transfer];
	auto& [transferQueue, transferSubmit] = transferQueueInfos.front();

	uint64_t transferSemaphoreValue = transferSubmit.maxTimelineValue;

	rhi.pipeline->resources().image =
		std::make_shared<Image<Vk>>(
			rhi.device,
			transferQueue,
			++transferSemaphoreValue,
			openFilePath,
			progress);

	rhi.pipeline->resources().imageView = std::make_shared<ImageView<Vk>>(
		rhi.device, *rhi.pipeline->resources().image, VK_IMAGE_ASPECT_COLOR_BIT);

	transferQueue.enqueueSubmit(QueueDeviceSyncInfo<Vk>{
		{transferSemaphore},
		{VK_PIPELINE_STAGE_TRANSFER_BIT},
		{transferSubmit.maxTimelineValue},
		{transferSemaphore},
		{++transferSemaphoreValue}});

	transferSubmit = transferQueue.submit();

	///////////

	auto [imageTransitionTask, imageTransitionFuture] = executor.createTask<uint32_t>([&rhi](
		Image<Vk>& image,
		SemaphoreHandle<Vk> waitSemaphore,
		uint64_t waitSemaphoreValue,
		uint32_t frameIndex)
	{
		auto& [graphicsQueueInfos, graphicsSemaphore] = rhi.queues[QueueType_Graphics];
		auto& [graphicsQueue, graphicsSubmit] = graphicsQueueInfos.at(frameIndex);

		{
			auto cmd = graphicsQueue.getPool().commands();

			GPU_SCOPE(cmd, graphicsQueue, transition);

			image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		graphicsQueue.enqueueSubmit(QueueDeviceSyncInfo<Vk>{
			{waitSemaphore},
			{VK_PIPELINE_STAGE_TRANSFER_BIT},
			{waitSemaphoreValue},
			{graphicsSemaphore},
			{1 + rhi.device->timelineValue().fetch_add(1, std::memory_order_relaxed)}});

		rhi.pipeline->setDescriptorData(
			"g_textures",
			DescriptorImageInfo<Vk>{
				{},
				*rhi.pipeline->resources().imageView,
				rhi.pipeline->resources().image->getImageLayout()},
			DescriptorSetCategory_GlobalTextures,
			1);
	}, *rhi.pipeline->resources().image, static_cast<SemaphoreHandle<Vk>>(transferSemaphore), transferSubmit.maxTimelineValue);

	rhi.drawCalls.enqueue(imageTransitionTask);

	///////////
}

void IMGUIPrepareDrawFunction(Rhi<Vk>& rhi, TaskExecutor& executor)
{
	ZoneScopedN("RhiApplication::IMGUIPrepareDraw");

	using namespace ImGui;

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

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	static bool showStatistics = false;
	{
		if (showStatistics)
		{
			if (Begin("Statistics", &showStatistics))
			{
				Text("Unknowns: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_UNKNOWN));
				Text("Instances: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_INSTANCE));
				Text(
					"Physical Devices: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
				Text("Devices: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_DEVICE));
				Text("Queues: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_QUEUE));
				Text("Semaphores: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
				Text(
					"Command Buffers: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
				Text("Fences: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_FENCE));
				Text("Device Memory: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
				Text("Buffers: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_BUFFER));
				Text("Images: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_IMAGE));
				Text("Events: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_EVENT));
				Text("Query Pools: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
				Text("Buffer Views: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
				Text("Image Views: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
				Text(
					"Shader Modules: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
				Text(
					"Pipeline Caches: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
				Text(
					"Pipeline Layouts: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
				Text("Render Passes: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
				Text("Pipelines: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_PIPELINE));
				Text(
					"Descriptor Set Layouts: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
				Text("Samplers: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_SAMPLER));
				Text(
					"Descriptor Pools: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
				Text(
					"Descriptor Sets: %u",
					rhi.device->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
				Text("Framebuffers: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
				Text("Command Pools: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
				Text("Surfaces: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
				Text("Swapchains: %u", rhi.device->getTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
			}
			End();
		}
	}
#endif

	static bool showDemoWindow = false;
	if (showDemoWindow)
		ShowDemoWindow(&showDemoWindow);

	static bool showAbout = false;
	if (showAbout && Begin("About client", &showAbout))
	{
		End();
	}
	
	static std::atomic_uint8_t progress = 0;
	static std::atomic_bool showProgress = false;
	if (bool b = showProgress.load(std::memory_order_relaxed) && Begin("Loading", &b, ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings))
	{
		SetWindowSize(ImVec2(160, 0));
		ProgressBar((1.f/255)*progress);
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
			std::format_to_n(buffer, std::size(buffer), "##node{0}", 4, node->id());

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
						std::format_to_n(buffer, std::size(buffer), "##inputattribute{0}", 4, inputAttribute.id);

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
						std::format_to_n(buffer, std::size(buffer), "##outputattribute{0}", 4, outputAttribute.id);

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
						std::format_to_n(
							buffer,
							std::size(buffer),
							"In {0}",
							static_cast<unsigned>(inOutNode->inputAttributes().size()));
						inOutNode->inputAttributes().emplace_back(
							Attribute{++myNodeGraph.uniqueId, buffer});
					}
				}
				if (Selectable("Add Output"))
				{
					if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
					{
						std::format_to_n(
							buffer,
							std::size(buffer),
							"Out {0}",
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
			if (MenuItem("Open OBJ..."))
			{
				auto [openFileTask, openFileFuture] = executor.createTask(
					openFileDialogue,
					std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["ResourcePath"]),
					"obj");

				auto [loadTask, loadFuture] = executor.createTask([&rhi, &executor](auto openFileFuture, auto loadOp)
				{
					ZoneScopedN("RhiApplication::draw::loadTask");

					assert(openFileFuture.valid());
					assert(openFileFuture.is_ready());

					auto [openFileResult, openFilePath] = openFileFuture.get();
					if (openFileResult == NFD_OKAY)
					{
						progress = 0;
						showProgress = true;
						loadOp(rhi, executor, openFilePath, progress);
						std::free(openFilePath);
						showProgress = false;
					}
				}, std::move(openFileFuture), loadModel);
				
				executor.addDependency(openFileTask, loadTask);
				rhi.mainCalls.enqueue(openFileTask);
			}
			
			if (MenuItem("Open Image..."))
			{
				auto [openFileTask, openFileFuture] = executor.createTask(
					openFileDialogue,
					std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["ResourcePath"]),
					"jpg,png");

				auto [loadTask, loadFuture] = executor.createTask([&rhi, &executor](auto openFileFuture, auto loadOp)
				{
					ZoneScopedN("RhiApplication::draw::loadTask");

					assert(openFileFuture.valid());
					assert(openFileFuture.is_ready());

					auto [openFileResult, openFilePath] = openFileFuture.get();
					if (openFileResult == NFD_OKAY)
					{
						progress = 0;
						showProgress = true;
						loadOp(rhi, executor, openFilePath, progress);
						std::free(openFilePath);
						showProgress = false;
					}
				}, std::move(openFileFuture), loadImage);
				
				executor.addDependency(openFileTask, loadTask);
				rhi.mainCalls.enqueue(openFileTask);
			}
			// if (MenuItem("Open GLTF...") && !myOpenFileFuture.valid())
			// {
			// 	auto [task, openFileFuture] = graph.createTask(
			// 		[&openFileDialogue, &resourcePath, &loadGlTF]
			// 		{ return openFileDialogue(resourcePath, "gltf,glb", loadGlTF); });
			// 	executor.submit(std::move(graph));
			// 	myOpenFileFuture = std::move(openFileFuture);
			// }
			Separator();
			if (MenuItem("Exit", "CTRL+Q"))
				Application::instance().lock()->requestExit();

			ImGui::EndMenu();
		}
		if (BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (BeginMenu("Layout"))
			{
				Extent2d<Vk> splitScreenGrid = rhi.windows.at(rhi_getCurrentWindow()).getConfig().splitScreenGrid;

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
					rhi.windows.at(rhi_getCurrentWindow()).onResizeSplitScreenGrid(splitScreenGrid.width, splitScreenGrid.height);
			}
#if (GRAPHICS_VALIDATION_LEVEL > 0)
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
	//UpdatePlatformWindows();
	Render();
}

void IMGUIDrawFunction(CommandBufferHandle<Vk> cmd)
{
	ZoneScopedN("RhiApplication::IMGUIDraw");

	using namespace ImGui;

	ImGui_ImplVulkan_RenderDrawData(GetDrawData(), cmd);
}

static void IMGUIInit(
	Window<Vk>& window,
	Rhi<Vk>& rhi,
	CommandBufferHandle<Vk> cmd)
{
	ZoneScopedN("RhiApplication::IMGUIInit");

	using namespace ImGui;
	using namespace rhiapplication;

	IMGUI_CHECKVERSION();
	CreateContext();
	auto& io = GetIO();
	static auto iniFilePath = (std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["UserProfilePath"]) / "imgui.ini").generic_string();
	io.IniFilename = iniFilePath.c_str();
	//io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	//io.FontGlobalScale = 1.0f;
	//io.FontAllowUserScaling = true;
	// auto& platformIo = ImGui::GetPlatformIO();
	// platformIo.Platform_CreateVkSurface =
	// 	(decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(*rhi.instance, "vkCreateWin32SurfaceKHR");

	const auto& surfaceCapabilities =
		rhi.instance->getSwapchainInfo(
			rhi.device->getPhysicalDevice(),
			window.getSurface()).capabilities;

#if defined(__OSX__)
	constexpr float dpiScaleX = 1.0f;
	constexpr float dpiScaleY = 1.0f;
#else
	float dpiScaleX = window.getConfig().contentScale.x;
	float dpiScaleY = window.getConfig().contentScale.y;
#endif

	io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.PixelSnapH = false;

	io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["ResourcePath"]));
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
		defaultFont = io.Fonts->AddFontFromFileTTF(
			fontPath.generic_string().c_str(),
			16.0f * std::max(dpiScaleX, dpiScaleY),
			&config);
	}

	// Setup style
	StyleColorsClassic();
	io.FontDefault = defaultFont;

	auto& [graphicsQueueInfos, graphicsSemaphore] = rhi.queues[QueueType_Graphics];
	auto& [graphicsQueue, graphicsSubmit] = graphicsQueueInfos.front();

	// Setup Vulkan binding
	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.Instance = *rhi.instance;
	initInfo.PhysicalDevice = rhi.device->getPhysicalDevice();
	initInfo.Device = *rhi.device;
	initInfo.QueueFamily = graphicsQueue.getDesc().queueFamilyIndex;
	initInfo.Queue = graphicsQueue;
	initInfo.PipelineCache = rhi.pipeline->getCache();
	initInfo.DescriptorPool = rhi.pipeline->getDescriptorPool();
	initInfo.MinImageCount = surfaceCapabilities.minImageCount;
	initInfo.ImageCount = window.getConfig().swapchainConfig.imageCount;
	initInfo.Allocator = &rhi.device->getInstance()->getHostAllocationCallbacks();
	initInfo.CheckVkResultFn = [](VkResult result) { VK_CHECK(result); };
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
	ImGui_ImplVulkan_Init(
		&initInfo,
		static_cast<RenderTargetHandle<Vk>>(window.getFrames().at(window.getCurrentFrameIndex())).first);
	ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(rhi_getCurrentWindow()), true);

	// Upload Fonts
	ImGui_ImplVulkan_CreateFontsTexture();

	// IMNODES_NAMESPACE::CreateContext();
	// IMNODES_NAMESPACE::LoadCurrentEditorStateFromIniString(
	//	myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

static void shutdownIMGUI()
{
	// size_t count;
	// myNodeGraph.layout.assign(IMNODES_NAMESPACE::SaveCurrentEditorStateToIniString(&count));
	// IMNODES_NAMESPACE::DestroyContext();

	ImGui_ImplVulkan_DestroyFontsTexture();

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();

	ImGui::DestroyContext();
}

static uint32_t detectSuitableGraphicsDevice(Instance<Vk>& instance, SurfaceHandle<Vk> surface)
{
	const auto& physicalDevices = instance.getPhysicalDevices();

	std::vector<std::tuple<uint32_t, uint32_t>> graphicsDeviceCandidates;
	graphicsDeviceCandidates.reserve(physicalDevices.size());

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << physicalDevices.size() << " vulkan physical device(s) found: " << std::endl;

	for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < physicalDevices.size();
			physicalDeviceIt++)
	{
		auto physicalDevice = physicalDevices[physicalDeviceIt];

		const auto& physicalDeviceInfo = instance.getPhysicalDeviceInfo(physicalDevice);
		const auto& swapchainInfo = instance.updateSwapchainInfo(physicalDevice, surface);

		if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
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
				instance.getPhysicalDeviceInfo(physicalDevices[lhsPhysicalDeviceIndex])
					.deviceProperties.properties.deviceType;
			auto rhsDeviceType =
				instance.getPhysicalDeviceInfo(physicalDevices[rhsPhysicalDeviceIndex])
					.deviceProperties.properties.deviceType;

			return deviceTypePriority[lhsDeviceType] < deviceTypePriority[rhsDeviceType];
		});

	if (graphicsDeviceCandidates.empty())
		throw std::runtime_error("failed to find a suitable GPU!");

	return std::get<0>(graphicsDeviceCandidates.front());
}

static SwapchainConfiguration<Vk> detectSuitableSwapchain(Device<Vk>& device, SurfaceHandle<Vk> surface)
{
	const auto& swapchainInfo =
		device.getInstance()->getSwapchainInfo(device.getPhysicalDevice(), surface);

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

static void createQueues(Rhi<Vk>& rhi)
{
	ZoneScopedN("rhiapplication::createQueues");

	const uint32_t frameCount = rhi.windows.at(rhi_getCurrentWindow()).getConfig().swapchainConfig.imageCount;
	const uint32_t graphicsQueueCount = frameCount;
	const uint32_t graphicsThreadCount = std::max(1u, std::thread::hardware_concurrency());
	const uint32_t computeQueueCount = 1u;
	const uint32_t transferQueueCount = 1u;
	const uint32_t defaultDrawThreadCount = 2u;

	auto& queues = rhi.queues;

	VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

	constexpr bool usePoolReset = true;
	if (usePoolReset)
		cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	const auto& queueFamilies = rhi.device->getQueueFamilies();
	for (unsigned queueFamilyIt = 0; queueFamilyIt < queueFamilies.size(); queueFamilyIt++)
	{
		const auto& queueFamily = queueFamilies[queueFamilyIt];

		for (auto type : AllQueueTypes)
		{
			if (queueFamily.flags & (1 << static_cast<uint8_t>(type)))
			{
				auto queueCount = queueFamily.queueCount;
				auto drawThreadCount = defaultDrawThreadCount;

				auto [it, wasInserted] = queues.emplace(
					type,
					std::make_tuple(
						std::vector<std::pair<Queue<Vk>, QueueHostSyncInfo<Vk>>>{},
						Semaphore<Vk>(rhi.device, SemaphoreCreateDesc<Vk>{VK_SEMAPHORE_TYPE_TIMELINE})));
				
				auto& [queueInfos, semaphore] = it->second;

				if (type == QueueType_Graphics)
				{
					if (queueInfos.size() >= graphicsQueueCount)
						continue;

					queueCount = std::min(queueCount, graphicsQueueCount);
				}
				else if (type == QueueType_Compute)
				{
					if (queueInfos.size() >= computeQueueCount)
						continue;

					queueCount = std::min(queueCount, computeQueueCount);
				}
				else if (type == QueueType_Transfer)
				{
					if (queueInfos.size() >= transferQueueCount)
						continue;

					queueCount = std::min(queueCount, transferQueueCount);
				}

				queueInfos.reserve(queueInfos.size() + queueCount);

				for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
				{
					queueInfos.emplace_back(std::make_pair(
						Queue<Vk>(
							rhi.device,
							CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIt, drawThreadCount},
							QueueCreateDesc<Vk>{queueIt, queueFamilyIt}),
						QueueHostSyncInfo<Vk>{}));
				}

				break;
			}
		}
	}
}

static void createWindowDependentObjects(Rhi<Vk>& rhi)
{
	ZoneScopedN("rhiapplication::createWindowDependentObjects");

	auto colorImage = std::make_shared<Image<Vk>>(
		rhi.device,
		ImageCreateDesc<Vk>{
			{{rhi.windows.at(rhi_getCurrentWindow()).getConfig().swapchainConfig.extent}},
			rhi.windows.at(rhi_getCurrentWindow()).getConfig().swapchainConfig.surfaceFormat.format,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	auto depthStencilImage = std::make_shared<Image<Vk>>(
		rhi.device,
		ImageCreateDesc<Vk>{
			{{rhi.windows.at(rhi_getCurrentWindow()).getConfig().swapchainConfig.extent}},
			findSupportedFormat(
				rhi.device->getPhysicalDevice(),
				{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
				VK_IMAGE_TILING_OPTIMAL,
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
					VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	rhi.renderImageSet =
		std::make_shared<RenderImageSet<Vk>>(rhi.device, std::vector{colorImage}, depthStencilImage);

	rhi.pipeline->setRenderTarget(rhi.renderImageSet);
}

auto createWindow(const auto& device, auto&& surface, auto&& windowConfig, auto&& windowState)
{
	return Window<Vk>(device, std::move(surface), std::move(windowConfig), std::move(windowState));
}

auto createPipeline(const auto& device)
{
	return std::make_unique<Pipeline<Vk>>(
		device,
		PipelineConfiguration<Vk>{(std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["UserProfilePath"]) / "pipeline.cache").string()});
}

auto createDevice(const auto& instance, auto physicalDeviceIndex)
{
	return std::make_shared<Device<Vk>>(
		instance,
		DeviceConfiguration<Vk>{physicalDeviceIndex});
}

auto createInstance(const auto& name)
{
	return std::make_shared<Instance<Vk>>(
		InstanceConfiguration<Vk>{
			name,
			"speedo",
			ApplicationInfo<Vk>{
				VK_STRUCTURE_TYPE_APPLICATION_INFO,
				nullptr,
				nullptr,
				VK_MAKE_VERSION(1, 0, 0),
				nullptr,
				VK_MAKE_VERSION(1, 0, 0),
				VK_API_VERSION_1_3}});
}

auto createRhi(const auto& name, CreateWindowFunc createWindowFunc)
{
	using namespace rhiapplication;

	WindowState windowState{};

	Window<Vk>::ConfigFile windowConfig = Window<Vk>::ConfigFile{
		std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["UserProfilePath"]) / "window.json"};

	windowState.width = windowConfig.swapchainConfig.extent.width / windowConfig.contentScale.x;
	windowState.height = windowConfig.swapchainConfig.extent.height / windowConfig.contentScale.y;

	std::shared_ptr<Rhi<Vk>> rhi;
	{
		auto windowHandle = createWindowFunc(&windowState);
		auto instance = createInstance(name);
		auto surface = createSurface(*instance, &instance->getHostAllocationCallbacks(), windowHandle);
		auto device = createDevice(instance, detectSuitableGraphicsDevice(*instance, surface));
		auto pipeline = createPipeline(device);
		rhi = std::make_shared<Rhi<Vk>>(
			std::move(instance),
			std::move(device),
			std::move(pipeline));

		windowConfig.swapchainConfig = detectSuitableSwapchain(*rhi->device, surface);
		windowConfig.contentScale = {windowState.xscale, windowState.yscale};

		auto [windowIt, windowEmplaceResult] = rhi->windows.emplace(
			windowHandle,
			createWindow(
				rhi->device,
				std::move(surface),
				std::move(windowConfig),
				std::move(windowState)));

		rhi_setWindows(&windowHandle, 1);
		rhi_setCurrentWindow(windowHandle);
	}

	createQueues(*rhi);
	createWindowDependentObjects(*rhi);

	// todo: create some resource global storage
	rhi->pipeline->resources().black = std::make_shared<Image<Vk>>(
		rhi->device,
		ImageCreateDesc<Vk>{
			{ImageMipLevelDesc<Vk>{Extent2d<Vk>{4, 4}, 16 * 4, 0}},
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_TILING_LINEAR,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	rhi->pipeline->resources().blackImageView = std::make_shared<ImageView<Vk>>(
		rhi->device, *rhi->pipeline->resources().black, VK_IMAGE_ASPECT_COLOR_BIT);

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
	rhi->pipeline->resources().samplers =
		std::make_shared<SamplerVector<Vk>>(rhi->device, std::move(samplerCreateInfos));
	//

	// initialize stuff on graphics queue
	constexpr uint32_t textureId = 1;
	constexpr uint32_t samplerId = 2;
	static_assert(textureId < ShaderTypes_GlobalTextureCount);
	static_assert(samplerId < ShaderTypes_GlobalSamplerCount);
	{
		auto& [graphicsQueueInfos, graphicsSemaphore] = rhi->queues[QueueType_Graphics];
		auto& [graphicsQueue, graphicsSubmit] = graphicsQueueInfos.front();
		
		auto cmd = graphicsQueue.getPool().commands();

		rhi->pipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		rhi->pipeline->resources().black->clear(cmd, {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}});
		rhi->pipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		IMGUIInit(rhi->windows.at(rhi_getCurrentWindow()), *rhi, cmd);

		auto materialData = std::make_unique<MaterialData[]>(ShaderTypes_MaterialCount);
		materialData[0].color = glm::vec4(1.0, 0.0, 0.0, 1.0);
		materialData[0].textureAndSamplerId =
			(textureId << ShaderTypes_GlobalTextureIndexBits) | samplerId;
		rhi->materials = std::make_unique<Buffer<Vk>>(
			rhi->device,
			graphicsQueue,
			1 + rhi->device->timelineValue().fetch_add(1, std::memory_order_relaxed),
			BufferCreateDesc<Vk>{
				ShaderTypes_MaterialCount * sizeof(MaterialData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
			materialData.get());

		auto modelInstances = std::make_unique<ModelInstance[]>(ShaderTypes_ModelInstanceCount);
		auto identityMatrix = glm::mat4x4(1.0f);
		modelInstances[666].modelTransform = identityMatrix;
		modelInstances[666].inverseTransposeModelTransform =
			glm::transpose(glm::inverse(modelInstances[666].modelTransform));
		rhi->modelInstances = std::make_unique<Buffer<Vk>>(
			rhi->device,
			graphicsQueue,
			1 + rhi->device->timelineValue().fetch_add(1, std::memory_order_relaxed),
			BufferCreateDesc<Vk>{
				ShaderTypes_ModelInstanceCount * sizeof(ModelInstance),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT},
			modelInstances.get());

		cmd.end();

		graphicsQueue.enqueueSubmit(QueueDeviceSyncInfo<Vk>{
			{},
			{},
			{},
			{graphicsSemaphore},
			{1 + rhi->device->timelineValue().fetch_add(1, std::memory_order_relaxed)}});

		graphicsSubmit = graphicsQueue.submit();
	}

	auto shaderIncludePath = std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["RootPath"]) / "src/rhi/shaders";
	auto shaderIntermediatePath = std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["UserProfilePath"]) / ".slang.intermediate";

	ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);
	auto shaderReflection = shaderLoader.load<Vk>(shaderIncludePath / "shaders.slang");

	auto layoutHandle = rhi->pipeline->createLayout(shaderReflection);

	rhi->pipeline->bindLayoutAuto(layoutHandle, VK_PIPELINE_BIND_POINT_GRAPHICS);

	for (uint8_t i = 0; i < ShaderTypes_FrameCount; i++)
	{
		rhi->pipeline->setDescriptorData(
			"g_viewData",
			DescriptorBufferInfo<Vk>{rhi->windows.at(rhi_getCurrentWindow()).getViewBuffer(i), 0, VK_WHOLE_SIZE},
			DescriptorSetCategory_View,
			i);
	}

	rhi->pipeline->setDescriptorData(
		"g_materialData",
		DescriptorBufferInfo<Vk>{*rhi->materials, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Material);

	rhi->pipeline->setDescriptorData(
		"g_modelInstances",
		DescriptorBufferInfo<Vk>{*rhi->modelInstances, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_ModelInstances);

	rhi->pipeline->setDescriptorData(
		"g_samplers",
		DescriptorImageInfo<Vk>{(*rhi->pipeline->resources().samplers)[0]},
		DescriptorSetCategory_GlobalSamplers,
		samplerId);

	for (uint8_t i = 0; i < ShaderTypes_FrameCount; i++)
	{
		rhi->pipeline->setDescriptorData(
			"g_viewData",
			DescriptorBufferInfo<Vk>{rhi->windows.at(rhi_getCurrentWindow()).getViewBuffer(i), 0, VK_WHOLE_SIZE},
			DescriptorSetCategory_View,
			i);
	}

	return rhi;
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


} // namespace rhiapplication

template <>
Rhi<Vk>& RhiApplication::internalRhi<Vk>()
{
	return *std::static_pointer_cast<Rhi<Vk>>(myRhi);
}

template <>
const Rhi<Vk>& RhiApplication::internalRhi<Vk>() const
{
	return *std::static_pointer_cast<Rhi<Vk>>(myRhi);
}

void RhiApplication::internalUpdateInput()
{
	using namespace rhiapplication;

	if (exitRequested())
		return;

	ImGui_ImplGlfw_NewFrame(); // will poll glfw input events and update input state

	auto& rhi = internalRhi<Vk>();
	auto& executor = internalExecutor();
	auto& imguiIO = ImGui::GetIO();
	auto& input = myInput;

	MouseEvent mouse;
	while (myMouseQueue.try_dequeue(mouse))
	{
		if (mouse.flags & MouseEvent::Position)
		{
			input.mouse.position[0] = static_cast<float>(mouse.xpos);
			input.mouse.position[1] = static_cast<float>(mouse.ypos);
			input.mouse.insideWindow = mouse.insideWindow;

			imguiIO.AddMousePosEvent(input.mouse.position[0], input.mouse.position[1]);
		}

		if (mouse.flags & MouseEvent::Button)
		{
			bool leftPressed = (mouse.button == GLFW_MOUSE_BUTTON_LEFT && mouse.action == GLFW_PRESS);
			bool rightPressed = (mouse.button == GLFW_MOUSE_BUTTON_RIGHT && mouse.action == GLFW_PRESS);
			bool leftReleased = (mouse.button == GLFW_MOUSE_BUTTON_LEFT && mouse.action == GLFW_RELEASE);
			bool rightReleased = (mouse.button == GLFW_MOUSE_BUTTON_RIGHT && mouse.action == GLFW_RELEASE);

			if (leftPressed)
			{
				input.mouse.leftDown = true;
				input.mouse.leftLastEventPosition[0] = input.mouse.position[0];
				input.mouse.leftLastEventPosition[1] = input.mouse.position[1];
				imguiIO.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, true);
			}
			else if (rightPressed)
			{
				input.mouse.rightDown = true;
				input.mouse.rightLastEventPosition[0] = input.mouse.position[0];
				input.mouse.rightLastEventPosition[1] = input.mouse.position[1];
				imguiIO.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_RIGHT, true);
			}
			else if (leftReleased)
			{
				input.mouse.leftDown = false;
				input.mouse.leftLastEventPosition[0] = input.mouse.position[0];
				input.mouse.leftLastEventPosition[1] = input.mouse.position[1];
				imguiIO.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, false);
			}
			else if (rightReleased)
			{
				input.mouse.rightDown = false;
				input.mouse.rightLastEventPosition[0] = input.mouse.position[0];
				input.mouse.rightLastEventPosition[1] = input.mouse.position[1];
				imguiIO.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_RIGHT, false);
			}
		}
	}

	KeyboardEvent keyboard;
	while (myKeyboardQueue.try_dequeue(keyboard))
	{
		if (keyboard.action == GLFW_PRESS)
			input.keyboard.keysDown[keyboard.key] = true;
		else if (keyboard.action == GLFW_RELEASE)
			input.keyboard.keysDown[keyboard.key] = false;

		//imguiIO.AddKeyEvent(keyboard.key, keyboard.action);
	}

	if (!imguiIO.WantCaptureMouse && !imguiIO.WantCaptureKeyboard)
		rhi.windows.at(rhi_getCurrentWindow()).onInputStateChanged(input);

	{
		auto drawPair = executor.createTask([this]{ draw(); });
		auto& [drawTask, drawFuture] = drawPair;
		executor.addDependency(g_updateTask.first, drawTask, true);
		g_drawTask = drawPair;
	}
}

void RhiApplication::internalDraw()
{
	using namespace rhiapplication;

	if (exitRequested())
		return;

	std::unique_lock lock(g_drawMutex);

	auto& rhi = internalRhi<Vk>();
	auto& executor = internalExecutor();

	FrameMark;
	ZoneScopedN("rhi::draw");

	auto& instance = *rhi.instance;
	auto& device = *rhi.device;
	auto& window = rhi.windows.at(rhi_getCurrentWindow());
	auto& pipeline = *rhi.pipeline;

	ImGui_ImplVulkan_NewFrame(); // no-op?
	IMGUIPrepareDrawFunction(rhi, executor); // todo: kick off earlier (but not before ImGui_ImplGlfw_NewFrame or ImGio::Newframe())

	auto [flipSuccess, lastFrameIndex, newFrameIndex] = window.flip();

	if (flipSuccess)
	{
		ZoneScopedN("rhi::draw::submit");

		auto& [graphicsQueueInfos, graphicsSemaphore] = rhi.queues[QueueType_Graphics];
		auto& [lastGraphicsQueue, lastGraphicsSubmit] = graphicsQueueInfos.at(lastFrameIndex);
		auto& [graphicsQueue, graphicsSubmit] = graphicsQueueInfos.at(newFrameIndex);

		auto& renderImageSet = *rhi.renderImageSet;

		if (auto timelineValue = graphicsSubmit.maxTimelineValue; timelineValue)
		{
			ZoneScopedN("rhi::draw::waitGraphics");

			graphicsSemaphore.wait(timelineValue);
			graphicsQueue.processTimelineCallbacks(timelineValue);
		}
		
		graphicsQueue.getPool().reset();

		TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("rhi::draw::drawCall");

			executor.call(drawCall, newFrameIndex);
		}

		auto cmd = graphicsQueue.getPool().commands();

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);
		
		{
			GPU_SCOPE(cmd, graphicsQueue, clear);

			renderImageSet.clearDepthStencil(cmd, {1.0f, 0});
			renderImageSet.clearColor(cmd, {{0.2f, 0.2f, 0.2f, 1.0f}}, 0);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, draw);

			renderImageSet.transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
			renderImageSet.transitionDepthStencil(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			
			auto renderPassInfo = renderImageSet.begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
			
			auto drawThreadCount = window.draw(
				pipeline,
				graphicsQueue,
				std::move(renderPassInfo)); // TODO: kick off jobs for this earier and join here

			for (uint32_t threadIt = 1ul; threadIt <= drawThreadCount; threadIt++)
				graphicsQueue.execute(
					threadIt,
					1 + device.timelineValue().fetch_add(1, std::memory_order_relaxed));

			renderImageSet.end(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, blit);

			renderImageSet.transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);

			window.blit(
				cmd,
				renderImageSet,
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				0,
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				0);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, imgui);

			window.transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
			window.begin(cmd, VK_SUBPASS_CONTENTS_INLINE);

			IMGUIDrawFunction(cmd);

			window.end(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, transition);
			
			window.transitionColor(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);
		}

		cmd.end();

		graphicsQueue.enqueueSubmit(QueueDeviceSyncInfo<Vk>{
			{graphicsSemaphore},
			{VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
			{lastGraphicsSubmit.maxTimelineValue},
			{graphicsSemaphore},
			{1 + device.timelineValue().fetch_add(1, std::memory_order_relaxed)}
		});

		graphicsSubmit = graphicsQueue.submit();
		graphicsQueue.enqueuePresent(window.preparePresent(graphicsSubmit));
		graphicsQueue.present();
	}

	// todo: move to own function?
	{
		auto updatePair = executor.createTask([this]{ updateInput(); });
		auto& [updateTask, updateFuture] = updatePair;
		executor.addDependency(g_drawTask.first, updateTask, true);
		g_updateTask = updatePair;
	}
}

RhiApplication::RhiApplication(
	std::string_view appName,
	Environment&& env,
	CreateWindowFunc createWindowFunc)
: Application(std::forward<std::string_view>(appName), std::forward<Environment>(env))
, myRhi(rhiapplication::createRhi(name(), createWindowFunc))
{
	ZoneScopedN("RhiApplication()");

	using namespace rhiapplication;

	// g_rootPath = std::get<std::filesystem::path>(environment().variables["RootPath"]);
	// g_resourcePath = std::get<std::filesystem::path>(environment().variables["ResourcePath"]);
	// g_userProfilePath = std::get<std::filesystem::path>(environment().variables["UserProfilePath"]);

	auto& executor = internalExecutor();
	auto& rhi = internalRhi<Vk>();

	auto updatePair = executor.createTask([this]{ updateInput(); });
	auto& [updateTask, updateFuture] = updatePair;
	g_updateTask = updatePair;
	executor.submit(updateTask);

	//myNodeGraph = g_userProfilePath / "nodegraph.json"; // temp - this should be stored in the resource path
}

RhiApplication::~RhiApplication()
{
	using namespace rhiapplication;

	ZoneScopedN("~RhiApplication()");

	requestExit();

	auto& executor = internalExecutor();

	executor.join(std::move(g_updateTask.second));
	executor.join(std::move(g_drawTask.second));

	g_updateTask = {};
	g_drawTask = {};

	auto& rhi = internalRhi<Vk>();
	auto device = rhi.device;
	auto instance = rhi.instance;

	auto& [graphicsQueueInfos, graphicsSemaphore] = rhi.queues[QueueType_Graphics];
	for (auto& [graphicsQueue, graphicsSubmit] : graphicsQueueInfos)	
	{
		ZoneScopedN("~RhiApplication()::waitGraphics");

		graphicsSemaphore.wait(graphicsSubmit.maxTimelineValue);
		graphicsQueue.processTimelineCallbacks(graphicsSubmit.maxTimelineValue);
	}

	auto& [transferQueueInfos, transferSemaphore] = rhi.queues[QueueType_Transfer];
	for (auto& [transferQueue, transferSubmit] : transferQueueInfos)	
	{
		ZoneScopedN("~RhiApplication()::waitTransfer");

		transferSemaphore.wait(transferSubmit.maxTimelineValue);
		transferQueue.processTimelineCallbacks(transferSubmit.maxTimelineValue);
	}

	shutdownIMGUI();

	myRhi.reset();

	assert(device.use_count() == 1);
	assert(instance.use_count() == 2);
}

void RhiApplication::tick()
{
	using namespace rhiapplication;

	ZoneScopedN("RhiApplication::tick");

	auto& executor = internalExecutor();
	auto& rhi = internalRhi<Vk>();
	
	TaskHandle mainCall;
	while (rhi.mainCalls.try_dequeue(mainCall))
	{
		ZoneScopedN("RhiApplication::tick::mainCall");

		executor.call(mainCall);
	}
}

void RhiApplication::onResizeFramebuffer(WindowHandle window, int w, int h)
{
	using namespace rhiapplication;

	std::unique_lock lock(g_drawMutex);

	ZoneScopedN("RhiApplication::onResizeFramebuffer");

	auto& rhi = internalRhi<Vk>();

	auto& [graphicsQueueInfos, graphicsSemaphore] = rhi.queues[QueueType_Graphics];
	for (auto& [graphicsQueue, graphicsSubmit] : graphicsQueueInfos)	
	{
		ZoneScopedN("RhiApplication::onResizeFramebuffer::waitGraphics");

		graphicsSemaphore.wait(graphicsSubmit.maxTimelineValue);
		graphicsQueue.processTimelineCallbacks(graphicsSubmit.maxTimelineValue);
	}

	rhi.windows.at(window).onResizeFramebuffer(w, h);

	createWindowDependentObjects(rhi);
}

WindowState* RhiApplication::getWindowState(WindowHandle window)
{
	using namespace rhiapplication;

	auto& rhi = internalRhi<Vk>();

	return &rhi.windows.at(window).getState();
}
