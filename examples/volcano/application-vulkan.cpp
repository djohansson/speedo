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

    auto surfaceCapabilities = getSurfaceCapabilities<GraphicsBackend::Vulkan>(myInstance->getSurface(), myDevice->getPhysicalDevice());

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
    initInfo.PipelineCache = myPipelineCache;
    initInfo.DescriptorPool = myDevice->getDescriptorPool();
    initInfo.MinImageCount = myDevice->getDesc().swapchainConfiguration->imageCount;
    initInfo.ImageCount = myDevice->getDesc().swapchainConfiguration->imageCount;
    initInfo.Allocator = nullptr;
    // initInfo.HostAllocationCallbacks = nullptr;
    initInfo.CheckVkResultFn = checkResult;
    ImGui_ImplVulkan_Init(
        &initInfo,
        myDefaultResources->renderTarget->getRenderPass());

    // Upload Fonts
    ImGui_ImplVulkan_CreateFontsTexture(commands);
    
    deviceContext->addGarbageCollectCallback([](uint64_t){
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
void Application<GraphicsBackend::Vulkan>::updateDescriptorSets(
    const Window<GraphicsBackend::Vulkan>& window,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& pipelineConfig) const
{
    ZoneScopedN("updateDescriptorSets");

    // todo: use reflection
    
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = window.getViewBuffer().getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = pipelineConfig.resources->texture->getImageLayout();
    imageInfo.imageView = pipelineConfig.resources->textureView;
    imageInfo.sampler = pipelineConfig.resources->sampler;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
    descriptorWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[0].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    descriptorWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[1].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfo;
    descriptorWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[2].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(
        myDevice->getDevice(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(),
        0, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::createFrameObjects(Extent2d<GraphicsBackend::Vulkan> frameBufferExtent)
{
    ZoneScopedN("createFrameObjects");

    myLastFrameIndex = myDevice->getDesc().swapchainConfiguration->imageCount - 1;

    auto colorTexture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice,
        TextureCreateDesc<GraphicsBackend::Vulkan>{
            {"rtColorTexture"},
            frameBufferExtent,
            myDevice->getDesc().swapchainConfiguration->surfaceFormat.format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    auto depthStencilTexture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice,
        TextureCreateDesc<GraphicsBackend::Vulkan>{
            {"rtDepthTexture"},
            frameBufferExtent,
            findSupportedFormat(
                myDevice->getPhysicalDevice(),
                {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT});
    
    myRenderTexture = std::make_shared<RenderTexture<GraphicsBackend::Vulkan>>(
        myDevice,
        "RenderTexture", 
        make_vector(colorTexture),
        depthStencilTexture);

    myDefaultResources->renderTarget = myRenderTexture;

    // for (all referenced resources/shaders)
    // {
        myGraphicsPipelineConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>();
        *myGraphicsPipelineConfig = createPipelineConfig(
            myDevice->getDevice(),
            myDevice->getDescriptorPool(),
            myPipelineCache,
            myGraphicsPipelineLayout,
            myDefaultResources);
    //}
}

template <>
void Application<GraphicsBackend::Vulkan>::destroyFrameObjects()
{
    ZoneScopedN("destroyFrameObjects");

    vkDestroyPipeline(myDevice->getDevice(), myGraphicsPipelineConfig->graphicsPipeline, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::collectGarbage(uint64_t frameLastSubmitTimelineValue)
{
    if (myLastTransferTimelineValue)
    {
        {
            ZoneScopedN("waitTransfer");

            myDevice->wait(myLastTransferTimelineValue);
        }

        myDevice->collectGarbage(std::min(frameLastSubmitTimelineValue, myLastTransferTimelineValue));
        
        myLastTransferTimelineValue = 0;

        // {
        //     ZoneScopedN("tracyVkCollectTransfer");

        //     auto& primaryCommandContext = myWindow->commandContexts()[frameIndex][0];
        //     auto primaryCommands = primaryCommandContext->beginScope();

        //     TracyVkCollect(
        //         myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
        //         primaryCommands);
        // }
    }
    else
    {
        myDevice->collectGarbage(frameLastSubmitTimelineValue);
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
        ScopedFileObject<InstanceConfiguration<GraphicsBackend::Vulkan>>(
            std::filesystem::absolute(myUserProfilePath / "instance.json"),
            "instanceConfiguration"),
        view);

    const auto& graphicsDeviceCandidates = myInstance->getGraphicsDeviceCandidates();
    if (graphicsDeviceCandidates.empty())
        throw std::runtime_error("failed to find a suitable GPU!");

    myDevice = std::make_shared<DeviceContext<GraphicsBackend::Vulkan>>(
        myInstance,
        ScopedFileObject<DeviceConfiguration<GraphicsBackend::Vulkan>>(
            std::filesystem::absolute(myUserProfilePath / "device.json"),
            "deviceConfiguration",
            {graphicsDeviceCandidates.front().first}));

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        std::filesystem::absolute(myUserProfilePath / "pipeline.cache"),
        myDevice->getDevice(),
        myInstance->getPhysicalDeviceInfos()[myDevice->getDesc().physicalDeviceIndex].deviceProperties);

    myDefaultResources = std::make_shared<GraphicsPipelineResourceView<GraphicsBackend::Vulkan>>();
    myDefaultResources->sampler = createSampler(myDevice->getDevice());

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myDevice->getDevice(), *slangShaders);

    // if (commandBufferLevel == 0)
    //     std::any_cast<command_vulkan::UserData>(&myUserData)->tracyContext =
    //         TracyVkContext(
    //             myDeviceContext->getPhysicalDevice(),
    //             myDeviceContext->getDevice(),
    //             queue,
    //             commands());

    // if (std::any_cast<command_vulkan::UserData>(&myUserData)->tracyContext)
    //     TracyVkDestroy(std::any_cast<command_vulkan::UserData>(&myUserData)->tracyContext);

    myTransferCommandContext = std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
        myDevice,
        CommandContextCreateDesc<GraphicsBackend::Vulkan>{myDevice->getTransferCommandPools()[0][0]});
    {
        {
            auto transferCommands = myTransferCommandContext->beginScope();

            myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
                myDevice,
                myTransferCommandContext,
                std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"));
            myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
                myDevice,
                myTransferCommandContext,
                std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"));
            myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

            myWindow = std::make_shared<Window<GraphicsBackend::Vulkan>>(
                myInstance,
                myDevice,    
                WindowCreateDesc<GraphicsBackend::Vulkan>{
                    {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                    {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                    {3, 2}});
        }

        // submit transfers.
        auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
        myLastTransferTimelineValue = myTransferCommandContext->submit({
            myDevice->getPrimaryTransferQueue(),
            0,
            nullptr,
            nullptr,
            nullptr,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});
    }

    createFrameObjects({static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    
    // stuff that needs to be initialized on graphics queue
    {
        auto& frame = myWindow->frames()[myLastFrameIndex];
        auto& primaryCommandContext = myWindow->commandContexts()[frame->getDesc().index][0];
        {
            auto primaryCommands = primaryCommandContext->beginScope();
            myDefaultResources->texture->transition(primaryCommands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            initIMGUI(myDevice, primaryCommands, myUserProfilePath);
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

    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

    auto openFileDialogue = [this](const nfdchar_t* filterList, std::function<void(nfdchar_t*)>&& onCompletionCallback)
    {
        std::string resourcePath = std::filesystem::absolute(myResourcePath).u8string();
        nfdchar_t* openFilePath;
        return std::make_tuple(NFD_OpenDialog(filterList, resourcePath.c_str(), &openFilePath),
            openFilePath, std::move(onCompletionCallback));
    };

    auto loadModel = [this](nfdchar_t* openFilePath)
    {
        // todo: replace with other sync method. garbage collect resource?
        VK_CHECK(vkQueueWaitIdle(myDevice->getPrimaryTransferQueue()));

        {
            auto transferCommands = myTransferCommandContext->beginScope();

            myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
                myDevice,
                myTransferCommandContext,
                std::filesystem::absolute(openFilePath));
        }

        updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

        // submit transfers.
        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
        auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
        myLastFrameTimelineValue = myTransferCommandContext->submit({
            myDevice->getPrimaryTransferQueue(),
            1,
            &myDevice->getTimelineSemaphore(),
            &waitDstStageMask,
            &waitTimelineValue,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});

        // todo: resource transition?
    };

    // auto loadTexture = [this](nfdchar_t* openFilePath)
    // {
    //     // todo: replace with other sync method
    //     VK_CHECK(vkQueueWaitIdle(myDevice->getPrimaryTransferQueue()));

    //     {
    //         auto transferCommands = myTransferCommandContext->beginScope();

    //         myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
    //             myDevice,
    //             myTransferCommandContext,
    //             std::filesystem::absolute(openFilePath));
    //     }

    //     vkDestroyImageView(myDevice->getDevice(), myDefaultResources->textureView, nullptr);
    //     myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

    //     updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

    //     // submit transfers.
    //     myLastTransferTimelineValue = myTransferCommandContext->submit({
    //         myDevice->getPrimaryTransferQueue()});

    //     // todo: resource transition
    // };

    myWindow->addIMGUIDrawCallback([this, openFileDialogue, loadModel]
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
                            PopClipRect();
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
                Text("Fences: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_FENCE));
                Text("Semaphores: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_SEMAPHORE));
                Text("Command Buffers: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_COMMAND_BUFFER));
                Text("Buffers: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_BUFFER));
                Text("Buffer Views: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_BUFFER_VIEW));
                Text("Images: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_IMAGE));
                Text("Image Views: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_IMAGE_VIEW));
                Text("Framebuffers: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_FRAMEBUFFER));
                Text("Command Pools: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_COMMAND_POOL));
                Text("Surfaces: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_SURFACE_KHR));
                Text("Swapchains: %u", DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(VK_OBJECT_TYPE_SWAPCHAIN_KHR));
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
                    auto rowCount = std::max(inOutNode->inputAttributes().size(), inOutNode->inputAttributes().size());
                    for (uint32_t rowIt = 0; rowIt < rowCount; rowIt++)
                    {
                        float inputTextWidth = 0.0f;
                        if (rowIt < inOutNode->inputAttributes().size())
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

                            imnodes::BeginOutputAttribute(outputAttribute.id);

                            float outputTextWidth = CalcTextSize(
                                outputAttribute.name.c_str(),
                                outputAttribute.name.c_str() + outputAttribute.name.size()).x;

                            // if (rowIt < inOutNode->inputAttributes().size())
                            //     ImGui::SameLine();

                            Indent(std::max(titleBarTextWidth, inputTextWidth + outputTextWidth) - outputTextWidth);

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
                if (ImGui::MenuItem("Open OBJ...", "CTRL+O") && !myOpenFileFuture.valid())
                    myOpenFileFuture = std::async(std::launch::async, openFileDialogue, "obj", loadModel);
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

    savePipelineCache<GraphicsBackend::Vulkan>(
        myUserProfilePath / "pipeline.cache",
        myDevice->getDevice(),
        myInstance->getPhysicalDeviceInfos()[myDevice->getDesc().physicalDeviceIndex].deviceProperties,
        myPipelineCache);
    
    vkDestroyPipelineCache(myDevice->getDevice(), myPipelineCache, nullptr);
    vkDestroyPipelineLayout(myDevice->getDevice(), myGraphicsPipelineLayout->layout, nullptr);

#ifdef PROFILING_ENABLE
    char* allocatorStatsJSON = nullptr;
    vmaBuildStatsString(myDevice->getAllocator(), &allocatorStatsJSON, true);
    std::cout << allocatorStatsJSON << std::endl;
    vmaFreeStatsString(myDevice->getAllocator(), allocatorStatsJSON);
#endif

    destroyFrameObjects();

    // todo: work on a resourcetable of some sort, and automatically delete all resources from it.
    vkDestroyImageView(myDevice->getDevice(), myDefaultResources->textureView, nullptr);
    vkDestroySampler(myDevice->getDevice(), myDefaultResources->sampler, nullptr);
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

    auto [flipSuccess, frameIndex] = myWindow->flipFrame(myLastFrameIndex);
    auto& frame = myWindow->frames()[frameIndex];
    uint64_t frameLastSubmitTimelineValue = frame->getLastSubmitTimelineValue();

    if (flipSuccess)
    {
        {
            ZoneScopedN("wait");
            
            assert(frameIndex != myLastFrameIndex);

            myDevice->wait(frameLastSubmitTimelineValue);
        }

        myWindow->updateInput(myInput, frameIndex, myLastFrameIndex);
    
        myDefaultResources->renderTarget = myRenderTexture;

        myLastFrameTimelineValue = myWindow->submitFrame(
            frameIndex,
            myLastFrameIndex,
            *myGraphicsPipelineConfig,
            std::max(myLastTransferTimelineValue, myLastFrameTimelineValue));
    }

    auto garbageCollectFuture(
        std::async(
            std::launch::async,
            &Application<GraphicsBackend::Vulkan>::collectGarbage,
            this,
            frameLastSubmitTimelineValue));

    if (flipSuccess)
        myWindow->presentFrame(frameIndex);

    // wait for garbage collect
    {
        ZoneScopedN("waitGarbageCollect");

        garbageCollectFuture.get();
    }

    // record transfers
    if (myOpenFileFuture.valid() && is_ready(myOpenFileFuture))
    {
        ZoneScopedN("openFileCallback");

        const auto& [openFileResult, openFilePath, onCompletionCallback] = myOpenFileFuture.get();
        if (openFileResult == NFD_OKAY)
            onCompletionCallback(openFilePath);
    }

    // submit transfers.
    Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
    auto signalTimelineValue = 1 + myDevice->timelineValue().fetch_add(1, std::memory_order_relaxed);
    myLastTransferTimelineValue = myTransferCommandContext->submit({
        myDevice->getPrimaryTransferQueue(),
        1,
        &myDevice->getTimelineSemaphore(),
        &waitDstStageMask,
        &waitTimelineValue,
        1,
        &myDevice->getTimelineSemaphore(),
        &signalTimelineValue});

    myLastFrameIndex = frameIndex;

    return myRequestExit;
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int, int)
{
    ZoneScopedN("resizeFramebuffer");

    {
        ZoneScopedN("deviceWaitIdle");

        // todo: replace with frame & transfer timline sync
        VK_CHECK(vkDeviceWaitIdle(myDevice->getDevice()));
    }

    uint32_t physicalDeviceIndex = myDevice->getDesc().physicalDeviceIndex;
    myInstance->updateSurfaceCapabilities(physicalDeviceIndex);
    auto frameBufferExtent = 
        myInstance->getPhysicalDeviceInfos()[physicalDeviceIndex].swapchainInfo.capabilities.currentExtent;

    destroyFrameObjects();
    myWindow->destroyFrameObjects();
    myWindow->createFrameObjects(frameBufferExtent);
    createFrameObjects(frameBufferExtent);
    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);
}
