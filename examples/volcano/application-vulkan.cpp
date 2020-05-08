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

#include <Tracy.hpp>

template <>
void Application<GraphicsBackend::Vulkan>::initIMGUI(
    CommandContext<GraphicsBackend::Vulkan>& commandContext) const
{
    ZoneScopedN("initIMGUI");

    using namespace ImGui;

    IMGUI_CHECKVERSION();
    CreateContext();
    auto& io = GetIO();
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    float dpiScaleX = 
        static_cast<float>(myWindow->getDesc().framebufferExtent.width) / myWindow->getDesc().windowExtent.width;
    float dpiScaleY = 
        static_cast<float>(myWindow->getDesc().framebufferExtent.height) / myWindow->getDesc().windowExtent.height;

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
    ImGui_ImplVulkan_CreateFontsTexture(commandContext.commands());
    
    commandContext.addGarbageCollectCallback([](uint64_t){ ImGui_ImplVulkan_DestroyFontUploadObjects(); });
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
void Application<GraphicsBackend::Vulkan>::createFrameObjects(CommandContext<GraphicsBackend::Vulkan>& commandContext)
{
    ZoneScopedN("createFrameObjects");

    myWindow->createFrameObjects(commandContext);

    // for (all referenced resources/shaders)
    // {
        myGraphicsPipelineConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>(
            createPipelineConfig(
                myDevice->getDevice(),
                myDevice->getDescriptorPool(),
                myPipelineCache,
                myGraphicsPipelineLayout,
                myDefaultResources));
    //}

    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);
}

template <>
void Application<GraphicsBackend::Vulkan>::destroyFrameObjects()
{
    ZoneScopedN("destroyFrameObjects");

    myWindow->destroyFrameObjects();

    vkDestroyPipeline(myDevice->getDevice(), myGraphicsPipelineConfig->graphicsPipeline, nullptr);
}

