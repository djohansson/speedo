namespace descriptorset
{

template <typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename>
inline constexpr bool always_false_v = false;

}

template <class Archive>
void serialize(Archive& archive, SerializableDescriptorSetLayoutBinding<Vk>& dsb)
{
    // runtime creation of descriptor sets relies on this:
    static_assert(sizeof(dsb) == sizeof(SerializableDescriptorSetLayoutBinding<Vk>::BaseType));

    archive(
        cereal::make_nvp("binding", dsb.binding), 
        cereal::make_nvp("descriptorType", dsb.descriptorType),
        cereal::make_nvp("descriptorCount", dsb.descriptorCount),
        cereal::make_nvp("stageFlags", dsb.stageFlags)
        //cereal::make_nvp("pImmutableSamplers", dsb.pImmutableSamplers), // todo: instantiate these samplers
    );
}

template <>
template <typename T>
void DescriptorSetVector<Vk>::set(T&& data, uint32_t set, uint32_t binding, DescriptorType<Vk> type, uint32_t index)
{
    auto& bindingVector = myData[set];
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
