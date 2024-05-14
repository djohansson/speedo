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

int& InputOutputNode::id()
{
    return myId;
}

std::optional<int>& InputOutputNode::selected()
{
    return mySelected;
}

std::string& InputOutputNode::Name()
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
