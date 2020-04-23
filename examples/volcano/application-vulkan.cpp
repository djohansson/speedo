#include "application.h"
#include "command-vulkan.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <examples/imgui_impl_vulkan.h>

#include <Tracy.hpp>

template <>
void Application<GraphicsBackend::Vulkan>::initIMGUI(
    CommandContext<GraphicsBackend::Vulkan>& commandContext,
    float dpiScaleX,
    float dpiScaleY) const
{
    using namespace ImGui;

    IMGUI_CHECKVERSION();
    CreateContext();
    auto& io = GetIO();
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

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
    initInfo.PhysicalDevice = myGraphicsDevice->getPhysicalDevice();
    initInfo.Device = myGraphicsDevice->getDevice();
    initInfo.QueueFamily = *myGraphicsDevice->getSelectedQueueFamilyIndex();
    initInfo.Queue = myGraphicsDevice->getSelectedQueue();
    initInfo.PipelineCache = myPipelineCache;
    initInfo.DescriptorPool = myGraphicsDevice->getDescriptorPool();
    initInfo.MinImageCount = myGraphicsDevice->getSwapchainConfiguration().selectedImageCount;
    initInfo.ImageCount = myGraphicsDevice->getSwapchainConfiguration().selectedImageCount;
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
    // todo: use reflection
    
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = window.getViewBuffer().getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
        myGraphicsDevice->getDevice(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(),
        0, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::createFrameObjects(CommandContext<GraphicsBackend::Vulkan>& commandContext)
{
    myWindow->createFrameObjects(commandContext);

    // for (all referenced resources/shaders)
    // {
        myGraphicsPipelineConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>(
            createPipelineConfig(
                myGraphicsDevice->getDevice(),
                myGraphicsDevice->getDescriptorPool(),
                myPipelineCache,
                myGraphicsPipelineLayout,
                myDefaultResources));
    //}

    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);
}

template <>
void Application<GraphicsBackend::Vulkan>::destroyFrameObjects()
{
    myWindow->destroyFrameObjects();

    vkDestroyPipeline(myGraphicsDevice->getDevice(), myGraphicsPipelineConfig->graphicsPipeline, nullptr);
}

template <>
Application<GraphicsBackend::Vulkan>::Application(void* view, int width, int height,
    int framebufferWidth, int framebufferHeight, const char* resourcePath)
    : myResourcePath(resourcePath)
{
    assert(std::filesystem::is_directory(myResourcePath));

    myInstance = std::make_shared<InstanceContext<GraphicsBackend::Vulkan>>(
        InstanceDesc<GraphicsBackend::Vulkan>{
            { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "volcano-vk", VK_MAKE_VERSION(1, 0, 0), "kiss", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_2 },
            view});

    myGraphicsDevice = std::make_shared<DeviceContext<GraphicsBackend::Vulkan>>(
        DeviceDesc<GraphicsBackend::Vulkan>{myInstance});

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        std::filesystem::absolute(myResourcePath / ".cache" / "pipeline.cache"),
        myGraphicsDevice->getDevice(),
        myGraphicsDevice->getPhysicalDeviceProperties());

    myDefaultResources = std::make_shared<GraphicsPipelineResourceView<GraphicsBackend::Vulkan>>();
    myDefaultResources->sampler = createTextureSampler(myGraphicsDevice->getDevice());

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myGraphicsDevice->getDevice(), *slangShaders);

    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreCreateInfo.pNext = &timelineCreateInfo;
    semaphoreCreateInfo.flags = 0;

    CHECK_VK(vkCreateSemaphore(myGraphicsDevice->getDevice(), &semaphoreCreateInfo, NULL, &myTimelineSemaphore));
    myTimelineValue = std::make_shared<std::atomic_uint64_t>();

    myTransferCommandContext = std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
        CommandContextDesc<GraphicsBackend::Vulkan>{
            myGraphicsDevice,
            myGraphicsDevice->getTransferCommandPool(),
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            myTimelineSemaphore,
            myTimelineValue});

#ifdef PROFILING_ENABLED
    myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext =
        TracyVkContext(
            myGraphicsDevice->getPhysicalDevice(),
            myGraphicsDevice->getDevice(),
            myGraphicsDevice->getSelectedQueue(),
            myTransferCommandContext->commands());        
#endif

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK_VK(vkCreateFence(myGraphicsDevice->getDevice(), &fenceInfo, nullptr, &myLastTransferSubmitFence));

    {
        auto transferCommands = myTransferCommandContext->beginEndScope();

#ifdef PROFILING_ENABLED
        TracyVkZone(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            transferCommands, "transfer");
#endif

        myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
            std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"),
            *myTransferCommandContext);
        myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
            std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"),
            *myTransferCommandContext);
        myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

        myWindow = std::make_shared<Window<GraphicsBackend::Vulkan>>(
            WindowDesc<GraphicsBackend::Vulkan>{
                myGraphicsDevice,
                myTimelineSemaphore,
                myTimelineValue,
                {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                {static_cast<uint32_t>(framebufferWidth), static_cast<uint32_t>(framebufferHeight)},
                {4, 4},
                true,
                {},
                true},
            *myTransferCommandContext);

        initIMGUI(
            *myTransferCommandContext,
            static_cast<float>(framebufferWidth) / width,
            static_cast<float>(framebufferHeight) / height);
    }

    // vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
    myLastFrameIndex = myWindow->getFrames().size() - 1;

    // temp temp temp

    myDefaultResources->renderTarget = &myWindow->getFrames()[0];

    // for (all referenced resources/shaders)
    // {
        myGraphicsPipelineConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>();
        *myGraphicsPipelineConfig = createPipelineConfig(
            myGraphicsDevice->getDevice(),
            myGraphicsDevice->getDescriptorPool(),
            myPipelineCache,
            myGraphicsPipelineLayout,
            myDefaultResources);
    //}

    updateDescriptorSets(*myWindow, *myGraphicsPipelineConfig);

    // temp temp temp
}

