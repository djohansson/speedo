#include <mutex>

#include <xxhash.h>

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    uint64_t shaderVariableNameHash,
    const DescriptorSetLayout<Vk>& layout,
    T&& data)
{
    const auto& [binding, descriptorType, descriptorCount] = layout.getShaderVariableBinding(shaderVariableNameHash);
    auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(layout);

    auto lock = std::lock_guard(mutex);
    
    auto [bindingIt, emplaceResult] = bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, RangeSet<uint32_t>{}));

    assert(bindingIt != bindingsMap.end());

    auto& [offset, count, type, ranges] = bindingIt->second;

    if (emplaceResult)
    {
        assert(count == 0);
        assert(ranges.empty());

        count = 1;
        ranges.insert({0u, 1u});

        if (bindingIt == bindingsMap.begin())
            offset = 0;
        else
            offset = std::get<0>(std::prev(bindingIt)->second) + std::get<1>(std::prev(bindingIt)->second);

        bindingsData.emplace(bindingsData.begin() + offset, std::move(data));

        // prefix sum offset
        for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end(); it++, prev++)
            std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
    }
    else
    {
        assert(count == 1);

        std::get<T>(bindingsData[offset]) = std::move(data);
    }

    setState = DescriptorSetStatus::Dirty;

    internalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    T&& data,
    uint32_t set)
{
    setDescriptorData(
        XXH3_64bits(
            shaderVariableName.data(),
            shaderVariableName.size()),
        getLayout().getDescriptorSetLayout(set),
        std::move(data));
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    uint64_t shaderVariableNameHash,
    const DescriptorSetLayout<Vk>& layout,
    const std::vector<T>& data)
{
    const auto& [binding, descriptorType, descriptorCount] = layout.getShaderVariableBinding(shaderVariableNameHash);
    auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(layout);

    assert(data.size() <= descriptorCount);

    auto lock = std::lock_guard(mutex);
    
    auto [bindingIt, emplaceResult] = bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, RangeSet<uint32_t>{}));

    assert(bindingIt != bindingsMap.end());

    auto& [offset, count, type, ranges] = bindingIt->second;

    if (emplaceResult)
    {
        assert(count == 0);
        assert(ranges.empty());

        count = data.size();
        ranges.insert({0u, count});

        if (bindingIt == bindingsMap.begin())
            offset = 0;
        else
            offset = std::get<0>(std::prev(bindingIt)->second) + std::get<1>(std::prev(bindingIt)->second);

        bindingsData.insert(bindingsData.begin() + offset, data.begin(), data.end());

        // prefix sum offset
        for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end(); it++, prev++)
            std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
    }
    else
    {
        assert(count > 0);

        if (count == data.size())
        {
            std::copy(data.begin(), data.end(), bindingsData.begin() + offset);
        }
        else
        {
            auto minCount = std::min(static_cast<uint32_t>(data.size()), count);

            std::copy(data.begin(), data.begin() + minCount, bindingsData.begin() + offset);

            if (count < data.size())
                bindingsData.insert(bindingsData.begin() + offset + minCount, data.begin() + minCount, data.end());
            else // todo: should we bother with erase?
                bindingsData.erase(bindingsData.begin() + offset + minCount, bindingsData.begin() + offset + count);

            count = data.size();
            ranges.clear();
            ranges.insert({0u, count});

            // prefix sum offset
            for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end(); it++, prev++)
                std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
        }
    }

    setState = DescriptorSetStatus::Dirty;

    internalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    const std::vector<T>& data,
    uint32_t set)
{
    setDescriptorData(
        XXH3_64bits(
            shaderVariableName.data(),
            shaderVariableName.size()),
        getLayout().getDescriptorSetLayout(set),
        data);
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    uint64_t shaderVariableNameHash,
    const DescriptorSetLayout<Vk>& layout,
    T&& data,
    uint32_t index)
{
    const auto& [binding, descriptorType, descriptorCount] = layout.getShaderVariableBinding(shaderVariableNameHash);
    auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(layout);

    auto lock = std::lock_guard(mutex);
    
    auto [bindingIt, emplaceResult] = bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, RangeSet<uint32_t>{}));

    assert(bindingIt != bindingsMap.end());

    auto& [offset, count, type, ranges] = bindingIt->second;

    if (emplaceResult)
    {
        assert(count == 0);
        assert(ranges.empty());

        count = 1;
        ranges.insert({index, index + 1});

        if (bindingIt == bindingsMap.begin())
            offset = 0;
        else
            offset = std::get<0>(std::prev(bindingIt)->second) + std::get<1>(std::prev(bindingIt)->second);

        bindingsData.emplace(bindingsData.begin() + offset, std::move(data));

        // prefix sum offset
        for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end(); it++, prev++)
            std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
    }
    else
    {
        assert(count > 0);

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
            std::get<T>(bindingsData[offset + indexOffset]) = std::move(data);
        }
        else
        {
            bindingsData.emplace(bindingsData.begin() + offset + indexOffset, std::move(data));
            
            count++;
            ranges.insert({index, index + 1});

            // prefix sum offset
            for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end(); it++, prev++)
                std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
        }
    }

    setState = DescriptorSetStatus::Dirty;

    internalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    T&& data,
    uint32_t set,
    uint32_t index)
{
    setDescriptorData(
        XXH3_64bits(
            shaderVariableName.data(),
            shaderVariableName.size()),
        getLayout().getDescriptorSetLayout(set),
        std::move(data),
        index);
}
