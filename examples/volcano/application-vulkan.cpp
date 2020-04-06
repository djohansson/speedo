#include "application.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>


template <>
void Application<GraphicsBackend::Vulkan>::initIMGUI(
    Window<GraphicsBackend::Vulkan>& window,
    float dpiScaleX,
    float dpiScaleY) const
{
    ZoneScoped;

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
    initInfo.DescriptorPool = myDescriptorPool;
    initInfo.MinImageCount = window.swapchain->getDesc().configuration.selectedImageCount;
    initInfo.ImageCount = window.swapchain->getDesc().configuration.selectedImageCount;
    initInfo.Allocator = nullptr; // myAllocator;
    // initInfo.HostAllocationCallbacks = nullptr;
    initInfo.CheckVkResultFn = CHECK_VK;
    ImGui_ImplVulkan_Init(&initInfo, myGraphicsPipelineConfig->renderPass);

    // Upload Fonts
    {
        auto commandBuffer = beginSingleTimeCommands(myGraphicsDevice->getDevice(), myTransferCommandPool);

        ZoneScopedN("uploadFontTexture");
        //TracyVkZone(myTracyContext, commandBuffer, "uploadFontTexture");

        ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
        endSingleTimeCommands(myGraphicsDevice->getDevice(), myGraphicsDevice->getSelectedQueue(), commandBuffer, myTransferCommandPool);
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    }
}

