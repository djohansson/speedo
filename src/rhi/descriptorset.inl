namespace rhi
{

namespace descriptorset
{

template <GraphicsApi G>
std::vector<DescriptorSetLayoutHandle<G>>
GetDescriptorSetLayoutHandles(const DescriptorSetLayoutFlatMap<G>& layouts)
{
	std::vector<DescriptorSetLayoutHandle<G>> handles;
	if (!layouts.empty())
	{
		handles.reserve(layouts.size());

		for (const auto& [set, layout] : layouts)
			handles.push_back(static_cast<DescriptorSetLayoutHandle<G>>(layout));
	}

	return handles;
}

template <GraphicsApi G>
std::vector<PushConstantRange<G>>
GetPushConstantRanges(const DescriptorSetLayoutFlatMap<G>& layouts)
{
	std::vector<PushConstantRange<G>> ranges;
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

// the descriptor classes' Swap and moves, and DescriptorUpdateTemplate's destructor (the backend destroys its template,
// InternalDestroyTemplate): the same for every backend

template <GraphicsApi G>
void DescriptorSetLayout<G>::Swap(DescriptorSetLayout& rhs) noexcept
{
	DeviceObject<DescriptorSetLayout<G>>::Swap(rhs);
	std::swap(myLayout, rhs.myLayout);
}

template <GraphicsApi G>
DescriptorSetLayout<G>::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
DescriptorSetLayout<G>& DescriptorSetLayout<G>::operator=(DescriptorSetLayout&& other) noexcept
{
	Swap(other);
	return *this;
}

template <GraphicsApi G>
void DescriptorSetArray<G>::Swap(DescriptorSetArray& rhs) noexcept
{
	DeviceObject<DescriptorSetArray<G>>::Swap(rhs);
	std::swap(myDescriptorSets, rhs.myDescriptorSets);
}

template <GraphicsApi G>
DescriptorSetArray<G>::DescriptorSetArray(DescriptorSetArray&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
DescriptorSetArray<G>& DescriptorSetArray<G>::operator=(DescriptorSetArray&& other) noexcept
{
	Swap(other);
	return *this;
}

template <GraphicsApi G>
void DescriptorUpdateTemplate<G>::Swap(DescriptorUpdateTemplate& rhs) noexcept
{
	DeviceObject<DescriptorUpdateTemplate<G>>::Swap(rhs);
	std::swap(myEntries, rhs.myEntries);
	std::swap(myHandle, rhs.myHandle);
}

template <GraphicsApi G>
DescriptorUpdateTemplate<G>::DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
DescriptorUpdateTemplate<G>::~DescriptorUpdateTemplate()
{
	if (this->IsValid())
		InternalDestroyTemplate();
}

template <GraphicsApi G>
DescriptorUpdateTemplate<G>& DescriptorUpdateTemplate<G>::operator=(DescriptorUpdateTemplate&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi

// global scope: found via ADL on the (global) Vulkan structs they serialize
#include <rhi/vulkan/descriptorset.inl>
