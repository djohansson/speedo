#include "nodegraph.h"

#include <cereal/archives/json.hpp>
#include <cereal/types/polymorphic.hpp>

CEREAL_REGISTER_POLYMORPHIC_RELATION(INode, InputOutputNode)

InputOutputNode::InputOutputNode(int id, std::string&& name)
: myId(id)
, myName(std::move(name))
{
}

void InputOutputNode::swap(InputOutputNode& rhs) noexcept
{
    std::swap(myId, rhs.myId);
    std::swap(myName, rhs.myName);
}

int& InputOutputNode::id()
{
    return myId;
}

std::optional<int>& InputOutputNode::selected()
{
    return mySelected;
}

std::string& InputOutputNode::name()
{
    return myName;
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
}

void SlangShaderNode::swap(SlangShaderNode& rhs) noexcept
{
    InputOutputNode::swap(rhs);
    std::swap(myPath, rhs.myPath);
}

std::filesystem::path& SlangShaderNode::path()
{
    return myPath;
}
