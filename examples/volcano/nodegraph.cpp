#include "nodegraph.h"

#include <cereal/archives/binary.hpp>
#include <cereal/archives/xml.hpp>
#include <cereal/archives/json.hpp>

CEREAL_REGISTER_POLYMORPHIC_RELATION(INode, InputOutputNode)

InputOutputNode::InputOutputNode(int id, std::string&& name)
: id_(id)
, name_(name)
{
}

const int& InputOutputNode::getId() const
{
    return id_;
}

std::string& InputOutputNode::name()
{
    return name_;
}

std::any& InputOutputNode::userData()
{
    return userData_;
}

std::vector<Attribute>& InputOutputNode::inputAttributes()
{
    return inputAttributes_;
}

std::vector<Attribute>& InputOutputNode::outputAttributes()
{
    return outputAttributes_;
}

CEREAL_REGISTER_TYPE(SlangShaderNode);

SlangShaderNode::SlangShaderNode(int id, std::string&& name)
: InputOutputNode{id, std::move(name)}
{
};

std::string& SlangShaderNode::path()
{
    return path_;
}
