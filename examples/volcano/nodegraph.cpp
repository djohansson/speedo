#include "nodegraph.h"

#include <cereal/archives/binary.hpp>
#include <cereal/archives/xml.hpp>
#include <cereal/archives/json.hpp>

CEREAL_REGISTER_POLYMORPHIC_RELATION(INode, InputOutputNode)

InputOutputNode::InputOutputNode(int id, std::string&& name)
: myId(id)
, myName(std::move(name))
{
}

int& InputOutputNode::id()
{
    return myId;
}

std::string& InputOutputNode::name()
{
    return myName;
}

std::any& InputOutputNode::userData()
{
    return myUserData;
}

std::vector<Attribute>& InputOutputNode::inputAttributes()
{
    return myInputAttributes;
}

std::vector<Attribute>& InputOutputNode::outputAttributes()
{
    return myOutputAttributes;
}

CEREAL_REGISTER_TYPE(SlangShaderNode);

SlangShaderNode::SlangShaderNode(int id, std::string&& name, std::filesystem::path&& path)
: InputOutputNode(id, std::move(name))
, myPath(std::move(path))
{
};

std::filesystem::path& SlangShaderNode::path()
{
    return myPath;
}
