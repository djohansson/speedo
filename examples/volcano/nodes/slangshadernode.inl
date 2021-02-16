#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>

template <class Archive>
void serialize(Archive& archive, SlangShaderNode& node)
{
    archive(
        cereal::virtual_base_class<InputOutputNode>(&node),
        cereal::make_nvp("path", node.path()));
}
