#include "inputoutputnode.h"

InputOutputNode::InputOutputNode(int id, std::string&& name)
: myId(id)
, myName(std::forward<std::string>(name))
{
}

void InputOutputNode::Swap(InputOutputNode& rhs) noexcept
{
    std::swap(myId, rhs.myId);
    std::swap(myName, rhs.myName);
}

int& InputOutputNode::Id()
{
    return myId;
}

std::optional<int>& InputOutputNode::Selected()
{
    return mySelected;
}

std::string& InputOutputNode::Name()
{
    return myName;
}

std::vector<Attribute>& InputOutputNode::InputAttributes()
{
    return myInputAttributes;
}

std::vector<Attribute>& InputOutputNode::OutputAttributes()
{
    return myOutputAttributes;
}
