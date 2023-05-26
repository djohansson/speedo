#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

template <class Archive>
void serialize(Archive& archive, InputOutputNode& node)
{
    archive(
        cereal::make_nvp("name", node.name()),
        cereal::make_nvp("id", node.id()),
        cereal::make_nvp("inputAttributes", node.inputAttributes()),
        cereal::make_nvp("outputAttributes", node.outputAttributes()));
}
