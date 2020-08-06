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
