#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>

template <class Archive>
void serialize(Archive& archive, SamplerCreateInfo<Vk>& samplerInfo)
{
	archive(cereal::make_nvp("flags", samplerInfo.flags));
    archive(cereal::make_nvp("magFilter", samplerInfo.magFilter));
    archive(cereal::make_nvp("minFilter", samplerInfo.minFilter));
    archive(cereal::make_nvp("mipmapMode", samplerInfo.mipmapMode));
    archive(cereal::make_nvp("addressModeU", samplerInfo.addressModeU));
    archive(cereal::make_nvp("addressModeV", samplerInfo.addressModeV));
    archive(cereal::make_nvp("addressModeW", samplerInfo.addressModeW));
    archive(cereal::make_nvp("mipLodBias", samplerInfo.mipLodBias));
    archive(cereal::make_nvp("anisotropyEnable", samplerInfo.anisotropyEnable));
    archive(cereal::make_nvp("maxAnisotropy", samplerInfo.maxAnisotropy));
    archive(cereal::make_nvp("compareEnable", samplerInfo.compareEnable));
    archive(cereal::make_nvp("compareOp", samplerInfo.compareOp));
    archive(cereal::make_nvp("minLod", samplerInfo.minLod));
    archive(cereal::make_nvp("maxLod", samplerInfo.maxLod));
    archive(cereal::make_nvp("borderColor", samplerInfo.borderColor));
    archive(cereal::make_nvp("unnormalizedCoordinates", samplerInfo.unnormalizedCoordinates));
}
