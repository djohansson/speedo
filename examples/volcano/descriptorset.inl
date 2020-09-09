namespace descriptorset
{

template <GraphicsBackend B>
std::vector<DescriptorSetLayoutHandle<B>> getDescriptorSetLayoutHandles(
    const std::vector<DescriptorSetLayout<B>>& layouts)
{
    std::vector<DescriptorSetLayoutHandle<Vk>> handles;
    handles.reserve(layouts.size());
    std::transform(
        layouts.begin(),
        layouts.end(),
        std::back_inserter(handles),
        [](const auto& layout){ return static_cast<DescriptorSetLayoutHandle<Vk>>(layout); });
    return handles;
}

}
