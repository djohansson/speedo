template <GraphicsBackend B>
void Application<B>::resizeWindow(const WindowState& state)
{
    // todo:
    // auto& window = *myDefaultResources->window;
    // if (state.fullscreen_enabled)
    // {
    //     window.width = state.fullscreen_width;
    //     window.height = state.fullscreen_height;
    // }
    // else
    // {
    //     window.width = state.width;
    //     window.height = state.height;
    // }
    //
}

// todo: encapsulate
template <GraphicsBackend B>
auto Application<B>::createPipelineConfig(DeviceHandle<B> device, 
    DescriptorPoolHandle<B> descriptorPool, PipelineCacheHandle<B> pipelineCache,
    std::shared_ptr<PipelineLayoutContext<B>> layoutContext, std::shared_ptr<GraphicsPipelineResourceView<B>> resources) const
{
    PipelineConfiguration<B> outPipelineConfig = {};

    outPipelineConfig.resources = resources;
    outPipelineConfig.layout = layoutContext;

    outPipelineConfig.graphicsPipeline = createGraphicsPipeline(device, pipelineCache, outPipelineConfig);

    outPipelineConfig.descriptorSets = allocateDescriptorSets(
            device, descriptorPool, layoutContext->descriptorSetLayouts.get(),
            layoutContext->descriptorSetLayouts.get_deleter().getSize());

    return outPipelineConfig;
}
