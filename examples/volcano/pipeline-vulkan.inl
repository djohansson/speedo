#include <xxhash.h>

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    T&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    uint32_t binding)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    auto& [bindingsTuple, setArrays] = myDescriptorMap.at(setLayout.getKey());
    auto& [bindingsMap, isDirty] = bindingsTuple;
    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(binding, std::make_tuple(type, std::vector<T>{}));
    auto& bindingVariantVector = std::get<1>(bindingDataPairIt->second);
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);
    
    assert(bindingVector.size() <= 1);
    bindingVector.clear();
    bindingVector.emplace_back(std::move(data));

    isDirty = true;
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    T&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    const std::string& shaderVariableName)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    const auto& setLayoutBindingsMap = setLayout.getBindingsMap();
    uint64_t shaderVariableNameHash = XXH3_64bits(shaderVariableName.c_str(), shaderVariableName.size());
    uint32_t binding = setLayoutBindingsMap.at(shaderVariableNameHash);

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
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    auto& [bindingsTuple, setArrays] = myDescriptorMap.at(setLayout.getKey());
    auto& [bindingsMap, isDirty] = bindingsTuple;
    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(binding, std::make_tuple(type, std::vector<T>{}));
    auto& bindingVariantVector = std::get<1>(bindingDataPairIt->second);
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);
    
    bindingVector = std::move(data);

    isDirty = true;
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    std::vector<T>&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    const std::string& shaderVariableName)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    const auto& setLayoutBindingsMap = setLayout.getBindingsMap();
    uint64_t shaderVariableNameHash = XXH3_64bits(shaderVariableName.c_str(), shaderVariableName.size());
    uint32_t binding = setLayoutBindingsMap.at(shaderVariableNameHash);

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
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    auto& [bindingsTuple, setArrays] = myDescriptorMap.at(setLayout.getKey());
    auto& [bindingsMap, isDirty] = bindingsTuple;
    auto [bindingDataPairIt, emplaceResult] = bindingsMap.emplace(binding, std::make_tuple(type, std::vector<T>{}));
    auto& bindingVariantVector = std::get<1>(bindingDataPairIt->second);
    auto& bindingVector = std::get<std::vector<T>>(bindingVariantVector);
    
    if (bindingVector.size() <= index)
        bindingVector.resize(index + 1);
    
    bindingVector[index] = std::move(data);

    isDirty = true;
}

template <>
template <typename T>
void PipelineContext<Vk>::setDescriptorData(
    T&& data,
    DescriptorType<Vk> type,
    uint32_t set,
    const std::string& shaderVariableName,
    uint32_t index)
{
    const auto& layout = *getLayout();
    const auto& setLayout = layout.getDescriptorSetLayouts().at(set);
    const auto& setLayoutBindingsMap = setLayout.getBindingsMap();
    uint64_t shaderVariableNameHash = XXH3_64bits(shaderVariableName.c_str(), shaderVariableName.size());
    uint32_t binding = setLayoutBindingsMap.at(shaderVariableNameHash);

    setDescriptorData(std::move(data), type, set, binding, index);
}
