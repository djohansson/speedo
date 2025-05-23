#include "../rhi.h"
#include "../rhiapplication.h"
#include "utils.h"

#include <core/task.h>
#include <gfx/scene.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

#include <gfx/imgui_extra.h>

#include <algorithm>
#include <memory>

//#include <imnodes.h>

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

namespace rhiapplication
{

static ConcurrentQueue<ImDrawData> gIMGUIDrawData;

void IMGUIPrepareDrawFunction(RHI<kVk>& rhi, TaskExecutor& executor)
{
	ZoneScopedN("RHIApplication::IMGUIPrepareDraw");

	using namespace ImGui;

	ImGui_ImplGlfw_NewFrame(); // will poll glfw input events and update input state

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

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	static bool gShowStatistics = false;
	{
		if (gShowStatistics)
		{
			if (Begin("Statistics", &gShowStatistics))
			{
				Text("Unknowns: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_UNKNOWN));
				Text("Instances: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_INSTANCE));
				Text(
					"Physical Devices: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
				Text("Devices: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_DEVICE));
				Text("Queues: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_QUEUE));
				Text("Semaphores: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
				Text(
					"Command Buffers: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
				Text("Fences: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_FENCE));
				Text("Device Memory: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
				Text("Buffers: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_BUFFER));
				Text("Images: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_IMAGE));
				Text("Events: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_EVENT));
				Text("Query Pools: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
				Text("Buffer Views: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
				Text("Image Views: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
				Text(
					"Shader Modules: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
				Text(
					"Pipeline Caches: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
				Text(
					"Pipeline Layouts: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
				Text("Render Passes: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
				Text("Pipelines: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_PIPELINE));
				Text(
					"Descriptor Set Layouts: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
				Text("Samplers: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_SAMPLER));
				Text(
					"Descriptor Pools: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
				Text(
					"Descriptor Sets: %u",
					rhi.device->GetTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
				Text("Framebuffers: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
				Text("Command Pools: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
				Text("Surfaces: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
				Text("Swapchains: %u", rhi.device->GetTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
			}
			End();
		}
	}
#endif

	if (RHIApplication::gShowDemoWindow)
		ShowDemoWindow(&RHIApplication::gShowDemoWindow);

	if (RHIApplication::gShowAbout && Begin("About client", &RHIApplication::gShowAbout))
	{
		End();
	}

	if (bool b = RHIApplication::gShowProgress.load(std::memory_order_relaxed) &&
				 Begin(
					 "Loading",
					 &b,
					 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
						 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
	{
		SetWindowSize(ImVec2(160, 0));
		ProgressBar((1.F / 255) * RHIApplication::gProgress);
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

			IMNODES_NAMESPACE::BeginNode(node->Id());

			// title bar
			std::format_to_n(buffer, std::size(buffer), "##node{0}", 4, node->Id());

			IMNODES_NAMESPACE::BeginNodeTitleBar();

			float titleBarTextWidth =
				editableTextField(node->Id(), buffer, node->GetName(), 160.0f, node->Selected());

			IMNODES_NAMESPACE::EndNodeTitleBar();

			if (IsItemClicked() && IsMouseDoubleClicked(0))
				node->Selected() = std::make_optional(node->Id());

			// attributes
			if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
			{
				auto rowCount = std::max(
					inOutNode->InputAttributes().size(), inOutNode->OutputAttributes().size());

				for (uint32_t rowIt = 0ul; rowIt < rowCount; rowIt++)
				{
					float inputTextWidth = 0.0f;
					bool hasInputPin = rowIt < inOutNode->InputAttributes().size();
					if (hasInputPin)
					{
						auto& inputAttribute = inOutNode->InputAttributes()[rowIt];
						std::format_to_n(buffer, std::size(buffer), "##inputattribute{0}", 4, inputAttribute.id);

						IMNODES_NAMESPACE::BeginInputAttribute(inputAttribute.id);

						inputTextWidth = editableTextField(
							inputAttribute.id,
							buffer,
							inputAttribute.name,
							80.0f,
							node->Selected());

						IMNODES_NAMESPACE::EndInputAttribute();

						if (IsItemClicked() && IsMouseDoubleClicked(0))
							node->Selected() = std::make_optional(inputAttribute.id);
					}

					if (rowIt < inOutNode->OutputAttributes().size())
					{
						auto& outputAttribute = inOutNode->OutputAttributes()[rowIt];
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
							node->Selected());

						IMNODES_NAMESPACE::EndOutputAttribute();

						if (IsItemClicked() && IsMouseDoubleClicked(0))
							node->Selected() = std::make_optional(outputAttribute.id);
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
							static_cast<unsigned>(inOutNode->InputAttributes().size()));
						inOutNode->InputAttributes().emplace_back(
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
							static_cast<unsigned>(inOutNode->OutputAttributes().size()));
						inOutNode->OutputAttributes().emplace_back(
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
										ASSERT(false);
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

	auto resourcePath = std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["ResourcePath"]);
	auto& window = rhi.GetWindow(GetCurrentWindow());

	if (BeginMainMenuBar())
	{
		if (BeginMenu("File"))
		{
			if (MenuItem("Open OBJ..."))
			{
				static const std::vector<nfdu8filteritem_t> filterList ={
					nfdu8filteritem_t{.name = "Wavefront OBJ", .spec = "obj"}
				};
				OpenFileDialogueAsync((resourcePath / "models").string(), filterList,
					[](std::string_view filePath, std::atomic_uint8_t& progressOut){
						auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
						ENSURE(app);
						auto& pipeline = app->GetRHI<kVk>().pipeline;
						ENSURE(pipeline);
						auto& resources = pipeline->GetResources();
						
						auto model = std::make_shared<Model<kVk>>(model::LoadModel(filePath, progressOut, std::atomic_load(&resources.model)));

						pipeline->SetVertexInputState(*model);
						pipeline->SetDescriptorData(
							"gVertexBuffer",
							DescriptorBufferInfo<kVk>{model->GetVertexBuffer(), 0, VK_WHOLE_SIZE},
							DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

						std::atomic_store(&resources.model, model);
					});
			}
			if (MenuItem("Open Image..."))
			{
				static const std::vector<nfdu8filteritem_t> filterList = {
					nfdu8filteritem_t{.name = "Image files", .spec = "jpg,jpeg,png,bmp,tga,gif,psd,hdr,pic,pnm"}
				};

				OpenFileDialogueAsync((resourcePath / "images").string(), filterList, 
					[](std::string_view filePath, std::atomic_uint8_t& progressOut){
						auto [newImage, newImageView] = image::LoadImage<kVk>(filePath, progressOut);
					});
			}
			// if (MenuItem("Open Scene..."))
			// {
			// 	static const std::vector<nfdu8filteritem_t> filterList = {
			// 		nfdu8filteritem_t{.name = "Scene files", .spec = "gltf,glb"}
			// 	};

			// 	OpenFileDialogueAsync((resourcePath / "scenes").string(), filterList, 
			// 		[&scene = Scene{}](std::string_view filePath, std::atomic_uint8_t& progress){ scene::LoadScene(scene, filePath, progress); });
			// }
			Separator();
			if (MenuItem("Exit", "CTRL+Q"))
				Application::Get().lock()->RequestExit();

			ImGui::EndMenu();
		}
		if (BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (BeginMenu("Layout"))
			{
				Extent2d<kVk> splitScreenGrid = window.GetConfig().splitScreenGrid;

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
				RHIApplication::gShowDemoWindow = !RHIApplication::gShowDemoWindow;
			Separator();
			if (MenuItem("About client..."))
				RHIApplication::gShowAbout = !RHIApplication::gShowAbout;
			ImGui::EndMenu();
		}

		EndMainMenuBar();
	}

	EndFrame();
	//UpdatePlatformWindows();
	Render();

	ImDrawData drawData;
	static imgui_extra::ImDrawDataSnapshot snapshot;
	if (auto data = GetDrawData())
	{
		snapshot.SnapUsingSwap(data, &drawData, GetTime());
		gIMGUIDrawData.enqueue(std::move(drawData));
	}
}

void IMGUIDrawFunction(
	CommandBufferHandle<kVk> cmd,
	std::vector<TaskHandle>& /*callbacks*/,
	PipelineHandle<kVk> pipeline = nullptr)
{
	ZoneScopedN("RHIApplication::IMGUIDraw");

	using namespace ImGui;

	static ImDrawData gDrawData;
	while (gIMGUIDrawData.try_dequeue(gDrawData));

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplVulkan_RenderDrawData(&gDrawData, cmd, /*callbacks,*/ pipeline);
}

static void IMGUIInit(
	Window<kVk>& window,
	RHI<kVk>& rhi,
	Queue<kVk>& graphicsQueue)
{
	ZoneScopedN("RHIApplication::IMGUIInit");

	using namespace ImGui;
	using namespace rhiapplication;

	IMGUI_CHECKVERSION();
	CreateContext();
	auto& io = GetIO();
	io.IniFilename = nullptr;

	//io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	//io.FontGlobalScale = 1.0f;
	//io.FontAllowUserScaling = true;
	// auto& platformIo = ImGui::GetPlatformIO();
	// platformIo.Platform_CreateVkSurface =
	// 	(decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(*rhi.instance, "vkCreateWin32SurfaceKHR");

	LoadIniSettingsFromMemory(window.GetConfig().imguiIniSettings.c_str(), window.GetConfig().imguiIniSettings.size());

	const auto& surfaceCapabilities =
		rhi.instance->GetSwapchainInfo(
			rhi.device->GetPhysicalDevice(),
			window.GetSurface()).capabilities;

#if defined(__OSX__)
	float dpiScaleX = 1.0F;
	float dpiScaleY = 1.0F;
#else
	float dpiScaleX = window.GetConfig().contentScale.x;
	float dpiScaleY = window.GetConfig().contentScale.y;
#endif

	io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.RasterizerDensity = std::max(window.GetConfig().contentScale.x, window.GetConfig().contentScale.y);
	config.PixelSnapH = false;

	io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["ResourcePath"]));
	fontPath /= "fonts";
	fontPath /= "foo";

	constexpr const char* kFonts[]{
		"Cousine-Regular.ttf",
		"DroidSans.ttf",
		"Karla-Regular.ttf",
		"ProggyClean.ttf",
		"ProggyTiny.ttf",
		"Roboto-Medium.ttf",
	};

	ImFont* defaultFont = nullptr;
	for (const auto* font : kFonts)
	{
		fontPath.replace_filename(font);
		defaultFont = io.Fonts->AddFontFromFileTTF(
			fontPath.generic_string().c_str(), 16.0F * std::max(dpiScaleX, dpiScaleY), &config);
	}

	// Setup style
	StyleColorsClassic();
	io.FontDefault = defaultFont;

	// Setup Vulkan binding
	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.Instance = *rhi.instance;
	initInfo.PhysicalDevice = rhi.device->GetPhysicalDevice();
	initInfo.Device = *rhi.device;
	initInfo.QueueFamily = graphicsQueue.GetDesc().queueFamilyIndex;
	initInfo.Queue = graphicsQueue;
	initInfo.PipelineCache = rhi.pipeline->GetCache();
	initInfo.DescriptorPool = rhi.pipeline->GetDescriptorPool();
	initInfo.MinImageCount = surfaceCapabilities.minImageCount;
	// initInfo.ImageCount is used to determine the number of buffers in flight inside imgui, 
	// and since we allow up to queuecount in-flight renders per frame due to triple buffering,
	// this needs to be set to the queue count accordingly.
	initInfo.ImageCount = window.GetConfig().swapchainConfig.imageCount;
	initInfo.Allocator = &rhi.device->GetInstance()->GetHostAllocationCallbacks();
	initInfo.CheckVkResultFn = [](VkResult result) { VK_ENSURE(result); };
	initInfo.UseDynamicRendering = window.GetConfig().swapchainConfig.useDynamicRendering;
	initInfo.RenderPass = initInfo.UseDynamicRendering ? VK_NULL_HANDLE : static_cast<RenderTargetPassHandle<kVk>>(window.GetFrames()[0]).first;
	initInfo.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfoKHR{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
		.pNext = nullptr,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &window.GetConfig().swapchainConfig.surfaceFormat.format,
	};
	ImGui_ImplVulkan_Init(&initInfo);
	ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(GetCurrentWindow()), true);

	// Upload Fonts
	ImGui_ImplVulkan_CreateFontsTexture();

	// IMNODES_NAMESPACE::CreateContext();
	// IMNODES_NAMESPACE::LoadCurrentEditorStateFromIniString(
	//	myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

static void ShutdownImgui()
{
	// size_t count;
	// myNodeGraph.layout.assign(IMNODES_NAMESPACE::SaveCurrentEditorStateToIniString(&count));
	// IMNODES_NAMESPACE::DestroyContext();

	ImGui_ImplVulkan_DestroyFontsTexture();

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();

	ImGui::DestroyContext();
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

bool RHIApplication::Main()
{
	using namespace rhiapplication;
	
	ZoneScopedN("RHIApplication::Main");

	auto& rhi = GetRHI<kVk>();
	auto& window = rhi.GetWindow(GetCurrentWindow());

	TaskHandle mainCall;
	while (rhi.mainCalls.try_dequeue(mainCall))
	{
		GetExecutor().Call(mainCall);
	}

	return !IsExitRequested();
}

void RHIApplication::OnInputStateChanged(const InputState& input)
{
	using namespace rhiapplication;
	
	ZoneScopedN("RHIApplication::OnInputStateChanged");

	auto& rhi = GetRHI<kVk>();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& io = ImGui::GetIO();

	if (io.WantSaveIniSettings)
	{
		size_t iniStringSize;
		const char* iniString = ImGui::SaveIniSettingsToMemory(&iniStringSize);
		window.GetConfig().imguiIniSettings.assign(iniString, iniStringSize);
		io.WantSaveIniSettings = false;
	}

	if (!io.WantCaptureMouse && !io.WantCaptureKeyboard)
		window.OnInputStateChanged(input);

	IMGUIPrepareDrawFunction(rhi, GetExecutor());
}

void RHIApplication::Draw()
{
	using namespace rhiapplication;

	FrameMark;
	ZoneScopedN("RHIApplication::Draw");

	std::unique_lock lock(gDrawMutex);

	auto& rhi = GetRHI<kVk>();
	auto& instance = *rhi.instance;
	auto& device = *rhi.device;
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& pipeline = *rhi.pipeline;

	std::vector<TaskHandle> frameTasks;
	frameTasks.emplace_back(CreateTask([&rhi, &executor = GetExecutor()]
		{
			auto transfer = ConcurrentReadScope(rhi.queues[kQueueTypeTransfer]);
			for (const auto& [transferQueue, transferSubmit] : transfer->queues)	
				transferQueue.SubmitCallbacks(executor, transfer->semaphore.GetValue());
		}).handle);

	auto [acquireNextImageFence, acquireNextImageSemaphore, lastFrameIndex, newFrameIndex, flipSuccess] = window.Flip();

	auto compute = ConcurrentWriteScope(rhi.queues[kQueueTypeCompute]);
	auto& [computeQueue, computeSubmit] = compute->queues.FetchAdd();

	frameTasks.emplace_back(CreateTask([&executor = GetExecutor(), &computeQueue, &computeSemaphore = compute->semaphore] {
		computeQueue.SubmitCallbacks(executor, computeSemaphore.GetValue()); }).handle);

	{
		ZoneScopedN("RHIApplication::Draw::waitCompute");

		for (auto& fence : computeSubmit.fences)
			while (!fence.Wait(0ULL))
				GetExecutor().JoinOne();

		// if (SupportsExtension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, device.GetPhysicalDevice()))
		// {
		// 	ZoneScopedN("RHIApplication::Draw::waitCompute::waitPresent");
	
		// 	for (auto presentId : computeSubmit.presentIds)
		// 		while (!window.WaitPresent(presentId, 0ULL))
		// 			GetExecutor().JoinOne();
		// }
		// else
		{
			ZoneScopedN("RHIApplication::Draw::waitCompute::fairyDust");

			using namespace std::chrono_literals;
			auto start = std::chrono::high_resolution_clock::now();
			std::chrono::microseconds delay = 1000us;
			auto end = start + delay;
			do { std::this_thread::yield();	} while (std::chrono::high_resolution_clock::now() < end);
		}
	}
	computeQueue.GetPool().Reset();	
	computeSubmit = {};

	if (flipSuccess)
	{
		ZoneScopedN("RHIApplication::Draw::submit");

		auto& lastFrame = window.GetFrames()[lastFrameIndex];
		auto& newFrame = window.GetFrames()[newFrameIndex];

		auto graphics = ConcurrentWriteScope(rhi.queues[kQueueTypeGraphics]);

		auto& [lastGraphicsQueue, lastGraphicsSubmit] = graphics->queues.FetchAdd();
		auto& [graphicsQueue, graphicsSubmit] = graphics->queues.Get();

		frameTasks.emplace_back(CreateTask([&executor = GetExecutor(), &graphicsQueue, &graphicsSemaphore = graphics->semaphore]
			{ graphicsQueue.SubmitCallbacks(executor, graphicsSemaphore.GetValue()); }).handle);

		if (!graphicsSubmit.fences.empty())
		{
			ZoneScopedN("RHIApplication::Draw::waitGraphics");

			for (auto& fence : graphicsSubmit.fences)
				while (!fence.Wait(0ULL))
					GetExecutor().JoinOne();
		}
		graphicsQueue.GetPool().Reset();
		graphicsSubmit = {};

		TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("RHIApplication::Draw::drawCall");

			GetExecutor().Call(drawCall, &graphics.Get());
		}

		window.UpdateViewBuffer(); // todo: move to drawCall
		
		auto& renderImageSet = rhi.renderImageSets[newFrameIndex];

		auto cmd = graphicsQueue.GetPool().Commands();

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);
		
		{
			GPU_SCOPE(cmd, graphicsQueue, draw);

			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);

			rhi.pipeline->SetRenderTarget(renderImageSet);
			pipeline.BindLayoutAuto(rhi.pipelineLayouts.at("Main"), VK_PIPELINE_BIND_POINT_GRAPHICS);
			
			auto renderTargetInfo = renderImageSet.Begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

			// setup draw parameters
			uint32_t drawCount = window.GetConfig().splitScreenGrid.width * window.GetConfig().splitScreenGrid.height;
			uint32_t drawThreadCount = 0;

			std::atomic_uint32_t drawAtomic = 0UL;

			// draw views using secondary command buffers
			// todo: generalize this to other types of draws
			if (std::atomic_load(&pipeline.GetResources().model))
			{
				ZoneScopedN("RHIApplication::Draw::drawViews");

				drawThreadCount = std::min<uint32_t>(drawCount, graphicsQueue.GetPool().GetDesc().levelCount);

				std::array<uint32_t, 128> seq;
				std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
				std::for_each_n(
					seq.begin(),
					drawThreadCount,
					[&pipeline,
					&queue = graphicsQueue,
					&renderTargetInfo,
					&renderImageSet,
					frameIndex = newFrameIndex,
					&drawAtomic,
					&drawCount,
					&desc = window.GetConfig()](uint32_t threadIt)
					{
						ZoneScoped;

						auto drawIt = drawAtomic++;
						if (drawIt >= drawCount)
							return;

						auto zoneNameStr = std::format("Window::drawPartition thread:{}", threadIt);

						ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

						CommandBufferInheritanceInfo<kVk> inheritInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
						CommandBufferAccessScopeDesc<kVk> beginInfo{};
						beginInfo.pInheritanceInfo = &inheritInfo;
						beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
						beginInfo.level = threadIt + 1;
						// for dynamic rendering, setting this here is just to silence vvl warnings
						beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
						//

						uint32_t dx = 0;
						uint32_t dy = 0;

						if (const auto* dynamicRenderingInfo = std::get_if<DynamicRenderingInfo<kVk>>(&renderTargetInfo))
						{
							inheritInfo.pNext = &dynamicRenderingInfo->inheritanceInfo;

							dx = dynamicRenderingInfo->renderInfo.renderArea.extent.width / desc.splitScreenGrid.width;
							dy = dynamicRenderingInfo->renderInfo.renderArea.extent.height / desc.splitScreenGrid.height;
						} 
						else if (const auto* renderPassBeginInfo = std::get_if<VkRenderPassBeginInfo>(&renderTargetInfo))
						{
							inheritInfo.renderPass = renderPassBeginInfo->renderPass;
							inheritInfo.framebuffer = renderPassBeginInfo->framebuffer;

							dx = renderPassBeginInfo->renderArea.extent.width / desc.splitScreenGrid.width;
							dy = renderPassBeginInfo->renderArea.extent.height / desc.splitScreenGrid.height;
						}

						auto cmd = queue.GetPool().Commands(beginInfo);

						auto& model = *std::atomic_load(&pipeline.GetResources().model);

						auto bindState = [&pipeline, &model](VkCommandBuffer cmd)
						{
							ZoneScopedN("bindState");

							// bind vertex inputs
							BufferHandle<kVk> vbs[] = {model.GetVertexBuffer()};
							DeviceSize<kVk> offsets[] = {0};
							vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
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

						PushConstants pushConstants{.frameIndex = frameIndex};

						ASSERT(dx > 0);
						ASSERT(dy > 0);

						while (drawIt < drawCount)
						{
							auto drawView = [&pushConstants, &pipeline, &model, &cmd, &dx, &dy, &desc](uint16_t viewIt)
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
									scissor.offset = {.x = x, .y = y};
									scissor.extent = {.width = width, .height = height};

									vkCmdSetViewport(cmd, 0, 1, &viewport);
									vkCmdSetScissor(cmd, 0, 1, &scissor);
								};

								setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

								uint16_t viewIndex = viewIt;
								uint16_t materialIndex = 0U;

								pushConstants.viewAndMaterialId = (static_cast<uint32_t>(viewIndex) << SHADER_TYPES_MATERIAL_INDEX_BITS) | materialIndex;

								auto drawModel = [&pushConstants, &pipeline, &model](VkCommandBuffer cmd)
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
											model.GetDesc().indexCount,
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

			for (uint32_t threadIt = 1UL; threadIt <= drawThreadCount; threadIt++)
				graphicsQueue.Execute(threadIt, graphics->timeline);

			renderImageSet.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, computeMain);

			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, 0);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);

			window.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			window.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			window.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);

			pipeline.BindLayoutAuto(rhi.pipelineLayouts.at("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);

			rhi.pipeline->SetDescriptorData(
				"gTextures",
				DescriptorImageInfo<kVk>{
					{},
					renderImageSet.GetAttachments()[0],
					renderImageSet.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				newFrameIndex);

			rhi.pipeline->SetDescriptorData(
				"gRWTextures",
				DescriptorImageInfo<kVk>{
					{},
					window.GetAttachments()[0],
					window.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				newFrameIndex);

			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES);
			pipeline.BindPipelineAuto(cmd);

			PushConstants pushConstants{.frameIndex = newFrameIndex};

			vkCmdPushConstants(
				cmd,
				pipeline.GetLayout(),
				VK_SHADER_STAGE_ALL, // todo: input active shader stages + ranges from pipeline
				0,
				sizeof(pushConstants),
				&pushConstants);

			vkCmdDispatch(cmd, 16U, 8U, 1U);
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
		std::vector<TaskHandle> graphicsCallbacks;
		{
			GPU_SCOPE(cmd, graphicsQueue, imgui);

			window.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, 0);
			window.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			window.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			
			rhi.pipeline->SetRenderTarget(newFrame);
			
			window.Begin(cmd, VK_SUBPASS_CONTENTS_INLINE);
			IMGUIDrawFunction(cmd, graphicsCallbacks);
			window.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, Transition);
			
			window.Transition(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		}

		cmd.End();

		SemaphoreHandle<kVk> acquireNextImageSemaphoreHandle = acquireNextImageSemaphore;
		graphicsCallbacks.emplace_back(
			CreateTask(
				[&executor = GetExecutor(),
				 fence = std::make_unique<Fence<kVk>>(std::move(acquireNextImageFence)),
				 acquireNextImageSemaphore = std::make_unique<Semaphore<kVk>>(std::move(acquireNextImageSemaphore))]
				 {
					ZoneScopedN("RHIApplication::Draw::waitAcquireNextImage");

					while (!fence->Wait(0ULL))
						executor.JoinOne();
				 }).handle);

		auto graphicsDoneSemaphore = Semaphore<kVk>(rhi.device, SemaphoreCreateDesc<kVk>{.type = VK_SEMAPHORE_TYPE_BINARY});
		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {graphics->semaphore, acquireNextImageSemaphoreHandle},
			.waitDstStageMasks = {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_NONE},
			.waitSemaphoreValues = {lastGraphicsSubmit.maxTimelineValue, 1},
			.signalSemaphores = {graphics->semaphore, graphicsDoneSemaphore},
			.signalSemaphoreValues = {++graphics->timeline, 1},
			.callbacks = std::move(graphicsCallbacks)});

		graphicsSubmit |= graphicsQueue.Submit();

		auto presentInfo = window.PreparePresent();
		presentInfo.waitSemaphores = {graphicsDoneSemaphore};
		computeQueue.EnqueuePresent(std::move(presentInfo));
		computeSubmit |= computeQueue.Present();
		computeSubmit.retiredSemaphores.emplace_back(std::move(graphicsDoneSemaphore));
	}

	GetExecutor().Submit(frameTasks);
}

RHIApplication::RHIApplication(
	std::string_view appName, Environment&& env, CreateWindowFunc createWindowFunc)
	: Application(std::forward<std::string_view>(appName), std::forward<Environment>(env))
	, myRHI(rhi::CreateRHI(kVk, GetName(), createWindowFunc))
{
	using namespace rhiapplication;
	
	auto& rhi = GetRHI<kVk>();
	auto& window = rhi.GetWindow(GetCurrentWindow());

	std::vector<TaskHandle> timelineCallbacks;

	// todo: create some resource global storage
	rhi.pipeline->GetResources().black = std::make_shared<Image<kVk>>(
		rhi.device,
		ImageCreateDesc<kVk>{
			.mipLevels = {ImageMipLevelDesc<kVk>{.extent = Extent2d<kVk>{4, 4}, .size = 16 * 4, .offset = 0}},
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.tiling = VK_IMAGE_TILING_LINEAR,
			.usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			.imageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.name = "Black"});

	rhi.pipeline->GetResources().blackImageView = std::make_shared<ImageView<kVk>>(
		rhi.device, *rhi.pipeline->GetResources().black, VK_IMAGE_ASPECT_COLOR_BIT);

	std::vector<SamplerCreateInfo<kVk>> samplerCreateInfos;
	samplerCreateInfos.emplace_back(SamplerCreateInfo<kVk>{
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		nullptr,
		0U,
		VK_FILTER_LINEAR,
		VK_FILTER_LINEAR,
		VK_SAMPLER_MIPMAP_MODE_LINEAR,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		0.0F,
		VK_TRUE,
		16.F,
		VK_FALSE,
		VK_COMPARE_OP_ALWAYS,
		0.0F,
		1000.0F,
		VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		VK_FALSE});
	rhi.pipeline->GetResources().samplers =
		std::make_shared<SamplerVector<kVk>>(rhi.device, std::move(samplerCreateInfos));
	//

	// initialize stuff on graphics queue
	constexpr uint32_t kTextureId = 15;
	constexpr uint32_t kSamplerId = 2;
	static_assert(kTextureId < SHADER_TYPES_GLOBAL_TEXTURE_COUNT);
	static_assert(kSamplerId < SHADER_TYPES_GLOBAL_SAMPLER_COUNT);
	{
		auto graphics = ConcurrentWriteScope(rhi.queues[kQueueTypeGraphics]);
		auto& [graphicsQueue, graphicsSubmit] = graphics->queues.Get();
		
		IMGUIInit(window, rhi, graphicsQueue);

		auto cmd = graphicsQueue.GetPool().Commands();

		rhi.pipeline->GetResources().black->Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		rhi.pipeline->GetResources().black->Clear(cmd, {.color = {{0.0F, 0.0F, 0.0F, 1.0F}}});
		rhi.pipeline->GetResources().black->Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		auto materialData = std::make_unique<MaterialData[]>(SHADER_TYPES_MATERIAL_COUNT);
		materialData[0].color[0] = 1.0;
		materialData[0].color[1] = 0.0;
		materialData[0].color[2] = 0.0;
		materialData[0].color[3] = 1.0;
		materialData[0].textureAndSamplerId =
			(kTextureId << SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS) | kSamplerId;

		TaskCreateInfo<void> materialTransfersDone;
		rhi.materials = std::make_unique<Buffer<kVk>>(
			rhi.device,
			materialTransfersDone,
			cmd,
			BufferCreateDesc<kVk>{
				.size = SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData),
				.usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				.name = "Materials"},
			materialData.get());
		timelineCallbacks.emplace_back(materialTransfersDone.handle);

		auto modelInstances = std::make_unique<ModelInstance[]>(SHADER_TYPES_MODEL_INSTANCE_COUNT);
		static const auto kIdentityMatrix = glm::mat4x4(1.0);
		std::copy_n(&kIdentityMatrix[0][0], 16, &modelInstances[666].modelTransform[0][0]);
		auto modelTransform = glm::make_mat4(&modelInstances[666].modelTransform[0][0]);
		auto inverseTransposeModelTransform = glm::transpose(glm::inverse(modelTransform));
		std::copy_n(&inverseTransposeModelTransform[0][0], 16, &modelInstances[666].inverseTransposeModelTransform[0][0]);

		TaskCreateInfo<void> modelTransfersDone;
		rhi.modelInstances = std::make_unique<Buffer<kVk>>(
			rhi.device,
			modelTransfersDone,
			cmd,
			BufferCreateDesc<kVk>{
				.size = SHADER_TYPES_MODEL_INSTANCE_COUNT * sizeof(ModelInstance),
				.usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				.name = "ModelInstances"},
			modelInstances.get());
		timelineCallbacks.emplace_back(modelTransfersDone.handle);

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline},
			.callbacks = std::move(timelineCallbacks)});

		graphicsSubmit |= graphicsQueue.Submit();
	}

	auto shaderIncludePath = std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["RootPath"]) / "src/rhi/shaders";
	auto shaderIntermediatePath = std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["UserProfilePath"]) / ".slang.intermediate";

	ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);

	auto shaderSourceFile = shaderIncludePath / "shaders.slang";

	const auto& [zPrepassShaderLayoutPairIt, zPrepassShaderLayoutWasInserted] = rhi.pipelineLayouts.emplace(
		"VertexZPrepass",
		rhi.pipeline->CreateLayout(shaderLoader.Load<kVk>(
			shaderSourceFile,
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.target = SLANG_SPIRV,
				.targetProfile = "SPIRV_1_6",
				.entryPoints = {{"VertexZPrepass", SLANG_STAGE_VERTEX}},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			})));

	rhi.pipeline->BindLayoutAuto(zPrepassShaderLayoutPairIt->second, VK_PIPELINE_BIND_POINT_GRAPHICS);

	rhi.pipeline->SetDescriptorData(
		"gModelInstances",
		DescriptorBufferInfo<kVk>{*rhi.modelInstances, 0, VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		rhi.pipeline->SetDescriptorData(
			"gViewData",
			DescriptorBufferInfo<kVk>{window.GetViewBuffer(i), 0, VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	const auto& [mainShaderLayoutPairIt, mainShaderLayoutWasInserted] = rhi.pipelineLayouts.emplace(
		"Main",
		rhi.pipeline->CreateLayout(shaderLoader.Load<kVk>(
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

	rhi.pipeline->BindLayoutAuto(mainShaderLayoutPairIt->second, VK_PIPELINE_BIND_POINT_GRAPHICS);

	rhi.pipeline->SetDescriptorData(
		"gMaterialData",
		DescriptorBufferInfo<kVk>{*rhi.materials, 0, VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MATERIAL);

	rhi.pipeline->SetDescriptorData(
		"gModelInstances",
		DescriptorBufferInfo<kVk>{*rhi.modelInstances, 0, VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	rhi.pipeline->SetDescriptorData(
		"gSamplers",
		DescriptorImageInfo<kVk>{(*rhi.pipeline->GetResources().samplers)[0]},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
		kSamplerId);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		rhi.pipeline->SetDescriptorData(
			"gViewData",
			DescriptorBufferInfo<kVk>{window.GetViewBuffer(i), 0, VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	rhi.pipeline->BindLayoutAuto(rhi.pipelineLayouts.at("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);
}

RHIApplication::~RHIApplication() noexcept(false)
{
	using namespace rhiapplication;

	ZoneScopedN("~RHIApplication()");

	auto& rhi = GetRHI<kVk>();
	
	rhi.device->WaitIdle();

	auto graphics = ConcurrentWriteScope(rhi.queues[kQueueTypeGraphics]);
	for (auto& [graphicsQueue, graphicsSubmit] : graphics->queues)	
		graphicsQueue.SubmitCallbacks(GetExecutor(), graphicsSubmit.maxTimelineValue);

	auto compute = ConcurrentWriteScope(rhi.queues[kQueueTypeCompute]);
	for (auto& [computeQueue, computeSubmit] : compute->queues)	
		computeQueue.SubmitCallbacks(GetExecutor(), computeSubmit.maxTimelineValue);

	auto transfer = ConcurrentWriteScope(rhi.queues[kQueueTypeTransfer]);
	for (auto& [transferQueue, transferSubmit] : transfer->queues)	
		transferQueue.SubmitCallbacks(GetExecutor(), transferSubmit.maxTimelineValue);

	ShutdownImgui();
}

void RHIApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhi;
	using namespace rhiapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("RHIApplication::OnResizeFramebuffer");

	auto& rhi = GetRHI<kVk>();

	rhi.device->WaitIdle();

	rhi.GetWindow(window).OnResizeFramebuffer(width, height);

	detail::ConstructWindowDependentObjects(rhi);
}

WindowState* RHIApplication::GetWindowState(WindowHandle window)
{
	using namespace rhiapplication;

	auto& rhi = GetRHI<kVk>();

	return &rhi.GetWindow(window).GetState();
}
