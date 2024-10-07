#include "../rhi.h"
#include "../rhiapplication.h"
#include "utils.h"

#include <gfx/scene.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

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

void IMGUIPrepareDrawFunction(RHI<kVk>& rhi, TaskExecutor& executor)
{
	ZoneScopedN("RHIApplication::IMGUIPrepareDraw");

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
						CHECK(app);
						auto& pipeline = app->GetRHI<kVk>().pipeline;
						CHECK(pipeline);
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
						auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
						CHECK(app);
						auto& pipeline = app->GetRHI<kVk>().pipeline;
						CHECK(pipeline);
						auto& resources = pipeline->GetResources();
						auto [newImage, newImageView] = image::LoadImage(
							filePath,
							progressOut,
							std::atomic_load(&resources.image),
							std::atomic_load(&resources.imageView));
						std::atomic_store(
							&resources.image,
							std::make_shared<::Image<kVk>>(std::move(newImage))); // todo: move Image into rhi namespace
						std::atomic_store(
							&resources.imageView,
							std::make_shared<ImageView<kVk>>(std::move(newImageView)));
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
				Extent2d<kVk> splitScreenGrid = rhi.windows.at(GetCurrentWindow()).GetConfig().splitScreenGrid;

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
					rhi.windows.at(GetCurrentWindow()).OnResizeSplitScreenGrid(splitScreenGrid.width, splitScreenGrid.height);
			}
#if (GRAPHICS_VALIDATION_LEVEL > 0)
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
}