template <>
void Application<GraphicsBackend::Vulkan>::updateDescriptorSets(
    const Window<GraphicsBackend::Vulkan>& window,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& pipelineConfig) const
{
    // todo: use reflection
    
    ZoneScoped;

    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = window.viewBuffer->getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = pipelineConfig.resources->textureView;
    imageInfo.sampler = pipelineConfig.resources->sampler;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = pipelineConfig.descriptorSets[0];
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfo;
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
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
void Application<GraphicsBackend::Vulkan>::createState(
    Window<GraphicsBackend::Vulkan>& window)
{
    ZoneScoped;

    window.views.resize(NX * NY);
    for (auto& view : window.views)
    {
        if (!view.viewport.width)
            view.viewport.width = window.framebufferWidth / NX;

        if (!view.viewport.height)
            view.viewport.height = window.framebufferHeight / NY;

        updateViewMatrix(view);
        updateProjectionMatrix(view);
    }

    assert(window.colorImages.empty());
    assert(window.colorViews.empty());
    window.colorImages = window.swapchain->getColorImages();
    window.colorViews.resize(window.colorImages.size());
    for (uint32_t i = 0; i < window.colorImages.size(); i++)
        window.colorViews[i] = createImageView2D(
            myGraphicsDevice->getDevice(),
            window.colorImages[i],
            window.swapchain->getDesc().configuration.selectedFormat().format,
            VK_IMAGE_ASPECT_COLOR_BIT);

    assert(window.depthView == 0);
    window.depthView = window.zBuffer->createView(VK_IMAGE_ASPECT_DEPTH_BIT);

    // for (all referenced resources/shaders)
    // {
        myGraphicsPipelineConfig = std::make_shared<PipelineConfiguration<GraphicsBackend::Vulkan>>();
        *myGraphicsPipelineConfig = createPipelineConfig(
            myGraphicsDevice->getDevice(),
            createRenderPass(myGraphicsDevice->getDevice(), window),
            myDescriptorPool,
            myPipelineCache,
            myGraphicsPipelineLayout,
            myDefaultResources);
    //}

    window.clearEnable = true;
    window.clearValue.color.float32[0] = 0.4f;
    window.clearValue.color.float32[1] = 0.4f;
    window.clearValue.color.float32[2] = 0.5f;
    window.clearValue.color.float32[3] = 1.0f;

    myFrameCommandPools.resize(myCommandBufferThreadCount);
    for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
    {
        myFrameCommandPools[threadIt] = createCommandPool(
            myGraphicsDevice->getDevice(),
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            *myGraphicsDevice->getSelectedQueueFamilyIndex());
    }

    auto createFramebuffer = [this, &window](uint32_t frameIndex)
    {
        std::array<VkImageView, 2> attachments = {window.colorViews[frameIndex], window.depthView};
        VkFramebufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = myGraphicsPipelineConfig->renderPass;
        info.attachmentCount = attachments.size();
        info.pAttachments = attachments.data();
        info.width = window.swapchain->getDesc().imageExtent.width;
        info.height = window.swapchain->getDesc().imageExtent.height;
        info.layers = 1;

        VkFramebuffer outFramebuffer = VK_NULL_HANDLE;
        CHECK_VK(vkCreateFramebuffer(myGraphicsDevice->getDevice(), &info, nullptr, &outFramebuffer));

        return outFramebuffer;
    };

    window.frames.resize(window.colorImages.size());
    for (uint32_t frameIt = 0; frameIt < window.frames.size(); frameIt++)
    {
        auto& frame = window.frames[frameIt];

        frame.index = frameIt;
        frame.frameBuffer = createFramebuffer(frameIt);
        frame.commandBuffers.resize(myCommandBufferThreadCount);

        for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
        {
            VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cmdInfo.commandPool = myFrameCommandPools[threadIt];
            cmdInfo.level = threadIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            cmdInfo.commandBufferCount = 1;
            CHECK_VK(vkAllocateCommandBuffers(myGraphicsDevice->getDevice(), &cmdInfo, &frame.commandBuffers[threadIt]));
        }

        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK_VK(vkCreateFence(myGraphicsDevice->getDevice(), &fenceInfo, nullptr, &frame.fence));

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK_VK(vkCreateSemaphore(
            myGraphicsDevice->getDevice(), &semaphoreInfo, nullptr, &frame.renderCompleteSemaphore));
        CHECK_VK(vkCreateSemaphore(
            myGraphicsDevice->getDevice(), &semaphoreInfo, nullptr, &frame.newImageAcquiredSemaphore));

        frame.graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
    }

    updateDescriptorSets(window, *myGraphicsPipelineConfig);

    // vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
    window.frameIndex = window.frames.size() - 1;
}

template <>
void Application<GraphicsBackend::Vulkan>::cleanupState(Window<GraphicsBackend::Vulkan>& window)
{
    ZoneScoped;

    for (auto& frame : window.frames)
    {
        vkDestroyFence(myGraphicsDevice->getDevice(), frame.fence, nullptr);
        vkDestroySemaphore(myGraphicsDevice->getDevice(), frame.renderCompleteSemaphore, nullptr);
        vkDestroySemaphore(myGraphicsDevice->getDevice(), frame.newImageAcquiredSemaphore, nullptr);

        for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
            vkFreeCommandBuffers(myGraphicsDevice->getDevice(), myFrameCommandPools[threadIt], 1, &frame.commandBuffers[threadIt]);

        vkDestroyFramebuffer(myGraphicsDevice->getDevice(), frame.frameBuffer, nullptr);
    }

    for (uint32_t threadIt = 0; threadIt < myCommandBufferThreadCount; threadIt++)
        vkDestroyCommandPool(myGraphicsDevice->getDevice(), myFrameCommandPools[threadIt], nullptr);	

    for (uint32_t i = 0; i < window.colorViews.size(); i++)
        vkDestroyImageView(myGraphicsDevice->getDevice(), window.colorViews[i], nullptr);

    window.colorImages.clear();
    window.colorViews.clear();

    vkDestroyImageView(myGraphicsDevice->getDevice(), window.depthView, nullptr);
    window.depthView = 0;

    vkDestroyRenderPass(myGraphicsDevice->getDevice(), myGraphicsPipelineConfig->renderPass, nullptr);

    vkDestroyPipeline(myGraphicsDevice->getDevice(), myGraphicsPipelineConfig->graphicsPipeline, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::updateViewBuffer(
    Window<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    void* data;
    CHECK_VK(vmaMapMemory(myAllocator, window.viewBuffer->getBufferMemory(), &data));

    for (uint32_t n = 0; n < (NX * NY); n++)
    {
        View::BufferData& ubo = reinterpret_cast<View::BufferData*>(data)[window.frameIndex * (NX * NY) + n];

        ubo.viewProj = window.views[n].projection * glm::mat4(window.views[n].view);
    }

    vmaFlushAllocation(
        myAllocator,
        window.viewBuffer->getBufferMemory(),
        sizeof(View::BufferData) * window.frameIndex * (NX * NY),
        sizeof(View::BufferData) * (NX * NY));

    vmaUnmapMemory(myAllocator, window.viewBuffer->getBufferMemory());
}

template <>
void Application<GraphicsBackend::Vulkan>::checkFlipOrPresentResult(
    Window<GraphicsBackend::Vulkan>& window,
    Result<GraphicsBackend::Vulkan> result) const
{
    switch (result)
    {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        std::cout << "warning: flip/present returned VK_SUBOPTIMAL_KHR";
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        window.createStateFlag = true;
        break;
    default:
        throw std::runtime_error("failed to flip swap chain image!");
    }
}

template <>
Application<GraphicsBackend::Vulkan>::Application(
    void* view,
    int width,
    int height,
    int framebufferWidth,
    int framebufferHeight,
    const char* resourcePath)
    : myResourcePath(resourcePath)
    , myCommandBufferThreadCount(NX * NY + 1)
    , myRequestedCommandBufferThreadCount(NX * NY + 1)
{
    ZoneScoped;

    assert(std::filesystem::is_directory(myResourcePath));

    myInstance = std::make_unique<InstanceContext<GraphicsBackend::Vulkan>>(        
        InstanceCreateDesc<GraphicsBackend::Vulkan>{
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        "volcano-vk",
        VK_MAKE_VERSION(1, 0, 0),
        "kiss",
        VK_MAKE_VERSION(1, 0, 0),
            VK_API_VERSION_1_2});

    myDefaultResources = std::make_shared<GraphicsPipelineResourceView<GraphicsBackend::Vulkan>>();
    
    myDefaultResources->window = std::make_shared<Window<GraphicsBackend::Vulkan>>();
    auto& window = *myDefaultResources->window;
    window.width = width;
    window.height = height;
    window.framebufferWidth = framebufferWidth;
    window.framebufferHeight = framebufferHeight;
    window.surface = createSurface<GraphicsBackend::Vulkan>(myInstance->getInstance(), view);

    myGraphicsDevice = std::make_unique<DeviceContext<GraphicsBackend::Vulkan>>(
        DeviceCreateDesc<GraphicsBackend::Vulkan>{
        myInstance->getInstance(),
            window.surface});

    myAllocator = createAllocator<GraphicsBackend::Vulkan>(myInstance->getInstance(), myGraphicsDevice->getDevice(), myGraphicsDevice->getPhysicalDevice());

    myTransferCommandPool = createCommandPool(
        myGraphicsDevice->getDevice(),
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        *myGraphicsDevice->getSelectedQueueFamilyIndex());

    myDefaultResources->sampler = createTextureSampler(myGraphicsDevice->getDevice());
    myDescriptorPool = createDescriptorPool<GraphicsBackend::Vulkan>(myGraphicsDevice->getDevice());

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myGraphicsDevice->getDevice(), *slangShaders);
    
    myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
        std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"),
        myGraphicsDevice->getDevice(), myTransferCommandPool, myGraphicsDevice->getSelectedQueue(), myAllocator);
    myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"),
        myGraphicsDevice->getDevice(), myTransferCommandPool, myGraphicsDevice->getSelectedQueue(), myAllocator);
    myDefaultResources->textureView = myDefaultResources->texture->createView(VK_IMAGE_ASPECT_COLOR_BIT);

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        std::filesystem::absolute(myResourcePath / ".cache" / "pipeline.cache"),
        myGraphicsDevice->getDevice(),
        myGraphicsDevice->getPhysicalDeviceProperties());

    window.swapchain = std::make_unique<SwapchainContext<GraphicsBackend::Vulkan>>(
        SwapchainCreateDesc<GraphicsBackend::Vulkan>{
            myGraphicsDevice->getSwapchainConfiguration(),
            myGraphicsDevice->getDevice(),
            myAllocator,
            window.surface,
            nullptr,
            Extent2d<GraphicsBackend::Vulkan>{
                static_cast<uint32_t>(framebufferWidth),
                static_cast<uint32_t>(framebufferHeight)}});

    window.viewBuffer = std::make_shared<Buffer<GraphicsBackend::Vulkan>>(
        BufferCreateDesc<GraphicsBackend::Vulkan>{
            window.swapchain->getDesc().configuration.selectedImageCount * (NX * NY) * sizeof(View::BufferData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
            "viewBuffer"},
        myGraphicsDevice->getDevice(),
        myTransferCommandPool,
        myGraphicsDevice->getSelectedQueue(),
        myAllocator);

    // todo: append stencil bit for depthstencil composite formats
    
    window.zBuffer = std::make_unique<Texture<GraphicsBackend::Vulkan>>(
        TextureCreateDesc<GraphicsBackend::Vulkan>{
        window.framebufferWidth,
        window.framebufferHeight,
        1,
        0,
        findSupportedFormat(
            myGraphicsDevice->getPhysicalDevice(),
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE, 
            "zBuffer"},
        myGraphicsDevice->getDevice(),
        myTransferCommandPool,
        myGraphicsDevice->getSelectedQueue(),
        myAllocator);

    createState(window);

    float dpiScaleX = static_cast<float>(framebufferWidth) / width;
    float dpiScaleY = static_cast<float>(framebufferHeight) / height;

    initIMGUI(window, dpiScaleX, dpiScaleY);
}

template <>
Application<GraphicsBackend::Vulkan>::~Application()
{
    ZoneScoped;

    {
        ZoneScopedN("deviceWaitIdle");

        CHECK_VK(vkDeviceWaitIdle(myGraphicsDevice->getDevice()));
    }

    std::filesystem::path cacheFilePath = std::filesystem::absolute(myResourcePath / ".cache");

    if (!std::filesystem::exists(cacheFilePath))
        std::filesystem::create_directory(cacheFilePath);

    savePipelineCache<GraphicsBackend::Vulkan>(
        cacheFilePath / "pipeline.cache",
        myGraphicsDevice->getDevice(), myGraphicsDevice->getPhysicalDeviceProperties(), myPipelineCache);

    auto& window = *myDefaultResources->window;

    cleanupState(window);

    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();

    {
        window.swapchain.reset();
        window.zBuffer.reset();
        window.viewBuffer.reset();
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

    vkDestroyDescriptorPool(myGraphicsDevice->getDevice(), myDescriptorPool, nullptr);
    vkDestroyCommandPool(myGraphicsDevice->getDevice(), myTransferCommandPool, nullptr);

    char* allocatorStatsJSON = nullptr;
    vmaBuildStatsString(myAllocator, &allocatorStatsJSON, true);
    std::cout << allocatorStatsJSON << std::endl;
    vmaFreeStatsString(myAllocator, allocatorStatsJSON);
    vmaDestroyAllocator(myAllocator);

    myGraphicsDevice.reset();
    
    vkDestroySurfaceKHR(myInstance->getInstance(), window.surface, nullptr);
}

template <>
void Application<GraphicsBackend::Vulkan>::submitFrame(
    const DeviceContext<GraphicsBackend::Vulkan>& deviceContext,
    uint32_t commandBufferThreadCount,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& config,
    Window<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    window.lastFrameIndex = window.frameIndex;
    auto& lastFrame = window.frames[window.lastFrameIndex];

    {
        ZoneScopedN("acquireNextImage");

        checkFlipOrPresentResult(
            window,
            vkAcquireNextImageKHR(
                deviceContext.getDevice(),
                window.swapchain->getSwapchain(),
                UINT64_MAX,
                lastFrame.newImageAcquiredSemaphore,
                VK_NULL_HANDLE,
                &window.frameIndex));
    }

    auto& frame = window.frames[window.frameIndex];
    
    // wait for frame to be completed before starting to use it
    {
        ZoneScopedN("waitForFrameFence");

        CHECK_VK(vkWaitForFences(deviceContext.getDevice(), 1, &frame.fence, VK_TRUE, UINT64_MAX));
        CHECK_VK(vkResetFences(deviceContext.getDevice(), 1, &frame.fence));

        frame.graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
        frame.graphicsDeltaTime = frame.graphicsFrameTimestamp - lastFrame.graphicsFrameTimestamp;
    }

    std::future<void> drawIMGUIFuture(std::async(std::launch::async, [this, &window]
    {
        if (window.imguiEnable)
        {
            ImGui_ImplVulkan_NewFrame();
            drawIMGUI(window);
        }
    }));

    std::future<void> updateViewBufferFuture(std::async(std::launch::async, [this, &window]
    {
        updateViewBuffer(window);
    }));
    
    std::future<void> beginCommandsFuture(std::async(std::launch::async, [&frame]
    {
        // begin primary command buffer
        {
            ZoneScopedN("beginCommands");

            CHECK_VK(vkResetCommandBuffer(frame.commandBuffers[0], 0));
            VkCommandBufferBeginInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            CHECK_VK(vkBeginCommandBuffer(frame.commandBuffers[0], &info));

        }

        // collect timing scopes
        {
            ZoneScopedN("tracyVkCollect");
            // TracyVkZone(tracyContext, frame.commandBuffers[0], "tracyVkCollect");
            // TracyVkCollect(tracyContext, frame.commandBuffers[0]);
        }
    }));

    std::array<ClearValue<GraphicsBackend::Vulkan>, 2> clearValues = {};
    clearValues[0] = window.clearValue;
    clearValues[1].depthStencil = {1.0f, 0};

    // create secondary command buffers
    {
        // setup draw parameters
        constexpr uint32_t drawCount = NX * NY;
        uint32_t segmentCount = std::max(commandBufferThreadCount - 1, 1u);

        assert(config.resources);

        // draw geometry using secondary command buffers
        {
            ZoneScopedN("waitDraw");

            uint32_t segmentDrawCount = drawCount / segmentCount;
            if (drawCount % segmentCount)
                segmentDrawCount += 1;

            uint32_t dx = window.framebufferWidth / NX;
            uint32_t dy = window.framebufferHeight / NY;

            std::array<uint32_t, 128> seq;
            std::iota(seq.begin(), seq.begin() + segmentCount, 0);
            std::for_each_n(
#if defined(__WINDOWS__)
				std::execution::par,
#endif
                seq.begin(), segmentCount,
                [/*this, */&config, &frame, &dx, &dy, &segmentDrawCount](uint32_t segmentIt)
                {

                    VkCommandBuffer cmd = frame.commandBuffers[segmentIt + 1];

                    {
                        ZoneScopedN("beginSecondaryCommands");

                        VkCommandBuffer cmd = frame.commandBuffers[segmentIt + 1];

                        VkCommandBufferInheritanceInfo inherit = {
                            VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
                        inherit.renderPass = config.renderPass;
                        inherit.framebuffer = frame.frameBuffer;

                        CHECK_VK(vkResetCommandBuffer(cmd, 0));
                        VkCommandBufferBeginInfo secBeginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                        secBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                            VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
                                            VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
                        secBeginInfo.pInheritanceInfo = &inherit;
                        CHECK_VK(vkBeginCommandBuffer(cmd, &secBeginInfo));
                    }

                    {
                        ZoneScopedN("drawSegment");
                        //TracyVkZone(myTracyContext, cmd, "drawSegment");

                        // bind pipeline and inputs
                        {
                            ZoneScopedN("bind");

                            // bind pipeline and vertex/index buffers
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, config.graphicsPipeline);

                            VkBuffer vertexBuffers[] = {config.resources->model->getVertexBuffer().getBuffer()};
                            VkDeviceSize vertexOffsets[] = {0};

                            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
                            vkCmdBindIndexBuffer(cmd, config.resources->model->getIndexBuffer().getBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        }

                        for (uint32_t drawIt = 0; drawIt < segmentDrawCount; drawIt++)
                        {
                            ZoneScopedN("draw");
                            //TracyVkZone(myTracyContext, cmd, "draw");
                            
                            uint32_t n = segmentIt * segmentDrawCount + drawIt;

                            if (n >= drawCount)
                                break;

                            uint32_t i = n % NX;
                            uint32_t j = n / NX;

                            auto setViewportAndScissor = [](VkCommandBuffer cmd, int32_t x, int32_t y,
                                                            int32_t width, int32_t height) {
                                VkViewport viewport = {};
                                viewport.x = static_cast<float>(x);
                                viewport.y = static_cast<float>(y);
                                viewport.width = static_cast<float>(width);
                                viewport.height = static_cast<float>(height);
                                viewport.minDepth = 0.0f;
                                viewport.maxDepth = 1.0f;

                                VkRect2D scissor = {};
                                scissor.offset = {x, y};
                                scissor.extent = {static_cast<uint32_t>(width),
                                                static_cast<uint32_t>(height)};

                                vkCmdSetViewport(cmd, 0, 1, &viewport);
                                vkCmdSetScissor(cmd, 0, 1, &scissor);
                            };

                            auto drawModel = [&n, &frame](
                                                VkCommandBuffer cmd, uint32_t indexCount,
                                                uint32_t descriptorSetCount,
                                                const VkDescriptorSet* descriptorSets,
                                                VkPipelineLayout pipelineLayout) {
                                uint32_t viewBufferOffset = (frame.index * drawCount + n) * sizeof(View::BufferData);
                                vkCmdBindDescriptorSets(
                                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                    descriptorSetCount, descriptorSets, 1, &viewBufferOffset);
                                vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
                            };

                            setViewportAndScissor(cmd, i * dx, j * dy, dx, dy);

                            drawModel(
                                cmd, config.resources->model->getDesc().indexCount, config.descriptorSets.size(),
                                config.descriptorSets.data(), config.layout->layout);
                        }
                    }

                    {
                        ZoneScopedN("endSecondaryCommands");

                        CHECK_VK(vkEndCommandBuffer(cmd));
                    }
                });
        }
    }

    // wait for primary command buffer to be ready to accept commands
    {
        ZoneScopedN("waitBeginCommands");

        beginCommandsFuture.get();
    }

    // call secondary command buffers
    {
        ZoneScopedN("executeCommands");
        //TracyVkZone(tracyContext, frame.commandBuffers[0], "executeCommands");

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = config.renderPass;
        beginInfo.framebuffer = frame.frameBuffer;
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {static_cast<uint32_t>(window.framebufferWidth),
                                        static_cast<uint32_t>(window.framebufferHeight)};
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(
            frame.commandBuffers[0], &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

        vkCmdExecuteCommands(frame.commandBuffers[0], (commandBufferThreadCount - 1), &frame.commandBuffers[1]);

        vkCmdEndRenderPass(frame.commandBuffers[0]);
    }

    // wait for imgui draw job
    {
        ZoneScopedN("waitDrawIMGUI");

        drawIMGUIFuture.get();
    }

    // Record Imgui Draw Data and draw funcs into primary command buffer
    if (window.imguiEnable)
    {
        ZoneScopedN("drawIMGUI");
        //TracyVkZone(tracyContext, frame.commandBuffers[0], "drawIMGUI");

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = config.renderPass;
        beginInfo.framebuffer = frame.frameBuffer;
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent.width = window.framebufferWidth;
        beginInfo.renderArea.extent.height = window.framebufferHeight;
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(frame.commandBuffers[0], &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffers[0]);

        vkCmdEndRenderPass(frame.commandBuffers[0]);
    }

    // Submit primary command buffer
    {
        {
            ZoneScopedN("waitViewBuffer");

            updateViewBufferFuture.get();
        }

        ZoneScopedN("submitCommands");

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &lastFrame.newImageAcquiredSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffers[0];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderCompleteSemaphore;

        CHECK_VK(vkEndCommandBuffer(frame.commandBuffers[0]));
        CHECK_VK(vkQueueSubmit(deviceContext.getSelectedQueue(), 1, &submitInfo, frame.fence));
    }
}

template <>
void Application<GraphicsBackend::Vulkan>::presentFrame(
    Window<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    auto& frame = window.frames[window.frameIndex];

    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame.renderCompleteSemaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &window.swapchain->getSwapchain();
    info.pImageIndices = &window.frameIndex;
    checkFlipOrPresentResult(window, vkQueuePresentKHR(myGraphicsDevice->getSelectedQueue(), &info));
}

template <>
void Application<GraphicsBackend::Vulkan>::updateInput(Window<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    auto& frame = window.frames[window.lastFrameIndex];
    float dt = frame.graphicsDeltaTime.count();

    // update input dependent state
    {
        auto& io = ImGui::GetIO();

        static bool escBufferState = false;
        bool escState = io.KeysDown[io.KeyMap[ImGuiKey_Escape]];
        if (escState && !escBufferState)
            window.imguiEnable = !window.imguiEnable;
        escBufferState = escState;
    }

    if (myCommandBufferThreadCount != myRequestedCommandBufferThreadCount)
        window.createStateFlag = true;

    if (window.activeView)
    {
        // std::cout << "window.activeView read/consume" << std::endl;

        float dx = 0;
        float dz = 0;

        for (const auto& [key, pressed] : myKeysPressed)
        {
            if (pressed)
            {
                switch (key)
                {
                case GLFW_KEY_W:
                    dz = 1;
                    break;
                case GLFW_KEY_S:
                    dz = -1;
                    break;
                case GLFW_KEY_A:
                    dx = 1;
                    break;
                case GLFW_KEY_D:
                    dx = -1;
                    break;
                default:
                    break;
                }
            }
        }

        auto& view = window.views[*window.activeView];

        bool doUpdateViewMatrix = false;

        if (dx != 0 || dz != 0)
        {
            auto forward = glm::vec3(view.view[0][2], view.view[1][2], view.view[2][2]);
            auto strafe = glm::vec3(view.view[0][0], view.view[1][0], view.view[2][0]);

            constexpr auto moveSpeed = 2.0f;

            view.camPos += dt * (dz * forward + dx * strafe) * moveSpeed;

            // std::cout << *window.activeView << ":pos:[" << view.camPos.x << ", " <<
            // view.camPos.y << ", " << view.camPos.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (myMouseButtonsPressed[0])
        {
            constexpr auto rotSpeed = 10.0f;

            auto dM = myMousePosition[0] - myMousePosition[1];

            view.camRot +=
                dt * glm::vec3(dM.y / view.viewport.height, dM.x / view.viewport.width, 0.0f) *
                rotSpeed;

            // std::cout << *window.activeView << ":rot:[" << view.camRot.x << ", " <<
            // view.camRot.y << ", " << view.camRot.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (doUpdateViewMatrix)
        {
            updateViewMatrix(window.views[*window.activeView]);
        }
    }
}

template <>
void Application<GraphicsBackend::Vulkan>::onMouse(const mouse_state& state)
{
    ZoneScoped;

    auto& window = *myDefaultResources->window;

    bool leftPressed = state.button == GLFW_MOUSE_BUTTON_LEFT && state.action == GLFW_PRESS;
    bool rightPressed = state.button == GLFW_MOUSE_BUTTON_RIGHT && state.action == GLFW_PRESS;
    
    auto screenPos = glm::vec2(state.xpos, state.ypos);
    // auto screenPos = ImGui::GetCursorScreenPos();

    if (state.inside_window && !myMouseButtonsPressed[0])
    {
        // todo: generic view index calculation
        size_t viewIdx = screenPos.x / (window.width / NX);
        size_t viewIdy = screenPos.y / (window.height / NY);
        window.activeView = std::min((viewIdy * NX) + viewIdx, window.views.size() - 1);

        // std::cout << *window.activeView << ":[" << screenPos.x << ", " << screenPos.y << "]"
        // 		  << std::endl;
    }
    else if (!leftPressed)
    {
        window.activeView.reset();

        // std::cout << "window.activeView.reset()" << std::endl;
    }

    myMousePosition[0] =
        glm::vec2{static_cast<float>(screenPos.x), static_cast<float>(screenPos.y)};
    myMousePosition[1] =
        leftPressed && !myMouseButtonsPressed[0] ? myMousePosition[0] : myMousePosition[1];

    myMouseButtonsPressed[0] = leftPressed;
    myMouseButtonsPressed[1] = rightPressed;
}

template <>
void Application<GraphicsBackend::Vulkan>::onKeyboard(const keyboard_state& state)
{
    ZoneScoped;

    if (state.action == GLFW_PRESS)
        myKeysPressed[state.key] = true;
    else if (state.action == GLFW_RELEASE)
        myKeysPressed[state.key] = false;
}

template <>
void Application<GraphicsBackend::Vulkan>::draw()
{
    FrameMark;

    auto& window = *myDefaultResources->window;

    // re-create frame resources if needed
    if (window.createStateFlag)
    {
        ZoneScopedN("recreateState");

        {
            ZoneScopedN("queueWaitIdle");

            CHECK_VK(vkQueueWaitIdle(myGraphicsDevice->getSelectedQueue()));
        }

        cleanupState(window);

        myCommandBufferThreadCount = std::min(static_cast<uint32_t>(myRequestedCommandBufferThreadCount), NX * NY + 1);

        createState(window);
    }

    updateInput(window);
    submitFrame(
        *myGraphicsDevice,
        myCommandBufferThreadCount,
        *myGraphicsPipelineConfig,
        window);
    presentFrame(window);
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(
    int width,
    int height)
{
    ZoneScoped;

    auto& window = *myDefaultResources->window;

    {
        ZoneScopedN("queueWaitIdle");

        CHECK_VK(vkQueueWaitIdle(myGraphicsDevice->getSelectedQueue()));
    }

    cleanupState(window);

    // window.width = width;
    // window.height = height;
    window.framebufferWidth = width;
    window.framebufferHeight = height;

    window.swapchain = std::make_unique<SwapchainContext<GraphicsBackend::Vulkan>>(
        SwapchainCreateDesc<GraphicsBackend::Vulkan>{
            myGraphicsDevice->getSwapchainConfiguration(),
            myGraphicsDevice->getDevice(),
            myAllocator,
            window.surface,
            window.swapchain->detatch(),
            Extent2d<GraphicsBackend::Vulkan>{
                static_cast<uint32_t>(window.framebufferWidth),
                static_cast<uint32_t>(window.framebufferHeight)}});

    // todo: append stencil bit for depthstencil composite formats
    window.zBuffer = std::make_unique<Texture<GraphicsBackend::Vulkan>>(
        TextureCreateDesc<GraphicsBackend::Vulkan>{
        window.framebufferWidth,
        window.framebufferHeight,
        1,
        0,
        findSupportedFormat(
            myGraphicsDevice->getPhysicalDevice(),
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        "zBuffer"
        },
        myGraphicsDevice->getDevice(),
        myTransferCommandPool,
        myGraphicsDevice->getSelectedQueue(),
        myAllocator);

    createState(window);
}
