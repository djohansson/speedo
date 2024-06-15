namespace descriptorset
{

template <GraphicsApi G>
std::vector<DescriptorSetLayoutHandle<G>>
GetDescriptorSetLayoutHandles(const DescriptorSetLayoutFlatMap<G>& layouts)
{
	std::vector<DescriptorSetLayoutHandle<kVk>> handles;
	if (!layouts.empty())
	{
		handles.reserve(layouts.size());

		for (const auto& [set, layout] : layouts)
			handles.push_back(static_cast<DescriptorSetLayoutHandle<kVk>>(layout));
	}

	return handles;
}

template <GraphicsApi G>
std::vector<PushConstantRange<G>>
GetPushConstantRanges(const DescriptorSetLayoutFlatMap<G>& layouts)
{
	std::vector<PushConstantRange<kVk>> ranges;
	if (!layouts.empty())
	{
		ranges.reserve(layouts.size());

		for (const auto& [set, layout] : layouts)
			if (auto pcr = layout.GetDesc().pushConstantRange; pcr)
				ranges.push_back(*pcr);
	}

	return ranges;
}

} // namespace descriptorset

#include "vulkan/descriptorset.inl"
