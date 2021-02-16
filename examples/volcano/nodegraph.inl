#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

template <class Archive>
void serialize(Archive& archive, NodeGraph& nodeGraph)
{
    archive(
        cereal::make_nvp("nodes", nodeGraph.nodes),
        cereal::make_nvp("links", nodeGraph.links),
        cereal::make_nvp("layout", nodeGraph.layout),
        cereal::make_nvp("uniqueId", nodeGraph.uniqueId));
}
