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
        while (layoutIt != layouts.end())
        {
            handles[layoutIt->first] = static_cast<DescriptorSetLayoutHandle<Vk>>(layoutIt->second);
            ++layoutIt;
        }
    }
    
    return handles;
}

template <GraphicsBackend B>
std::vector<PushConstantRange<B>> getPushConstantRanges(
    const DescriptorSetLayoutMap<B>& layouts)
{
    std::vector<PushConstantRange<Vk>> ranges;
    if (!layouts.empty())
    {
        auto layoutIt = layouts.begin();
        while (layoutIt != layouts.end())
        {
            if (auto pcr = layoutIt->second.getDesc().pushConstantRange; pcr.has_value())
                ranges.push_back(*pcr);

            ++layoutIt;
        }
    }
    
    return ranges;
}

}
