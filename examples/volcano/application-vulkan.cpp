#include "application.h"

template <>
Application<GraphicsBackend::Vulkan>::Application(
		void* view, int width, int height, int framebufferWidth, int framebufferHeight,
		const char* resourcePath)
		: myResourcePath(resourcePath)
		, myCommandBufferThreadCount(2)
		, myRequestedCommandBufferThreadCount(2)
{
    ZoneScoped;

    assert(std::filesystem::is_directory(myResourcePath));

    myInstance = createInstance<GraphicsBackend::Vulkan>();
    myDebugCallback = createDebugCallback(myInstance);

    myDefaultResources = std::make_shared<GraphicsPipelineResourceContext<GraphicsBackend::Vulkan>>();
    myDefaultResources->window = std::make_shared<WindowData<GraphicsBackend::Vulkan>>();
    auto& window = *myDefaultResources->window;

    window.width = width;
    window.height = height;
    window.framebufferWidth = framebufferWidth;
    window.framebufferHeight = framebufferHeight;
    window.surface = createSurface(myInstance, view);

    uint32_t frameCount = 0;

    std::tie(
        myDevice, myPhysicalDevice, myPhysicalDeviceProperties, window.swapchain.info, window.surfaceFormat,
        window.presentMode, frameCount, myQueue, myQueueFamilyIndex) =
        createDevice(myInstance, window.surface);

    myAllocator = createAllocator(myDevice, myPhysicalDevice);

    myTransferCommandPool = createCommandPool(
        myDevice,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        myQueueFamilyIndex);

    myDefaultResources->sampler = createTextureSampler(myDevice);
    myDescriptorPool = createDescriptorPool();

    auto slangShaders = loadSlangShaders<GraphicsBackend::Vulkan>(std::filesystem::absolute(myResourcePath / "shaders" / "shaders.slang"));

    myGraphicsPipelineLayout = std::make_shared<PipelineLayoutContext<GraphicsBackend::Vulkan>>();
    *myGraphicsPipelineLayout = createPipelineLayoutContext(myDevice, *slangShaders);
    
    myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        std::filesystem::absolute(myResourcePath / "models" / "gallery.obj"));
    myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        std::filesystem::absolute(myResourcePath / "images" / "gallery.jpg"));

    std::tie(window.uniformBuffer, window.uniformBufferMemory) = createBuffer(
        myAllocator, frameCount * (NX * NY) * sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "uniformBuffer");

    myPipelineCache = loadPipelineCache<GraphicsBackend::Vulkan>(
        myDevice,
        myPhysicalDeviceProperties,
        std::filesystem::absolute(myResourcePath / ".cache" / "pipeline.cache"));

    myDefaultResources->window->swapchain = createSwapchainContext(
        myDevice, myPhysicalDevice, myAllocator, frameCount,
        *myDefaultResources->window);

    // todo: append stencil bit for depthstencil composite formats
    TextureData<GraphicsBackend::Vulkan> textureData = 
    {
        window.framebufferWidth,
        window.framebufferHeight,
        1,
        0,
        nullptr, 
        findSupportedFormat(
            myPhysicalDevice,
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        "zBuffer"
    };
    window.zBuffer = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        textureData);

    std::vector<Model<GraphicsBackend::Vulkan>*> models;
    models.push_back(myDefaultResources->model.get());
    createFrameResources(*myDefaultResources->window, models);

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

        CHECK_VK(vkDeviceWaitIdle(myDevice));
    }

    std::filesystem::path cacheFilePath = std::filesystem::absolute(myResourcePath / ".cache");

    if (!std::filesystem::exists(cacheFilePath))
        std::filesystem::create_directory(cacheFilePath);

    savePipelineCache<GraphicsBackend::Vulkan>(myDevice, myPipelineCache, myPhysicalDeviceProperties, cacheFilePath / "pipeline.cache");

    cleanup();
}

