#include "application.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <examples/imgui_impl_glfw.h>
#include <examples/imgui_impl_vulkan.h>

#include <imnodes.h>

template <>
void Application<Vk>::initIMGUI(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CommandBufferHandle<Vk> commands,
    const std::filesystem::path& userProfilePath) const
{
    ZoneScopedN("Application::initIMGUI");

    using namespace ImGui;

    IMGUI_CHECKVERSION();
    CreateContext();
    auto& io = GetIO();
    static auto iniFilePath = std::filesystem::absolute(userProfilePath / "imgui.ini").generic_string();
    io.IniFilename = iniFilePath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.FontGlobalScale = 1.0f;
    //io.FontAllowUserScaling = true;
    io.ConfigDockingWithShift = true;

    // auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)
    //     vkGetInstanceProcAddr(myInstance->getInstance(), "vkCreateWin32SurfaceKHR");
    
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    auto& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_CreateVkSurface = (decltype(platformIo.Platform_CreateVkSurface))vkGetInstanceProcAddr(
        myInstance->getInstance(), "vkCreateWin32SurfaceKHR");

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
            fontPath.generic_string().c_str(), 16.0f,
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
    initInfo.Queue = myDevice->getGraphicsQueue();
    initInfo.PipelineCache = myGraphicsPipeline->getCache();
    initInfo.DescriptorPool = myDevice->getDescriptorPool();
    initInfo.MinImageCount = myDevice->getDesc().swapchainConfig->imageCount;
    initInfo.ImageCount = myDevice->getDesc().swapchainConfig->imageCount;
    initInfo.Allocator = nullptr;
    // initInfo.HostAllocationCallbacks = nullptr;
    initInfo.CheckVkResultFn = checkResult;
    initInfo.DeleteBufferFn = [](void* user_data, VkBuffer buffer, VkDeviceMemory buffer_memory, const VkAllocationCallbacks* allocator)
    {
        DeviceContext<Vk>* deviceContext = static_cast<DeviceContext<Vk>*>(user_data);
        deviceContext->addTimelineCallback([device = deviceContext->getDevice(), buffer, buffer_memory, allocator](uint64_t)
        {
            if (buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, buffer, allocator);
            if (buffer_memory != VK_NULL_HANDLE)
                vkFreeMemory(device, buffer_memory, allocator);
        });
    };
    initInfo.UserData = deviceContext.get();
    ImGui_ImplVulkan_Init(
        &initInfo,
        myIMGUIRenderPass);

    // Upload Fonts
    ImGui_ImplVulkan_CreateFontsTexture(commands);
    
    deviceContext->addTimelineCallback([](uint64_t){
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    });

    imnodes::Initialize();
    imnodes::LoadCurrentEditorStateFromIniString(myNodeGraph.layout.c_str(), myNodeGraph.layout.size());
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
void Application<Vk>::createWindowDependentObjects(
    Extent2d<Vk> frameBufferExtent)
{
    ZoneScopedN("Application::createWindowDependentObjects");

    auto colorImage = std::make_shared<Image<Vk>>(
        myDevice,
        ImageCreateDesc<Vk>{
            {"rtColorImage"},
            { { frameBufferExtent } },
            myDevice->getDesc().swapchainConfig->surfaceFormat.format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    
    auto depthStencilImage = std::make_shared<Image<Vk>>(
        myDevice,
        ImageCreateDesc<Vk>{
            {"rtDepthImage"},
            { { frameBufferExtent } },
            findSupportedFormat(
                myDevice->getPhysicalDevice(),
                {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    
    myRenderImageSet = std::make_shared<RenderImageSet<Vk>>(
        myDevice,
        "RenderImageSet", 
        make_vector(colorImage),
        depthStencilImage);

    myGraphicsPipeline->resources()->renderTarget = myRenderImageSet;
}

template <>
void Application<Vk>::processTimelineCallbacks(uint64_t timelineValue)
{
    if (myLastTransferTimelineValue)
    {
        {
            ZoneScopedN("Application::waitTransfer");

            myDevice->wait(myLastTransferTimelineValue);
        }

        myDevice->processTimelineCallbacks(std::min(timelineValue, myLastTransferTimelineValue));
        
        myLastTransferTimelineValue = 0;

        // {
        //     ZoneScopedN("Application::tracyVkCollectTransfer");

        //     auto& commandContext = myWindow->commandContext(frameIndex);
        //     auto cmd = commandContext->beginScope();

        //     TracyVkCollect(
        //         myTransferCommands->userData<command::UserData>().tracyContext,
        //         cmd);
        // }
    }
    else
    {
        myDevice->processTimelineCallbacks(timelineValue);
    }
}

template <>
Application<Vk>::Application(
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
    
    myInstance = std::make_shared<InstanceContext<Vk>>(
        AutoSaveJSONFileObject<InstanceConfiguration<Vk>>(
            myUserProfilePath / "instance.json",
            "instanceConfiguration"),
        view);

    const auto& graphicsDeviceCandidates = myInstance->getGraphicsDeviceCandidates();
    if (graphicsDeviceCandidates.empty())
        throw std::runtime_error("failed to find a suitable GPU!");

    myDevice = std::make_shared<DeviceContext<Vk>>(
        myInstance,
        AutoSaveJSONFileObject<DeviceConfiguration<Vk>>(
            myUserProfilePath / "device.json",
            "deviceConfiguration",
            {graphicsDeviceCandidates.front().first}));

    auto shaderModule = loadSlangShaders<Vk>(
        std::filesystem::path(std::getenv("VK_SDK_PATH")) / "bin",
        myResourcePath / "shaders" / "shaders.slang");

    myGraphicsPipeline = std::make_shared<PipelineContext<Vk>>(
        myDevice,
        PipelineContextCreateDesc<Vk>{
            { "GraphicsPipeline" },
            myUserProfilePath / "pipeline.cache" });

    myGraphicsPipelineLayout = std::make_shared<PipelineLayout<Vk>>(
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

    myTransferCommands = std::make_shared<CommandContext<Vk>>(
        myDevice,
        CommandContextCreateDesc<Vk>{myDevice->getQueueFamilies()[myDevice->getTransferQueueFamilyIndex()].commandPools[0]});

    myGraphicsQueue = std::make_shared<Queue<Vk>>(myDevice, QueueCreateDesc<Vk>{{"GraphicsQueue"}, myDevice->getGraphicsQueue()});
    myTransferQueue = std::make_shared<Queue<Vk>>(myDevice, QueueCreateDesc<Vk>{{"TransferQueue"}, myDevice->getTransferQueue()});

    {
        myGraphicsPipeline->resources()->model = std::make_shared<Model<Vk>>(
            myDevice,
            myTransferCommands,
            myResourcePath / "models" / "gallery.obj");
        myGraphicsPipeline->resources()->image = std::make_shared<Image<Vk>>(
            myDevice,
            myTransferCommands,
            myResourcePath / "images" / "gallery.jpg");
        myGraphicsPipeline->resources()->imageView = std::make_shared<ImageView<Vk>>(
            myDevice,
            *(myGraphicsPipeline->resources()->image),
            VK_IMAGE_ASPECT_COLOR_BIT);

        myWindow = std::make_shared<WindowContext<Vk>>(
            myDevice,    
            WindowCreateDesc<Vk>{
                {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                {3, 2}});
        
        myTransferQueue->enqueue(
            myTransferCommands->flush({
                {},
                {},
                {},
                {myDevice->getTimelineSemaphore()}, 
                {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

        myLastTransferTimelineValue = myTransferQueue->submit();
    }

    createWindowDependentObjects({static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    
    // stuff that needs to be initialized on graphics queue
    {
        const auto& frame = *myWindow->getSwapchain()->getFrames()[myWindow->getSwapchain()->getFrames().size() - 1];
        auto& commandContext = myWindow->commandContext(frame.getDesc().index);
        auto cmd = commandContext->commands();

        auto initDrawCommands = [this](CommandBufferHandle<Vk> cmd, uint32_t frameIndex)
        {
            myIMGUIRenderPass = std::get<0>(
                myWindow->getSwapchain()->getFrames()[frameIndex]->renderPassAndFramebuffer());

            initIMGUI(myDevice, cmd, myUserProfilePath);

            myGraphicsPipeline->resources()->image->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        };

        initDrawCommands(cmd, frame.getDesc().index);

        cmd.end();
        
        myGraphicsQueue->enqueue(
            commandContext->flush({
                {myDevice->getTimelineSemaphore()},
                {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
                {std::max(myLastTransferTimelineValue, myLastFrameTimelineValue)},
                {myDevice->getTimelineSemaphore()},
                {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

        myLastFrameTimelineValue = myGraphicsQueue->submit();
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
        auto model = std::make_shared<Model<Vk>>(
            myDevice,
            myTransferCommands,
            openFilePath);

        myTransferQueue->enqueue(
            myTransferCommands->flush({
                {},
                {},
                {},
                {myDevice->getTimelineSemaphore()},
                {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

        myLastTransferTimelineValue = myTransferQueue->submit();

        myDevice->addTimelineCallback(myLastTransferTimelineValue, [this, model](uint64_t /*timelineValue*/)
        {
            const auto& resources = myGraphicsPipeline->resources();
            const auto& layout = myGraphicsPipeline->layout();
            
            resources->model = model;
            
            myGraphicsPipeline->descriptorSets() = std::make_shared<DescriptorSetVector<Vk>>(
                myDevice,
                layout->getDescriptorSetLayouts());

            myGraphicsPipeline->updateDescriptorSets(myWindow->getViewBuffer().getBufferHandle());
        });
    };

    auto loadImage = [this](nfdchar_t* openFilePath)
    {
        std::shared_ptr<Image<Vk>> image = std::make_shared<Image<Vk>>(
            myDevice,
            myTransferCommands,
            openFilePath);

        myTransferQueue->enqueue(
            myTransferCommands->flush({
                {},
                {},
                {},
                {myDevice->getTimelineSemaphore()},
                {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));

        myLastTransferTimelineValue = myTransferQueue->submit();

        myDevice->addTimelineCallback(myLastTransferTimelineValue, [this, image](uint64_t /*timelineValue*/)
        {
            const auto& resources = myGraphicsPipeline->resources();
            const auto& layout = myGraphicsPipeline->layout();

            resources->image = image;
            resources->imageView = std::make_shared<ImageView<Vk>>(
                myDevice,
                *image,
                VK_IMAGE_ASPECT_COLOR_BIT);

            myGraphicsPipeline->descriptorSets() = std::make_shared<DescriptorSetVector<Vk>>(
                myDevice,
                layout->getDescriptorSetLayouts());

            myGraphicsPipeline->updateDescriptorSets(myWindow->getViewBuffer().getBufferHandle());
        });
    };

    myIMGUIPrepareDrawFunction = [this, openFileDialogue, loadModel, loadImage]
    {
        ZoneScopedN("Application::IMGUIPrepareDraw");

        using namespace ImGui;

        ImGui_ImplVulkan_NewFrame();
        NewFrame();

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
                if (IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IsMouseClicked(0))
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
            ImGui::SetNextWindowSize(ImVec2(800, 450), ImGuiCond_FirstUseEver);

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

                imnodes::BeginNode(node->id());

                // title bar
                stbsp_sprintf(buffer, "##node%.*u", 4, node->id());

                imnodes::BeginNodeTitleBar();

                float titleBarTextWidth = editableTextField(
                    node->id(),
                    buffer,
                    node->name(),
                    160.0f,
                    std::any_cast<NodeUserData&>(node->userData()).inputId);

                imnodes::EndNodeTitleBar();

                if (IsItemClicked() && IsMouseDoubleClicked(0))
                    std::any_cast<NodeUserData&>(node->userData()).inputId = node->id();
                
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
                            stbsp_sprintf(buffer, "##inputattribute%.*u", 4, inputAttribute.id);
                            
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
                            stbsp_sprintf(buffer, "##outputattribute%.*u", 4, outputAttribute.id);

                            if (hasInputPin)
                                SameLine();

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
                
                if (BeginPopupContextItem())
                {
                    if (Selectable("Add Input"))
                    {
                        if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
                        {
                            stbsp_sprintf(buffer, "In %u", inOutNode->inputAttributes().size());
                            inOutNode->inputAttributes().emplace_back(Attribute{++myNodeGraph.uniqueId, buffer});
                        }
                    }
                    if (Selectable("Add Output"))
                    {
                        if (auto inOutNode = std::dynamic_pointer_cast<InputOutputNode>(node))
                        {
                            stbsp_sprintf(buffer, "Out %u", inOutNode->outputAttributes().size());
                            inOutNode->outputAttributes().emplace_back(Attribute{++myNodeGraph.uniqueId, buffer});
                        }
                    }
                    EndPopup();
                }
            }
        
            for (int linkIt = 0; linkIt < myNodeGraph.links.size(); linkIt++)
                imnodes::Link(linkIt, myNodeGraph.links[linkIt].fromId, myNodeGraph.links[linkIt].toId);
            
            imnodes::EndNodeEditor();

            int hoveredNodeId;
            if (/*imnodes::IsEditorHovered() && */
                !imnodes::IsNodeHovered(&hoveredNodeId) &&
                BeginPopupContextItem("Node Editor Context Menu"))
            {
                ImVec2 clickPos = GetMousePosOnOpeningCurrentPopup();

                enum class NodeType { SlangShaderNode };
                static constexpr std::pair<NodeType, std::string_view> menuItems[] = { { NodeType::SlangShaderNode, "Slang Shader"} };

                for (const auto& menuItem : menuItems)
                {
                    if (Selectable(menuItem.second.data()))
                    {
                        int id = ++myNodeGraph.uniqueId;
                        imnodes::SetNodeScreenSpacePos(id, clickPos);
                        myNodeGraph.nodes.emplace_back([&menuItem, &id]() -> std::shared_ptr<INode> 
                        {
                            switch (menuItem.first)
                            {
                                case NodeType::SlangShaderNode:
                                    return std::make_shared<SlangShaderNode>(SlangShaderNode(id, std::string(menuItem.second.data()), {}));
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
                    myOpenFileFuture = std::async(std::launch::async, openFileDialogue, myResourcePath, "obj", loadModel);
                if (MenuItem("Open Image...") && !myOpenFileFuture.valid())
                    myOpenFileFuture = std::async(std::launch::async, openFileDialogue, myResourcePath, "jpg,png", loadImage);
                Separator();
                if (MenuItem("Exit", "CTRL+Q"))
                    myRequestExit = true;
                    
                ImGui::EndMenu();
            }
            if (BeginMenu("View"))
            {
                if (MenuItem("Node Editor..."))
                    showNodeEditor = !showNodeEditor;
                if (MenuItem("Statistics..."))
                    showStatistics = !showStatistics;
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
}

template <>
Application<Vk>::~Application()
{
    ZoneScopedN("~Application()");

    {
        ZoneScopedN("Application::deviceWaitIdle");

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
void Application<Vk>::onMouse(const MouseState& state)
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

    auto& swapchain = myWindow->getSwapchain();

    auto [flipSuccess, frameTimelineValue] = swapchain->flip();

    if (flipSuccess)
    {
        std::future<void> imguiPrepareDrawFuture(std::async(std::launch::async, [this]
        {
            myIMGUIPrepareDrawFunction();
        }));

        if (frameTimelineValue)
        {
            ZoneScopedN("Application::waitFrameGPUCommands");

            myDevice->wait(frameTimelineValue);
        }

        auto& commandContext = myWindow->commandContext(swapchain->getFrameIndex());
        {
            auto cmd = commandContext->commands();

            myRenderImageSet->clearDepthStencil(cmd, { 1.0f, 0 });
            myRenderImageSet->transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
                
            myWindow->updateInput(myInput);
            myWindow->draw(myGraphicsPipeline);

            swapchain->begin(cmd, VK_SUBPASS_CONTENTS_INLINE);
            {
                ZoneScopedN("Application::waitImguiPrepareDraw");

                imguiPrepareDrawFuture.get();
            }
            myIMGUIDrawFunction(cmd);
            swapchain->end(cmd);

            swapchain->transitionColor(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);
        }
        
        {
            auto [imageAquired, renderComplete] = swapchain->getFrameSyncSemaphores();
            
            myGraphicsQueue->enqueue(
                commandContext->flush({
                    {myDevice->getTimelineSemaphore(), imageAquired},
                    {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
                    {std::max(myLastTransferTimelineValue, myLastFrameTimelineValue), 1},
                    {myDevice->getTimelineSemaphore(), renderComplete},
                    {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed), 1}}));

            myLastFrameTimelineValue = myGraphicsQueue->submit();
        }
    }

    auto processTimelineCallbacksFuture(
        std::async(
            std::launch::async,
            &Application<Vk>::processTimelineCallbacks,
            this,
            frameTimelineValue));

    if (flipSuccess)
        swapchain->present(myLastFrameTimelineValue);

    // wait for timeline callbacks
    {
        ZoneScopedN("Application::waitProcessTimelineCallbacks");

        processTimelineCallbacksFuture.get();
    }

    // record and submit transfers
    {
        ZoneScopedN("Application::transfers");

        uint32_t transferCount = 0;

        if (myOpenFileFuture.valid() && is_ready(myOpenFileFuture))
        {
            ZoneScopedN("Application::openFileCallback");

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
            myTransferQueue->enqueue(
                myTransferCommands->flush({
                    {myDevice->getTimelineSemaphore()},
                    {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
                    {std::max(myLastTransferTimelineValue, myLastFrameTimelineValue)},
                    {myDevice->getTimelineSemaphore()},
                    {1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed)}}));
            
            myLastTransferTimelineValue = myTransferQueue->submit();
        }
    }

    return myRequestExit;
}

template <>
void Application<Vk>::resizeFramebuffer(int, int)
{
    ZoneScopedN("Application::resizeFramebuffer");

    {
        ZoneScopedN("Application::waitGPU");

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
