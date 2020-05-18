#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>


template <class Archive>
void serialize(Archive& archive, Attribute& attribute)
{
    archive(
        cereal::make_nvp("id", attribute.id),
        cereal::make_nvp("name", attribute.name));
}

template <class Archive>
void serialize(Archive& archive, InputOutputNode& node)
{
    archive(
        cereal::make_nvp("name", node.name_),
        cereal::make_nvp("id", node.id_),
        cereal::make_nvp("inputAttributes", node.inputAttributes_),
        cereal::make_nvp("outputAttributes", node.outputAttributes_));
}

template <class Archive>
void serialize(Archive& archive, Link& link)
{
    archive(
        cereal::make_nvp("fromId", link.fromId),
        cereal::make_nvp("toId", link.toId));
}

template <class Archive>
void serialize(Archive& archive, SlangShaderNode& node)
{
    archive(
        cereal::virtual_base_class<InputOutputNode>(&node),
        cereal::make_nvp("path", node.path_));
}

template <class Archive>
void serialize(Archive& archive, NodeGraph& nodeGraph)
{
    archive(
        cereal::make_nvp("nodes", nodeGraph.nodes),
        cereal::make_nvp("links", nodeGraph.links),
        cereal::make_nvp("layout", nodeGraph.layout),
        cereal::make_nvp("uniqueId", nodeGraph.uniqueId));
}