void IMGUIDrawFunction(CommandBufferHandle<kVk> cmd, PipelineHandle<kVk> pipeline = nullptr)
{
	ZoneScopedN("RHIApplication::IMGUIDraw");

	using namespace ImGui;

	ImGui_ImplVulkan_RenderDrawData(GetDrawData(), cmd, pipeline);
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
	initInfo.ImageCount = window.GetConfig().swapchainConfig.imageCount;
	initInfo.Allocator = &rhi.device->GetInstance()->GetHostAllocationCallbacks();
	initInfo.CheckVkResultFn = [](VkResult result) { VK_CHECK(result); };
	initInfo.UseDynamicRendering = window.GetConfig().swapchainConfig.useDynamicRendering;
	initInfo.RenderPass = initInfo.UseDynamicRendering ? VK_NULL_HANDLE : static_cast<RenderTargetHandle<kVk>>(window.GetFrames()[0]).first;
	initInfo.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
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

void RHIApplication::InternalUpdateInput()
{
	using namespace rhiapplication;

	auto& rhi = GetRHI<kVk>();
	auto& imguiIO = ImGui::GetIO();
	auto& input = myInput;

	MouseEvent mouse;
	while (myMouseQueue.try_dequeue(mouse))
	{
		if ((mouse.flags & MouseEvent::kPosition) != 0)
		{
			input.mouse.position[0] = static_cast<float>(mouse.xpos);
			input.mouse.position[1] = static_cast<float>(mouse.ypos);
			input.mouse.insideWindow = mouse.insideWindow;

			imguiIO.AddMousePosEvent(input.mouse.position[0], input.mouse.position[1]);
		}

		if ((mouse.flags & MouseEvent::kButton) != 0)
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
		rhi.windows.at(GetCurrentWindow()).OnInputStateChanged(input);
}

void RHIApplication::InternalDraw()
{
	using namespace rhiapplication;

	std::unique_lock lock(gDrawMutex);

	auto& rhi = GetRHI<kVk>();

	FrameMark;
	ZoneScopedN("rhi::draw");

	ImGui_ImplVulkan_NewFrame(); // no-op?

	auto& instance = *rhi.instance;
	auto& device = *rhi.device;
	auto& window = rhi.windows.at(GetCurrentWindow());
	auto& pipeline = *rhi.pipeline;

	TaskCreateInfo<void> IMGUIPrepareDraw;
	IMGUIPrepareDraw = CreateTask(IMGUIPrepareDrawFunction, rhi, GetExecutor());
	GetExecutor().Submit({&IMGUIPrepareDraw.handle, 1});

	{
		ZoneScopedN("rhi::draw::processGraphics");

		auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
		auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
		for (auto& [graphicsQueue, graphicsSubmit] : *graphicsQueueInfosWriteScope)	
			graphicsQueue.SubmitCallbacks(GetExecutor(), graphicsSemaphore.GetValue());
	}

	{
		ZoneScopedN("rhi::draw::processCompute");

		auto& [computeSemaphore, computeSemaphoreValue, computeQueueInfos] = rhi.queues[kQueueTypeCompute];
		auto computeQueueInfosWriteScope = ConcurrentWriteScope(computeQueueInfos);
		for (auto& [computeQueue, computeSubmit] : *computeQueueInfosWriteScope)	
			computeQueue.SubmitCallbacks(GetExecutor(), computeSemaphore.GetValue());
	}

	{
		ZoneScopedN("rhi::draw::processTransfers");

		auto& [transferSemaphore, transferSemaphoreValue, transferQueueInfos] = rhi.queues[kQueueTypeTransfer];
		auto transferQueueInfosWriteScope = ConcurrentWriteScope(transferQueueInfos);
		for (auto& [transferQueue, transferSubmit] : *transferQueueInfosWriteScope)	
			transferQueue.SubmitCallbacks(GetExecutor(), transferSemaphore.GetValue());
	}
	
	auto [flipSuccess, lastFrameIndex, newFrameIndex] = window.Flip();

	if (flipSuccess)
	{
		ZoneScopedN("rhi::draw::submit");

		static uint8_t gGraphicsQueueIndex = 0;
		static uint8_t gComputeQueueIndex = 0;
		auto graphicsQueueIndex = gGraphicsQueueIndex++;
		auto computeQueueIndex = gComputeQueueIndex++;

		{
			ZoneScopedN("rhi::draw::waitGraphics");

			auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
			auto graphicsQueueInfosReadScope = ConcurrentReadScope(graphicsQueueInfos);
			graphicsQueueIndex %= graphicsQueueInfosReadScope->size();
			auto& [graphicsQueue, graphicsSubmit] = (*graphicsQueueInfosReadScope)[graphicsQueueIndex];
			
			graphicsQueue.WaitIdle();
		}

		auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
		auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
		auto& [graphicsQueue, graphicsSubmit] = (*graphicsQueueInfosWriteScope)[graphicsQueueIndex];
		
		graphicsQueue.GetPool().Reset();

		auto& [computeSemaphore, computeSemaphoreValue, computeQueueInfos] = rhi.queues[kQueueTypeCompute];
		auto computeQueueInfosWriteScope = ConcurrentWriteScope(computeQueueInfos);
		computeQueueIndex %= computeQueueInfosWriteScope->size();
		auto& [computeQueue, computeSubmit] = (*computeQueueInfosWriteScope)[computeQueueIndex];
		
		auto& newFrame = window.GetFrames()[newFrameIndex];

		TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("rhi::draw::drawCall");

			GetExecutor().Call(drawCall, newFrameIndex);
		}

		window.UpdateViewBuffer(); // todo: move to drawCall

		static constexpr VkClearValue clearValues[] = {
			{.color = {0.2F, 0.2F, 0.2F, 1.0F}},
			{.depthStencil = {1.0F, 0}}};

		auto cmd = graphicsQueue.GetPool().Commands();
		auto& renderImageSet = *rhi.renderImageSet;

		GPU_SCOPE_COLLECT(cmd, graphicsQueue);
		
		{
			GPU_SCOPE(cmd, graphicsQueue, draw);

			pipeline.BindLayoutAuto(rhi.pipelineLayouts.at("Main"), VK_PIPELINE_BIND_POINT_GRAPHICS);

			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, renderImageSet.GetAttachments().size() - 1);
			
			auto renderInfo = renderImageSet.Begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS, clearValues);

			// setup draw parameters
			uint32_t drawCount = window.GetConfig().splitScreenGrid.width * window.GetConfig().splitScreenGrid.height;
			uint32_t drawThreadCount = 0;

			std::atomic_uint32_t drawAtomic = 0UL;

			// draw views using secondary command buffers
			// todo: generalize this to other types of draws
			if (std::atomic_load(&pipeline.GetResources().model))
			{
				ZoneScopedN("rhi::draw::drawViews");

				drawThreadCount = std::min<uint32_t>(drawCount, graphicsQueue.GetPool().GetDesc().levelCount);

				std::array<uint32_t, 128> seq;
				std::iota(seq.begin(), seq.begin() + drawThreadCount, 0);
				std::for_each_n(
					seq.begin(),
					drawThreadCount,
					[&pipeline,
					&queue = graphicsQueue,
					&renderInfo,
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

						CommandBufferInheritanceInfo<kVk> inheritInfo{
							VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};

						CommandBufferAccessScopeDesc<kVk> beginInfo{};
						beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
										VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
						beginInfo.pInheritanceInfo = &inheritInfo;
						beginInfo.level = threadIt + 1;

						uint32_t dx = 0;
						uint32_t dy = 0;

						if (const auto* renderPassBeginInfo = std::get_if<VkRenderPassBeginInfo>(&renderInfo))
						{
							inheritInfo.renderPass = renderPassBeginInfo->renderPass;
							inheritInfo.framebuffer = renderPassBeginInfo->framebuffer;

							dx = renderPassBeginInfo->renderArea.extent.width / desc.splitScreenGrid.width;
							dy = renderPassBeginInfo->renderArea.extent.height / desc.splitScreenGrid.height;
						}
						else
						{
							// todo: dynamic rendering
							CHECK(false);
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
									scissor.offset = {x, y};
									scissor.extent = {width, height};

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
				graphicsQueue.Execute(threadIt, ++graphicsSemaphoreValue);

			renderImageSet.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, copy);

			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
			window.Copy(
				cmd,
				renderImageSet,
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				0,
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				0);
		}
		// {
		// 	GPU_SCOPE(cmd, graphicsQueue, blit);

		// 	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
		// 	window.Blit(
		// 		cmd,
		// 		renderImageSet,
		// 		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		// 		0,
		// 		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		// 		0,
		// 		VK_FILTER_NEAREST);
		// }
		{
			GPU_SCOPE(cmd, graphicsQueue, imgui);

			window.SetLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD, 0);
			window.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			window.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
			
			window.Begin(cmd, VK_SUBPASS_CONTENTS_INLINE, {});

			GetExecutor().Join(std::move(IMGUIPrepareDraw.future));
			IMGUIDrawFunction(cmd);

			window.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, Transition);
			
			window.Transition(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);
		}

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			{graphicsSemaphore},
			{VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
			{graphicsSubmit.maxTimelineValue},
			{graphicsSemaphore, newFrame.GetSemaphore()},
			{++graphicsSemaphoreValue, 0},
			{}});

		computeSubmit = graphicsSubmit = graphicsQueue.Submit();
		computeQueue.EnqueuePresent(window.PreparePresent(std::move(computeSubmit)));
		computeSubmit = computeQueue.Present();
	}
}

