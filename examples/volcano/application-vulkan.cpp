#include "application.h"
#include "command.h"
#include "gfx.h"
#include "file.h"
#include "vk-utils.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <examples/imgui_impl_glfw.h>
#include <examples/imgui_impl_vulkan.h>


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
    initInfo.CheckVkResultFn = CHECK_VK;
    ImGui_ImplVulkan_Init(&initInfo, myWindow->getRenderPass());

    // Upload Fonts
    ImGui_ImplVulkan_CreateFontsTexture(commands);
    
    deviceContext->addGarbageCollectCallback([](uint64_t){
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    });
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
void Application<GraphicsBackend::Vulkan>::createFrameObjects()
{
    ZoneScopedN("createFrameObjects");

    myLastFrameIndex = myDevice->getDesc().swapchainConfiguration->imageCount - 1;
    auto& frame = myWindow->frames()[myLastFrameIndex];
    myDefaultResources->renderTarget = std::static_pointer_cast<RenderTarget<GraphicsBackend::Vulkan>>(frame);

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
Application<GraphicsBackend::Vulkan>::Application(
    void* view, int width, int height, const char* resourcePath, const char* userProfilePath)
    : myResourcePath(resourcePath ? resourcePath : "./resources/")
    , myUserProfilePath(userProfilePath ? userProfilePath : "./.profile/")
{
    ZoneScopedN("Application()");
    
    if (!std::filesystem::exists(myUserProfilePath))
        std::filesystem::create_directory(myUserProfilePath);

    assert(std::filesystem::is_directory(myResourcePath));
    assert(std::filesystem::is_directory(myUserProfilePath));

    std::filesystem::path instanceConfigFile(std::filesystem::absolute(myUserProfilePath / "instance.json"));
    
    myInstance = std::make_shared<InstanceContext<GraphicsBackend::Vulkan>>(
        ScopedFileObject<InstanceConfiguration<GraphicsBackend::Vulkan>>(
            instanceConfigFile,
            "instanceConfiguration"),
        view);

    const auto& graphicsDeviceCandidates = myInstance->getGraphicsDeviceCandidates();
    if (graphicsDeviceCandidates.empty())
        throw std::runtime_error("failed to find a suitable GPU!");

    std::filesystem::path deviceConfigFile(std::filesystem::absolute(myUserProfilePath / "device.json"));

    myDevice = std::make_shared<DeviceContext<GraphicsBackend::Vulkan>>(
        myInstance,
        ScopedFileObject<DeviceConfiguration<GraphicsBackend::Vulkan>>(
            deviceConfigFile,
            "deviceConfiguration",
            {graphicsDeviceCandidates.front().first}));

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        std::filesystem::absolute(myUserProfilePath / "pipeline.cache"),
        myDevice->getDevice(),
        myInstance->getPhysicalDeviceInfos()[myDevice->getDesc().physicalDeviceIndex].deviceProperties);

    myDefaultResources = std::make_shared<GraphicsPipelineResourceView<GraphicsBackend::Vulkan>>();
    myDefaultResources->sampler = createTextureSampler(myDevice->getDevice());

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myDevice->getDevice(), *slangShaders);

    myTransferCommandContext = std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
        myDevice,
        CommandContextCreateDesc<GraphicsBackend::Vulkan>{
            myDevice->getTransferCommandPools()[0][0],
            VK_COMMAND_BUFFER_LEVEL_PRIMARY});
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
                    {3, 2}});
        }

        // submit transfers.
        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
        auto signalTimelineValue = 1 + myDevice->timelineValue()->fetch_add(1, std::memory_order_relaxed);
        myLastTransferTimelineValue = myTransferCommandContext->submit({
            myDevice->getPrimaryTransferQueue(),
            1,
            &myDevice->getTimelineSemaphore(),
            &waitDstStageMask,
            &waitTimelineValue,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});
    }

    createFrameObjects();
    
    // stuff that needs to be initialized on graphics queue
    {
        auto& frame = myWindow->frames()[myLastFrameIndex];
        auto& primaryCommandContext = myWindow->commandContexts()[frame->getDesc().index][0];
        {
            auto primaryCommands = primaryCommandContext->beginScope();
            myDefaultResources->texture->transition(primaryCommands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            initIMGUI(myDevice, primaryCommands, myUserProfilePath);
        }

        myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext =
            TracyVkContext(
                myDevice->getPhysicalDevice(),
                myDevice->getDevice(),
                myDevice->getPrimaryGraphicsQueue(),
                primaryCommandContext->commands());

        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
        auto signalTimelineValue = 1 + myDevice->timelineValue()->fetch_add(1, std::memory_order_relaxed);
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
        CHECK_VK(vkQueueWaitIdle(myDevice->getPrimaryTransferQueue()));

        {
            auto beginScope(myTransferCommandContext->beginScope());

            myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
                myDevice,
                myTransferCommandContext,
                std::filesystem::absolute(openFilePath));
        }

        updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

        // submit transfers.
        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
        auto signalTimelineValue = 1 + myDevice->timelineValue()->fetch_add(1, std::memory_order_relaxed);
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
    //     CHECK_VK(vkQueueWaitIdle(myDevice->getPrimaryTransferQueue()));

    //     {
    //         auto beginScope(myTransferCommandContext->beginScope());

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

        static bool showStatistics = false;
        if (showStatistics)
        {
            if (Begin("Statistics", &showStatistics))
            {
                // ...
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

        // {
        //     Begin("GUI Options");
        //     // static int styleIndex = 0;
        //     ShowStyleSelector("Styles" /*, &styleIndex*/);
        //     ShowFontSelector("Fonts");
        //     if (Button("Show User Guide"))
        //     {
        //         SetNextWindowPos(ImVec2(0.5f, 0.5f));
        //         OpenPopup("UserGuide");
        //     }
        //     if (BeginPopup(
        //             "UserGuide", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        //     {
        //         ShowUserGuide();
        //         EndPopup();
        //     }
        //     End();
        // }  

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open OBJ", "CTRL+O") && !myOpenFileFuture.valid())
                    myOpenFileFuture = std::async(std::launch::async, openFileDialogue, "obj", loadModel);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Statistics..."))
                    showStatistics = !showStatistics;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("About"))
            {
                if (ImGui::MenuItem("About volcano..."))
                    showAbout = !showAbout;
                if (ImGui::MenuItem("Show IMGUI Demo..."))
                    showDemoWindow = !showDemoWindow;
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }  

        // {
        //     Begin("Render Options");
        //     // DragInt(
        //     //     "Command Buffer Threads", &myRequestedCommandBufferThreadCount, 0.1f, 2, 32);
        //     ColorEdit3(
        //         "Clear Color", &myDesc.clearValue.color.float32[0]);
        //     End();
        // }    
    });
}

