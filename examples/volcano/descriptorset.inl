namespace descriptorset
{

template <GraphicsBackend B>
std::vector<DescriptorSetLayoutHandle<B>> getDescriptorSetLayoutHandles(
    const DescriptorSetLayoutMap<B>& layouts)
{
    std::vector<DescriptorSetLayoutHandle<Vk>> handles;
    if (!layouts.empty())
    {
        handles.resize(layouts.rbegin()->first + 1);
        
        auto layoutIt = layouts.begin();
        std::fill(handles.begin(), handles.end(), static_cast<DescriptorSetLayoutHandle<Vk>>(layoutIt->second));
        while (++layoutIt != layouts.end())
            handles[layoutIt->first] = static_cast<DescriptorSetLayoutHandle<Vk>>(layoutIt->second);
    }
    
    return handles;
}

}