RHIApplication::RHIApplication(
	std::string_view appName, Environment&& env, CreateWindowFunc createWindowFunc)
	: Application(std::forward<std::string_view>(appName), std::forward<Environment>(env))
	, myRHI(rhi::CreateRHI(kVk, GetName(), createWindowFunc))
{
	using namespace rhiapplication;
	
	auto& rhi = GetRHI<kVk>();

	std::vector<TaskHandle> timelineCallbacks;

	// todo: create some resource global storage
	rhi.pipeline->GetResources().black = std::make_shared<Image<kVk>>(
		rhi.device,
		ImageCreateDesc<kVk>{
			{ImageMipLevelDesc<kVk>{Extent2d<kVk>{4, 4}, 16 * 4, 0}},
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_TILING_LINEAR,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED,
			"Black"});

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
	constexpr uint32_t kTextureId = 1;
	constexpr uint32_t kSamplerId = 2;
	static_assert(kTextureId < SHADER_TYPES_GLOBAL_TEXTURE_COUNT);
	static_assert(kSamplerId < SHADER_TYPES_GLOBAL_SAMPLER_COUNT);
	{
		auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
		auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
		auto& [graphicsQueue, graphicsSubmit] = graphicsQueueInfosWriteScope->front();
		
		IMGUIInit(rhi.windows.at(GetCurrentWindow()), rhi, graphicsQueue);

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
				SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				"Materials"},
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
				SHADER_TYPES_MODEL_INSTANCE_COUNT * sizeof(ModelInstance),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				"ModelInstances"},
			modelInstances.get());
		timelineCallbacks.emplace_back(modelTransfersDone.handle);

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			{},
			{},
			{},
			{graphicsSemaphore},
			{++graphicsSemaphoreValue},
			std::move(timelineCallbacks)});

		graphicsSubmit = graphicsQueue.Submit();
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
				SLANG_SOURCE_LANGUAGE_SLANG,
				SLANG_SPIRV,
				"SPIRV_1_6",
				{{"VertexZPrepass", SLANG_STAGE_VERTEX}},
				SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
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
			DescriptorBufferInfo<kVk>{rhi.windows.at(GetCurrentWindow()).GetViewBuffer(i), 0, VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	const auto& [mainShaderLayoutPairIt, mainShaderLayoutWasInserted] = rhi.pipelineLayouts.emplace(
		"Main",
		rhi.pipeline->CreateLayout(shaderLoader.Load<kVk>(
			shaderSourceFile,
			{
				SLANG_SOURCE_LANGUAGE_SLANG,
				SLANG_SPIRV,
				"SPIRV_1_6",
				{
					{"VertexMain", SLANG_STAGE_VERTEX},
					{"FragmentMain", SLANG_STAGE_FRAGMENT},
					{"ComputeMain", SLANG_STAGE_COMPUTE},
				},
				SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
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
			DescriptorBufferInfo<kVk>{rhi.windows.at(GetCurrentWindow()).GetViewBuffer(i), 0, VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}
}

RHIApplication::~RHIApplication() noexcept(false)
{
	using namespace rhiapplication;

	ZoneScopedN("~RHIApplication()");

	auto& rhi = GetRHI<kVk>();
	auto device = rhi.device;
	auto instance = rhi.instance;

	auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
	auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
	for (auto& [graphicsQueue, graphicsSubmit] : *graphicsQueueInfosWriteScope)	
	{
		ZoneScopedN("~RHIApplication()::waitGraphics");

		graphicsQueue.WaitIdle();
		graphicsQueue.SubmitCallbacks(GetExecutor(), graphicsSubmit.maxTimelineValue);
	}

	auto& [computeSemaphore, computeSemaphoreValue, computeQueueInfos] = rhi.queues[kQueueTypeCompute];
	auto computeQueueInfosWriteScope = ConcurrentWriteScope(computeQueueInfos);
	for (auto& [computeQueue, computeSubmit] : *computeQueueInfosWriteScope)	
	{
		ZoneScopedN("~RHIApplication()::waitCompute");

		computeQueue.WaitIdle();
		computeQueue.SubmitCallbacks(GetExecutor(), computeSubmit.maxTimelineValue);
	}

	auto& [transferSemaphore, transferSemaphoreValue, transferQueueInfos] = rhi.queues[kQueueTypeTransfer];
	auto transferQueueInfosWriteScope = ConcurrentWriteScope(transferQueueInfos);
	for (auto& [transferQueue, transferSubmit] : *transferQueueInfosWriteScope)	
	{
		ZoneScopedN("~RHIApplication()::waitTransfer");

		transferQueue.WaitIdle();
		transferQueue.SubmitCallbacks(GetExecutor(), transferSubmit.maxTimelineValue);
	}

	ShutdownImgui();

	myRHI.reset();

	ASSERT(device.use_count() == 1);
	ASSERT(instance.use_count() == 2);
}

void RHIApplication::Tick()
{
	using namespace rhiapplication;

	ZoneScopedN("RHIApplication::tick");

	auto& rhi = GetRHI<kVk>();
	
	TaskHandle mainCall;
	while (rhi.mainCalls.try_dequeue(mainCall))
	{
		ZoneScopedN("RHIApplication::tick::mainCall");

		GetExecutor().Call(mainCall);
	}

	ImGui_ImplGlfw_NewFrame(); // will poll glfw input events and update input state
}

void RHIApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhi;
	using namespace rhiapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("RHIApplication::OnResizeFramebuffer");

	auto& rhi = GetRHI<kVk>();

	auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
	auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
	for (auto& [graphicsQueue, graphicsSubmit] : *graphicsQueueInfosWriteScope)	
	{
		ZoneScopedN("RHIApplication::OnResizeFramebuffer::waitGraphics");

		graphicsQueue.WaitIdle();
	}

	rhi.windows.at(window).OnResizeFramebuffer(width, height);

	detail::ConstructWindowDependentObjects(rhi);
}

WindowState* RHIApplication::GetWindowState(WindowHandle window)
{
	using namespace rhiapplication;

	auto& rhi = GetRHI<kVk>();

	return &rhi.windows.at(window).GetState();
}
