#include <mutex>

#include <xxhash.h>

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    uint64_t shaderVariableNameHash,
    const DescriptorSetLayout<Vk>& layout,
    T&& data)
{
    auto& [bindingsMap, mutex, setState, templateHandle, setOptionalArrayList] = myDescriptorMap.at(layout);
    const auto& shaderVariableBindingsMap = layout.getShaderVariableBindingsMap();
    const auto& [type, binding] = shaderVariableBindingsMap.at(shaderVariableNameHash);
    
    assert(binding < 0x80000000);

    auto lock = std::lock_guard(mutex);

    uint32_t key = (binding & 0x7FFFFFFF);
    
    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(key, std::make_tuple(type, T{}, RangeSet<uint32_t>{}));
    auto& [descriptorType, bindingVariant, ranges] = bindingDataPairIt->second;
    auto& bindingData = std::get<T>(bindingVariant);

    if (emplaceResult)
        ranges.insert({0u, 1u});

    if (memcmp(&data, &bindingData, sizeof(data)))
    {
        bindingData = std::move(data);
        setState = DescriptorSetStatus::Dirty;
    }
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
        getLayout()->getDescriptorSetLayout(set),
        std::move(data));
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    uint64_t shaderVariableNameHash,
    const DescriptorSetLayout<Vk>& layout,
    std::vector<T>&& data)
{
    auto& [bindingsMap, mutex, setState, templateHandle, setOptionalArrayList] = myDescriptorMap.at(layout);
    const auto& shaderVariableBindingsMap = layout.getShaderVariableBindingsMap();
    const auto& [type, binding] = shaderVariableBindingsMap.at(shaderVariableNameHash);

    assert(binding < 0x80000000);

    auto lock = std::lock_guard(mutex);

    uint32_t key = (binding & 0x7FFFFFFF) | static_cast<uint32_t>(BindingFlags::IsArray);

    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(key, std::make_tuple(type, std::vector<T>{}, RangeSet<uint32_t>{}));
    auto& [descriptorType, bindingVariantVector, ranges] = bindingDataPairIt->second;
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);

    if (data.size() != bindingVector.size() || memcmp(data.data(), bindingVector.data(), data.size() * sizeof(T)))
    {
        bindingVector = std::move(data);
        ranges.clear();
        ranges.insert({0u, static_cast<uint32_t>(bindingVector.size())});
        setState = DescriptorSetStatus::Dirty;
    }
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::string_view shaderVariableName,
    std::vector<T>&& data,
    uint32_t set)
{
    setDescriptorData(
        XXH3_64bits(
            shaderVariableName.data(),
            shaderVariableName.size()),
        getLayout()->getDescriptorSetLayout(set),
        std::move(data));
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    uint64_t shaderVariableNameHash,
    const DescriptorSetLayout<Vk>& layout,
    T&& data,
    uint32_t index)
{
    auto& [bindingsMap, mutex, setState, templateHandle, setOptionalArrayList] = myDescriptorMap.at(layout);
    const auto& shaderVariableBindingsMap = layout.getShaderVariableBindingsMap();
    const auto& [type, binding] = shaderVariableBindingsMap.at(shaderVariableNameHash);

    assert(binding < 0x80000000);

    auto lock = std::lock_guard(mutex);

    uint32_t key = (binding & 0x7FFFFFFF) | static_cast<uint32_t>(BindingFlags::IsArray);

    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(key, std::make_tuple(type, std::vector<T>{}, RangeSet<uint32_t>{}));
    auto& [descriptorType, bindingVariantVector, ranges] = bindingDataPairIt->second;
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);
    
    if (bindingVector.size() <= index)
        bindingVector.resize(index + 1);

    if (memcmp(&data, &bindingVector[index], sizeof(data)))
    {
        bindingVector[index] = std::move(data);
        ranges.insert({index, index + 1});
        setState = DescriptorSetStatus::Dirty;
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
    setDescriptorData(
        XXH3_64bits(
            shaderVariableName.data(),
            shaderVariableName.size()),
        getLayout()->getDescriptorSetLayout(set),
        std::move(data),
        index);
}