template <>
Application<GraphicsBackend::Vulkan>::~Application()
{
    ZoneScopedN("~Application()");

    {
        ZoneScopedN("deviceWaitIdle");

        // todo: replace with frame & transfer sync
        CHECK_VK(vkDeviceWaitIdle(myDevice->getDevice()));
    }
    
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();

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
void Application<GraphicsBackend::Vulkan>::draw()
{
    ZoneScopedN("draw");

    auto [flipSuccess, frameIndex] = myWindow->flipFrame(myLastFrameIndex);
    auto& frame = myWindow->frames()[frameIndex];
    uint64_t frameLastSubmitTimelineValue = frame->getLastSubmitTimelineValue();

    myWindow->updateInput(myInput, frameIndex, myLastFrameIndex);

    if (flipSuccess)
    {
        {
            ZoneScopedN("wait");
            
            assert(frameIndex != myLastFrameIndex);

            myDevice->wait(frameLastSubmitTimelineValue);

            for (auto& context : myWindow->commandContexts()[frame->getDesc().index])
                context->reset();
        }
    
        myDefaultResources->renderTarget = frame;

        myLastFrameTimelineValue = myWindow->submitFrame(
            frameIndex,
            myLastFrameIndex,
            *myGraphicsPipelineConfig,
            std::max(myLastTransferTimelineValue, myLastFrameTimelineValue));
    }

    myWindow->presentFrame(frameIndex);

    // todo: launch async work on another queue
    if (myOpenFileFuture.valid() && is_ready(myOpenFileFuture))
    {
        ZoneScopedN("openFileCallback");

        const auto& [openFileResult, openFilePath, onCompletionCallback] = myOpenFileFuture.get();
        if (openFileResult == NFD_OKAY)
            onCompletionCallback(openFilePath);
    }

    if (myLastTransferTimelineValue)
    {
        {
            ZoneScopedN("waitTransfer");

            myDevice->wait(myLastTransferTimelineValue);

            myTransferCommandContext->reset();
        }

        myDevice->collectGarbage(std::min(frameLastSubmitTimelineValue, myLastTransferTimelineValue));
        
        myLastTransferTimelineValue = 0;

        {
            ZoneScopedN("tracyVkCollectTransfer");

            auto& primaryCommandContext = myWindow->commandContexts()[frameIndex][0];
            auto primaryCommands = primaryCommandContext->beginScope();

            TracyVkCollect(
                myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
                primaryCommands);
        }
    }
    else
    {
        myDevice->collectGarbage(frameLastSubmitTimelineValue);
    }

    if (!myTransferCommandContext->isPendingEmpty())
    {
        // submit transfers.
        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t waitTimelineValue = std::max(myLastTransferTimelineValue, myLastFrameTimelineValue);
        auto signalTimelineValue = 1 + myDevice->timelineValue()->fetch_add(1, std::memory_order_relaxed);
        myLastTransferTimelineValue = myTransferCommandContext->submit({
            myDevice->getPrimaryTransferQueue(),
            1,
            &myDevice->getTimelineSemaphore(),
            &waitDstStageMask,
            &waitTimelineValue,
            1,
            &myDevice->getTimelineSemaphore(),
            &signalTimelineValue});
    }

    myLastFrameIndex = frameIndex;
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int, int)
{
    ZoneScopedN("resizeFramebuffer");

    {
        ZoneScopedN("deviceWaitIdle");

        // todo: replace with frame & transfer timline sync
        CHECK_VK(vkDeviceWaitIdle(myDevice->getDevice()));
    }

    destroyFrameObjects();
    myWindow->destroyFrameObjects();
    myWindow->createFrameObjects();
    createFrameObjects();
    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);
}