template <>
Application<GraphicsBackend::Vulkan>::~Application()
{
    {   
        // todo: replace with frame & transfer sync
        CHECK_VK(vkDeviceWaitIdle(myGraphicsDevice->getDevice()));
    }

    {
        vkDestroyFence(myGraphicsDevice->getDevice(), myLastTransferSubmitFence, nullptr);

        myTransferCommandContext.reset();
    }

    destroyFrameObjects();

    vkDestroySemaphore(myGraphicsDevice->getDevice(), myTimelineSemaphore, nullptr);

    std::filesystem::path cacheFilePath = std::filesystem::absolute(myResourcePath / ".cache");

    if (!std::filesystem::exists(cacheFilePath))
        std::filesystem::create_directory(cacheFilePath);

    savePipelineCache<GraphicsBackend::Vulkan>(
        cacheFilePath / "pipeline.cache",
        myGraphicsDevice->getDevice(), myGraphicsDevice->getPhysicalDeviceProperties(), myPipelineCache);

    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();

    {
        myDefaultResources->model.reset();
        vkDestroyImageView(myGraphicsDevice->getDevice(), myDefaultResources->textureView, nullptr);
        myDefaultResources->texture.reset();
    }

    vkDestroyPipelineCache(myGraphicsDevice->getDevice(), myPipelineCache, nullptr);
    vkDestroyPipelineLayout(myGraphicsDevice->getDevice(), myGraphicsPipelineLayout->layout, nullptr);

    // todo: wrap these in a deleter.
    myGraphicsPipelineLayout->shaders.reset();
    myGraphicsPipelineLayout->descriptorSetLayouts.reset();

    vkDestroySampler(myGraphicsDevice->getDevice(), myDefaultResources->sampler, nullptr);

    char* allocatorStatsJSON = nullptr;
    vmaBuildStatsString(myGraphicsDevice->getAllocator(), &allocatorStatsJSON, true);
    std::cout << allocatorStatsJSON << std::endl;
    vmaFreeStatsString(myGraphicsDevice->getAllocator(), allocatorStatsJSON);

    myGraphicsDevice.reset();
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
    FrameMark;

#ifdef PROFILING_ENABLED
    {
        ZoneScopedN("tracyVkCollect (transfer)");

        auto cmd = myTransferCommandContext->beginEndScope();

        TracyVkZone(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            cmd, "tracyVkCollect (transfer)");

        TracyVkCollect(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            cmd);
    }
#endif

    {
        ZoneScopedN("submit (transfer)");

        // submit transfers.
        myAppTransferTimelineValue = myTransferCommandContext->submit({
            myGraphicsDevice->getSelectedQueue(),
            myLastTransferSubmitFence});
    }

    auto [flipSuccess, frameIndex] = myWindow->flipFrame(myLastFrameIndex);
    
    myWindow->updateInput(myInput, frameIndex, myLastFrameIndex);
    
    if (flipSuccess)
    {
        myDefaultResources->renderTarget = &myWindow->getFrames()[frameIndex];

        myWindow->submitFrame(frameIndex, myLastFrameIndex, *myGraphicsPipelineConfig);
        myWindow->presentFrame(frameIndex);

        myLastFrameIndex = frameIndex;
    }

    // sync transfers. todo: pass in all fences to one frame sync location?
    CHECK_VK(vkWaitForFences(myGraphicsDevice->getDevice(), 1, &myLastTransferSubmitFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(myGraphicsDevice->getDevice(), 1, &myLastTransferSubmitFence));
    
    // collect garbage
    myTransferCommandContext->collectGarbage();
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int width, int height)
{
    ZoneScopedN("resizeFramebuffer");

    {
        // todo: replace with frame & transfer sync
        CHECK_VK(vkQueueWaitIdle(myGraphicsDevice->getSelectedQueue()));
    }

    destroyFrameObjects();

    {
        auto cmd = myTransferCommandContext->beginEndScope();

#ifdef PROFILING_ENABLED
        TracyVkZone(
            myTransferCommandContext->userData<command_vulkan::UserData>().tracyContext,
            cmd, "transfer");
#endif
        
        createFrameObjects(*myTransferCommandContext);
    }

    myAppTransferTimelineValue = myTransferCommandContext->submit({
        myGraphicsDevice->getSelectedQueue(),
        myLastTransferSubmitFence});
}
