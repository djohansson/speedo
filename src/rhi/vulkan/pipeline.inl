namespace rhi
{

namespace pipeline
{

// used to skip redundant descriptor updates: every update marks the descriptor set dirty, and binding a dirty set
// consumes a fresh descriptor set from the pool. compared per field, since the vulkan structs have padding.
[[nodiscard]] inline bool SameBindingValue(const DescriptorBufferInfo<kVk>& lhs, const DescriptorBufferInfo<kVk>& rhs) noexcept
{
	return lhs.buffer == rhs.buffer && lhs.offset == rhs.offset && lhs.range == rhs.range;
}

[[nodiscard]] inline bool SameBindingValue(const DescriptorImageInfo<kVk>& lhs, const DescriptorImageInfo<kVk>& rhs) noexcept
{
	return lhs.sampler == rhs.sampler && lhs.imageView == rhs.imageView && lhs.imageLayout == rhs.imageLayout;
}

template <typename T>
[[nodiscard]] bool SameBindingValue(const T& lhs, const T& rhs) noexcept // handles, inline uniform blocks
{
	return lhs == rhs;
}

template <typename T>
[[nodiscard]] bool SameBinding(const BindingVariant<kVk>& existing, const T& data) noexcept
{
	const auto* current = std::get_if<std::remove_cvref_t<T>>(&existing);
	return current != nullptr && SameBindingValue(*current, data);
}

} // namespace pipeline

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	uint64_t shaderVariableNameHash, const DescriptorSetLayout<kVk>& layout, T&& data)
{
	const auto& [binding, descriptorType, descriptorCount] =
		layout.GetShaderVariableBinding(shaderVariableNameHash);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(layout);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, core::RangeSet<uint32_t>{}));

	ENSURE(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ENSURE(count == 0);
		ENSURE(ranges.empty());

		count = 1;
		ranges.insert({0U, 1U});

		if (bindingIt == bindingsMap.begin())
			offset = 0;
		else
			offset = std::get<0>(std::prev(bindingIt)->second) +
					 std::get<1>(std::prev(bindingIt)->second);

		bindingsData.emplace(bindingsData.begin() + offset, std::forward<T>(data));

		// prefix sum offset
		for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
			 it++, prev++)
			std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
	}
	else
	{
		ENSURE(count == 1);

		if (pipeline::SameBinding(bindingsData[offset], data))
			return;

		std::get<T>(bindingsData[offset]) = std::forward<T>(data);
	}

	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	std::string_view shaderVariableName, T&& data, uint32_t set)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		std::forward<T>(data));
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	uint64_t shaderVariableNameHash,
	const DescriptorSetLayout<kVk>& layout,
	const std::vector<T>& data)
{
	const auto& [binding, descriptorType, descriptorCount] =
		layout.GetShaderVariableBinding(shaderVariableNameHash);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(layout);

	ENSURE(data.size() <= descriptorCount);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, core::RangeSet<uint32_t>{}));

	ENSURE(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ENSURE(count == 0);
		ENSURE(ranges.empty());

		count = data.size();
		ranges.insert({0U, count});

		if (bindingIt == bindingsMap.begin())
			offset = 0;
		else
			offset = std::get<0>(std::prev(bindingIt)->second) +
					 std::get<1>(std::prev(bindingIt)->second);

		bindingsData.insert(bindingsData.begin() + offset, data.begin(), data.end());

		// prefix sum offset
		for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
			 it++, prev++)
			std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
	}
	else
	{
		ENSURE(count > 0);

		if (count == data.size())
		{
			if (std::equal(data.begin(), data.end(), bindingsData.begin() + offset,
					[](const T& value, const BindingVariant<kVk>& existing) { return pipeline::SameBinding(existing, value); }))
				return;

			std::copy(data.begin(), data.end(), bindingsData.begin() + offset);
		}
		else
		{
			auto minCount = std::min(static_cast<uint32_t>(data.size()), count);

			std::copy(data.begin(), data.begin() + minCount, bindingsData.begin() + offset);

			if (count < data.size())
				bindingsData.insert(
					bindingsData.begin() + offset + minCount, data.begin() + minCount, data.end());
			else // todo: should we bother with erase?
				bindingsData.erase(
					bindingsData.begin() + offset + minCount,
					bindingsData.begin() + offset + count);

			count = data.size();
			ranges.clear();
			ranges.insert({0U, count});

			// prefix sum offset
			for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
				 it++, prev++)
				std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
		}
	}

	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	std::string_view shaderVariableName, const std::vector<T>& data, uint32_t set)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		data);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	uint64_t shaderVariableNameHash,
	const DescriptorSetLayout<kVk>& layout,
	T&& data,
	uint32_t index)
{
	const auto& [binding, descriptorType, descriptorCount] =
		layout.GetShaderVariableBinding(shaderVariableNameHash);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(layout);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, core::RangeSet<uint32_t>{}));

	ENSURE(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ENSURE(count == 0);
		ENSURE(ranges.empty());

		count = 1;
		ranges.insert({index, index + 1});

		if (bindingIt == bindingsMap.begin())
			offset = 0;
		else
			offset = std::get<0>(std::prev(bindingIt)->second) +
					 std::get<1>(std::prev(bindingIt)->second);

		bindingsData.emplace(bindingsData.begin() + offset, std::forward<T>(data));

		// prefix sum offset
		for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
			 it++, prev++)
			std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
	}
	else
	{
		ENSURE(count > 0);

		auto rangeIt = ranges.begin();
		uint32_t indexOffset = 0;
		for (; rangeIt != ranges.end(); rangeIt++)
		{
			const auto& [low, high] = *rangeIt;

			if (index >= low && index < high)
			{
				indexOffset += index - low;
				break;
			}

			indexOffset += high - low;
		}

		if (rangeIt != ranges.end())
		{
			if (pipeline::SameBinding(bindingsData[offset + indexOffset], data))
				return;

			std::get<T>(bindingsData[offset + indexOffset]) = std::forward<T>(data);
		}
		else
		{
			bindingsData.emplace(
				bindingsData.begin() + offset + indexOffset, std::forward<T>(data));

			count++;
			ranges.insert({index, index + 1});

			// prefix sum offset
			for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
				 it++, prev++)
				std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
		}
	}

	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	std::string_view shaderVariableName, T&& data, uint32_t set, uint32_t index)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		std::forward<T>(data),
		index);
}

} // namespace rhi
