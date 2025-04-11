#include <xxhash.h>

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
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, RangeSet<uint32_t>{}));

	ASSERT(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ASSERT(count == 0);
		ASSERT(ranges.empty());

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
		ASSERT(count == 1);

		std::get<T>(bindingsData[offset]) = std::forward<T>(data);
	}

	setState.store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	std::string_view shaderVariableName, T&& data, uint32_t set)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myLayouts.end());
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

	ASSERT(data.size() <= descriptorCount);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, RangeSet<uint32_t>{}));

	ASSERT(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ASSERT(count == 0);
		ASSERT(ranges.empty());

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
		ASSERT(count > 0);

		if (count == data.size())
		{
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

	setState.store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	std::string_view shaderVariableName, const std::vector<T>& data, uint32_t set)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myLayouts.end());
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
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, RangeSet<uint32_t>{}));

	ASSERT(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ASSERT(count == 0);
		ASSERT(ranges.empty());

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
		ASSERT(count > 0);

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

	setState.store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void Pipeline<kVk>::SetDescriptorData(
	std::string_view shaderVariableName, T&& data, uint32_t set, uint32_t index)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		std::forward<T>(data),
		index);
}
