#include "../command.h"
#include "../device.h"
#include "../instance.h"
#include "../pipeline.h"
#include "../renderimageset.h"
#include "../rhiapplication.h"
#include "../types.h"
#include "../window.h"

#include "../shaders/shadertypes.h"

template <GraphicsApi G>
struct Rhi
{
	std::shared_ptr<Instance<G>> instance;
	std::shared_ptr<Device<G>> device;

	std::shared_ptr<Window<G>> mainWindow;
	std::shared_ptr<Pipeline<G>> pipeline;

	CircularContainer<Queue<G>> graphicsQueues;
	CircularContainer<Queue<G>> computeQueues;
	CircularContainer<Queue<G>> transferQueues;

	std::array<CircularContainer<CommandPoolContext<G>>, CommandType_Count> commands;

	//std::shared_ptr<ResourceContext<G>> resources;

	std::shared_ptr<RenderImageSet<G>> renderImageSet;

	std::shared_ptr<Buffer<G>> materials;
	std::shared_ptr<Buffer<G>> objects;

	Future<void> presentFuture;
	std::function<void()> IMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<Vk> cmd)> IMGUIDrawFunction; // todo: remove GraphicsApi specialization
};

#include "utils.h"

#include <core/application.h>
// #include <core/gltfstream.h>
// #include <core/nodes/inputoutputnode.h>
// #include <core/nodes/slangshadernode.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

//#include <imnodes.h>

