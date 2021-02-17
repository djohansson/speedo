#include <mutex>

#include <xxhash.h>

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    T&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    uint32_t binding)
{
    assert(binding < 0x80000000);

    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    auto& [bindingsMap, mutex, setState, setOptionalArrayList] = myDescriptorMap.at(setLayout.getKey());

    auto lock = std::lock_guard(mutex);

    uint32_t key = (binding & 0x7FFFFFFF);
    
    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(key, std::make_tuple(type, T{}, RangeSet<uint32_t>{}));
    auto& [descriptorType, bindingVariant, dirtyRanges] = bindingDataPairIt->second;
    auto& bindingData = std::get<T>(bindingVariant);

    if (memcmp(&data, &bindingData, sizeof(data)))
    {
        bindingData = std::move(data);
        dirtyRanges.insert({0u, 1u});
        setState = DescriptorSetState::Dirty;
    }
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    T&& data,
    uint32_t set)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    const auto& setLayoutBindingsMap = setLayout.getBindingsMap();
    uint64_t shaderVariableNameHash = XXH3_64bits(shaderVariableName.data(), shaderVariableName.size());
    auto& [type, binding] = setLayoutBindingsMap.at(shaderVariableNameHash);

    setDescriptorData(std::move(data), type, set, binding);
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::vector<T>&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    uint32_t binding)
{
    assert(binding < 0x80000000);

    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    auto& [bindingsMap, mutex, setState, setOptionalArrayList] = myDescriptorMap.at(setLayout.getKey());

    auto lock = std::lock_guard(mutex);

    uint32_t key = (binding & 0x7FFFFFFF) | static_cast<uint32_t>(BindingTypeFlags::IsArray);

    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(key, std::make_tuple(type, std::vector<T>{}, RangeSet<uint32_t>{}));
    auto& [descriptorType, bindingVariantVector, dirtyRanges] = bindingDataPairIt->second;
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);

    if (data.size() != bindingVector.size() || memcmp(data.data(), bindingVector.data(), data.size() * sizeof(T)))
    {
        bindingVector = std::move(data);
        dirtyRanges.insert({0u, static_cast<uint32_t>(bindingVector.size())});
        setState = DescriptorSetState::Dirty;
    }
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    std::vector<T>&& data,
    uint32_t set)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    const auto& setLayoutBindingsMap = setLayout.getBindingsMap();
    uint64_t shaderVariableNameHash = XXH3_64bits(shaderVariableName.data(), shaderVariableName.size());
    auto& [type, binding] = setLayoutBindingsMap.at(shaderVariableNameHash);

    setDescriptorData(std::move(data), type, set, binding);
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    T&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    uint32_t binding,
    uint32_t index)
{
    assert(binding < 0x80000000);

    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    auto& [bindingsMap, mutex, setState, setOptionalArrayList] = myDescriptorMap.at(setLayout.getKey());

    auto lock = std::lock_guard(mutex);

    uint32_t key = (binding & 0x7FFFFFFF) | static_cast<uint32_t>(BindingTypeFlags::IsArray);

    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(key, std::make_tuple(type, std::vector<T>{}, RangeSet<uint32_t>{}));
    auto& [descriptorType, bindingVariantVector, dirtyRanges] = bindingDataPairIt->second;
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);
    
    if (bindingVector.size() <= index)
        bindingVector.resize(index + 1);

    if (memcmp(&data, &bindingVector[index], sizeof(data)))
    {
        bindingVector[index] = std::move(data);
        dirtyRanges.insert({index, index + 1});
        setState = DescriptorSetState::Dirty;
    }
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    T&& data,
    uint32_t set,
    uint32_t index)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    const auto& setLayoutBindingsMap = setLayout.getBindingsMap();
    uint64_t shaderVariableNameHash = XXH3_64bits(shaderVariableName.data(), shaderVariableName.size());
    auto& [type, binding] = setLayoutBindingsMap.at(shaderVariableNameHash);

    setDescriptorData(std::move(data), type, set, binding, index);
}
