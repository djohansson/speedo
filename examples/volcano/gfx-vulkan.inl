template <class Archive>
void serialize(Archive& archive, ApplicationInfo<GraphicsBackend::Vulkan>& desc)
{
    archive(
        //cereal::make_nvp("sType", desc.sType),
        //cereal::make_nvp("pApplicationName", desc.pApplicationName),
        cereal::make_nvp("applicationVersion", desc.applicationVersion),
        //cereal::make_nvp("pEngineName", desc.pEngineName),
        cereal::make_nvp("engineVersion", desc.engineVersion),
        cereal::make_nvp("apiVersion", desc.apiVersion));
}

template <class Archive>
void serialize(Archive& archive, SerializableDescriptorSetLayoutBinding<GraphicsBackend::Vulkan>& dsb)
{
    // runtime creation of descriptor sets relies on this:
    static_assert(sizeof(dsb) == sizeof(SerializableDescriptorSetLayoutBinding<GraphicsBackend::Vulkan>::BaseType));

    archive(
        cereal::make_nvp("binding", dsb.binding), 
        cereal::make_nvp("descriptorType", dsb.descriptorType),
        cereal::make_nvp("descriptorCount", dsb.descriptorCount),
        cereal::make_nvp("stageFlags", dsb.stageFlags)
        //cereal::make_nvp("pImmutableSamplers", dsb.pImmutableSamplers), // todo: instantiate these samplers
    );
}
