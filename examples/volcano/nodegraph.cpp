#include "nodegraph.h"

#include <cereal/archives/binary.hpp>
#include <cereal/archives/xml.hpp>
#include <cereal/archives/json.hpp>

CEREAL_REGISTER_POLYMORPHIC_RELATION(INode, InputOutputNode)

InputOutputNode::InputOutputNode(int idx, std::string&& name)
: idx(idx)
, name(name)
{
}

int InputOutputNode::getIdx() const
{
    return idx;
}

const std::string& InputOutputNode::getName() const
{
    return name;
}

CEREAL_REGISTER_TYPE(SlangShaderNode);

SlangShaderNode::SlangShaderNode(int idx, std::string&& name)
: InputOutputNode{idx, std::move(name)}
{
};
