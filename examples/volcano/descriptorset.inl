namespace descriptorset
{

template <GraphicsBackend B>
std::vector<DescriptorSetLayoutHandle<B>> getDescriptorSetLayoutHandles(
    const DescriptorSetLayoutMapType<B>& layouts)
{
    std::vector<DescriptorSetLayoutHandle<Vk>> handles;
    if (!layouts.empty())
    {
        auto layoutIt = layouts.begin();
        while (layoutIt != layouts.end())
        {
            if ((layoutIt->first + 1) > handles.size())
                handles.resize(layoutIt->first + 1);

            handles[layoutIt->first] = static_cast<DescriptorSetLayoutHandle<Vk>>(layoutIt->second);

            ++layoutIt;
        }

        // barf @ validation layer that requires this...
        for (auto& handle : handles)
            if (!handle)
                handle = handles[0];
        //
    }
    
    return handles;
}

template <GraphicsBackend B>
std::vector<PushConstantRange<B>> getPushConstantRanges(
    const DescriptorSetLayoutMapType<B>& layouts)
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
