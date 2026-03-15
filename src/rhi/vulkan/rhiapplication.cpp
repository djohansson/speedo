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

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
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

	ImGui_ImplVulkan_NewFrame(); // calls ImGui_ImplVulkan_CreateFontsTexture
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
				Text("Unknowns: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_UNKNOWN));
				Text("Instances: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_INSTANCE));
				Text(
					"Physical Devices: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
				Text("Devices: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_DEVICE));
				Text("Queues: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_QUEUE));
				Text("Semaphores: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
				Text(
					"Command Buffers: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
				Text("Fences: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_FENCE));
				Text("Device Memory: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
				Text("Buffers: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_BUFFER));
				Text("Images: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_IMAGE));
				Text("Events: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_EVENT));
				Text("Query Pools: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
				Text("Buffer Views: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
				Text("Image Views: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
				Text(
					"Shader Modules: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
				Text(
					"Pipeline Caches: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
				Text(
					"Pipeline Layouts: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
				Text("Render Passes: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
				Text("Pipelines: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_PIPELINE));
				Text(
					"Descriptor Set Layouts: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
				Text("Samplers: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_SAMPLER));
				Text(
					"Descriptor Pools: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
				Text(
					"Descriptor Sets: %u",
					rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
				Text("Framebuffers: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
				Text("Command Pools: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
				Text("Surfaces: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
				Text("Swapchains: %u", rhi.GetDevice()->GetTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
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

	if (bool loading = RHIApplication::gShowProgress.load(std::memory_order_relaxed) &&
				 Begin(
					 "Loading",
					 &loading,
					 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
						 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
	{
		constexpr uint8_t kProgressMax = 255;
		constexpr float kProgressWindowWidth = 160.0F;
		SetWindowSize(ImVec2(kProgressWindowWidth, 0));
		ProgressBar((1.F / kProgressMax) * static_cast<float>(RHIApplication::gProgress));
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
				static const std::vector<nfdu8filteritem_t> kFilterList ={
					nfdu8filteritem_t{.name = "Wavefront OBJ", .spec = "obj"}
				};
				OpenFileDialogueAsync((resourcePath / "models").string(), kFilterList,
					[](std::string_view filePath, std::atomic_uint8_t& progressOut){
						auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
						ENSURE(app);
						auto& rhi = app->GetRHI<kVk>();
						auto& pipeline = rhi.GetPipeline();
						ENSURE(pipeline);
						auto& resources = pipeline->GetResources();
						
						auto model = std::make_shared<Model<kVk>>(model::LoadModel(rhi, filePath, progressOut, std::atomic_load(&resources.model)));

						pipeline->SetVertexInputState(*model);
						pipeline->SetDescriptorData(
							"gVertexBuffer",
							DescriptorBufferInfo<kVk>{.buffer = model->GetVertexBuffer(), .offset = 0, .range = VK_WHOLE_SIZE},
							DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

						std::atomic_store(&resources.model, model);
					});
			}
			if (MenuItem("Open Image..."))
			{
				static const std::vector<nfdu8filteritem_t> kFilterList = {
					nfdu8filteritem_t{.name = "Image files", .spec = "jpg,jpeg,png,bmp,tga,gif,psd,hdr,pic,pnm"}
				};

				OpenFileDialogueAsync((resourcePath / "images").string(), kFilterList, 
					[](std::string_view filePath, std::atomic_uint8_t& progressOut){
						auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
						ENSURE(app);
						auto& rhi = app->GetRHI<kVk>();
						auto [newImage, newImageView] = image::LoadImage<kVk>(rhi, filePath, progressOut);
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
	static imgui_extra::ImDrawDataSnapshot gSnapshot;
	if (auto *data = GetDrawData())
	{
		gSnapshot.SnapUsingSwap(data, &drawData, GetTime());
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
	auto& imguiIO = GetIO();
	imguiIO.IniFilename = nullptr;

	//imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	//imguiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	//imguiIO.FontGlobalScale = 1.0f;
	//imguiIO.FontAllowUserScaling = true;
	// auto& platformIo = ImGui::GetPlatformIO();
	// platformIo.Platform_CreateVkSurface =
	// 	(decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(*rhi.GetInstance(), "vkCreateWin32SurfaceKHR");

	LoadIniSettingsFromMemory(window.GetConfig().imguiIniSettings.c_str(), window.GetConfig().imguiIniSettings.size());

	const auto& surfaceCapabilities =
		rhi.GetInstance()->GetSwapchainInfo(
			rhi.GetDevice()->GetPhysicalDevice(),
			window.GetSurface()).capabilities;

#if defined(__OSX__)
	float dpiScaleX = 1.0F;
	float dpiScaleY = 1.0F;
#else
	float dpiScaleX = window.GetConfig().contentScale.x;
	float dpiScaleY = window.GetConfig().contentScale.y;
#endif

	imguiIO.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	GetStyle().ScaleAllSizes(std::max(dpiScaleX, dpiScaleY));

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.RasterizerDensity = std::max(window.GetConfig().contentScale.x, window.GetConfig().contentScale.y);
	config.PixelSnapH = false;

	imguiIO.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["ResourcePath"]));
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
	initInfo.Instance = *rhi.GetInstance();
	initInfo.PhysicalDevice = rhi.GetDevice()->GetPhysicalDevice();
	initInfo.Device = *rhi.GetDevice();
	initInfo.QueueFamily = graphicsQueue.GetDesc().queueFamilyIndex;
	initInfo.Queue = graphicsQueue;
	initInfo.PipelineCache = rhi.GetPipeline()->GetCache();
	initInfo.DescriptorPool = rhi.GetPipeline()->GetDescriptorPool();
	initInfo.MinImageCount = surfaceCapabilities.minImageCount;
	// initInfo.ImageCount is used to determine the number of buffers in flight inside imgui, 
	// and since we allow up to queuecount in-flight renders per frame due to triple buffering,
	// this needs to be set to the queue count accordingly.
	initInfo.ImageCount = window.GetConfig().swapchainConfig.imageCount;
	initInfo.Allocator = &rhi.GetInstance()->GetHostAllocationCallbacks();
	initInfo.CheckVkResultFn = [](VkResult result) { VK_CHECK(result); };
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

	ImGui::DestroyContext();
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

	auto& renderImageSet = rhi.GetResources().renderImageSets[newFrameIndex];

	renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
	renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
	renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
	renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
	renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);

	rhi.GetPipeline()->SetRenderTarget(renderImageSet);

	auto renderTargetInfo = renderImageSet.Begin(cmd, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

	pipeline.BindLayoutAuto(rhi.GetPipelineLayouts().at("Main"), VK_PIPELINE_BIND_POINT_GRAPHICS);

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

		constexpr uint32_t kMaxDrawThreads = 128;
		std::array<uint32_t, kMaxDrawThreads> seq;
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

					deltaX = dynamicRenderingInfo->renderInfo.renderArea.extent.width / desc.splitScreenGrid.width;
					deltaY = dynamicRenderingInfo->renderInfo.renderArea.extent.height / desc.splitScreenGrid.height;
				}
				else if (const auto* renderPassBeginInfo = std::get_if<VkRenderPassBeginInfo>(&renderTargetInfo))
				{
					inheritInfo.renderPass = renderPassBeginInfo->renderPass;
					inheritInfo.framebuffer = renderPassBeginInfo->framebuffer;

					deltaX = renderPassBeginInfo->renderArea.extent.width / desc.splitScreenGrid.width;
					deltaY = renderPassBeginInfo->renderArea.extent.height / desc.splitScreenGrid.height;
				}

				auto cmd = queue.GetPool().Commands(beginInfo);

				auto& model = *std::atomic_load(&pipeline.GetResources().model);

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

				PushConstants pushConstants{.frameIndex = frameIndex};

				ASSERT(deltaX > 0);
				ASSERT(deltaY > 0);

				while (drawIt < drawCount)
				{
					auto drawView = [&pushConstants, &pipeline, &model, &cmd, &deltaX, &deltaY, &desc](uint16_t viewIt)
					{
						ZoneScopedN("drawView");

						uint32_t col = viewIt % desc.splitScreenGrid.width;
						uint32_t row = viewIt / desc.splitScreenGrid.width;

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
	auto& imguiIO = ImGui::GetIO();

	if (imguiIO.WantSaveIniSettings)
	{
		size_t iniStringSize;
		const char* iniString = ImGui::SaveIniSettingsToMemory(&iniStringSize);
		window.GetConfig().imguiIniSettings.assign(iniString, iniStringSize);
		imguiIO.WantSaveIniSettings = false;
	}

	if (!imguiIO.WantCaptureMouse && !imguiIO.WantCaptureKeyboard)
		window.OnInputStateChanged(input);

	IMGUIPrepareDrawFunction(rhi, GetExecutor());
}

void RHIApplication::Draw()
{
	using namespace rhiapplication;

	FrameMark;
	ZoneScopedN("RHIApplication::Draw");

	std::unique_lock lock(gDrawMutex);
	std::vector<TaskHandle> frameTasks;

	auto& rhi = GetRHI<kVk>();
	auto& instance = *rhi.GetInstance();
	auto& device = *rhi.GetDevice();
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto& pipeline = *rhi.GetPipeline();
	auto& executor = GetExecutor();

	auto [acquireNextImageFence, acquireNextImageSemaphore, lastFrameIndex, newFrameIndex, flipSuccess] = window.Flip();

	bool dedicatedTransfer = ConcurrentReadScope(rhi.GetQueues()[kQueueTypeTransfer])->queueFamilyIndex != 
		ConcurrentReadScope(rhi.GetQueues()[kQueueTypeCompute])->queueFamilyIndex;

	bool dedicatedCompute = ConcurrentReadScope(rhi.GetQueues()[kQueueTypeCompute])->queueFamilyIndex != 
		ConcurrentReadScope(rhi.GetQueues()[kQueueTypeGraphics])->queueFamilyIndex;

	auto Cleanup = [&rhi, &window](auto& queueContext)
	{
		auto& device = *rhi.GetDevice();
		auto& [queue, submits] = queueContext;
		if (SupportsExtension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, device.GetPhysicalDevice()))
		{
			for (auto it = submits.presentIds.begin(); it != submits.presentIds.end();)
			{
				if (window.WaitPresent(*it, 0ULL))
					it = submits.presentIds.erase(it);
				else
					++it;
			}
		}
		for (auto it = submits.fences.begin(); it != submits.fences.end();)
		{
			if (it->Wait(0ULL))
				it = submits.fences.erase(it);
			else
				++it;
		}

		if (submits.fences.empty() && submits.presentIds.empty())
			submits = {};
	};

	// if (dedicatedCompute)
	// 	cleanup(kQueueTypeCompute);
	// if (dedicatedTransfer)
	// 	cleanup(kQueueTypeTransfer);

	if (flipSuccess)
	{
		auto& lastFrame = window.GetFrames()[lastFrameIndex];
		auto& newFrame = window.GetFrames()[newFrameIndex];

		auto graphics = ConcurrentWriteScope(rhi.GetQueues()[kQueueTypeGraphics]);
		auto& lastGraphicsContext = graphics->queues.FetchAdd();
		auto& [lastGraphicsQueue, lastGraphicsSubmits] = lastGraphicsContext;
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		auto& graphicsContext = graphics->queues.Get();
		auto& [computeQueue, computeSubmits] = dedicatedCompute ?
			ConcurrentWriteScope(rhi.GetQueues()[kQueueTypeCompute])->queues.Get() :
			graphicsContext;

		frameTasks.emplace_back(
			CreateTask([&executor, &queue = graphicsQueue, &semaphore = graphics->semaphore]
			{ queue.SubmitCallbacks(executor, semaphore.GetValue()); }).handle);

		for (auto& fence : graphicsSubmits.fences)
			fence.Wait();
		
		graphicsQueue.SwapAndResetPool();
		Cleanup(graphicsContext);
		
		TaskHandle drawCall;
		while (rhi.drawCalls.try_dequeue(drawCall))
		{
			ZoneScopedN("RHIApplication::Draw::drawCall");
			GetExecutor().Call(drawCall, graphics.Get().get());
		}
		
		auto& renderImageSet = rhi.GetResources().renderImageSets[newFrameIndex];

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

			window.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			window.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			window.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);

			pipeline.BindLayoutAuto(rhi.GetPipelineLayouts().at("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);

			rhi.GetPipeline()->SetDescriptorData(
				"gTextures",
				DescriptorImageInfo<kVk>{
					.sampler={},
					.imageView=renderImageSet.GetAttachments()[0],
					.imageLayout=renderImageSet.GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				newFrameIndex);

			rhi.GetPipeline()->SetDescriptorData(
				"gRWTextures",
				DescriptorImageInfo<kVk>{
					.sampler={},
					.imageView=window.GetAttachments()[0],
					.imageLayout=window.GetLayout(0)},
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

			constexpr uint32_t kComputeDispatchGroupsX = 16U;
			constexpr uint32_t kComputeDispatchGroupsY = 8U;
			constexpr uint32_t kComputeDispatchGroupsZ = 1U;
			vkCmdDispatch(cmd, kComputeDispatchGroupsX, kComputeDispatchGroupsY, kComputeDispatchGroupsZ);
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
			
			rhi.GetPipeline()->SetRenderTarget(newFrame);
			
			window.Begin(cmd, VK_SUBPASS_CONTENTS_INLINE);
			IMGUIDrawFunction(cmd, graphicsCallbacks);
			window.End(cmd);
		}
		{
			GPU_SCOPE(cmd, graphicsQueue, Transition);
			
			window.Transition(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		}

		cmd.End();

		//NOLINTEND(bugprone-suspicious-stringview-data-usage)

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

		auto graphicsDoneSemaphore = Semaphore<kVk>(rhi.GetDevice(), SemaphoreCreateDesc<kVk>{.type = VK_SEMAPHORE_TYPE_BINARY});
		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {graphics->semaphore, acquireNextImageSemaphoreHandle},
			.waitDstStageMasks = {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_NONE},
			.waitSemaphoreValues = {lastGraphicsSubmits.maxTimelineValue, 1},
			.signalSemaphores = {graphics->semaphore, graphicsDoneSemaphore},
			.signalSemaphoreValues = {++graphics->timeline, 1},
			.callbacks = std::move(graphicsCallbacks)});

		graphicsSubmits |= graphicsQueue.Submit();

		auto presentInfo = window.PreparePresent();
		presentInfo.waitSemaphores = {graphicsDoneSemaphore};
		computeQueue.EnqueuePresent(std::move(presentInfo));
		computeSubmits |= computeQueue.Present();
		computeSubmits.retiredSemaphores.emplace_back(std::move(graphicsDoneSemaphore));
	}

	GetExecutor().Submit(frameTasks);
}

RHIApplication::RHIApplication(
	std::string_view appName, Environment&& env, CreateWindowFunc createWindowFunc)
	: Application(std::forward<std::string_view>(appName), std::forward<Environment>(env))
	, myRHI(std::make_unique<RHI<kVk>>(appName, createWindowFunc))
{
	using namespace rhiapplication;
	
	auto& rhi = GetRHI<kVk>();
	auto& window = rhi.GetWindow(GetCurrentWindow());

	std::vector<TaskHandle> timelineCallbacks;

	// todo: create some resource global storage
	constexpr uint32_t kBlackTextureWidth = 4;
	constexpr uint32_t kBlackTextureHeight = 4;
	constexpr uint32_t kBlackTextureSize = kBlackTextureWidth * kBlackTextureHeight * 4;
	rhi.GetPipeline()->GetResources().black = std::make_shared<Image<kVk>>(
		rhi.GetDevice(),
		ImageCreateDesc<kVk>{
			.mipLevels = {ImageMipLevelDesc<kVk>{.extent = Extent2d<kVk>{.width=kBlackTextureWidth, .height=kBlackTextureHeight}, .size = kBlackTextureSize, .offset = 0}},
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.tiling = VK_IMAGE_TILING_LINEAR,
			.usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			.imageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.name = "Black"});

	rhi.GetPipeline()->GetResources().blackImageView = std::make_shared<ImageView<kVk>>(
		rhi.GetDevice(), *rhi.GetPipeline()->GetResources().black, VK_IMAGE_ASPECT_COLOR_BIT);

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
	rhi.GetPipeline()->GetResources().samplers =
		std::make_shared<SamplerVector<kVk>>(rhi.GetDevice(), std::move(samplerCreateInfos));
	//

	// initialize stuff on graphics queue
	constexpr uint32_t kTextureId = 15;
	constexpr uint32_t kSamplerId = 2;
	static_assert(kTextureId < SHADER_TYPES_GLOBAL_TEXTURE_COUNT);
	static_assert(kSamplerId < SHADER_TYPES_GLOBAL_SAMPLER_COUNT);
	{
		auto graphics = ConcurrentWriteScope(rhi.GetQueues()[kQueueTypeGraphics]);
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		
		IMGUIInit(window, rhi, graphicsQueue);

		auto cmd = graphicsQueue.GetPool().Commands();

		rhi.GetPipeline()->GetResources().black->Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		rhi.GetPipeline()->GetResources().black->Clear(cmd, {.color = {{0.0F, 0.0F, 0.0F, 1.0F}}});
		rhi.GetPipeline()->GetResources().black->Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		std::vector<MaterialData> materialData(SHADER_TYPES_MATERIAL_COUNT);
		materialData[0].color[0] = 1.0;
		materialData[0].color[1] = 0.0;
		materialData[0].color[2] = 0.0;
		materialData[0].color[3] = 1.0;
		materialData[0].textureAndSamplerId =
			(kTextureId << SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS) | kSamplerId;

		TaskCreateInfo<void> materialTransfersDone;
		rhi.GetResources().materials = std::make_unique<Buffer<kVk>>(
			rhi.GetDevice(),
			materialTransfersDone,
			cmd,
			BufferCreateDesc<kVk>{
				.size = SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData),
				.usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				.name = "Materials"},
			materialData.data());
		timelineCallbacks.emplace_back(materialTransfersDone.handle);

		constexpr uint32_t kDefaultModelInstanceId = 666;
		constexpr uint32_t kMatrix4x4ElementCount = 16;
		std::vector<ModelInstance> modelInstances(SHADER_TYPES_MODEL_INSTANCE_COUNT);
		static const auto kIdentityMatrix = glm::mat4x4(1.0);
		std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[kDefaultModelInstanceId].modelTransform[0][0]);
		auto modelTransform = glm::make_mat4(&modelInstances[kDefaultModelInstanceId].modelTransform[0][0]);
		auto inverseTransposeModelTransform = glm::transpose(glm::inverse(modelTransform));
		std::copy_n(&inverseTransposeModelTransform[0][0], kMatrix4x4ElementCount, &modelInstances[kDefaultModelInstanceId].inverseTransposeModelTransform[0][0]);

		TaskCreateInfo<void> modelTransfersDone;
		rhi.GetResources().modelInstances = std::make_unique<Buffer<kVk>>(
			rhi.GetDevice(),
			modelTransfersDone,
			cmd,
			BufferCreateDesc<kVk>{
				.size = SHADER_TYPES_MODEL_INSTANCE_COUNT * sizeof(ModelInstance),
				.usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				.name = "ModelInstances"},
			modelInstances.data());
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

	auto shaderIncludePath = std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["RootPath"]) / "src/rhi/shaders";
	auto shaderIntermediatePath = std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["UserProfilePath"]) / ".slang.intermediate";

	ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);

	auto shaderSourceFile = shaderIncludePath / "shaders.slang";

	const auto& [zPrepassShaderLayoutPairIt, zPrepassShaderLayoutWasInserted] = rhi.GetPipelineLayouts().emplace(
		"VertexZPrepass",
		rhi.GetPipeline()->CreateLayout(shaderLoader.Load<kVk>(
			shaderSourceFile,
			{
				.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
				.target = SLANG_SPIRV,
				.targetProfile = "SPIRV_1_6",
				.entryPoints = {{"VertexZPrepass", SLANG_STAGE_VERTEX}},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			})));

	rhi.GetPipeline()->BindLayoutAuto(zPrepassShaderLayoutPairIt->second, VK_PIPELINE_BIND_POINT_GRAPHICS);

	rhi.GetPipeline()->SetDescriptorData(
		"gModelInstances",
		DescriptorBufferInfo<kVk>{.buffer = *rhi.GetResources().modelInstances, .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		rhi.GetPipeline()->SetDescriptorData(
			"gViewData",
			DescriptorBufferInfo<kVk>{.buffer = window.GetViewBuffer(i), .offset = 0, .range = VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	const auto& [mainShaderLayoutPairIt, mainShaderLayoutWasInserted] = rhi.GetPipelineLayouts().emplace(
		"Main",
		rhi.GetPipeline()->CreateLayout(shaderLoader.Load<kVk>(
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

	rhi.GetPipeline()->BindLayoutAuto(mainShaderLayoutPairIt->second, VK_PIPELINE_BIND_POINT_GRAPHICS);

	rhi.GetPipeline()->SetDescriptorData(
		"gMaterialData",
		DescriptorBufferInfo<kVk>{.buffer = *rhi.GetResources().materials, .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MATERIAL);

	rhi.GetPipeline()->SetDescriptorData(
		"gModelInstances",
		DescriptorBufferInfo<kVk>{.buffer = *rhi.GetResources().modelInstances, .offset = 0, .range = VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

	rhi.GetPipeline()->SetDescriptorData(
		"gSamplers",
		DescriptorImageInfo<kVk>{.sampler=(*rhi.GetPipeline()->GetResources().samplers)[0]},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
		kSamplerId);

	for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
	{
		rhi.GetPipeline()->SetDescriptorData(
			"gViewData",
			DescriptorBufferInfo<kVk>{.buffer = window.GetViewBuffer(i), .offset = 0, .range = VK_WHOLE_SIZE},
			DESCRIPTOR_SET_CATEGORY_VIEW,
			i);
	}

	rhi.GetPipeline()->BindLayoutAuto(rhi.GetPipelineLayouts().at("Main"), VK_PIPELINE_BIND_POINT_COMPUTE);
}

RHIApplication::~RHIApplication() noexcept(false)
{
	using namespace rhiapplication;

	ZoneScopedN("~RHIApplication()");

	auto& rhi = GetRHI<kVk>();
	
	rhi.GetDevice()->WaitIdle();

	auto graphics = ConcurrentWriteScope(rhi.GetQueues()[kQueueTypeGraphics]);
	for (auto& [graphicsQueue, graphicsSubmits] : graphics->queues)	
		graphicsQueue.SubmitCallbacks(GetExecutor(), graphicsSubmits.maxTimelineValue);

	auto compute = ConcurrentWriteScope(rhi.GetQueues()[kQueueTypeCompute]);
	for (auto& [computeQueue, computeSubmits] : compute->queues)	
		computeQueue.SubmitCallbacks(GetExecutor(), computeSubmits.maxTimelineValue);

	auto transfer = ConcurrentWriteScope(rhi.GetQueues()[kQueueTypeTransfer]);
	for (auto& [transferQueue, transferSubmits] : transfer->queues)	
		transferQueue.SubmitCallbacks(GetExecutor(), transferSubmits.maxTimelineValue);

	ShutdownImgui();
}

void RHIApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhi;
	using namespace rhiapplication;

	std::unique_lock lock(gDrawMutex);

	ZoneScopedN("RHIApplication::OnResizeFramebuffer");

	auto& rhi = GetRHI<kVk>();

	rhi.GetDevice()->WaitIdle();

	rhi.GetWindow(window).OnResizeFramebuffer(width, height);

	detail::ConstructWindowDependentObjects(rhi);
}

WindowState* RHIApplication::GetWindowState(WindowHandle window)
{
	using namespace rhiapplication;

	auto& rhi = GetRHI<kVk>();

	return &rhi.GetWindow(window).GetState();
}
