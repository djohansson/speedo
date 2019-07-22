template <GraphicsBackend B>
void Application<B>::resizeWindow(const window_state& state)
{
    auto& window = *myDefaultResources->window;

    if (state.fullscreen_enabled)
    {
        window.width = state.fullscreen_width;
        window.height = state.fullscreen_height;
    }
    else
    {
        window.width = state.width;
        window.height = state.height;
    }
}

// todo: encapsulate
template <GraphicsBackend B>
PipelineConfiguration<B> Application<B>::createPipelineConfig(DeviceHandle<B> device, RenderPassHandle<B> renderPass,
    DescriptorPoolHandle<B> descriptorPool, PipelineCacheHandle<B> pipelineCache,
    std::shared_ptr<PipelineLayoutContext<B>> layoutContext, std::shared_ptr<GraphicsPipelineResourceView<B>> resources) const
{
    PipelineConfiguration<B> outPipelineConfig = {};

    outPipelineConfig.resources = resources;
    outPipelineConfig.layout = layoutContext;

    outPipelineConfig.renderPass = renderPass;
    outPipelineConfig.graphicsPipeline = createGraphicsPipeline(device, pipelineCache, outPipelineConfig);

    outPipelineConfig.descriptorSets = allocateDescriptorSets(
            device, descriptorPool, layoutContext->descriptorSetLayouts.get(),
            layoutContext->descriptorSetLayouts.get_deleter().getSize());

    return outPipelineConfig;
}

template <GraphicsBackend B>
void Application<B>::updateViewMatrix(View& view) const
{
    auto Rx = glm::rotate(glm::mat4(1.0), view.camRot.x, glm::vec3(-1, 0, 0));
    auto Ry = glm::rotate(glm::mat4(1.0), view.camRot.y, glm::vec3(0, -1, 0));
    auto T = glm::translate(glm::mat4(1.0), -view.camPos);

    view.view = glm::inverse(T * Ry * Rx);
}

template <GraphicsBackend B>
void Application<B>::updateProjectionMatrix(View& view) const
{
    constexpr glm::mat4 clip = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f,  0.5f, 1.0f};

    constexpr auto fov = 75.0f;
    auto aspect =
        static_cast<float>(view.viewport.width) / static_cast<float>(view.viewport.height);
    constexpr auto nearplane = 0.01f;
    constexpr auto farplane = 100.0f;

    view.projection = clip * glm::perspective(glm::radians(fov), aspect, nearplane, farplane);
}

template <GraphicsBackend B>
void Application<B>::drawIMGUI(WindowData<B>& window)
{
    ZoneScoped;

    using namespace ImGui;

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
                myDefaultResources->model = std::make_shared<Model<B>>(
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
                myDefaultResources->texture = std::make_shared<Texture<B>>(
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
