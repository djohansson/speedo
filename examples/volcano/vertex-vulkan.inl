#include <cereal/cereal.hpp>

template <class Archive>
void serialize(Archive& archive, SerializableVertexInputAttributeDescription<Vk>& desc)
{
    archive(cereal::make_nvp("location", desc.location));
    archive(cereal::make_nvp("binding", desc.binding));
    archive(cereal::make_nvp("format", desc.format));
    archive(cereal::make_nvp("offset", desc.offset));
}
