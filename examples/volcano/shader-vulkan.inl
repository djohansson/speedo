#include <cereal/cereal.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>

template <class Archive>
void serialize(Archive& archive, ShaderSet<Vk>& shaderSet)
{
	archive(cereal::make_nvp("shaders", shaderSet.shaders));
	archive(cereal::make_nvp("bindings", shaderSet.layouts));
}
