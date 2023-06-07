#pragma once

#include "nodecommon.h"

class InputOutputNode : public INode
{
public:

    InputOutputNode() noexcept = default;
    InputOutputNode(int id, std::string&& name);
    InputOutputNode(const InputOutputNode&) = default;
    InputOutputNode(InputOutputNode&&) noexcept = default;
    ~InputOutputNode() = default;

    InputOutputNode& operator=(const InputOutputNode&) = default;
    InputOutputNode& operator=(InputOutputNode&&) noexcept = default;

    void swap(InputOutputNode& rhs) noexcept;
    friend void swap(InputOutputNode& lhs, InputOutputNode& rhs) noexcept { lhs.swap(rhs); }

    int& id() final;
    std::optional<int>& selected() final;
    std::string& name() final;

    std::vector<Attribute>& inputAttributes();
    std::vector<Attribute>& outputAttributes();

private:

    int myId = 0;
    std::optional<int> mySelected;
    std::string myName;
    std::vector<Attribute> myInputAttributes;
    std::vector<Attribute> myOutputAttributes;
};

#include "inputoutputnode.inl"
