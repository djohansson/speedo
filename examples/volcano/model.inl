#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, ModelCreateDesc<B>& desc)
{
	archive(cereal::make_nvp("aabb", desc.aabb));
	archive(cereal::make_nvp("indexBufferSize", desc.indexBufferSize));
	archive(cereal::make_nvp("vertexBufferSize", desc.vertexBufferSize));
	archive(cereal::make_nvp("indexCount", desc.indexCount));
	archive(cereal::make_nvp("attributes", desc.attributes));
}
