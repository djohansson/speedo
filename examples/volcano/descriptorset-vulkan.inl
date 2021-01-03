#include <cereal/cereal.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

template <class Archive>
void serialize(Archive& archive, DescriptorSetLayoutBinding<Vk>& dsb)
{
    archive(
        cereal::make_nvp("binding", dsb.binding), 
        cereal::make_nvp("descriptorType", dsb.descriptorType),
        cereal::make_nvp("descriptorCount", dsb.descriptorCount),
        cereal::make_nvp("stageFlags", dsb.stageFlags)
    );
}

template <class Archive>
void serialize(Archive& archive, PushConstantRange<Vk>& pcr)
{
    archive(
        cereal::make_nvp("stageFlags", pcr.stageFlags),
        cereal::make_nvp("offset", pcr.offset),
        cereal::make_nvp("size", pcr.size)        
    );
}

template <class Archive>
void serialize(Archive& archive, DescriptorSetLayoutCreateDesc<Vk>& desc)
{
    archive(
        cereal::make_nvp("bindings", desc.bindings),
        cereal::make_nvp("variableNames", desc.variableNames),
        cereal::make_nvp("immutableSamplers", desc.immutableSamplers),
        cereal::make_nvp("pushConstantRange", desc.pushConstantRange),
        cereal::make_nvp("flags", desc.flags)
    );
}
