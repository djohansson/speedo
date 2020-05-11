template <GraphicsBackend B>
void Application<B>::resizeWindow(const WindowState& state)
{
    if (state.fullscreenEnabled)
    {
        myWindow->onResize({state.fullscreenWidth, state.fullscreenHeight});
    }
    else
    {
        myWindow->onResize({state.width, state.height});
    }
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

template <GraphicsBackend B>
const char* Application<B>::getName() const
{
    return myInstance->getConfig().applicationName.c_str();
}