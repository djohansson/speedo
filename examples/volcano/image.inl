#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, ImageMipLevelDesc<B>& mipLevel)
{
    archive(cereal::make_nvp("width", mipLevel.extent.width));
    archive(cereal::make_nvp("height", mipLevel.extent.height));
    archive(cereal::make_nvp("size", mipLevel.size));
    archive(cereal::make_nvp("offset", mipLevel.offset));
}

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, ImageCreateDesc<B>& desc)
{
    archive(cereal::make_nvp("mipLevels", desc.mipLevels));
    archive(cereal::make_nvp("format", desc.format));
    archive(cereal::make_nvp("tiling", desc.tiling));
    archive(cereal::make_nvp("usageFlags", desc.usageFlags));
    archive(cereal::make_nvp("memoryFlags", desc.memoryFlags));
}
