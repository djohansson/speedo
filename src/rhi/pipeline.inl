#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, PipelineConfiguration<B>& config)
{
	archive(cereal::make_nvp("cachePath", config.cachePath));
}

template <GraphicsBackend B>
const DescriptorSetLayout<B>& PipelineLayout<B>::getDescriptorSetLayout(uint32_t set) const noexcept
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
