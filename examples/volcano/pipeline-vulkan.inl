
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
void PipelineContext<Vk>::setDescriptor(
    T&& data,
    DescriptorType<Vk> type,
    uint8_t set,
    uint32_t binding,
    uint32_t index)
{
    auto& bindingVector = myDescriptorValues[internalMakeDescriptorKey(*myCurrentLayout, set)];
    if (bindingVector.size() <= binding)
    {
        bindingVector.resize(binding + 1);
        std::get<0>(bindingVector[binding]) = type;
        std::get<1>(bindingVector[binding]) = std::vector<T>{};
    }

    auto& [bindingType, variant] = bindingVector[binding];
    assert(bindingType == type);

    auto& variantVector = std::get<std::vector<T>>(variant);
    
    if (variantVector.size() <= index)
        variantVector.resize(index + 1);

    variantVector[index] = std::move(data);
}
