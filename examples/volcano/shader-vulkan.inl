#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

template <class Archive>
void serialize(Archive& archive, ShaderReflectionInfo<Vk>& module)
{
	archive(cereal::make_nvp("shaders", module.shaders));
	archive(cereal::make_nvp("bindings", module.bindingsMap));
}