namespace rhiapplication
{

static void initIMGUI(
	const std::filesystem::path& resourcePath,
	const std::filesystem::path& userProfilePath,
	const WindowState& window,
	const Rhi<Vk>& rhi,
	CommandBufferHandle<Vk> cmd)
{
	ZoneScopedN("RhiApplication::initIMGUI");

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
		(decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(*rhi.instance, "vkCreateWin32SurfaceKHR");

	const auto& surfaceCapabilities =
		rhi.instance->getSwapchainInfo(rhi.device->getPhysicalDevice(), rhi.mainWindow->getSurface()).capabilities;

	float dpiScaleX = static_cast<float>(surfaceCapabilities.currentExtent.width) /
					  rhi.mainWindow->getConfig().windowExtent.width;
	float dpiScaleY = static_cast<float>(surfaceCapabilities.currentExtent.height) /
					  rhi.mainWindow->getConfig().windowExtent.height;

	io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.PixelSnapH = false;

	io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	std::filesystem::path fontPath(resourcePath);
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
	initInfo.Instance = *rhi.instance;
	initInfo.PhysicalDevice = rhi.device->getPhysicalDevice();
	initInfo.Device = *rhi.device;
	initInfo.QueueFamily = rhi.graphicsQueues.get().getDesc().queueFamilyIndex;
	initInfo.Queue = rhi.graphicsQueues.get();
	initInfo.PipelineCache = rhi.pipeline->getCache();
	initInfo.DescriptorPool = rhi.pipeline->getDescriptorPool();
	initInfo.MinImageCount = rhi.mainWindow->getConfig().swapchainConfig.imageCount;
	initInfo.ImageCount = rhi.mainWindow->getConfig().swapchainConfig.imageCount;
	initInfo.Allocator = &rhi.device->getInstance()->getHostAllocationCallbacks();
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
	ImGui_ImplVulkan_Init(&initInfo, rhi.mainWindow->getRenderPass());
	ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window.handle), true);

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

static void createWindowDependentObjects(Rhi<Vk>& rhi)
{
	ZoneScopedN("RhiApplication::createWindowDependentObjects");

	auto colorImage = std::make_shared<Image<Vk>>(
		rhi.device,
		ImageCreateDesc<Vk>{
			{{rhi.mainWindow->getConfig().swapchainConfig.extent}},
			rhi.mainWindow->getConfig().swapchainConfig.surfaceFormat.format,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	auto depthStencilImage = std::make_shared<Image<Vk>>(
		rhi.device,
		ImageCreateDesc<Vk>{
			{{rhi.mainWindow->getConfig().swapchainConfig.extent}},
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

} // namespace rhiapplication

template <>
Rhi<Vk>& RhiApplication::rhi<Vk>()
{
	return *std::static_pointer_cast<Rhi<Vk>>(myRhi);
}

template <>
const Rhi<Vk>& RhiApplication::rhi<Vk>() const
{
	return *std::static_pointer_cast<Rhi<Vk>>(myRhi);
}

RhiApplication::RhiApplication(std::string_view appName, Environment&& env)
: Application(std::forward<std::string_view>(appName), std::forward<Environment>(env))
, myRhi(std::make_shared<Rhi<Vk>>(
		std::make_shared<Instance<Vk>>(
			InstanceConfiguration<Vk>{
				name(),
				"speedo",
				ApplicationInfo<Vk>{
					VK_STRUCTURE_TYPE_APPLICATION_INFO,
					nullptr,
					nullptr,
					VK_MAKE_VERSION(1, 0, 0),
					nullptr,
					VK_MAKE_VERSION(1, 0, 0),
					VK_API_VERSION_1_2}})))
{
	ZoneScopedN("RhiApplication()");

	auto rootPath = std::get<std::filesystem::path>(environment().variables["RootPath"]);
	auto resourcePath = std::get<std::filesystem::path>(environment().variables["ResourcePath"]);
	auto userProfilePath = std::get<std::filesystem::path>(environment().variables["UserProfilePath"]);

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
			rhi<Vk>().commands[CommandType_DedicatedTransfer].fetchAdd();

		auto& transferQueue = rhi<Vk>().transferQueues.fetchAdd();

		rhi<Vk>().device->wait(transferQueue.getLastSubmitTimelineValue().value_or(0));

		dedicatedTransferContext.reset();

		rhi<Vk>().pipeline->setModel(
			std::make_shared<Model<Vk>>(rhi<Vk>().device, dedicatedTransferContext, openFilePath));

		transferQueue.enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {rhi<Vk>().device->getTimelineSemaphore()},
			 {1 + rhi<Vk>().device->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		return transferQueue.submit();
	};

	auto loadImage = [this](nfdchar_t* openFilePath)
	{
		auto& dedicatedTransferContext =
			rhi<Vk>().commands[CommandType_DedicatedTransfer].fetchAdd();

		auto& transferQueue = rhi<Vk>().transferQueues.fetchAdd();

		rhi<Vk>().device->wait(transferQueue.getLastSubmitTimelineValue().value_or(0));

		dedicatedTransferContext.reset();

		rhi<Vk>().pipeline->resources().image =
			std::make_shared<Image<Vk>>(rhi<Vk>().device, dedicatedTransferContext, openFilePath);
		rhi<Vk>().pipeline->resources().imageView = std::make_shared<ImageView<Vk>>(
			rhi<Vk>().device, *rhi<Vk>().pipeline->resources().image, VK_IMAGE_ASPECT_COLOR_BIT);

		transferQueue.enqueueSubmit(dedicatedTransferContext.prepareSubmit(
			{{},
			 {},
			 {},
			 {rhi<Vk>().device->getTimelineSemaphore()},
			 {1 + rhi<Vk>().device->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		transferQueue.submit();

		///////////

		auto& generalTransferContext = rhi<Vk>().commands[CommandType_GeneralTransfer].fetchAdd();
		auto cmd = generalTransferContext.commands();

		auto& graphicsQueue = rhi<Vk>().graphicsQueues.get();

		rhi<Vk>().pipeline->resources().image->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		cmd.end();

		graphicsQueue.enqueueSubmit(generalTransferContext.prepareSubmit(
			{{rhi<Vk>().device->getTimelineSemaphore()},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
			 {transferQueue.getLastSubmitTimelineValue().value_or(0)},
			 {rhi<Vk>().device->getTimelineSemaphore()},
			 {1 + rhi<Vk>().device->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		rhi<Vk>().pipeline->setDescriptorData(
			"g_textures",
			DescriptorImageInfo<Vk>{
				{},
				*rhi<Vk>().pipeline->resources().imageView,
				rhi<Vk>().pipeline->resources().image->getImageLayout()},
			DescriptorSetCategory_GlobalTextures,
			1);

		return graphicsQueue.submit();
	};

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

	rhi<Vk>().IMGUIPrepareDrawFunction =
		[this, openFileDialogue, loadModel, loadImage/*, loadGlTF*/, resourcePath]
	{
		ZoneScopedN("RhiApplication::IMGUIPrepareDraw");

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
					Text("Unknowns: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_UNKNOWN));
					Text("Instances: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_INSTANCE));
					Text(
						"Physical Devices: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
					Text("Devices: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_DEVICE));
					Text("Queues: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_QUEUE));
					Text("Semaphores: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
					Text(
						"Command Buffers: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
					Text("Fences: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_FENCE));
					Text("Device Memory: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
					Text("Buffers: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_BUFFER));
					Text("Images: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_IMAGE));
					Text("Events: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_EVENT));
					Text("Query Pools: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
					Text("Buffer Views: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
					Text("Image Views: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
					Text(
						"Shader Modules: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
					Text(
						"Pipeline Caches: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
					Text(
						"Pipeline Layouts: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
					Text("Render Passes: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
					Text("Pipelines: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_PIPELINE));
					Text(
						"Descriptor Set Layouts: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
					Text("Samplers: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_SAMPLER));
					Text(
						"Descriptor Pools: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
					Text(
						"Descriptor Sets: %u",
						rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
					Text("Framebuffers: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
					Text("Command Pools: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
					Text("Surfaces: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
					Text("Swapchains: %u", rhi<Vk>().device->getTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
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
				if (MenuItem("Open OBJ...") && !myOpenFileFuture.valid())
				{
					TaskGraph graph;
					auto [task, openFileFuture] = graph.createTask(
						[&openFileDialogue, &resourcePath, &loadModel]
						{ return openFileDialogue(resourcePath, "obj", loadModel); });
					executor().submit(std::move(graph));
					myOpenFileFuture = std::move(openFileFuture);
				}
				if (MenuItem("Open Image...") && !myOpenFileFuture.valid())
				{
					TaskGraph graph;
					auto [task, openFileFuture] = graph.createTask(
						[&openFileDialogue, &resourcePath, &loadImage]
						{ return openFileDialogue(resourcePath, "jpg,png", loadImage); });
					executor().submit(std::move(graph));
					myOpenFileFuture = std::move(openFileFuture);
				}
				// if (MenuItem("Open GLTF...") && !myOpenFileFuture.valid())
				// {
				// 	TaskGraph graph;
				// 	auto [task, openFileFuture] = graph.createTask(
				// 		[&openFileDialogue, &resourcePath, &loadGlTF]
				// 		{ return openFileDialogue(resourcePath, "gltf,glb", loadGlTF); });
				// 	executor().submit(std::move(graph));
				// 	myOpenFileFuture = std::move(openFileFuture);
				// }
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

	rhi<Vk>().IMGUIDrawFunction = [](CommandBufferHandle<Vk> cmd)
	{
		ZoneScopedN("RhiApplication::IMGUIDraw");

		using namespace ImGui;

		ImGui_ImplVulkan_RenderDrawData(GetDrawData(), cmd);
	};

	//myNodeGraph = std::filesystem::path(client_getUserProfilePath()) / "nodegraph.json"; // temp - this should be stored in the resource path
}

void RhiApplication::createDevice(const WindowState& window)
{
	using namespace rhiapplication;

	auto rootPath = std::get<std::filesystem::path>(environment().variables["RootPath"]);
	auto resourcePath = std::get<std::filesystem::path>(environment().variables["ResourcePath"]);
	auto userProfilePath = std::get<std::filesystem::path>(environment().variables["UserProfilePath"]);

	auto shaderIncludePath = rootPath / "src/rhi/shaders";
	auto shaderIntermediatePath = userProfilePath / ".slang.intermediate";

	auto vkSDKPathEnv = std::getenv("VULKAN_SDK");
	auto vkSDKPath = vkSDKPathEnv ? std::filesystem::path(vkSDKPathEnv) : rootPath;
	auto vkSDKBinPath = vkSDKPath / "bin";

	ShaderLoader shaderLoader(
		{shaderIncludePath},
		{std::make_tuple(SLANG_SOURCE_LANGUAGE_HLSL, SLANG_PASS_THROUGH_DXC, vkSDKBinPath)},
		shaderIntermediatePath);

	auto shaderReflection = shaderLoader.load<Vk>(shaderIncludePath / "shaders.slang");
	
	auto surface = createSurface(*rhi<Vk>().instance, &rhi<Vk>().instance->getHostAllocationCallbacks(), window.nativeHandle);

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

	rhi<Vk>().device = std::make_shared<Device<Vk>>(
		rhi<Vk>().instance, DeviceConfiguration<Vk>{detectSuitableGraphicsDevice(rhi<Vk>().instance, surface)});

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

	rhi<Vk>().pipeline = std::make_shared<Pipeline<Vk>>(
		rhi<Vk>().device, PipelineConfiguration<Vk>{(userProfilePath / "pipeline.cache").string()});

	rhi<Vk>().mainWindow = std::make_shared<Window<Vk>>(
		rhi<Vk>().device,
		std::move(surface),
		WindowConfiguration<Vk>{
			detectSuitableSwapchain(rhi<Vk>().instance, rhi<Vk>().device, surface),
			{window.width, window.height},
			{1ul, 1ul}});

	{
		uint32_t frameCount = rhi<Vk>().mainWindow->getConfig().swapchainConfig.imageCount;

		auto& primaryContexts = rhi<Vk>().commands[CommandType_GeneralPrimary];
		auto& secondaryContexts = rhi<Vk>().commands[CommandType_GeneralSecondary];
		auto& generalTransferContexts = rhi<Vk>().commands[CommandType_GeneralTransfer];
		auto& dedicatedComputeContexts = rhi<Vk>().commands[CommandType_DedicatedCompute];
		auto& dedicatedTransferContexts = rhi<Vk>().commands[CommandType_DedicatedTransfer];

		auto& graphicsQueues = rhi<Vk>().graphicsQueues;
		auto& computeQueues = rhi<Vk>().computeQueues;
		auto& transferQueues = rhi<Vk>().transferQueues;

		VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

		constexpr bool useCommandBufferCreateReset = true;
		if (useCommandBufferCreateReset)
			cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		const auto& queueFamilies = rhi<Vk>().device->getQueueFamilies();
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
						rhi<Vk>().device, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

					for (uint32_t secondaryContextIt = 0ul; secondaryContextIt < 4;
						 secondaryContextIt++)
					{
						secondaryContexts.emplace_back(CommandPoolContext<Vk>(
							rhi<Vk>().device,
							CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));
					}
				}

				generalTransferContexts.emplace_back(CommandPoolContext<Vk>(
					rhi<Vk>().device, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				graphicsQueues.emplace_back(Queue<Vk>(
					rhi<Vk>().device,
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
					rhi<Vk>().device, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				computeQueues.emplace_back(
					Queue<Vk>(rhi<Vk>().device, QueueCreateDesc<Vk>{0, queueFamilyIndex}));
			}
			else if (queueFamily.flags & QueueFamilyFlagBits_Transfer)
			{
				// only need one for now..

				dedicatedTransferContexts.emplace_back(CommandPoolContext<Vk>(
					rhi<Vk>().device, CommandPoolCreateDesc<Vk>{cmdPoolCreateFlags, queueFamilyIndex}));

				transferQueues.emplace_back(
					Queue<Vk>(rhi<Vk>().device, QueueCreateDesc<Vk>{0, queueFamilyIndex}));
			}
		}
	}

	createWindowDependentObjects(rhi<Vk>());

	// todo: create some resource global storage
	rhi<Vk>().pipeline->resources().black = std::make_shared<Image<Vk>>(
		rhi<Vk>().device,
		ImageCreateDesc<Vk>{
			{ImageMipLevelDesc<Vk>{Extent2d<Vk>{4, 4}, 16 * 4, 0}},
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_TILING_LINEAR,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});

	rhi<Vk>().pipeline->resources().blackImageView = std::make_shared<ImageView<Vk>>(
		rhi<Vk>().device, *rhi<Vk>().pipeline->resources().black, VK_IMAGE_ASPECT_COLOR_BIT);

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
	rhi<Vk>().pipeline->resources().samplers =
		std::make_shared<SamplerVector<Vk>>(rhi<Vk>().device, std::move(samplerCreateInfos));
	//

	// initialize stuff on general transfer queue
	{
		auto& generalTransferContext = rhi<Vk>().commands[CommandType_GeneralTransfer].fetchAdd();
		auto cmd = generalTransferContext.commands();

		rhi<Vk>().pipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		rhi<Vk>().pipeline->resources().black->clear(cmd, {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}});
		rhi<Vk>().pipeline->resources().black->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		initIMGUI(resourcePath, userProfilePath, window, rhi<Vk>(), cmd);

		cmd.end();

		rhi<Vk>().graphicsQueues.get().enqueueSubmit(generalTransferContext.prepareSubmit(
			{{rhi<Vk>().device->getTimelineSemaphore()},
			 {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT},
			 {rhi<Vk>().graphicsQueues.get().getLastSubmitTimelineValue().value_or(0)},
			 {rhi<Vk>().device->getTimelineSemaphore()},
			 {1 + rhi<Vk>().device->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		rhi<Vk>().graphicsQueues.get().submit();
	}

	constexpr uint32_t textureId = 1;
	constexpr uint32_t samplerId = 2;
	static_assert(textureId < ShaderTypes_GlobalTextureCount);
	static_assert(samplerId < ShaderTypes_GlobalSamplerCount);
	{
		auto& dedicatedTransferContext =
			rhi<Vk>().commands[CommandType_DedicatedTransfer].fetchAdd();

		auto& transferQueue = rhi<Vk>().transferQueues.fetchAdd();

		auto materialData = std::make_unique<MaterialData[]>(ShaderTypes_MaterialCount);
		materialData[0].color = glm::vec4(1.0, 0.0, 0.0, 1.0);
		materialData[0].textureAndSamplerId =
			(textureId << ShaderTypes_GlobalTextureIndexBits) | samplerId;
		rhi<Vk>().materials = std::make_shared<Buffer<Vk>>(
			rhi<Vk>().device,
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
		rhi<Vk>().objects = std::make_shared<Buffer<Vk>>(
			rhi<Vk>().device,
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
			 {rhi<Vk>().device->getTimelineSemaphore()},
			 {1 + rhi<Vk>().device->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

		transferQueue.submit();
	}

	auto layoutHandle = rhi<Vk>().pipeline->createLayout(shaderReflection);

	rhi<Vk>().pipeline->bindLayoutAuto(layoutHandle, VK_PIPELINE_BIND_POINT_GRAPHICS);

	rhi<Vk>().pipeline->setDescriptorData(
		"g_viewData",
		DescriptorBufferInfo<Vk>{rhi<Vk>().mainWindow->getViewBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_View);

	rhi<Vk>().pipeline->setDescriptorData(
		"g_materialData",
		DescriptorBufferInfo<Vk>{*rhi<Vk>().materials, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Material);

	rhi<Vk>().pipeline->setDescriptorData(
		"g_objectData",
		DescriptorBufferInfo<Vk>{*rhi<Vk>().objects, 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_Object,
		42);

	rhi<Vk>().pipeline->setDescriptorData(
		"g_samplers",
		DescriptorImageInfo<Vk>{(*rhi<Vk>().pipeline->resources().samplers)[0]},
		DescriptorSetCategory_GlobalSamplers,
		samplerId);
}

RhiApplication::~RhiApplication()
{
	using namespace rhiapplication;

	ZoneScopedN("~RhiApplication()");

	auto device = rhi<Vk>().device;
	auto instance = rhi<Vk>().instance;

	{
		ZoneScopedN("~RhiApplication()::waitCPU");

		executor().join(std::move(rhi<Vk>().presentFuture));
	}

	{
		ZoneScopedN("~RhiApplication()::waitGPU");

		device->waitIdle();
	}

	shutdownIMGUI();

	myRhi.reset();

	assert(device.use_count() == 1);
	assert(instance.use_count() == 2);

#ifdef TRACY_ENABLE
	tracy::GetProfiler().RequestShutdown();
	while (!tracy::GetProfiler().HasShutdownFinished())
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
}

void RhiApplication::onMouse(const MouseState& mouse)
{
	rhi<Vk>().mainWindow->onMouse(mouse);
}

void RhiApplication::onKeyboard(const KeyboardState& keyboard)
{
	rhi<Vk>().mainWindow->onKeyboard(keyboard);
}

bool RhiApplication::tick()
{
	ZoneScopedN("RhiApplication::tick");

	{
		ZoneScopedN("RhiApplication::tick::imgui_newframe");

		ImGui_ImplGlfw_NewFrame();
	}

	TaskGraph frameGraph;

	auto [drawTask, drawFuture] = frameGraph.createTask([this]
	{
		ZoneScopedN("RhiApplication::draw");

		auto [flipSuccess, lastPresentTimelineValue] = rhi<Vk>().mainWindow->flip();

		if (flipSuccess)
		{
			ZoneScopedN("RhiApplication::draw::submit");

			auto& primaryContext = rhi<Vk>().commands[CommandType_GeneralPrimary].fetchAdd();
			constexpr uint32_t secondaryContextCount = 4;
			auto secondaryContexts = &rhi<Vk>().commands[CommandType_GeneralSecondary].fetchAdd(
				secondaryContextCount);

			auto& graphicsQueue = rhi<Vk>().graphicsQueues.fetchAdd();

			if (lastPresentTimelineValue)
			{
				{
					ZoneScopedN("RhiApplication::draw::waitFrame");

					rhi<Vk>().device->wait(lastPresentTimelineValue);
				}

				primaryContext.reset();

				for (uint32_t secIt = 0; secIt < secondaryContextCount; secIt++)
					secondaryContexts[secIt].reset();
			}

			auto cmd = primaryContext.commands();

			GPU_SCOPE_COLLECT(cmd, graphicsQueue);

			{
				GPU_SCOPE(cmd, graphicsQueue, clear);

				rhi<Vk>().renderImageSet->clearDepthStencil(cmd, {1.0f, 0});
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, transition);
				
				rhi<Vk>().renderImageSet->transitionColor(
					cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
				rhi<Vk>().renderImageSet->transitionDepthStencil(
					cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, draw);
				
				rhi<Vk>().mainWindow->draw(
					executor(),
					*rhi<Vk>().pipeline,
					primaryContext,
					secondaryContexts,
					secondaryContextCount);
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, imgui);
				
				rhi<Vk>().mainWindow->begin(cmd, VK_SUBPASS_CONTENTS_INLINE);

				rhi<Vk>().IMGUIPrepareDrawFunction(); // todo: kick off earlier (but not before ImGui_ImplGlfw_NewFrame)
				rhi<Vk>().IMGUIDrawFunction(cmd);

				rhi<Vk>().mainWindow->end(cmd);
			}
			{
				GPU_SCOPE(cmd, graphicsQueue, transitionColor);
				
				rhi<Vk>().mainWindow->transitionColor(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);
			}

			cmd.end();

			auto [imageAquired, renderComplete] = rhi<Vk>().mainWindow->getFrameSyncSemaphores();

			graphicsQueue.enqueueSubmit(primaryContext.prepareSubmit(
				{{rhi<Vk>().device->getTimelineSemaphore(), imageAquired},
					{VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
					{graphicsQueue.getLastSubmitTimelineValue().value_or(0), 1},
					{rhi<Vk>().device->getTimelineSemaphore(), renderComplete},
					{1 + rhi<Vk>().device->timelineValue().fetch_add(1, std::memory_order_relaxed), 1}}));

			graphicsQueue.enqueuePresent(
				rhi<Vk>().mainWindow->preparePresent(graphicsQueue.submit()));
		}

		if (lastPresentTimelineValue)
		{
			ZoneScopedN("RhiApplication::draw::processTimelineCallbacks");

			// todo: what if the thread pool could monitor Host+Device visible memory heap using atomic_wait? then we could trigger callbacks on GPU completion events with minimum latency.
			rhi<Vk>().device->processTimelineCallbacks(static_cast<uint64_t>(lastPresentTimelineValue));
		}

		{
			if (myOpenFileFuture.valid() && myOpenFileFuture.is_ready())
			{
				ZoneScopedN("RhiApplication::draw::openFileCallback");

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
		[](Queue<Vk>* queue) { queue->present(); }, &rhi<Vk>().graphicsQueues.get());

	frameGraph.addDependency(drawTask, presentTask);

	{
		ZoneScopedN("RhiApplication::tick::waitPresent");

		executor().join(std::move(rhi<Vk>().presentFuture));
	}

	rhi<Vk>().presentFuture = std::move(presentFuture);

	executor().submit(std::move(frameGraph));

	return !myRequestExit;
}

void RhiApplication::onResizeFramebuffer(uint32_t, uint32_t)
{
	using namespace rhiapplication;

	ZoneScopedN("RhiApplication::onResizeFramebuffer");

	{
		ZoneScopedN("RhiApplication::onResizeFramebuffer::waitGPU");

		rhi<Vk>().device->wait(rhi<Vk>().graphicsQueues.get().getLastSubmitTimelineValue().value_or(0));
	}

	auto physicalDevice = rhi<Vk>().device->getPhysicalDevice();
	rhi<Vk>().instance->updateSurfaceCapabilities(physicalDevice, rhi<Vk>().mainWindow->getSurface());
	auto framebufferExtent =
		rhi<Vk>().instance->getSwapchainInfo(physicalDevice, rhi<Vk>().mainWindow->getSurface())
			.capabilities.currentExtent;

	rhi<Vk>().mainWindow->onResizeFramebuffer(framebufferExtent);

	rhi<Vk>().pipeline->setDescriptorData(
		"g_viewData",
		DescriptorBufferInfo<Vk>{rhi<Vk>().mainWindow->getViewBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_View);

	createWindowDependentObjects(rhi<Vk>());
}

void RhiApplication::onResizeWindow(const WindowState& state)
{
	if (state.fullscreenEnabled)
	{
		rhi<Vk>().mainWindow->onResizeWindow({state.fullscreenWidth, state.fullscreenHeight});
	}
	else
	{
		rhi<Vk>().mainWindow->onResizeWindow({state.width, state.height});
	}
}