template <>
void Application<GraphicsBackend::Vulkan>::updateInput(WindowData<GraphicsBackend::Vulkan>& window) const
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
        window.createFrameResourcesFlag = true;

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
void Application<GraphicsBackend::Vulkan>::draw()
{
    FrameMark;
    ZoneScoped;

    auto& window = *myDefaultResources->window;

    // re-create frame resources if needed
    if (window.createFrameResourcesFlag)
    {
        ZoneScopedN("recreateFrameResources");

        {
            ZoneScopedN("queueWaitIdle");

            CHECK_VK(vkQueueWaitIdle(myQueue));
        }

        cleanupFrameResources();

        myCommandBufferThreadCount = std::min(static_cast<uint32_t>(myRequestedCommandBufferThreadCount), NX * NY);

        std::vector<Model<GraphicsBackend::Vulkan>*> models;
        models.push_back(myDefaultResources->model.get());
        createFrameResources(window, models);
    }

    updateInput(window);
    submitFrame(window);
    presentFrame(window);
}

template <>
void Application<GraphicsBackend::Vulkan>::resizeFramebuffer(int width, int height)
{
    ZoneScoped;

    auto& window = *myDefaultResources->window;
    auto frameCount = window.swapchain.colorImages.size();

    {
        ZoneScopedN("queueWaitIdle");

        CHECK_VK(vkQueueWaitIdle(myQueue));
    }

    cleanupFrameResources();

    // hack to shut up validation layer createPipelineLayoutContext.
    // see https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/624
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        myPhysicalDevice, window.surface, &window.swapchain.info.capabilities);

    // window.width = width;
    // window.height = height;
    window.framebufferWidth = width;
    window.framebufferHeight = height;

    myDefaultResources->window->swapchain = createSwapchainContext(
        myDevice, myPhysicalDevice, myAllocator, frameCount,
        *myDefaultResources->window);

    // todo: append stencil bit for depthstencil composite formats
    TextureData<GraphicsBackend::Vulkan> textureData = 
    {
        window.framebufferWidth,
        window.framebufferHeight,
        1,
        0,
        nullptr, 
        findSupportedFormat(
            myPhysicalDevice,
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        "zBuffer"
    };
    window.zBuffer = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
        myDevice, myTransferCommandPool, myQueue, myAllocator,
        textureData);

    std::vector<Model<GraphicsBackend::Vulkan>*> models;
    models.push_back(myDefaultResources->model.get());
    createFrameResources(*myDefaultResources->window, models);
}

template <>
void Application<GraphicsBackend::Vulkan>::drawIMGUI(WindowData<GraphicsBackend::Vulkan>& window)
{
    ZoneScoped;

    using namespace ImGui;

    ImGui_ImplVulkan_NewFrame();

    NewFrame();
    ShowDemoWindow();

    {
        Begin("Render Options");
        DragInt(
            "Command Buffer Threads", &myRequestedCommandBufferThreadCount, 0.1f, 2, 32);
        ColorEdit3(
            "Clear Color", &window.clearValue.color.float32[0]);
        End();
    }

    {
        Begin("GUI Options");
        // static int styleIndex = 0;
        ShowStyleSelector("Styles" /*, &styleIndex*/);
        ShowFontSelector("Fonts");
        if (Button("Show User Guide"))
        {
            SetNextWindowPosCenter(0);
            OpenPopup("UserGuide");
        }
        if (BeginPopup(
                "UserGuide", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ShowUserGuide();
            EndPopup();
        }
        End();
    }

    {
        Begin("File");

        if (Button("Open OBJ file"))
        {
            nfdchar_t* pathStr;
            auto res = NFD_OpenDialog("obj", myResourcePath.u8string().c_str(), &pathStr);
            if (res == NFD_OKAY)
            {
                myDefaultResources->model = std::make_shared<Model<GraphicsBackend::Vulkan>>(
                    myDevice, myTransferCommandPool, myQueue, myAllocator,
                    std::filesystem::absolute(pathStr));

                updateDescriptorSets(window, *myGraphicsPipelineConfig);
            }
        }

        if (Button("Open JPG file"))
        {
            nfdchar_t* pathStr;
            auto res = NFD_OpenDialog("jpg", myResourcePath.u8string().c_str(), &pathStr);
            if (res == NFD_OKAY)
            {
                myDefaultResources->texture = std::make_shared<Texture<GraphicsBackend::Vulkan>>(
                    myDevice, myTransferCommandPool, myQueue, myAllocator,
                    std::filesystem::absolute(pathStr));

                updateDescriptorSets(window, *myGraphicsPipelineConfig);
            }
        }

        End();
    }

    {
        ShowMetricsWindow();
    }

    Render();
}
