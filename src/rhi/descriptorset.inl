#include "vulkan/descriptorset.inl"

namespace descriptorset
{

template <GraphicsBackend B>
std::vector<DescriptorSetLayoutHandle<B>>
getDescriptorSetLayoutHandles(const DescriptorSetLayoutFlatMap<B>& layouts)
{
	std::vector<DescriptorSetLayoutHandle<Vk>> handles;
	if (!layouts.empty())
	{
		handles.reserve(layouts.size());

		for (const auto& [set, layout] : layouts)
			handles.push_back(static_cast<DescriptorSetLayoutHandle<Vk>>(layout));
	}

	return handles;
}

template <GraphicsBackend B>
std::vector<PushConstantRange<B>>
getPushConstantRanges(const DescriptorSetLayoutFlatMap<B>& layouts)
{
	std::vector<PushConstantRange<Vk>> ranges;
	if (!layouts.empty())
	{
		ranges.reserve(layouts.size());

		for (const auto& [set, layout] : layouts)
			if (auto pcr = layout.getDesc().pushConstantRange; pcr)
				ranges.push_back(*pcr);
	}

	return ranges;
}

} // namespace descriptorset
