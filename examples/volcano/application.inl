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
auto Application<B>::createPipelineConfig(DeviceHandle<B> device, RenderPassHandle<B> renderPass,
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
void Application<B>::drawIMGUI(Window<B>& window) const
{
    ZoneScoped;

    using namespace ImGui;

    NewFrame();
    ShowDemoWindow();

    {
        Begin("Render Options");
        // DragInt(
        //     "Command Buffer Threads", &myRequestedCommandBufferThreadCount, 0.1f, 2, 32);
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
            SetNextWindowPos(ImVec2(0.5f, 0.5f));
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

    // {
    //     Begin("File");

    //     if (Button("Open OBJ file"))
    //     {
    //         nfdchar_t* pathStr;
    //         auto res = NFD_OpenDialog("obj", std::filesystem::absolute(myResourcePath).u8string().c_str(), &pathStr);
    //         if (res == NFD_OKAY)
    //         {
    //             myDefaultResources->model = std::make_shared<Model<B>>(
    //                 myDevice, myTransferCommandPool, myQueue, myAllocator,
    //                 std::filesystem::absolute(pathStr));

    //             updateDescriptorSets(window, *myGraphicsPipelineConfig);
    //         }
    //     }

    //     if (Button("Open JPG file"))
    //     {
    //         nfdchar_t* pathStr;
    //         auto res = NFD_OpenDialog("jpg", std::filesystem::absolute(myResourcePath).u8string().c_str(), &pathStr);
    //         if (res == NFD_OKAY)
    //         {
    //             myDefaultResources->texture = std::make_shared<Texture<B>>(
    //                 myDevice, myTransferCommandPool, myQueue, myAllocator,
    //                 std::filesystem::absolute(pathStr));

    //             updateDescriptorSets(window, *myGraphicsPipelineConfig);
    //         }
    //     }

    //     End();
    // }

    {
        ShowMetricsWindow();
    }

    Render();
}
