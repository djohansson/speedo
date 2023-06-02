#include <cereal/cereal.hpp>

template <typename Archive>
void serialize(Archive& archive, Extent2d<Vk>& obj)
{
	archive(cereal::make_nvp("width", obj.width));
	archive(cereal::make_nvp("height", obj.height));
}

template <typename Archive>
void serialize(Archive& archive, SurfaceFormat<Vk>& obj)
{
	archive(cereal::make_nvp("format", obj.format));
	archive(cereal::make_nvp("colorSpace", obj.colorSpace));
}
