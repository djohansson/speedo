
namespace pipeline
{

template <typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename>
inline constexpr bool always_false_v = false;

}

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
