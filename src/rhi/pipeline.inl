template <GraphicsApi G>
const DescriptorSetLayout<G>& PipelineLayout<G>::getDescriptorSetLayout(uint32_t set) const noexcept
{
	const auto& setLayouts = getDescriptorSetLayouts();
	auto setLayoutIt = std::lower_bound(
		setLayouts.begin(),
		setLayouts.end(),
		set,
		[](const auto& setLayout, uint32_t set) { return setLayout.first < set; });
	assert(setLayoutIt != setLayouts.end());
	const auto& [_set, setLayout] = *setLayoutIt;
	assert(_set == set);

	return setLayout;
}

#include "vulkan/pipeline.inl"
