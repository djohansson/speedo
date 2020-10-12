#include <cereal/cereal.hpp>

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
