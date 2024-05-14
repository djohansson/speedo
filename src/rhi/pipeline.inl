template <GraphicsApi G>
const DescriptorSetLayout<G>& PipelineLayout<G>::GetDescriptorSetLayout(uint32_t set) const noexcept
{
	const auto& setLayouts = GetDescriptorSetLayouts();
	auto setLayoutIt = std::lower_bound(
		setLayouts.begin(),
		setLayouts.end(),
		set,
		[](const auto& setLayout, uint32_t set) { return setLayout.first < set; });
	ASSERT(setLayoutIt != setLayouts.end());
	const auto& [_set, setLayout] = *setLayoutIt;
	ASSERT(_set == set);

	return setLayout;
}

#include "vulkan/pipeline.inl"
