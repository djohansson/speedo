#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>


template <class Archive>
void serialize(Archive& archive, Attribute& attribute)
{
    archive(
        cereal::make_nvp("idx", attribute.idx),
        cereal::make_nvp("name", attribute.name));
}

template <class Archive>
void serialize(Archive& archive, InputOutputNode& node)
{
    archive(
        cereal::make_nvp("name", node.name),
        cereal::make_nvp("idx", node.idx),
        cereal::make_nvp("inputAttributes", node.inputAttributes),
        cereal::make_nvp("outputAttributes", node.outputAttributes));
}

template <class Archive>
void serialize(Archive& archive, Link& link)
{
    archive(
        cereal::make_nvp("fromIdx", link.fromIdx),
        cereal::make_nvp("toIdx", link.toIdx));
}

template <class Archive>
void serialize(Archive& archive, SlangShaderNode& node)
{
    archive(
        cereal::virtual_base_class<InputOutputNode>(&node),
        cereal::make_nvp("path", node.path.u8string()));
}

template <class Archive>
void serialize(Archive& archive, NodeGraph& nodeGraph)
{
    archive(
        cereal::make_nvp("nodes", nodeGraph.nodes),
        cereal::make_nvp("links", nodeGraph.links));
}