template <>
Application<GraphicsBackend::Vulkan>::Application(void* view, int width, int height,
    int framebufferWidth, int framebufferHeight, const char* resourcePath, const char* userProfilePath)
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
        InstanceCreateDesc<GraphicsBackend::Vulkan>{
            ScopedFileObject<InstanceConfiguration<GraphicsBackend::Vulkan>>(
                instanceConfigFile,
                "instanceConfiguration"),
            view});

    const auto& graphicsDeviceCandidates = myInstance->getGraphicsDeviceCandidates();
    if (graphicsDeviceCandidates.empty())
        throw std::runtime_error("failed to find a suitable GPU!");

    std::filesystem::path deviceConfigFile(std::filesystem::absolute(myUserProfilePath / "device.json"));

    myDevice = std::make_shared<DeviceContext<GraphicsBackend::Vulkan>>(
        DeviceCreateDesc<GraphicsBackend::Vulkan>{
            ScopedFileObject<DeviceConfiguration<GraphicsBackend::Vulkan>>(
                deviceConfigFile,
                "deviceConfiguration",
                {graphicsDeviceCandidates.front().first}),
            myInstance});

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        std::filesystem::absolute(myUserProfilePath / "pipeline.cache"),
        myDevice->getDevice(),
        myInstance->getPhysicalDeviceInfos()[myDevice->getDesc().physicalDeviceIndex].deviceProperties);

    myDefaultResources = std::make_shared<GraphicsPipelineResourceView<GraphicsBackend::Vulkan>>();
    myDefaultResources->sampler = createTextureSampler(myDevice->getDevice());

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myDevice->getDevice(), *slangShaders);

    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;

    bool useTimelineSemaphores = myDevice->getDesc().useTimelineSemaphores.value();

    VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreCreateInfo.pNext = useTimelineSemaphores ? &timelineCreateInfo : nullptr;
    semaphoreCreateInfo.flags = 0;

    CHECK_VK(vkCreateSemaphore(
        myDevice->getDevice(), &semaphoreCreateInfo, nullptr, &myTimelineSemaphore));

    myTimelineValue = std::make_shared<std::atomic_uint64_t>();

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK_VK(vkCreateFence(myDevice->getDevice(), &fenceInfo, nullptr, &myTransferFence));

    myTransferCommandContext = std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
        CommandContextCreateDesc<GraphicsBackend::Vulkan>{
            myDevice,
            myDevice->getTransferCommandPools()[0][0],
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            useTimelineSemaphores ? myTimelineSemaphore : 0,
            useTimelineSemaphores ? myTimelineValue : nullptr});
    {
        {
            auto transferCommands = myTransferCommandContext->beginEndScope();

            myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
                std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"),
                *myTransferCommandContext);
            myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
                std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"),
                *myTransferCommandContext);
            myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

            myWindow = std::make_shared<Window<GraphicsBackend::Vulkan>>(
                WindowCreateDesc<GraphicsBackend::Vulkan>{
                    myDevice,
                    myTimelineSemaphore,
                    myTimelineValue,
                    {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                    {static_cast<uint32_t>(framebufferWidth), static_cast<uint32_t>(framebufferHeight)},
                    {2, 1}},
                *myTransferCommandContext);
        }

        // submit transfers.
        myLastTransferTimelineValue = myTransferCommandContext->submit({
            myDevice->getPrimaryTransferQueue(),
            myTransferFence});
    }

    myLastFrameIndex = myDevice->getDesc().swapchainConfiguration->imageCount - 1;
    myWindow->waitFrame(myLastFrameIndex);
    auto& frame = myWindow->frames()[myLastFrameIndex];
    myDefaultResources->renderTarget = std::static_pointer_cast<RenderTarget<GraphicsBackend::Vulkan>>(frame);

    // resource transitions, etc
    {
        auto& primaryCommandContext = frame->commandContexts()[0];
        {
            auto primaryCommands = primaryCommandContext->beginEndScope();
            myWindow->depthTexture().transition(primaryCommands, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            myDefaultResources->texture->transition(primaryCommands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            initIMGUI(*primaryCommandContext);
        }

        myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext =
            TracyVkContext(
                myDevice->getPhysicalDevice(),
                myDevice->getDevice(),
                myDevice->getPrimaryGraphicsQueue(),
                primaryCommandContext->commands());

        Flags<GraphicsBackend::Vulkan> waitDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        myLastFrameTimelineValue = primaryCommandContext->submit({
            myDevice->getPrimaryGraphicsQueue(),
            frame->getFence(),
            0,
            nullptr,
            1,
            &myTimelineSemaphore,
            &waitDstStageMask,
            &myLastTransferTimelineValue,
        });
    }

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

    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

    myWindow->addIMGUIDrawCallback([this]
    {
        auto openFileDialogue = [this](const nfdchar_t* filterList, std::function<void(nfdchar_t*)>&& onCompletionCallback)
        {
            std::string resourcePath = std::filesystem::absolute(myResourcePath).u8string();
            nfdchar_t* openFilePath;
            return std::make_tuple(NFD_OpenDialog(filterList, resourcePath.c_str(), &openFilePath),
                openFilePath, std::move(onCompletionCallback));
        };

        auto loadModel = [this](nfdchar_t* openFilePath)
        {
            // todo: replace with other sync method
            CHECK_VK(vkQueueWaitIdle(myDevice->getPrimaryTransferQueue()));

            {
                auto beginEndScope(myTransferCommandContext->beginEndScope());

                myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
                    std::filesystem::absolute(openFilePath),
                    *myTransferCommandContext);
            }

            updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

            // submit transfers.
            myLastTransferTimelineValue = myTransferCommandContext->submit({
                myDevice->getPrimaryTransferQueue(),
                myTransferFence});

            // todo: resource transition
        };

        auto loadTexture = [this](nfdchar_t* openFilePath)
        {
            // todo: replace with other sync method
            CHECK_VK(vkQueueWaitIdle(myDevice->getPrimaryTransferQueue()));

            {
                auto beginEndScope(myTransferCommandContext->beginEndScope());

                myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
                    std::filesystem::absolute(openFilePath),
                    *myTransferCommandContext);
            }

            vkDestroyImageView(myDevice->getDevice(), myDefaultResources->textureView, nullptr);
            myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

            updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

            // submit transfers.
            myLastTransferTimelineValue = myTransferCommandContext->submit({
                myDevice->getPrimaryTransferQueue(),
                myTransferFence});

            // todo: resource transition
        };

        using namespace ImGui;

        Begin("File");

        if (Button("Open OBJ file") && !myOpenFileFuture.valid())
            myOpenFileFuture = std::async(std::launch::async, openFileDialogue, "obj", loadModel);

        if (Button("Open JPG file") && !myOpenFileFuture.valid())
            myOpenFileFuture = std::async(std::launch::async, openFileDialogue, "jpg", loadTexture);

        End();
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

    myTransferCommandContext->clear();
    vkDestroyFence(myDevice->getDevice(), myTransferFence, nullptr);

    destroyFrameObjects();

    {
        myDefaultResources->texture.reset();
        myDefaultResources->model.reset();
        myDefaultResources->renderTarget.reset();
        vkDestroyImageView(myDevice->getDevice(), myDefaultResources->textureView, nullptr);
        vkDestroySampler(myDevice->getDevice(), myDefaultResources->sampler, nullptr);
    }

    vkDestroySemaphore(myDevice->getDevice(), myTimelineSemaphore, nullptr);

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
    
    myWindow->updateInput(myInput, frameIndex, myLastFrameIndex);
    
    if (flipSuccess)
    {
        myWindow->waitFrame(frameIndex);

        myDefaultResources->renderTarget = myWindow->frames()[frameIndex];

        myLastFrameTimelineValue = myWindow->submitFrame(frameIndex,
            myLastFrameIndex,
            *myGraphicsPipelineConfig,
            myLastTransferTimelineValue);

        myLastFrameIndex = frameIndex;
    }

    if (!myTransferCommandContext->isSubmittedEmpty())
    {
        // collect garbage
        myTransferCommandContext->collectGarbage(
            myLastTransferTimelineValue,
            myTransferFence);
    }

    {
        ZoneScopedN("tracyVkCollectTransfer");

        auto& frame = myWindow->frames()[frameIndex];
        auto& primaryCommandContext = frame->commandContexts()[0];
        auto primaryCommands = primaryCommandContext->beginEndScope();

        TracyVkZone(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            primaryCommands, "tracyVkCollectTransfer");

        TracyVkCollect(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            primaryCommands);
    }

    // todo: launch async work on another queue
    if (myOpenFileFuture.valid() && is_ready(myOpenFileFuture))
    {
        ZoneScopedN("openFileCallback");

        const auto& [openFileResult, openFilePath, onCompletionCallback] = myOpenFileFuture.get();
        if (openFileResult == NFD_OKAY)
            onCompletionCallback(openFilePath);
    }

    if (!myTransferCommandContext->isPendingEmpty())
    {
        // submit transfers.
        VkPipelineStageFlags waitDstStageMasks = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        myLastTransferTimelineValue = myTransferCommandContext->submit({
            myDevice->getPrimaryTransferQueue(),
            myTransferFence,
            0,
            nullptr,
            1,
            &myTimelineSemaphore,
            &waitDstStageMasks,
            &myLastFrameTimelineValue});
    }

    myWindow->presentFrame(frameIndex);
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int width, int height)
{
    ZoneScopedN("resizeFramebuffer");

    {
        ZoneScopedN("deviceWaitIdle");

        // todo: replace with frame & transfer sync
        CHECK_VK(vkDeviceWaitIdle(myDevice->getDevice()));
    }

    destroyFrameObjects();

    {
        auto cmd = myTransferCommandContext->beginEndScope();

        TracyVkZone(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            cmd, "transfer");
        
        createFrameObjects(*myTransferCommandContext);
    }

    // submit transfers.
    myLastTransferTimelineValue = myTransferCommandContext->submit({
        myDevice->getPrimaryTransferQueue(),
        myTransferFence});
}
