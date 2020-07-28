#include "application.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <examples/imgui_impl_glfw.h>
#include <examples/imgui_impl_vulkan.h>

#include <imnodes.h>


template <>
void Application<GraphicsBackend::Vulkan>::initIMGUI(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    CommandBufferHandle<GraphicsBackend::Vulkan> commands,
    const std::filesystem::path& userProfilePath) const
{
    ZoneScopedN("initIMGUI");

    using namespace ImGui;

    IMGUI_CHECKVERSION();
    CreateContext();
    auto& io = GetIO();
    static auto iniFilePath = std::filesystem::absolute(userProfilePath / "imgui.ini").u8string();
    io.IniFilename = iniFilePath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.FontAllowUserScaling = true;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // auto& platformIo = ImGui::GetPlatformIO();
    // platformIo.Platform_CreateVkSurface = ...

    const auto& surfaceCapabilities = myInstance->getPhysicalDeviceInfo(myDevice->getPhysicalDevice()).swapchainInfo.capabilities;

    float dpiScaleX = 
        static_cast<float>(surfaceCapabilities.currentExtent.width) / myWindow->getDesc().windowExtent.width;
    float dpiScaleY = 
        static_cast<float>(surfaceCapabilities.currentExtent.height) / myWindow->getDesc().windowExtent.height;

    io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;

    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    std::filesystem::path fontPath(myResourcePath);
    fontPath = std::filesystem::absolute(fontPath);
    fontPath /= "fonts";
    fontPath /= "foo";

    const char* fonts[] = {
        "Cousine-Regular.ttf", "DroidSans.ttf",  "Karla-Regular.ttf",
        "ProggyClean.ttf",	 "ProggyTiny.ttf", "Roboto-Medium.ttf",
    };

    ImFont* defaultFont = nullptr;
    for (auto font : fonts)
    {
        fontPath.replace_filename(font);
        defaultFont = io.Fonts->AddFontFromFileTTF(
            fontPath.u8string().c_str(), 16.0f,
            &config);
    }

    // Setup style
    StyleColorsClassic();
    io.FontDefault = defaultFont;

    // Setup Vulkan binding
    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = myInstance->getInstance();
    initInfo.PhysicalDevice = myDevice->getPhysicalDevice();
    initInfo.Device = myDevice->getDevice();
    initInfo.QueueFamily = myDevice->getGraphicsQueueFamilyIndex();
    initInfo.Queue = myDevice->getPrimaryGraphicsQueue();
    initInfo.PipelineCache = myGraphicsPipeline->getCache();
    initInfo.DescriptorPool = myDevice->getDescriptorPool();
    initInfo.MinImageCount = myDevice->getDesc().swapchainConfiguration->imageCount;
    initInfo.ImageCount = myDevice->getDesc().swapchainConfiguration->imageCount;
    initInfo.Allocator = nullptr;
    // initInfo.HostAllocationCallbacks = nullptr;
    initInfo.CheckVkResultFn = checkResult;
    ImGui_ImplVulkan_Init(
        &initInfo,
        myGraphicsPipeline->resources()->renderTarget->getRenderPass());

    // Upload Fonts
    ImGui_ImplVulkan_CreateFontsTexture(commands);
    
    deviceContext->addTimelineCallback([](uint64_t){
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    });

    imnodes::Initialize();
    imnodes::LoadCurrentEditorStateFromIniString(myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
}

template <>
void Application<GraphicsBackend::Vulkan>::shutdownIMGUI()
{
    size_t count;
    myNodeGraph.layout.assign(imnodes::SaveCurrentEditorStateToIniString(&count));
    imnodes::Shutdown();

    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
}

template <>
void Application<GraphicsBackend::Vulkan>::createWindowDependentObjects(
    Extent2d<GraphicsBackend::Vulkan> frameBufferExtent)
{
    ZoneScopedN("createWindowDependentObjects");

    myLastFrameIndex = myDevice->getDesc().swapchainConfiguration->imageCount - 1;

    auto colorImage = std::make_shared<Image<GraphicsBackend::Vulkan>>(
        myDevice,
        ImageCreateDesc<GraphicsBackend::Vulkan>{
            {"rtColorImage"},
            frameBufferExtent,
            myDevice->getDesc().swapchainConfiguration->surfaceFormat.format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    
    auto depthStencilImage = std::make_shared<Image<GraphicsBackend::Vulkan>>(
        myDevice,
        ImageCreateDesc<GraphicsBackend::Vulkan>{
            {"rtDepthImage"},
            frameBufferExtent,
            findSupportedFormat(
                myDevice->getPhysicalDevice(),
                {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    
    myRenderImageSet = std::make_shared<RenderImageSet<GraphicsBackend::Vulkan>>(
        myDevice,
        "RenderImageSet", 
        make_vector(colorImage),
        depthStencilImage);

    myGraphicsPipeline->resources()->renderTarget = myRenderImageSet;
    myGraphicsPipeline->createGraphicsPipeline();
}

template <>
void Application<GraphicsBackend::Vulkan>::processTimelineCallbacks(uint64_t timelineValue)
{
    if (myLastTransferTimelineValue)
    {
        {
            ZoneScopedN("waitTransfer");

            myDevice->wait(myLastTransferTimelineValue);
        }

        myDevice->processTimelineCallbacks(std::min(timelineValue, myLastTransferTimelineValue));
        
        myLastTransferTimelineValue = 0;

        // {
        //     ZoneScopedN("tracyVkCollectTransfer");

        //     auto& primaryCommandContext = myWindow->commandContexts()[frameIndex][0];
        //     auto primaryCommands = primaryCommandContext->beginScope();

        //     TracyVkCollect(
        //         myTransferCommands->userData<command::UserData>().tracyContext,
        //         primaryCommands);
        // }
    }
    else
    {
        myDevice->processTimelineCallbacks(timelineValue);
    }
}

template <>
Application<GraphicsBackend::Vulkan>::Application(
    void* view,
    int width,
    int height,
    const char* resourcePath,
    const char* userProfilePath)
: myResourcePath([](const char* pathStr, const char* defaultPathStr)
{
    auto path = std::filesystem::path(pathStr ? pathStr : defaultPathStr);
    assert(std::filesystem::is_directory(path));
    return path;
}(resourcePath, "./resources/"))
, myUserProfilePath([](const char* pathStr, const char* defaultPathStr)
{
    auto path = std::filesystem::path(pathStr ? pathStr : defaultPathStr);
     if (!std::filesystem::exists(path))
        std::filesystem::create_directory(path);
    assert(std::filesystem::is_directory(path));
    return path;
}(userProfilePath, "./.profile/"))
, myNodeGraph(myUserProfilePath / "nodegraph.json", "nodeGraph") // temp - this should be stored in the resource path
{
    ZoneScopedN("Application()");
    
    myInstance = std::make_shared<InstanceContext<GraphicsBackend::Vulkan>>(
        AutoSaveJSONFileObject<InstanceConfiguration<GraphicsBackend::Vulkan>>(
            myUserProfilePath / "instance.json",
            "instanceConfiguration"),
        view);

    const auto& graphicsDeviceCandidates = myInstance->getGraphicsDeviceCandidates();
    if (graphicsDeviceCandidates.empty())
        throw std::runtime_error("failed to find a suitable GPU!");

    myDevice = std::make_shared<DeviceContext<GraphicsBackend::Vulkan>>(
        myInstance,
        AutoSaveJSONFileObject<DeviceConfiguration<GraphicsBackend::Vulkan>>(
            myUserProfilePath / "device.json",
            "deviceConfiguration",
            {graphicsDeviceCandidates.front().first}));

    auto shaderModule = loadSlangShaders<GraphicsBackend::Vulkan>(
        std::filesystem::path("D:\\github\\hlsl.bin\\RelWithDebInfo\\bin"),
        myResourcePath / "shaders" / "shaders.slang");

    myGraphicsPipeline = std::make_shared<PipelineContext<GraphicsBackend::Vulkan>>(
        myDevice,
        PipelineContextCreateDesc<GraphicsBackend::Vulkan>{ { "PipelineContext" }, myUserProfilePath });

    myGraphicsPipelineLayout = std::make_shared<PipelineLayout<GraphicsBackend::Vulkan>>(
        myDevice,
        shaderModule);

    myGraphicsPipeline->layout() = myGraphicsPipelineLayout;

    // if (commandBufferLevel == 0)
    //     std::any_cast<command::UserData>(&myUserData)->tracyContext =
    //         TracyVkContext(
    //             myDevice->getPhysicalDevice(),
    //             myDevice->getDevice(),
    //             queue,
    //             commands());

    // if (std::any_cast<command::UserData>(&myUserData)->tracyContext)
    //     TracyVkDestroy(std::any_cast<command::UserData>(&myUserData)->tracyContext);

    myTransferCommands = std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
        myDevice,
        CommandContextCreateDesc<GraphicsBackend::Vulkan>{myDevice->getTransferCommandPools()[0][0]});
    {
        myGraphicsPipeline->resources()->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
            myDevice,
            myTransferCommands,
            myResourcePath / "models" / "gallery.obj");
        myGraphicsPipeline->resources()->image = std::make_shared<Image<GraphicsBackend::Vulkan>>(
            myDevice,
            myTransferCommands,
            myResourcePath / "images" / "gallery.jpg");
        myGraphicsPipeline->resources()->imageView = std::make_shared<ImageView<GraphicsBackend::Vulkan>>(
            myDevice,
            *(myGraphicsPipeline->resources()->image),
            VK_IMAGE_ASPECT_COLOR_BIT);

        myWindow = std::make_shared<WindowContext<GraphicsBackend::Vulkan>>(
            myInstance,
            myDevice,    
            WindowCreateDesc<GraphicsBackend::Vulkan>{
                {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                {3, 2}});
        
        // submit transfers.
        auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
        myLastTransferTimelineValue = myTransferCommands->submit({
            myDevice->getPrimaryTransferQueue(),
            0,
            nullptr,
            nullptr,
            nullptr,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});
    }

    createWindowDependentObjects({static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    
    // stuff that needs to be initialized on graphics queue
    {
        auto& frame = myWindow->frames()[myLastFrameIndex];
        auto& primaryCommandContext = myWindow->commandContexts()[frame->getDesc().index][0];
        
        {
            auto primaryCommands = primaryCommandContext->commands();

            initIMGUI(myDevice, primaryCommands, myUserProfilePath);

            myGraphicsPipeline->resources()->image->transition(primaryCommands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        
        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
        auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
        myLastFrameTimelineValue = primaryCommandContext->submit({
            myDevice->getPrimaryGraphicsQueue(),
            1,
            &myDevice->getTimelineSemaphore(),
            &waitDstStageMask,
            &waitTimelineValue,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});

        frame->setLastSubmitTimelineValue(myLastFrameTimelineValue); // todo: remove
    }

    myGraphicsPipeline->updateDescriptorSets(myWindow->getViewBuffer().getBufferHandle());

    auto openFileDialogue = [](const std::filesystem::path& resourcePath, const nfdchar_t* filterList, std::function<void(nfdchar_t*)>&& onCompletionCallback)
    {
        std::string resourcePathStr = std::filesystem::absolute(resourcePath).u8string();
        nfdchar_t* openFilePath;
        return std::make_tuple(NFD_OpenDialog(filterList, resourcePathStr.c_str(), &openFilePath),
            openFilePath, std::move(onCompletionCallback));
    };

    auto loadModel = [this](nfdchar_t* openFilePath)
    {
        auto model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
            myDevice,
            myTransferCommands,
            openFilePath);

        auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
        myLastTransferTimelineValue = myTransferCommands->submit({
            myDevice->getPrimaryTransferQueue(),
            0,
            nullptr,
            nullptr,
            nullptr,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});

        myDevice->addTimelineCallback(myLastTransferTimelineValue, [this, model](uint64_t /*timelineValue*/)
        {
            const auto& resources = myGraphicsPipeline->resources();
            const auto& layout = myGraphicsPipeline->layout();
            
            resources->model = model;
            
            myGraphicsPipeline->descriptorSets() = std::make_shared<DescriptorSetVector<GraphicsBackend::Vulkan>>(
                myDevice,
                layout->getDescriptorSetLayouts());

            myGraphicsPipeline->updateDescriptorSets(myWindow->getViewBuffer().getBufferHandle());
        });
    };

    auto loadImage = [this](nfdchar_t* openFilePath)
    {
        std::shared_ptr<Image<GraphicsBackend::Vulkan>> image = std::make_shared<Image<GraphicsBackend::Vulkan>>(
            myDevice,
            myTransferCommands,
            openFilePath);

        auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
        myLastTransferTimelineValue = myTransferCommands->submit({
            myDevice->getPrimaryTransferQueue(),
            0,
            nullptr,
            nullptr,
            nullptr,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});

        myDevice->addTimelineCallback(myLastTransferTimelineValue, [this, image](uint64_t /*timelineValue*/)
        {
            const auto& resources = myGraphicsPipeline->resources();
            const auto& layout = myGraphicsPipeline->layout();

            resources->image = image;
            resources->imageView = std::make_shared<ImageView<GraphicsBackend::Vulkan>>(
                myDevice,
                *image,
                VK_IMAGE_ASPECT_COLOR_BIT);

            myGraphicsPipeline->descriptorSets() = std::make_shared<DescriptorSetVector<GraphicsBackend::Vulkan>>(
                myDevice,
                layout->getDescriptorSetLayouts());

            myGraphicsPipeline->updateDescriptorSets(myWindow->getViewBuffer().getBufferHandle());
        });
    };

    myWindow->addIMGUIDrawCallback([this, openFileDialogue, loadModel, loadImage]
    {
        using namespace ImGui;

        // todo: move elsewhere
        auto editableTextField = [](int id, const char* label, std::string& str, float maxTextWidth, int& inputId)
        {
            auto textSize = std::max(
                maxTextWidth,
                CalcTextSize(
                    str.c_str(),
                    str.c_str() + str.size()).x);

            PushItemWidth(textSize);
            //PushClipRect(textSize);
            
            if (id == inputId)
            {
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsMouseClicked(0))
                {
                    PushAllowKeyboardFocus(true);
                    SetKeyboardFocusHere();

                    if (InputText(
                        label,
                        &str,
                        ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CallbackAlways,
                        [](ImGuiInputTextCallbackData* data)
                        {
                            //PopClipRect();
                            PopItemWidth();

                            auto textSize = std::max(
                                *static_cast<float*>(data->UserData),
                                CalcTextSize(
                                    data->Buf,
                                    data->Buf + data->BufTextLen).x);

                            PushItemWidth(textSize);
                            //PushClipRect(textSize);

                            data->SelectionStart = data->SelectionEnd;

                            return 0;

                        }, &maxTextWidth))
                    {
                        if (str.empty())
                            str = "Empty";
                        
                        inputId = 0;
                    }

                    PopAllowKeyboardFocus();
                }
                else
                {
                    if (str.empty())
                        str = "Empty";
                            
                    inputId = 0;
                }
            }
            else
            {
                TextUnformatted(str.c_str(), str.c_str() + str.size());
            }

            //PopClipRect();
            PopItemWidth();

            return std::max(
                maxTextWidth,
                CalcTextSize(
                    str.c_str(),
                    str.c_str() + str.size()).x);;
        };

        static bool showStatistics = false;
        if (showStatistics)
        {
            if (Begin("Statistics", &showStatistics))
            {
                // Text("Unknowns: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_UNKNOWN));
                // Text("Instances: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_INSTANCE));
                // Text("Physical Devices: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_PHYSICAL_DEVICE));
                Text("Devices: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DEVICE));
                Text("Queues: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_QUEUE));
                Text("Semaphores: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
                Text("Command Buffers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
                Text("Fences: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_FENCE));
                Text("Device Memory: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DEVICE_MEMORY));
                Text("Buffers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_BUFFER));
                Text("Images: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_IMAGE));
                Text("Events: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_EVENT));
                Text("Query Pools: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_QUERY_POOL));
                Text("Buffer Views: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
                Text("Image Views: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
                Text("Shader Modules: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SHADER_MODULE));
                Text("Pipeline Caches: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE_CACHE));
                Text("Pipeline Layouts: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE_LAYOUT));
                Text("Render Passes: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_RENDER_PASS));
                Text("Pipelines: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_PIPELINE));
                Text("Descriptor Set Layouts: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT));
                Text("Samplers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SAMPLER));
                Text("Descriptor Pools: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_POOL));
                Text("Descriptor Sets: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_DESCRIPTOR_SET));
                Text("Framebuffers: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
                Text("Command Pools: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
                Text("Surfaces: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
                Text("Swapchains: %u", myDevice->getTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
            }
            End();
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
            Begin("Node Editor Window", &showNodeEditor);

            PushAllowKeyboardFocus(false);
            
            imnodes::BeginNodeEditor();

            struct NodeUserData
            {
                int inputId = 0;
            };

            for (const auto& node : myNodeGraph.nodes)
            {
                if (!node->userData().has_value())
                    node->userData() = NodeUserData{};

                char buffer[64];

                imnodes::BeginNode(node->getId());

                // title bar
                sprintf_s(buffer, sizeof(buffer), "##node%.*u", 4, node->getId());

                imnodes::BeginNodeTitleBar();

                float titleBarTextWidth = editableTextField(
                    node->getId(),
                    buffer,
                    node->name(),
                    160.0f,
                    std::any_cast<NodeUserData&>(node->userData()).inputId);

                imnodes::EndNodeTitleBar();

                if (IsItemClicked() && IsMouseDoubleClicked(0))
                    std::any_cast<NodeUserData&>(node->userData()).inputId = node->getId();
                
                // attributes
                if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
                {
                    auto rowCount = std::max(inOutNode->inputAttributes().size(), inOutNode->outputAttributes().size());

                    for (uint32_t rowIt = 0; rowIt < rowCount; rowIt++)
                    {
                        float inputTextWidth = 0.0f;
                        bool hasInputPin = rowIt < inOutNode->inputAttributes().size();
                        if (hasInputPin)
                        {
                            auto& inputAttribute = inOutNode->inputAttributes()[rowIt];
                            sprintf_s(buffer, sizeof(buffer), "##inputattribute%.*u", 4, inputAttribute.id);
                            
                            imnodes::BeginInputAttribute(inputAttribute.id);

                            inputTextWidth = editableTextField(
                                inputAttribute.id,
                                buffer,
                                inputAttribute.name,
                                80.0f,
                                std::any_cast<NodeUserData&>(node->userData()).inputId);
                            
                            imnodes::EndInputAttribute();

                            if (IsItemClicked() && IsMouseDoubleClicked(0))
                                std::any_cast<NodeUserData&>(node->userData()).inputId = inputAttribute.id;
                        }

                        if (rowIt < inOutNode->outputAttributes().size())
                        {
                            auto& outputAttribute = inOutNode->outputAttributes()[rowIt];
                            sprintf_s(buffer, sizeof(buffer), "##outputattribute%.*u", 4, outputAttribute.id);

                            if (hasInputPin)
                                ImGui::SameLine();

                            imnodes::BeginOutputAttribute(outputAttribute.id);

                            float outputTextWidth = CalcTextSize(
                                outputAttribute.name.c_str(),
                                outputAttribute.name.c_str() + outputAttribute.name.size()).x;

                            if (hasInputPin)
                                Indent(std::max(titleBarTextWidth, inputTextWidth + outputTextWidth) - outputTextWidth);
                            else
                                Indent(std::max(titleBarTextWidth, outputTextWidth + 80.0f) - outputTextWidth);

                            editableTextField(
                                outputAttribute.id,
                                buffer,
                                outputAttribute.name,
                                80.0f,
                                std::any_cast<NodeUserData&>(node->userData()).inputId);
                            
                            imnodes::EndOutputAttribute();

                            if (IsItemClicked() && IsMouseDoubleClicked(0))
                                std::any_cast<NodeUserData&>(node->userData()).inputId = outputAttribute.id;
                        }
                    }
                }
            
                imnodes::EndNode();
                
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::Selectable("Add Input"))
                    {
                        if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
                        {
                            sprintf_s(buffer, sizeof(buffer), "In %u", inOutNode->inputAttributes().size());
                            inOutNode->inputAttributes().emplace_back(Attribute{++myNodeGraph.uniqueId, buffer});
                        }
                    }
                    if (ImGui::Selectable("Add Output"))
                    {
                        if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
                        {
                            sprintf_s(buffer, sizeof(buffer), "Out %u", inOutNode->outputAttributes().size());
                            inOutNode->outputAttributes().emplace_back(Attribute{++myNodeGraph.uniqueId, buffer});
                        }
                    }
                    ImGui::EndPopup();
                }
            }
        
            for (int linkIt = 0; linkIt < myNodeGraph.links.size(); linkIt++)
                imnodes::Link(linkIt, myNodeGraph.links[linkIt].fromId, myNodeGraph.links[linkIt].toId);
            
            imnodes::EndNodeEditor();

            int hoveredNodeId;
            if (/*imnodes::IsEditorHovered() && */
                !imnodes::IsNodeHovered(&hoveredNodeId) &&
                ImGui::BeginPopupContextItem("Node Editor Context Menu"))
            {
                ImVec2 clickPos = ImGui::GetMousePosOnOpeningCurrentPopup();

                enum class NodeType { SlangShaderNode };
                static constexpr std::pair<NodeType, std::string_view> menuItems[] = { { NodeType::SlangShaderNode, "Slang Shader"} };

                for (const auto& menuItem : menuItems)
                {
                    if (ImGui::Selectable(menuItem.second.data()))
                    {
                        int id = ++myNodeGraph.uniqueId;
                        imnodes::SetNodeScreenSpacePos(id, clickPos);
                        myNodeGraph.nodes.emplace_back([&menuItem, &id]() -> std::shared_ptr<INode> 
                        {
                            switch (menuItem.first)
                            {
                                case NodeType::SlangShaderNode:
                                    return std::make_shared<SlangShaderNode>(SlangShaderNode(id, std::string(menuItem.second.data())));
                                default:
                                    assert(false);
                                    break;
                            }
                            return {};
                        }());
                    }
                }

                ImGui::EndPopup();
            }

            PopAllowKeyboardFocus();

            ImGui::End();

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

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open OBJ...") && !myOpenFileFuture.valid())
                    myOpenFileFuture = std::async(std::launch::async, openFileDialogue, myResourcePath, "obj", std::move(loadModel));
                if (ImGui::MenuItem("Open Image...") && !myOpenFileFuture.valid())
                    myOpenFileFuture = std::async(std::launch::async, openFileDialogue, myResourcePath, "jpg,png", std::move(loadImage));
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "CTRL+Q"))
                    myRequestExit = true;
                    
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Node Editor..."))
                    showNodeEditor = !showNodeEditor;
                if (ImGui::MenuItem("Statistics..."))
                    showStatistics = !showStatistics;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("About"))
            {
                if (ImGui::MenuItem("Show IMGUI Demo..."))
                    showDemoWindow = !showDemoWindow;
                ImGui::Separator();
                if (ImGui::MenuItem("About volcano..."))
                    showAbout = !showAbout;
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        } 
    });
}

template <>
Application<GraphicsBackend::Vulkan>::~Application()
{
    ZoneScopedN("~Application()");

    {
        ZoneScopedN("deviceWaitIdle");

        // todo: replace with frame & transfer sync
        VK_CHECK(vkDeviceWaitIdle(myDevice->getDevice()));
    }
    
    shutdownIMGUI();

#ifdef PROFILING_ENABLE
    char* allocatorStatsJSON = nullptr;
    vmaBuildStatsString(myDevice->getAllocator(), &allocatorStatsJSON, true);
    std::cout << allocatorStatsJSON << std::endl;
    vmaFreeStatsString(myDevice->getAllocator(), allocatorStatsJSON);
#endif
}

template <>
void Application<GraphicsBackend::Vulkan>::onMouse(const MouseState& state)
{
    bool leftPressed = state.button == GLFW_MOUSE_BUTTON_LEFT && state.action == GLFW_PRESS;
    bool rightPressed = state.button == GLFW_MOUSE_BUTTON_RIGHT && state.action == GLFW_PRESS;
    
    auto screenPos = glm::vec2(state.xpos, state.ypos);

    myInput.mousePosition[0] =
        glm::vec2{static_cast<float>(screenPos.x), static_cast<float>(screenPos.y)};
    myInput.mousePosition[1] =
        leftPressed && !myInput.mouseButtonsPressed[0] ? myInput.mousePosition[0] : myInput.mousePosition[1];

    myInput.mouseButtonsPressed[0] = leftPressed;
    myInput.mouseButtonsPressed[1] = rightPressed;
    myInput.mouseButtonsPressed[2] = state.insideWindow && !myInput.mouseButtonsPressed[0];
}

template <>
void Application<GraphicsBackend::Vulkan>::onKeyboard(const KeyboardState& state)
{
    if (state.action == GLFW_PRESS)
        myInput.keysPressed[state.key] = true;
    else if (state.action == GLFW_RELEASE)
        myInput.keysPressed[state.key] = false;
}

template <>
bool Application<GraphicsBackend::Vulkan>::draw()
{
    ZoneScopedN("draw");

    auto [flipSuccess, frameIndex, frameTimelineValue] = myWindow->flipFrame(myLastFrameIndex);

    if (flipSuccess)
    {
        if (frameTimelineValue)
        {
            ZoneScopedN("waitFrameGPUCommands");
            
            assert(frameIndex != myLastFrameIndex);

            myDevice->wait(frameTimelineValue);
        }

        myWindow->updateInput(myInput, frameIndex, myLastFrameIndex);

        if (const auto& depthStencilImage = myRenderImageSet->getDepthStencilImages(); depthStencilImage)
        {
            auto& primaryCommandContext = myWindow->commandContexts()[frameIndex][0];
            auto primaryCommands = primaryCommandContext->commands();

            depthStencilImage->clearDepthStencil(primaryCommands, { 1.0f, 0 });
            depthStencilImage->transition(primaryCommands, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }

        myLastFrameTimelineValue = myWindow->submitFrame(
            frameIndex,
            myLastFrameIndex,
            myGraphicsPipeline,
            std::max(myLastTransferTimelineValue, myLastFrameTimelineValue));
    }

    auto processTimelineCallbacksFuture(
        std::async(
            std::launch::async,
            &Application<GraphicsBackend::Vulkan>::processTimelineCallbacks,
            this,
            frameTimelineValue));

    if (flipSuccess)
        myWindow->presentFrame(frameIndex);

    myLastFrameIndex = frameIndex;

    // wait for timeline callbacks
    {
        ZoneScopedN("waitProcessTimelineCallbacks");

        processTimelineCallbacksFuture.get();
    }

    // record and submit transfers
    {
        ZoneScopedN("transfers");

        uint32_t transferCount = 0;

        if (myOpenFileFuture.valid() && is_ready(myOpenFileFuture))
        {
            ZoneScopedN("openFileCallback");

            const auto& [openFileResult, openFilePath, onCompletionCallback] = myOpenFileFuture.get();
            if (openFileResult == NFD_OKAY)
            {
                onCompletionCallback(openFilePath);
                free(openFilePath);
                ++transferCount;
            }
        }

        if (transferCount)
        {
            Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
            auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
            myLastTransferTimelineValue = myTransferCommands->submit({
                myDevice->getPrimaryTransferQueue(),
                1,
                &myDevice->getTimelineSemaphore(),
                &waitDstStageMask,
                &waitTimelineValue,
                1,
                &myDevice->getTimelineSemaphore(),
                &signalTimelineValue});
        }
    }

    return myRequestExit;
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int, int)
{
    ZoneScopedN("resizeFramebuffer");

    {
        ZoneScopedN("waitGPU");

        myDevice->wait(std::max(myLastTransferTimelineValue, myLastFrameTimelineValue));
    }

    auto physicalDevice = myDevice->getPhysicalDevice();
    myInstance->updateSurfaceCapabilities(physicalDevice);
    auto framebufferExtent = 
        myInstance->getPhysicalDeviceInfo(physicalDevice).swapchainInfo.capabilities.currentExtent;
    
    myWindow->onResizeFramebuffer(framebufferExtent);
    myGraphicsPipeline->updateDescriptorSets(myWindow->getViewBuffer().getBufferHandle());

    createWindowDependentObjects(framebufferExtent);
}
